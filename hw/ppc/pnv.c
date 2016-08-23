/*
 * QEMU PowerPC PowerNV model
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 * Copyright (c) 2010 David Gibson, IBM Corporation.
 * Copyright (c) 2014-2016 BenH, IBM Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */
#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "hw/fw-path-provider.h"
#include "elf.h"
#include "net/net.h"
#include "sysemu/block-backend.h"
#include "sysemu/cpus.h"
#include "sysemu/kvm.h"
#include "sysemu/numa.h"
#include "sysemu/device_tree.h"
#include "kvm_ppc.h"
#include "mmu-hash64.h"
#include "qom/cpu.h"

#include "hw/boards.h"
#include "hw/ppc/fdt.h"
#include "hw/ppc/ppc.h"
#include "hw/ppc/pnv.h"
#include "hw/loader.h"
#include "hw/ppc/xics.h"
#include "hw/ppc/pnv_xscom.h"

#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/msi.h"
#include "hw/isa/isa.h"
#include "hw/char/serial.h"
#include "hw/timer/mc146818rtc.h"
#include "hw/pci-host/pnv_phb3.h"
#include "hw/usb.h"
#include "hw/ide/pci.h"
#include "hw/ide/ahci.h"

#include "exec/address-spaces.h"
#include "qemu/config-file.h"
#include "qapi/error.h"
#include "trace.h"

#include "hw/compat.h"
#include "hw/ipmi/ipmi.h"
#include "qemu/cutils.h"

#include <libfdt.h>

#define FDT_ADDR                0x01000000
#define FDT_MAX_SIZE            0x00100000
#define FW_MAX_SIZE             0x00400000
#define FW_FILE_NAME            "skiboot.lid"

#define TIMEBASE_FREQ           512000000ULL

#define MAX_CPUS                255

static XICSState *try_create_xics(const char *type, int nr_servers,
                                  int nr_irqs, Error **errp)
{
    Error *err = NULL;
    DeviceState *dev;

    dev = qdev_create(NULL, type);
    qdev_prop_set_uint32(dev, "nr_servers", nr_servers);
    object_property_set_bool(OBJECT(dev), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        object_unparent(OBJECT(dev));
        return NULL;
    }

    return XICS_COMMON(dev);
}

static XICSState *xics_system_init(int nr_servers, int nr_irqs)
{
    XICSState *xics = NULL;

#if 0 /* Some fixing needed to handle native ICS in KVM mode */
    if (kvm_enabled()) {
        QemuOpts *machine_opts = qemu_get_machine_opts();
        bool irqchip_allowed = qemu_opt_get_bool(machine_opts,
                                                "kernel_irqchip", true);
        bool irqchip_required = qemu_opt_get_bool(machine_opts,
                                                  "kernel_irqchip", false);
        if (irqchip_allowed) {
                icp = try_create_xics(TYPE_KVM_XICS, nr_servers, nr_irqs,
                                      &error_abort);
        }

        if (irqchip_required && !icp) {
            perror("Failed to create in-kernel XICS\n");
            abort();
        }
    }
#endif

    if (!xics) {
        xics = try_create_xics(TYPE_XICS_NATIVE, nr_servers, nr_irqs,
                               &error_abort);
    }

    if (!xics) {
        perror("Failed to create XICS\n");
        abort();
    }
    return xics;
}

static void powernv_populate_memory_node(void *fdt, int nodeid, hwaddr start,
                                         hwaddr size)
{
    /* Probablly bogus, need to match with what's going on in CPU nodes */
    uint32_t chip_id[] = {
        cpu_to_be32(0x0), cpu_to_be32(nodeid)
    };
    char *mem_name;
    uint64_t mem_reg_property[2];

    mem_reg_property[0] = cpu_to_be64(start);
    mem_reg_property[1] = cpu_to_be64(size);

    mem_name = g_strdup_printf("memory@"TARGET_FMT_lx, start);
    _FDT((fdt_begin_node(fdt, mem_name)));
    g_free(mem_name);
    _FDT((fdt_property_string(fdt, "device_type", "memory")));
    _FDT((fdt_property(fdt, "reg", mem_reg_property,
                       sizeof(mem_reg_property))));
    _FDT((fdt_property(fdt, "ibm,chip-id", chip_id, sizeof(chip_id))));
    _FDT((fdt_end_node(fdt)));
}

