/*
 * QEMU PowerPC sPAPR XIVE interrupt controller model
 *
 * Copyright (c) 2017-2018, IBM Corporation.
 *
 * This code is licensed under the GPL version 2 or later. See the
 * COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "target/ppc/cpu.h"
#include "sysemu/cpus.h"
#include "monitor/monitor.h"
#include "hw/ppc/spapr.h"
#include "hw/ppc/spapr_xive.h"
#include "hw/ppc/xive.h"
#include "hw/ppc/xive_regs.h"

/*
 * XIVE Virtualization Controller BAR and Thread Managment BAR that we
 * use for the ESB pages and the TIMA pages
 */
#define SPAPR_XIVE_VC_BASE   0x0006010000000000ull
#define SPAPR_XIVE_TM_BASE   0x0006030203180000ull

void spapr_xive_pic_print_info(sPAPRXive *xive, Monitor *mon)
{
    int i;

    xive_source_pic_print_info(&xive->source, 0, mon);

    monitor_printf(mon, "IVE Table\n");
    for (i = 0; i < xive->nr_irqs; i++) {
        xive_router_print_ive(XIVE_ROUTER(xive), i, &xive->ivt[i], mon);
    }
}

static void spapr_xive_reset(DeviceState *dev)
{
    sPAPRXive *xive = SPAPR_XIVE(dev);
    XiveSource *xsrc = &xive->source;
    int i;

    /* Xive Source reset is done through SysBus, it should put all
     * IRQs to OFF (!P|Q) */

    /* Mask all valid IVEs in the IRQ number space. */
    for (i = 0; i < xive->nr_irqs; i++) {
        XiveIVE *ive = &xive->ivt[i];
        if (ive->w & IVE_VALID) {
            ive->w |= IVE_MASKED;
        }
    }

    for (i = 0; i < xive->nr_eqs; i++) {
        xive_eq_reset(&xive->eqdt[i]);
    }

    /* Map the EQ ESB pages after the source ESBs */
    xive->eq_base = xive->vc_base + (1ull << xsrc->esb_shift) * xsrc->nr_irqs;

    /* Map ESB pages and TIMA pages */
    sysbus_mmio_map(SYS_BUS_DEVICE(&xive->source), 0, xive->vc_base);
    sysbus_mmio_map(SYS_BUS_DEVICE(&xive->eq_source), 0, xive->eq_base);
    sysbus_mmio_map(SYS_BUS_DEVICE(xive), 0, xive->tm_base);
}

static void spapr_xive_instance_init(Object *obj)
{
    sPAPRXive *xive = SPAPR_XIVE(obj);

    object_initialize(&xive->source, sizeof(xive->source), TYPE_XIVE_SOURCE);
    object_property_add_child(obj, "source", OBJECT(&xive->source), NULL);

    object_initialize(&xive->eq_source, sizeof(xive->eq_source),
                      TYPE_XIVE_EQ_SOURCE);
    object_property_add_child(obj, "eq_source", OBJECT(&xive->eq_source), NULL);
}

