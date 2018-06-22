/*
 * QEMU PowerPC sPAPR XIVE interrupt controller model
 *
 * Copyright (c) 2017-2018, IBM Corporation.
 *
 * This code is licensed under the GPL version 2 or later. See the
 * COPYING file in the top-level directory.
 */

#ifndef PPC_SPAPR_XIVE_H
#define PPC_SPAPR_XIVE_H

#include "hw/sysbus.h"
#include "hw/ppc/xive.h"

#define TYPE_SPAPR_XIVE "spapr-xive"
#define SPAPR_XIVE(obj) OBJECT_CHECK(sPAPRXive, (obj), TYPE_SPAPR_XIVE)

typedef struct sPAPRXive {
    XiveRouter   parent;

    /* Internal interrupt source for IPIs and virtual devices */
    XiveSource   source;
    hwaddr       vc_base;

    /* EQ ESB MMIOs */
    XiveEQSource eq_source;
    hwaddr       eq_base;

    /* Routing table */
    XiveIVE      *ivt;
    uint32_t     nr_irqs;
    XiveEQ       *eqdt;
    uint32_t     nr_eqs;

    /* TIMA mapping address */
    hwaddr       tm_base;
    MemoryRegion tm_mmio;

    /* KVM support */
    int          fd;
    void         *tm_mmap;
} sPAPRXive;

#define SPAPR_XIVE_CLASS(klass) \
     OBJECT_CLASS_CHECK(sPAPRXiveClass, (klass), TYPE_SPAPR_XIVE)
#define SPAPR_XIVE_GET_CLASS(obj) \
     OBJECT_GET_CLASS(sPAPRXiveClass, (obj), TYPE_SPAPR_XIVE)

typedef struct sPAPRXiveClass {
    XiveRouterClass parent_class;

    void (*synchronize_state)(sPAPRXive *xive);
    int  (*pre_save)(sPAPRXive *xsrc);
    int  (*post_load)(sPAPRXive *xsrc, int version_id);
} sPAPRXiveClass;

bool spapr_xive_irq_enable(sPAPRXive *xive, uint32_t lisn, bool lsi);
bool spapr_xive_irq_disable(sPAPRXive *xive, uint32_t lisn);
void spapr_xive_pic_print_info(sPAPRXive *xive, Monitor *mon);
qemu_irq spapr_xive_qirq(sPAPRXive *xive, uint32_t lisn);
void spapr_xive_common_realize(sPAPRXive *xive, Error **errp);

/*
 * sPAPR VP and EQ indexing helpers
 */
static inline uint32_t spapr_xive_vp_to_target(sPAPRXive *xive, uint8_t vp_blk,
                                               uint32_t vp_idx)
{
    return vp_idx;
}
int spapr_xive_target_to_vp(XiveRouter *xrtr, uint32_t target,
                            uint8_t *out_vp_blk, uint32_t *out_vp_idx);
int spapr_xive_target_to_eq(XiveRouter *xrtr, uint32_t target, uint8_t prio,
                            uint8_t *out_eq_blk, uint32_t *out_eq_idx);
int spapr_xive_cpu_to_eq(XiveRouter *xrtr, PowerPCCPU *cpu, uint8_t prio,
                         uint8_t *out_eq_blk, uint32_t *out_eq_idx);
bool spapr_xive_eq_is_valid(uint8_t priority);

typedef struct sPAPRMachineState sPAPRMachineState;

void spapr_xive_hcall_init(sPAPRMachineState *spapr);
void spapr_dt_xive(sPAPRXive *xive, int nr_servers, void *fdt,
                   uint32_t phandle);

/*
 * XIVE KVM models
 */

#define TYPE_SPAPR_XIVE_KVM "spapr-xive-kvm"
#define SPAPR_XIVE_KVM(obj) \
    OBJECT_CHECK(sPAPRXive, (obj), TYPE_SPAPR_XIVE_KVM)

#define TYPE_XIVE_SOURCE_KVM "xive-source-kvm"
#define XIVE_SOURCE_KVM(obj) \
    OBJECT_CHECK(XiveSource, (obj), TYPE_XIVE_SOURCE_KVM)

#define TYPE_XIVE_TCTX_KVM "xive-tctx-kvm"
#define XIVE_TCTX_KVM(obj) \
    OBJECT_CHECK(XiveTCTX, (obj), TYPE_XIVE_TCTX_KVM)

#endif /* PPC_SPAPR_XIVE_H */