static int powernv_populate_memory(void *fdt)
{
    hwaddr mem_start, node_size;
    int i, nb_nodes = nb_numa_nodes;
    NodeInfo *nodes = numa_info;
    NodeInfo ramnode;

    /* No NUMA nodes, assume there is just one node with whole RAM */
    if (!nb_numa_nodes) {
        nb_nodes = 1;
        ramnode.node_mem = ram_size;
        nodes = &ramnode;
    }

    for (i = 0, mem_start = 0; i < nb_nodes; ++i) {
        if (!nodes[i].node_mem) {
            continue;
        }
        if (mem_start >= ram_size) {
            node_size = 0;
        } else {
            node_size = nodes[i].node_mem;
            if (node_size > ram_size - mem_start) {
                node_size = ram_size - mem_start;
            }
        }
        for ( ; node_size; ) {
            hwaddr sizetmp = pow2floor(node_size);

            /* mem_start != 0 here */
            if (ctzl(mem_start) < ctzl(sizetmp)) {
                sizetmp = 1ULL << ctzl(mem_start);
            }

            powernv_populate_memory_node(fdt, i, mem_start, sizetmp);
            node_size -= sizetmp;
            mem_start += sizetmp;
        }
    }

    return 0;
}

static void powernv_create_cpu_node(void *fdt, CPUState *cs, int smt_threads)
{
    PowerPCCPU *cpu = POWERPC_CPU(cs);
    CPUPPCState *env = &cpu->env;
    DeviceClass *dc = DEVICE_GET_CLASS(cs);
    PowerPCCPUClass *pcc = POWERPC_CPU_GET_CLASS(cs);
    uint32_t servers_prop[smt_threads];
    uint32_t gservers_prop[smt_threads * 2];
    int i, index = ppc_get_vcpu_dt_id(cpu);
    uint32_t segs[] = {cpu_to_be32(28), cpu_to_be32(40),
                       0xffffffff, 0xffffffff};
    uint32_t tbfreq = kvm_enabled() ? kvmppc_get_tbfreq() : TIMEBASE_FREQ;
    uint32_t cpufreq = kvm_enabled() ? kvmppc_get_clockfreq() : 1000000000;
    uint32_t page_sizes_prop[64];
    size_t page_sizes_prop_size;
    char *nodename;
    const uint8_t pa_features[] = { 24, 0,
                                    0xf6, 0x3f, 0xc7, 0xc0, 0x80, 0xf0,
                                    0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
                                    0x80, 0x00, 0x80, 0x00, 0x80, 0x00 };

    if ((index % smt_threads) != 0) {
        return;
    }

    nodename = g_strdup_printf("%s@%x", dc->fw_name, index);

    _FDT((fdt_begin_node(fdt, nodename)));

    g_free(nodename);

    _FDT((fdt_property_cell(fdt, "reg", index)));
    _FDT((fdt_property_string(fdt, "device_type", "cpu")));

    _FDT((fdt_property_cell(fdt, "cpu-version", env->spr[SPR_PVR])));
    _FDT((fdt_property_cell(fdt, "d-cache-block-size",
                            env->dcache_line_size)));
    _FDT((fdt_property_cell(fdt, "d-cache-line-size",
                            env->dcache_line_size)));
    _FDT((fdt_property_cell(fdt, "i-cache-block-size",
                            env->icache_line_size)));
    _FDT((fdt_property_cell(fdt, "i-cache-line-size",
                            env->icache_line_size)));

    if (pcc->l1_dcache_size) {
        _FDT((fdt_property_cell(fdt, "d-cache-size", pcc->l1_dcache_size)));
    } else {
        error_report("Warning: Unknown L1 dcache size for cpu");
    }
    if (pcc->l1_icache_size) {
        _FDT((fdt_property_cell(fdt, "i-cache-size", pcc->l1_icache_size)));
    } else {
        error_report("Warning: Unknown L1 icache size for cpu");
    }

    _FDT((fdt_property_cell(fdt, "timebase-frequency", tbfreq)));
    _FDT((fdt_property_cell(fdt, "clock-frequency", cpufreq)));
    _FDT((fdt_property_cell(fdt, "ibm,slb-size", env->slb_nr)));
    _FDT((fdt_property_string(fdt, "status", "okay")));
    _FDT((fdt_property(fdt, "64-bit", NULL, 0)));

    if (env->spr_cb[SPR_PURR].oea_read) {
        _FDT((fdt_property(fdt, "ibm,purr", NULL, 0)));
    }

    if (env->mmu_model & POWERPC_MMU_1TSEG) {
        _FDT((fdt_property(fdt, "ibm,processor-segment-sizes",
                           segs, sizeof(segs))));
    }

    /* Advertise VMX/VSX (vector extensions) if available
     *   0 / no property == no vector extensions
     *   1               == VMX / Altivec available
     *   2               == VSX available */
    if (env->insns_flags & PPC_ALTIVEC) {
        uint32_t vmx = (env->insns_flags2 & PPC2_VSX) ? 2 : 1;

        _FDT((fdt_property_cell(fdt, "ibm,vmx", vmx)));
    }

    /* Advertise DFP (Decimal Floating Point) if available
     *   0 / no property == no DFP
     *   1               == DFP available */
    if (env->insns_flags2 & PPC2_DFP) {
        _FDT((fdt_property_cell(fdt, "ibm,dfp", 1)));
    }

    page_sizes_prop_size = ppc_create_page_sizes_prop(env, page_sizes_prop,
                                                  sizeof(page_sizes_prop));
    if (page_sizes_prop_size) {
        _FDT((fdt_property(fdt, "ibm,segment-page-sizes",
                           page_sizes_prop, page_sizes_prop_size)));
    }

    _FDT((fdt_property(fdt, "ibm,pa-features",
                       pa_features, sizeof(pa_features))));

    /* XXX Just a hack for now */
    _FDT((fdt_property_cell(fdt, "ibm,chip-id", 0)));

    if (cpu->cpu_version) {
        _FDT((fdt_property_cell(fdt, "cpu-version", cpu->cpu_version)));
    }

    /* Build interrupt servers and gservers properties */
    for (i = 0; i < smt_threads; i++) {
        servers_prop[i] = cpu_to_be32(index + i);
        /* Hack, direct the group queues back to cpu 0 */
        gservers_prop[i * 2] = cpu_to_be32(index + i);
        gservers_prop[i * 2 + 1] = 0;
    }
    _FDT((fdt_property(fdt, "ibm,ppc-interrupt-server#s",
                       servers_prop, sizeof(servers_prop))));
    _FDT((fdt_property(fdt, "ibm,ppc-interrupt-gserver#s",
                       gservers_prop, sizeof(gservers_prop))));

    _FDT((fdt_end_node(fdt)));
}

