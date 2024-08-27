// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#ifndef ACPI_EGM_MEMORY_H
#define ACPI_EGM_MEMORY_H

#include "qom/object_interfaces.h"

#define TYPE_ACPI_EGM_MEMORY "acpi-egm-memory"

typedef struct AcpiEgmMemory {
    /* private */
    Object parent;

    /* public */
    char *pci_dev;
    uint16_t node;
} AcpiEgmMemory;

#endif
