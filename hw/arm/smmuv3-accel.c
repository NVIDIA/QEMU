/*
 * Copyright (c) 2025 Huawei Technologies R & D (UK) Ltd
 * Copyright (C) 2025 NVIDIA
 * Written by Nicolin Chen, Shameer Kolothum
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"

#include "hw/arm/smmuv3.h"
#include "hw/iommu.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-host/gpex.h"
#include "hw/vfio/pci.h"

#include "smmuv3-accel.h"

static SMMUv3AccelDevice *smmuv3_accel_get_dev(SMMUState *bs, SMMUPciBus *sbus,
                                               PCIBus *bus, int devfn)
{
    SMMUDevice *sdev = sbus->pbdev[devfn];
    SMMUv3AccelDevice *accel_dev;

    if (sdev) {
        return container_of(sdev, SMMUv3AccelDevice, sdev);
    }

    accel_dev = g_new0(SMMUv3AccelDevice, 1);
    sdev = &accel_dev->sdev;

    sbus->pbdev[devfn] = sdev;
    smmu_init_sdev(bs, sdev, bus, devfn);
    return accel_dev;
}

static bool smmuv3_accel_pdev_allowed(PCIDevice *pdev, bool *vfio_pci)
{

    if (object_dynamic_cast(OBJECT(pdev), TYPE_PCI_BRIDGE) ||
        object_dynamic_cast(OBJECT(pdev), TYPE_PXB_PCIE_DEV) ||
        object_dynamic_cast(OBJECT(pdev), TYPE_GPEX_ROOT_DEVICE)) {
        return true;
    } else if ((object_dynamic_cast(OBJECT(pdev), TYPE_VFIO_PCI))) {
        *vfio_pci = true;
        if (object_property_get_link(OBJECT(pdev), "iommufd", NULL)) {
            return true;
        }
    }
    return false;
}

static AddressSpace *smmuv3_accel_find_add_as(PCIBus *bus, void *opaque,
                                              int devfn)
{
    PCIDevice *pdev = pci_find_device(bus, pci_bus_num(bus), devfn);
    SMMUState *bs = opaque;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3AccelDevice *accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    SMMUDevice *sdev = &accel_dev->sdev;
    bool vfio_pci = false;

    if (pdev && !smmuv3_accel_pdev_allowed(pdev, &vfio_pci)) {
        if (DEVICE(pdev)->hotplugged) {
            if (vfio_pci) {
                warn_report("Hot plugging a vfio-pci device (%s) without "
                            "iommufd as backend is not supported", pdev->name);
            } else {
                warn_report("Hot plugging an emulated device %s with "
                            "accelerated SMMUv3. This will bring down "
                            "performace", pdev->name);
            }
            /*
             * Both cases, we will return IOMMU address space. For hotplugged
             * vfio-pci dev without iommufd as backend, it will fail later in
             * smmuv3_notify_flag_changed() with "requires iommu MAP notifier"
             * error message.
             */
             return &sdev->as;
        } else {
            error_report("Device(%s) not allowed. Only PCIe root complex "
                         "devices or PCI bridge devices or vfio-pci endpoint "
                         "devices with iommufd as backend is allowed with "
                         "arm-smmuv3,accel=on", pdev->name);
            exit(1);
        }
    }

    /*
     * We return the system address for vfio-pci devices(with iommufd as
     * backend) so that the VFIO core can set up Stage-2 (S2) mappings for
     * guest RAM. This is needed because, in the accelerated SMMUv3 case,
     * the host SMMUv3 runs in nested (S1 + S2)  mode where the guest
     * manages its own S1 page tables while the host manages S2.
     *
     * We are using the global &address_space_memory here, as this will ensure
     * same system address space pointer for all devices behind the accelerated
     * SMMUv3s in a VM. That way VFIO/iommufd can reuse a single IOAS ID in
     * iommufd_cdev_attach(), allowing the Stage-2 page tables to be shared
     * within the VM instead of duplicating them for every SMMUv3 instance.
     */
    if (vfio_pci) {
        return &address_space_memory;
    } else {
        return &sdev->as;
    }
}

static uint64_t smmuv3_accel_get_viommu_flags(void *opaque)
{
    /*
     * We return VIOMMU_FLAG_WANT_NESTING_PARENT to inform VFIO core to create a
     * nesting parent which is required for accelerated SMMUv3 support.
     * The real HW nested support should be reported from host SMMUv3 and if
     * it doesn't, the nesting parent allocation will fail anyway in VFIO core.
     */
    return VIOMMU_FLAG_WANT_NESTING_PARENT;
}

static const PCIIOMMUOps smmuv3_accel_ops = {
    .get_address_space = smmuv3_accel_find_add_as,
    .get_viommu_flags = smmuv3_accel_get_viommu_flags,
};

void smmuv3_accel_init(SMMUv3State *s)
{
    SMMUState *bs = ARM_SMMU(s);

    bs->iommu_ops = &smmuv3_accel_ops;
}