static void *powernv_create_fdt(sPowerNVMachineState *s,
                                const char *kernel_cmdline,
                                uint32_t initrd_base, uint32_t initrd_size)
{
    void *fdt;
    CPUState *cs;
    int smt = kvmppc_smt_threads();
    uint32_t start_prop = cpu_to_be32(initrd_base);
    uint32_t end_prop = cpu_to_be32(initrd_base + initrd_size);
    char *buf;
    const char plat_compat[] = "qemu,powernv\0ibm,powernv";
    unsigned int i;

    fdt = g_malloc0(FDT_MAX_SIZE);
    _FDT((fdt_create(fdt, FDT_MAX_SIZE)));
    _FDT((fdt_finish_reservemap(fdt)));

    /* Root node */
    _FDT((fdt_begin_node(fdt, "")));
    _FDT((fdt_property_string(fdt, "model", "IBM PowerNV (emulated by qemu)")));
    _FDT((fdt_property(fdt, "compatible", plat_compat, sizeof(plat_compat))));

    /*
     * Add info to guest to indentify which host is it being run on
     * and what is the uuid of the guest
     */
    if (kvmppc_get_host_model(&buf)) {
        _FDT((fdt_property_string(fdt, "host-model", buf)));
        g_free(buf);
    }
    if (kvmppc_get_host_serial(&buf)) {
        _FDT((fdt_property_string(fdt, "host-serial", buf)));
        g_free(buf);
    }

    buf = g_strdup_printf(UUID_FMT, qemu_uuid[0], qemu_uuid[1],
                          qemu_uuid[2], qemu_uuid[3], qemu_uuid[4],
                          qemu_uuid[5], qemu_uuid[6], qemu_uuid[7],
                          qemu_uuid[8], qemu_uuid[9], qemu_uuid[10],
                          qemu_uuid[11], qemu_uuid[12], qemu_uuid[13],
                          qemu_uuid[14], qemu_uuid[15]);

    _FDT((fdt_property_string(fdt, "vm,uuid", buf)));
    g_free(buf);

    _FDT((fdt_begin_node(fdt, "chosen")));
    if (kernel_cmdline) {
        _FDT((fdt_property_string(fdt, "bootargs", kernel_cmdline)));
    }
    _FDT((fdt_property(fdt, "linux,initrd-start",
                       &start_prop, sizeof(start_prop))));
    _FDT((fdt_property(fdt, "linux,initrd-end",
                       &end_prop, sizeof(end_prop))));
    _FDT((fdt_end_node(fdt)));

    _FDT((fdt_property_cell(fdt, "#address-cells", 0x2)));
    _FDT((fdt_property_cell(fdt, "#size-cells", 0x2)));

    /* cpus */
    _FDT((fdt_begin_node(fdt, "cpus")));
    _FDT((fdt_property_cell(fdt, "#address-cells", 0x1)));
    _FDT((fdt_property_cell(fdt, "#size-cells", 0x0)));

    CPU_FOREACH(cs) {
        powernv_create_cpu_node(fdt, cs, smt);
    }

    _FDT((fdt_end_node(fdt)));

    /* ICPs */
    CPU_FOREACH(cs) {
        PowerPCCPU *cpu = POWERPC_CPU(cs);
        uint32_t base_server = ppc_get_vcpu_dt_id(cpu);
        xics_create_native_icp_node(s->xics, fdt, base_server, smt);
    }

    /* Memory */
    _FDT((powernv_populate_memory(fdt)));

    /* For each chip */
    for (i = 0; i < s->num_chips; i++) {
        /* Populate XSCOM */
        _FDT((xscom_populate_fdt(s->chips[i].xscom, fdt)));
    }

    /* /hypervisor node */
    if (kvm_enabled()) {
        uint8_t hypercall[16];

        /* indicate KVM hypercall interface */
        _FDT((fdt_begin_node(fdt, "hypervisor")));
        _FDT((fdt_property_string(fdt, "compatible", "linux,kvm")));
        if (kvmppc_has_cap_fixup_hcalls()) {
            /*
             * Older KVM versions with older guest kernels were broken with the
             * magic page, don't allow the guest to map it.
             */
            kvmppc_get_hypercall(first_cpu->env_ptr, hypercall,
                                 sizeof(hypercall));
            _FDT((fdt_property(fdt, "hcall-instructions", hypercall,
                              sizeof(hypercall))));
        }
        _FDT((fdt_end_node(fdt)));
    }

    _FDT((fdt_end_node(fdt))); /* close root node */
    _FDT((fdt_finish(fdt)));

    return fdt;
}

