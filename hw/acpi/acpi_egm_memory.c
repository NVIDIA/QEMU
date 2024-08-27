// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include "qemu/osdep.h"
#include "hw/acpi/acpi_egm_memory.h"
#include "hw/acpi/aml-build.h"
#include "hw/boards.h"
#include "hw/pci/pci_device.h"
#include "qemu/error-report.h"
#include "hw/arm/virt.h"
#include "include/hw/boards.h"

typedef struct AcpiEgmMemoryClass {
    ObjectClass parent_class;
} AcpiEgmMemoryClass;

static int gpu_id;

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

static void acpi_dsdt_add_gpu(Aml *dev, int32_t devfn, uint64_t egm_mem_base,
                              uint64_t egm_mem_size, int egm_mem_pxm)
{
    Aml *dev_gpu = aml_device("GPU%d", gpu_id++);
    Aml *pkg = aml_package(3);
    Aml *pkg1 = aml_package(2);
    Aml *pkg2 = aml_package(2);
    Aml *pkg3 = aml_package(2);
    Aml *dev_pkg = aml_package(2);
    Aml *UUID;

    aml_append(dev_gpu, aml_name_decl("_ADR", aml_int(PCI_SLOT(devfn) << 16)));

    aml_append(pkg1, aml_string("nvidia,egm-base-pa"));
    aml_append(pkg1, aml_int(egm_mem_base));

    aml_append(pkg2, aml_string("nvidia,egm-size"));
    aml_append(pkg2, aml_int(egm_mem_size));

    aml_append(pkg3, aml_string("nvidia,egm-pxm"));
    aml_append(pkg3, aml_int(egm_mem_pxm));

    aml_append(pkg, pkg1);
    aml_append(pkg, pkg2);
    aml_append(pkg, pkg3);

    UUID = aml_touuid("DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301");
    aml_append(dev_pkg, UUID);
    aml_append(dev_pkg, pkg);

    aml_append(dev_gpu, aml_name_decl("_DSD", dev_pkg));
    aml_append(dev, dev_gpu);
}

typedef struct DsdtInfo {
    Aml *dev;
    int bus;
} DsdtInfo;

static int build_all_acpi_egm_memory_dsdt(Object *obj, void *opaque)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    AcpiEgmMemory *egm;
    DsdtInfo *info = opaque;
    PCIDevice *pci_dev;
    Object *o;
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());
    uint64_t mem_base;
    int i;

    if (!object_dynamic_cast(obj, TYPE_ACPI_EGM_MEMORY)) {
        return 0;
    }

    egm = ACPI_EGM_MEMORY(obj);
    if (egm->node >= ms->numa_state->num_nodes) {
        error_printf("%s: Specified node %d is invalid.\n",
                     TYPE_ACPI_EGM_MEMORY, egm->node);
        exit(1);
    }

    o = object_resolve_path_type(egm->pci_dev, TYPE_PCI_DEVICE, NULL);
    if (!o) {
        error_printf("%s: Specified device must be a PCI device.\n",
                     TYPE_ACPI_EGM_MEMORY);
        exit(1);
    }

    pci_dev = PCI_DEVICE(o);

    if (info->bus != pci_bus_num(pci_get_bus(pci_dev))) {
        return 0;
    }

    mem_base = vms->memmap[VIRT_MEM].base;
    for (i = 0; i < ms->numa_state->num_nodes; ++i) {
        if (i == egm->node) {
            acpi_dsdt_add_gpu(info->dev, pci_dev->devfn, mem_base,
                              ms->numa_state->nodes[i].node_mem, i);
            break;
        }

        if (ms->numa_state->nodes[i].node_mem > 0) {
            mem_base += ms->numa_state->nodes[i].node_mem;
        }
    }

    return 0;
}

void build_acpi_egm_memory_dsdt(Aml *dev, int bus)
{
    DsdtInfo info = {dev, bus};

    object_child_foreach_recursive(object_get_root(),
                                   build_all_acpi_egm_memory_dsdt,
                                   &info);
}
