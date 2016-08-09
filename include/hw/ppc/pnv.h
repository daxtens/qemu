/*
 * QEMU PowerNV various definitions
 *
 * Copyright (c) 2014-2016 BenH, IBM Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _PPC_PNV_H
#define _PPC_PNV_H

#include "hw/boards.h"
typedef struct XScomBus XScomBus;
typedef struct ISABus ISABus;
typedef struct PnvLpcController PnvLpcController;
typedef struct PnvPsiController PnvPsiController;
typedef struct XICSState XICSState;
typedef struct PnvOCCState PnvOCCState;
typedef struct PCIBus PCIBus;

typedef enum PnvChipType {
    PNV_CHIP_P8E,   /* AKA Murano (default) */
    PNV_CHIP_P8,    /* AKA Venice */
    PNV_CHIP_P8NVL, /* AKA Naples */
} PnvChipType;

/* Should we turn that into a QOjb of some sort ? */
typedef struct PnvChip {
    uint32_t         chip_id;
    XScomBus         *xscom;
    PnvLpcController *lpc;
    ISABus           *lpc_bus;
    PnvPsiController *psi;
    PnvOCCState      *occ;
#define PNV_MAX_CHIP_PHB	4
    PCIBus           *phb[PNV_MAX_CHIP_PHB];
} PnvChip;

#define PNV_MAX_CHIPS   1

#define TYPE_POWERNV_MACHINE      "powernv-machine"
#define POWERNV_MACHINE(obj) \
    OBJECT_CHECK(sPowerNVMachineState, (obj), TYPE_POWERNV_MACHINE)


typedef struct sPowerNVMachineState {
    /*< private >*/
    MachineState parent_obj;

    XICSState *xics;
    uint32_t  num_chips;
    PnvChipType chip_type;
    PnvChip   chips[PNV_MAX_CHIPS];
    hwaddr fdt_addr;
    void *fdt_skel;
    Notifier powerdown_notifier;
} sPowerNVMachineState;

extern void pnv_lpc_create(PnvChip *chip, bool has_serirq);
extern void pnv_psi_create(PnvChip *chip, XICSState *xics);
extern void pnv_occ_create(PnvChip *chip);

typedef enum PnvPsiIrq {
    PSIHB_IRQ_PSI, /* internal use only */
    PSIHB_IRQ_FSP, /* internal use only */
    PSIHB_IRQ_OCC,
    PSIHB_IRQ_FSI,
    PSIHB_IRQ_LPC_I2C,
    PSIHB_IRQ_LOCAL_ERR,
    PSIHB_IRQ_EXTERNAL,
} PnvPsiIrq;

extern void pnv_psi_irq_set(PnvPsiController *psi, PnvPsiIrq irq, bool state);

#endif /* _PPC_PNV_H */