static void powernv_cpu_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;

    cpu_reset(cs);

    env->spr[SPR_PIR] = ppc_get_vcpu_dt_id(cpu);
    env->spr[SPR_HIOR] = 0;
    env->gpr[3] = FDT_ADDR;
    env->nip = 0x10;
    env->msr |= MSR_HVB;
}

/* Returns whether we want to use VGA or not */
static int pnv_vga_init(PCIBus *pci_bus)
{
    switch (vga_interface_type) {
    case VGA_NONE:
        return false;
    case VGA_DEVICE:
        return true;
    case VGA_STD:
    case VGA_VIRTIO:
        return pci_vga_init(pci_bus) != NULL;
    default:
        fprintf(stderr, "This vga model is not supported,"
                "currently it only supports -vga std\n");
        exit(0);
    }
}

static void pnv_nic_init(PCIBus *pci_bus)
{
    int i;

    for (i = 0; i < nb_nics; i++) {
        NICInfo *nd = &nd_table[i];
        DeviceState *dev;
        PCIDevice *pdev;
        Error *err = NULL;

        pdev = pci_create(pci_bus, -1, "e1000");
        dev = &pdev->qdev;
        qdev_set_nic_properties(dev, nd);
        object_property_set_bool(OBJECT(dev), true, "realized", &err);
        if (err) {
            error_report_err(err);
            object_unparent(OBJECT(dev));
            exit(1);
        }
    }
}

#define MAX_SATA_PORTS     6

static void pnv_storage_init(PCIBus *pci_bus)
{
    DriveInfo *hd[MAX_SATA_PORTS];
    PCIDevice *ahci;

    /* Add an AHCI device. We use an ICH9 since that's all we have
     * at hand for PCI AHCI but it shouldn't really matter
     */
    ahci = pci_create_simple(pci_bus, -1, "ich9-ahci");
    g_assert(MAX_SATA_PORTS == ICH_AHCI(ahci)->ahci.ports);
    ide_drive_get(hd, ICH_AHCI(ahci)->ahci.ports);
    ahci_ide_create_devs(ahci, hd);
}

static PCIBus *pnv_create_pci_legacy_bridge(PCIBus *parent, uint8_t chassis_nr)
{
    PCIDevice *dev;

    dev = pci_create_multifunction(parent, 0, false, "pci-bridge");
    qdev_prop_set_uint8(&dev->qdev, "chassis_nr", chassis_nr);
    dev->qdev.id = "pci";
    qdev_init_nofail(&dev->qdev);
    return pci_bridge_get_sec_bus(PCI_BRIDGE(dev));
}

