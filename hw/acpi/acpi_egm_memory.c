// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include "qemu/osdep.h"
#include "hw/acpi/acpi_egm_memory.h"
#include "hw/boards.h"
#include "qemu/error-report.h"

typedef struct AcpiEgmMemoryClass {
    ObjectClass parent_class;
} AcpiEgmMemoryClass;

OBJECT_DEFINE_TYPE_WITH_INTERFACES(AcpiEgmMemory, acpi_egm_memory,
                   ACPI_EGM_MEMORY, OBJECT,
                   { TYPE_USER_CREATABLE },
                   { NULL })

OBJECT_DECLARE_SIMPLE_TYPE(AcpiEgmMemory, ACPI_EGM_MEMORY)

static void acpi_egm_memory_init(Object *obj)
{
    AcpiEgmMemory *egm = ACPI_EGM_MEMORY(obj);

    egm->node = MAX_NODES;
    egm->pci_dev = NULL;
}

static void acpi_egm_memory_finalize(Object *obj)
{
    AcpiEgmMemory *egm = ACPI_EGM_MEMORY(obj);

    g_free(egm->pci_dev);
}

static void acpi_egm_memory_set_pci_device(Object *obj, const char *val,
                                                  Error **errp)
{
    AcpiEgmMemory *egm = ACPI_EGM_MEMORY(obj);

    egm->pci_dev = g_strdup(val);
}

static void acpi_egm_memory_set_node(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    AcpiEgmMemory *egm = ACPI_EGM_MEMORY(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    if (value >= MAX_NODES) {
        error_printf("%s: Invalid NUMA node specified\n",
                     TYPE_ACPI_EGM_MEMORY);
        exit(1);
    }

    egm->node = value;
}

static void acpi_egm_memory_class_init(ObjectClass *oc, void *data)
{
    object_class_property_add_str(oc, "pci-dev", NULL,
        acpi_egm_memory_set_pci_device);
    object_class_property_add(oc, "node", "int", NULL,
        acpi_egm_memory_set_node, NULL, NULL);
}
