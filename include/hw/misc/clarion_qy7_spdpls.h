/*
 * Clarion QY7 speed pulse counter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_CLARION_QY7_SPDPLS_H
#define HW_MISC_CLARION_QY7_SPDPLS_H

#include "system/memory.h"
#include "hw/core/irq.h"

typedef struct ClarionQy7Spdpls ClarionQy7Spdpls;

ClarionQy7Spdpls *clarion_qy7_spdpls_init(MemoryRegion *sysmem,
                                         hwaddr base, qemu_irq irq,
                                         uint32_t speed_kmh,
                                         uint32_t pulses_per_km);

#endif