static void pnv_lpc_irq_handler_cpld(void *opaque, int n, int level)
{
#define MAX_ISA_IRQ 16
    static uint32_t irqstate;
    uint32_t old_state = irqstate;
    PnvPsiController *psi = opaque;

    if (n >= MAX_ISA_IRQ) {
        return;
    }
    if (level) {
        irqstate |= 1u << n;
    } else {
        irqstate &= ~(1u << n);
    }
    if (irqstate != old_state) {
        pnv_psi_irq_set(psi, PSIHB_IRQ_EXTERNAL, irqstate != 0);
    }
}

static void pnv_create_chip(sPowerNVMachineState *s, unsigned int chip_no,
                            bool has_lpc, bool has_lpc_irq, unsigned int num_phbs)
{
    PnvChip *chip = &s->chips[chip_no];
    unsigned int i;

    if (chip_no >= PNV_MAX_CHIPS) {
            return;
    }

    /* XXX Improve chip numbering to better match HW */
    chip->chip_id = chip_no;

    /* Set up XSCOM bus */
    xscom_create(chip, s->chip_type);

    /* Create PSI */
    pnv_psi_create(chip, s->xics);

    /* Create LPC controller */
    if (has_lpc) {
        pnv_lpc_create(chip, has_lpc_irq);

        /* If we don't use the built-in LPC interrupt deserializer, we need
         * to provide a set of qirqs for the ISA bus or things will go bad.
         *
         * Most machines using pre-Naples chips (without said deserializer)
         * have a CPLD that will collect the SerIRQ and shoot them as a
         * single level interrupt to the P8 chip. So let's setup a hook
         * for doing just that.
         */
        if (!has_lpc_irq) {
            isa_bus_irqs(chip->lpc_bus,
                         qemu_allocate_irqs(pnv_lpc_irq_handler_cpld,
                                            chip->psi, 16));
        }
    }

    /* Create the simplified OCC model */
    pnv_occ_create(chip);

    /* Create a PCI, for now do one chip with 2 PHBs */
    for (i = 0; i < num_phbs; i++) {
        pnv_phb3_create(chip, s->xics, i);
    }
}

static int powernv_populate_rtc(ISADevice *d, void *fdt, int lpc_off)
{
    uint32_t io_base = d->ioport_id;
    uint32_t io_regs[] = {
        cpu_to_be32(1),
        cpu_to_be32(io_base),
        cpu_to_be32(2)
    };
    char *name;
    int node;
    int ret;

    name = g_strdup_printf("%s@i%x", qdev_fw_name(DEVICE(d)), io_base);
    node = fdt_add_subnode(fdt, lpc_off, name);
    g_free(name);
    if (node <= 0) {
        return node;
    }
    ret = fdt_setprop(fdt, node, "reg", io_regs, sizeof(io_regs));
    ret |= fdt_setprop_string(fdt, node, "compatible", "pnpPNP,b00");
    return ret;
}

static int powernv_populate_ipmi_sensor(Object *objbmc, void *fdt)
{
    int node;
    int ret;
    int i;
    const struct ipmi_sdr_compact *sdr;

    node = qemu_fdt_add_subnode(fdt, "/bmc");
    if (node <= 0) {
        return -1;
    }

    ret = fdt_setprop_string(fdt, node, "name", "bmc");
    ret |= fdt_setprop_cell(fdt, node, "#address-cells", 0x1);
    ret |= fdt_setprop_cell(fdt, node, "#size-cells", 0x0);

    node = fdt_add_subnode(fdt, node, "sensors");
    if (node <= 0) {
        return -1;
    }
    ret |= fdt_setprop_cell(fdt, node, "#address-cells", 0x1);
    ret |= fdt_setprop_cell(fdt, node, "#size-cells", 0x0);

    for (i = 0; !ipmi_bmc_sdr_find(IPMI_BMC(objbmc), i, &sdr, NULL); i++) {
        int snode;
        char sensor_name[32];

        sprintf(sensor_name, "sensor@%x", sdr->sensor_owner_number);
        snode = fdt_add_subnode(fdt, node, sensor_name);
        if (snode <= 0) {
            return -1;
        }

        ret |= fdt_setprop_cell(fdt, snode, "reg", sdr->sensor_owner_number);
        ret |= fdt_setprop_string(fdt, snode, "name", "sensor");
        ret |= fdt_setprop_string(fdt, snode, "compatible", "ibm,ipmi-sensor");
        ret |= fdt_setprop_cell(fdt, snode, "ipmi-sensor-reading-type",
                                sdr->reading_type);
        ret |= fdt_setprop_cell(fdt, snode, "ipmi-entity-id",
                                sdr->entity_id);
        ret |= fdt_setprop_cell(fdt, snode, "ipmi-entity-instance",
                                sdr->entity_instance);
        ret |= fdt_setprop_cell(fdt, snode, "ipmi-sensor-type",
                                sdr->sensor_type);
    }

    return ret;
}