static void spapr_xive_realize(DeviceState *dev, Error **errp)
{
    sPAPRXive *xive = SPAPR_XIVE(dev);
    XiveSource *xsrc = &xive->source;
    XiveEQSource *eq_xsrc = &xive->eq_source;
    Error *local_err = NULL;

    if (!xive->nr_irqs) {
        error_setg(errp, "Number of interrupt needs to be greater 0");
        return;
    }

    if (!xive->nr_eqs) {
        error_setg(errp, "Number of interrupt needs to be greater 0");
        return;
    }

    /*
     * Initialize the internal sources, for IPIs and virtual devices.
     */
    object_property_set_int(OBJECT(xsrc), xive->nr_irqs, "nr-irqs",
                            &error_fatal);
    object_property_add_const_link(OBJECT(xsrc), "xive", OBJECT(xive),
                                   &error_fatal);
    object_property_set_bool(OBJECT(xsrc), true, "realized", &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    qdev_set_parent_bus(DEVICE(xsrc), sysbus_get_default());

    /*
     * Initialize the EQ ESB source
     */
    object_property_set_int(OBJECT(eq_xsrc), xive->nr_irqs, "nr-eqs",
                            &error_fatal);
    object_property_add_const_link(OBJECT(eq_xsrc), "xive", OBJECT(xive),
                                   &error_fatal);
    object_property_set_bool(OBJECT(eq_xsrc), true, "realized", &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    qdev_set_parent_bus(DEVICE(eq_xsrc), sysbus_get_default());

    /* Allocate the routing tables */
    xive->ivt = g_new0(XiveIVE, xive->nr_irqs);
    xive->eqdt = g_new0(XiveEQ, xive->nr_eqs);

    /* TIMA */
    memory_region_init_io(&xive->tm_mmio, OBJECT(xive), &xive_tm_ops, xive,
                          "xive.tima", 4ull << TM_SHIFT);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &xive->tm_mmio);
}

static int spapr_xive_get_ive(XiveRouter *xrtr, uint32_t lisn, XiveIVE *ive)
{
    sPAPRXive *xive = SPAPR_XIVE(xrtr);

    if (lisn >= xive->nr_irqs) {
        return -1;
    }

    *ive = xive->ivt[lisn];
    return 0;
}

static int spapr_xive_set_ive(XiveRouter *xrtr, uint32_t lisn, XiveIVE *ive)
{
    sPAPRXive *xive = SPAPR_XIVE(xrtr);

    if (lisn >= xive->nr_irqs) {
        return -1;
    }

    xive->ivt[lisn] = *ive;
    return 0;
}

static int spapr_xive_get_eq(XiveRouter *xrtr,
                             uint8_t eq_blk, uint32_t eq_idx, XiveEQ *eq)
{
    sPAPRXive *xive = SPAPR_XIVE(xrtr);

    if (eq_idx >= xive->nr_eqs) {
        return -1;
    }

    memcpy(eq, &xive->eqdt[eq_idx], sizeof(XiveEQ));
    return 0;
}

static int spapr_xive_set_eq(XiveRouter *xrtr,
                             uint8_t eq_blk, uint32_t eq_idx, XiveEQ *eq)
{
    sPAPRXive *xive = SPAPR_XIVE(xrtr);

    if (eq_idx >= xive->nr_eqs) {
        return -1;
    }

    memcpy(&xive->eqdt[eq_idx], eq, sizeof(XiveEQ));
    return 0;
}

static const VMStateDescription vmstate_spapr_xive_eq = {
    .name = TYPE_SPAPR_XIVE "/eq",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField []) {
        VMSTATE_UINT32(w0, XiveEQ),
        VMSTATE_UINT32(w1, XiveEQ),
        VMSTATE_UINT32(w2, XiveEQ),
        VMSTATE_UINT32(w3, XiveEQ),
        VMSTATE_UINT32(w4, XiveEQ),
        VMSTATE_UINT32(w5, XiveEQ),
        VMSTATE_UINT32(w6, XiveEQ),
        VMSTATE_UINT32(w7, XiveEQ),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_spapr_xive_ive = {
    .name = TYPE_SPAPR_XIVE "/ive",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField []) {
        VMSTATE_UINT64(w, XiveIVE),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_spapr_xive = {
    .name = TYPE_SPAPR_XIVE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_EQUAL(nr_irqs, sPAPRXive, NULL),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(ivt, sPAPRXive, nr_irqs,
                                     vmstate_spapr_xive_ive, XiveIVE),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(eqdt, sPAPRXive, nr_eqs,
                                             vmstate_spapr_xive_eq, XiveEQ),
        VMSTATE_END_OF_LIST()
    },
};

static Property spapr_xive_properties[] = {
    DEFINE_PROP_UINT32("nr-irqs", sPAPRXive, nr_irqs, 0),
    DEFINE_PROP_UINT32("nr-eqs",  sPAPRXive, nr_eqs, 0),
    DEFINE_PROP_UINT64("vc-base", sPAPRXive, vc_base, SPAPR_XIVE_VC_BASE),
    DEFINE_PROP_UINT64("tm-base", sPAPRXive, tm_base, SPAPR_XIVE_TM_BASE),
    DEFINE_PROP_END_OF_LIST(),
};

static void spapr_xive_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    XiveRouterClass *xrc = XIVE_ROUTER_CLASS(klass);

    dc->desc    = "sPAPR XIVE Interrupt Controller";
    dc->props   = spapr_xive_properties;
    dc->realize = spapr_xive_realize;
    dc->reset   = spapr_xive_reset;
    dc->vmsd    = &vmstate_spapr_xive;

    xrc->get_ive = spapr_xive_get_ive;
    xrc->set_ive = spapr_xive_set_ive;
    xrc->get_eq  = spapr_xive_get_eq;
    xrc->set_eq  = spapr_xive_set_eq;
}

static const TypeInfo spapr_xive_info = {
    .name = TYPE_SPAPR_XIVE,
    .parent = TYPE_XIVE_ROUTER,
    .instance_init = spapr_xive_instance_init,
    .instance_size = sizeof(sPAPRXive),
    .class_init = spapr_xive_class_init,
};

static void spapr_xive_register_types(void)
{
    type_register_static(&spapr_xive_info);
}

type_init(spapr_xive_register_types)

bool spapr_xive_irq_enable(sPAPRXive *xive, uint32_t lisn, bool lsi)
{
    XiveSource *xsrc = &xive->source;

    if (lisn >= xive->nr_irqs) {
        return false;
    }

    xive->ivt[lisn].w |= IVE_VALID;
    xive_source_irq_set(xsrc, lisn, lsi);
    return true;
}

bool spapr_xive_irq_disable(sPAPRXive *xive, uint32_t lisn)
{
    XiveSource *xsrc = &xive->source;

    if (lisn >= xive->nr_irqs) {
        return false;
    }

    xive->ivt[lisn].w &= ~IVE_VALID;
    xive_source_irq_set(xsrc, lisn, false);
    return true;
}

qemu_irq spapr_xive_qirq(sPAPRXive *xive, uint32_t lisn)
{
    XiveSource *xsrc = &xive->source;

    if (lisn >= xive->nr_irqs) {
        return NULL;
    }

    if (!(xive->ivt[lisn].w & IVE_VALID)) {
        qemu_log_mask(LOG_GUEST_ERROR, "XIVE: invalid LISN %x\n", lisn);
        return NULL;
    }

    return xive_source_qirq(xsrc, lisn);
}