static int powernv_populate_ipmi_bt(ISADevice *d, void *fdt, int lpc_off)
{
    const char compatible[] = "bt\0ipmi-bt";
    uint32_t io_base = 0x0;
    uint32_t io_regs[] = {
        cpu_to_be32(1),
        cpu_to_be32(io_base),
        cpu_to_be32(3)
    };
    uint32_t irq;
    char *name;
    int node;
    int ret;
    Error *err = NULL;
    Object *obj;

    io_base = object_property_get_int(OBJECT(d), "ioport", &err);
    if (err) {
        return -1;
    }
    io_regs[1] = cpu_to_be32(io_base);

    irq = object_property_get_int(OBJECT(d), "irq", &err);
    if (err) {
        return -1;
    }

    name = g_strdup_printf("%s@i%x", qdev_fw_name(DEVICE(d)), io_base);
    node = fdt_add_subnode(fdt, lpc_off, name);
    g_free(name);
    if (node <= 0) {
        return node;
    }
    ret = fdt_setprop(fdt, node, "reg", io_regs, sizeof(io_regs));
    ret |= fdt_setprop(fdt, node, "compatible", compatible, sizeof(compatible));

    /* Mark it as reserved to avoid Linux trying to claim it */
    ret |= fdt_setprop_string(fdt, node, "status", "reserved");
    ret |= fdt_setprop_cell(fdt, node, "interrupts", irq);
    ret |= fdt_setprop_cell(fdt, node, "interrupt-parent",
                            fdt_get_phandle(fdt, lpc_off));

    /* a ipmi bt device necessarily comes with a bmc :
     *   -device ipmi-bmc-sim,id=bmc0
     */
    obj = object_resolve_path_type("", "ipmi-bmc-sim", NULL);
    if (obj) {
        ret = powernv_populate_ipmi_sensor(obj, fdt);
    } else {
        fprintf(stderr, "bmc simulator is not running !?");
    }

    return ret;
}
#if 0
static DeviceState *ipmi_bt_create(BusState *bus, const char *bmcname,
                                   Error **errp)
{
    Error *err = NULL;
    DeviceState *dev;

    dev = qdev_create(bus, "isa-ipmi-bt");
    qdev_prop_set_int32(dev, "irq", 10);
    qdev_prop_set_string(dev, "bmc", bmcname);
    object_property_set_bool(OBJECT(dev), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        object_unparent(OBJECT(dev));
        return NULL;
    }

    return dev;
}
#endif
static int walk_isa_device(DeviceState *dev, void *fdt)
{
    ISADevice *d = ISA_DEVICE(dev);
    Object *obj = OBJECT(dev);
    int lpc_off;

    lpc_off = fdt_node_offset_by_compatible(fdt, -1, "ibm,power8-lpc");
    if (lpc_off < 0) {
        return lpc_off;
    }

    if (object_dynamic_cast(obj, TYPE_MC146818_RTC)) {
        powernv_populate_rtc(d, fdt, lpc_off);
    } else if (object_dynamic_cast(obj, "isa-ipmi-bt")) {
        powernv_populate_ipmi_bt(d, fdt, lpc_off);
    } else {
        fprintf(stderr, "unknown isa device %s@i%x\n", qdev_fw_name(dev),
                d->ioport_id);
    }

    return 0;
}

/*
 * OEM SEL Event data packet sent by BMC in response of a Read Event
 * Message Buffer command
 */
struct oem_sel {
    /* SEL header */
    uint8_t id[2];
    uint8_t type;
    uint8_t timestamp[4];
    uint8_t manuf_id[3];
    /* OEM SEL data (6 bytes) follows */
    uint8_t netfun;
    uint8_t cmd;
    uint8_t data[4];
};

#define SOFT_OFF               0x00
#define SOFT_REBOOT            0x01

static void pnv_gen_oem_sel(uint8_t reboot)
{
    Object *obj;
    uint8_t evt[16];
    struct oem_sel sel = {
        .id     = { 0x55 , 0x55 },
        .type           = 0xC0, /* OEM */
        .manuf_id       = { 0x0, 0x0, 0x0 },
        .timestamp      = { 0x0, 0x0, 0x0, 0x0 },
        .netfun         = 0x3a, /* IBM */
        .cmd            = 0x04, /* AMI OEM SEL Power Notification */
        .data           = { reboot, 0xFF, 0xFF, 0xFF },
    };

    obj = object_resolve_path_type("", "ipmi-bmc-sim", NULL);
    if (!obj) {
        fprintf(stderr, "bmc simulator is not running\n");
        return;
    }

    memcpy(evt, &sel, 16);
    ipmi_bmc_gen_event(IPMI_BMC(obj), evt, 0 /* do not log the event */);
}

static void pnv_powerdown_notify(Notifier *n, void *opaque)
{
    pnv_gen_oem_sel(SOFT_OFF);
}


static char *pnv_get_chip_type(Object *obj, Error **errp)
{
    sPowerNVMachineState *pnv = POWERNV_MACHINE(obj);

    switch(pnv->chip_type) {
    case PNV_CHIP_P8:
        return g_strdup("P8");
    case PNV_CHIP_P8NVL:
        return g_strdup("P8NVL");
    case PNV_CHIP_P8E:
    default:
        return g_strdup("P8E");
    }
}

static void pnv_set_chip_type(Object *obj, const char *value, Error **errp)
{
    sPowerNVMachineState *pnv = POWERNV_MACHINE(obj);

    if (!strcasecmp(value, "P8E")) {
        pnv->chip_type = PNV_CHIP_P8E;
    } else if (!strcasecmp(value, "P8")) {
        pnv->chip_type = PNV_CHIP_P8;
    } else if (!strcasecmp(value, "P8NVL")) {
        pnv->chip_type = PNV_CHIP_P8NVL;
    } else {
        error_setg(errp, "Unknown chip type");
    }
}

static void powernv_machine_initfn(Object *obj)
{
    sPowerNVMachineState *pnv = POWERNV_MACHINE(obj);

    pnv->chip_type = PNV_CHIP_P8E;
    object_property_add_str(obj, "chip-type",
                            pnv_get_chip_type, pnv_set_chip_type, NULL);
    object_property_set_description(obj, "chip-type",
                                    "Specifies processor chip type (P8, P8NVL)",
                                    NULL);
}

static void ppc_powernv_reset(void)
{
    sPowerNVMachineState *pnv = POWERNV_MACHINE(qdev_get_machine());
    void *fdt;
    Object *obj;

    qemu_devices_reset();

    fdt = g_malloc(FDT_MAX_SIZE);

    _FDT((fdt_open_into(pnv->fdt_skel, fdt, FDT_MAX_SIZE)));

    obj = object_resolve_path_type("", TYPE_ISA_BUS, NULL);
    if (!obj) {
        fprintf(stderr, "no isa bus ?!\n");
        return;
    }

    qbus_walk_children(BUS(obj), walk_isa_device, NULL, NULL, NULL, fdt);

    cpu_physical_memory_write(pnv->fdt_addr, fdt, fdt_totalsize(fdt));

    g_free(fdt);
}

static void ppc_powernv_init(MachineState *machine)
{
    ram_addr_t ram_size = machine->ram_size;
    const char *cpu_model = machine->cpu_model;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    uint32_t initrd_base = 0;
    long initrd_size = 0;
    PowerPCCPU *cpu;
    CPUPPCState *env;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    sPowerNVMachineState *pnv_machine = POWERNV_MACHINE(machine);
    XICSState *xics;
    PCIBus *pbus;
    ISABus *isa_bus;
    bool has_gfx = false;
    long fw_size;
    char *filename;
    void *fdt;
    int i;

    /* MSIs are supported on this platform */
    msi_nonbroken = true;

    /* Set up Interrupt Controller before we create the VCPUs */
    xics = xics_system_init(smp_cpus * kvmppc_smt_threads() / smp_threads,
                            XICS_IRQS_POWERNV);
    pnv_machine->xics = xics;

    /* init CPUs */
    if (cpu_model == NULL) {
        cpu_model = kvm_enabled() ? "host" : "POWER8";
    }

    for (i = 0; i < smp_cpus; i++) {
        cpu = cpu_ppc_init(cpu_model);
        if (cpu == NULL) {
            error_report("Unable to find PowerPC CPU definition");
            exit(1);
        }
        env = &cpu->env;

        /* Set time-base frequency to 512 MHz */
        cpu_ppc_tb_init(env, TIMEBASE_FREQ);

        /* MSR[IP] doesn't exist nowadays */
        env->msr_mask &= ~(1 << 6);

        xics_cpu_setup(xics, cpu);

        qemu_register_reset(powernv_cpu_reset, cpu);
    }

    if (ram_size < (1 * G_BYTE))
        error_report("Warning: skiboot may not work with < 1GB of RAM");

    /* allocate RAM */
    memory_region_allocate_system_memory(ram, NULL, "ppc_powernv.ram",
                                         ram_size);
    memory_region_add_subregion(sysmem, 0, ram);

    /* XXX We should decide how many chips to create based on #cores and
     * Venice vs. Murano vs. Naples chip type etc..., for now, just create
     * one chip. Also creation of the CPUs should be done per-chip
     */
    pnv_machine->num_chips = 1;

    /* Create only one chip for now with an LPC bus and one PHB
      */
    pnv_create_chip(pnv_machine, 0, true, false, 1);

    /* Grab chip 0's ISA bus */
    isa_bus = pnv_machine->chips[0].lpc_bus;

     /* Create serial port */
    serial_hds_isa_init(isa_bus, MAX_SERIAL_PORTS);

    /* Create an RTC ISA device too */
    rtc_init(isa_bus, 2000, NULL);

    /* Add a PCI switch */
    pbus = pnv_create_pci_legacy_bridge(pnv_machine->chips[0].phb[0], 128);

    /* Graphics */
    if (pnv_vga_init(pbus)) {
        has_gfx = true;
        machine->usb |= defaults_enabled() && !machine->usb_disabled;
    }
    if (machine->usb) {
        pci_create_simple(pbus, -1, "nec-usb-xhci");
        if (has_gfx) {
            USBBus *usb_bus = usb_bus_find(-1);

            usb_create_simple(usb_bus, "usb-kbd");
            usb_create_simple(usb_bus, "usb-mouse");
        }
    }

    /* Add NIC */
    pnv_nic_init(pbus);

    /* Add storage */
    pnv_storage_init(pbus);

    if (bios_name == NULL) {
        bios_name = FW_FILE_NAME;
    }
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    fw_size = load_image_targphys(filename, 0, FW_MAX_SIZE);
    if (fw_size < 0) {
        hw_error("qemu: could not load OPAL '%s'\n", filename);
        exit(1);
    }
    g_free(filename);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, kernel_filename);
    if (!filename) {
        hw_error("qemu: could find kernel '%s'\n", kernel_filename);
        exit(1);
    }

    fw_size = load_image_targphys(filename, 0x20000000, 0x2000000);
    if (fw_size < 0) {
        hw_error("qemu: could not load kernel'%s'\n", filename);
        exit(1);
    }
    g_free(filename);

    /* load initrd */
    if (initrd_filename) {
            /* Try to locate the initrd in the gap between the kernel
             * and the firmware. Add a bit of space just in case
             */
            initrd_base = 0x40000000;
            initrd_size = load_image_targphys(initrd_filename, initrd_base,
                                              0x10000000); /* 128MB max */
            if (initrd_size < 0) {
                    error_report("qemu: could not load initial ram disk '%s'",
                            initrd_filename);
                    exit(1);
            }
    } else {
            initrd_base = 0;
            initrd_size = 0;
    }
    fdt = powernv_create_fdt(pnv_machine, machine->kernel_cmdline,
                             initrd_base, initrd_size);
    pnv_machine->fdt_skel = fdt;
    pnv_machine->fdt_addr = FDT_ADDR;

    pnv_machine->powerdown_notifier.notify = pnv_powerdown_notify;
    qemu_register_powerdown_notifier(&pnv_machine->powerdown_notifier);
}

static int powernv_kvm_type(const char *vm_type)
{
    /* Always force PR KVM */
    return 2;
}

static void powernv_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = ppc_powernv_init;
    mc->reset = ppc_powernv_reset;
    mc->block_default_type = IF_IDE;
    mc->max_cpus = MAX_CPUS;
    mc->no_parallel = 1;
    mc->default_boot_order = NULL;
    mc->kvm_type = powernv_kvm_type;
    mc->default_ram_size = 1 * G_BYTE;
}

static const TypeInfo powernv_machine_info = {
    .name          = TYPE_POWERNV_MACHINE,
    .parent        = TYPE_MACHINE,
    .abstract      = true,
    .instance_size = sizeof(sPowerNVMachineState),
    .instance_init = powernv_machine_initfn,
    .class_init    = powernv_machine_class_init,
};

static void powernv_machine_3_0_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->name = "powernv-3.0";
    mc->desc = "PowerNV v3.0";
    mc->alias = "powernv";
}

static const TypeInfo powernv_machine_3_0_info = {
    .name          = MACHINE_TYPE_NAME("powernv-3.0"),
    .parent        = TYPE_POWERNV_MACHINE,
    .class_init    = powernv_machine_3_0_class_init,
};

static void powernv_machine_register_types(void)
{
    type_register_static(&powernv_machine_info);
    type_register_static(&powernv_machine_3_0_info);
}

type_init(powernv_machine_register_types)
