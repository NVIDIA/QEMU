/*
 * Copyright (C) 2025-2026 NVIDIA
 * Written by Nicolin Chen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "target/arm/kvm_arm.h"

#include "hw/pci/pci_bridge.h"
#include "hw/pci-host/gpex.h"
#include "hw/core/iommu.h"
#include "hw/vfio/pci.h"

#include "smmuv3-realm.h"

#define SMMU_STE_VALID      (1ULL << 0)
#define SMMU_STE_CFG_BYPASS (1ULL << 3)
#define SMMU_STE_EATS_TRANS (1ULL << 28)

void smmuv3_realm_idr_override(SMMUv3State *s)
{
    if (!s->realm) {
        return;
    }

    s->idr[0] = VSMMU_IDR0_VALUE;
    s->idr[1] = VSMMU_IDR1_VALUE;
    s->idr[3] = VSMMU_IDR3_VALUE;
    s->idr[5] = VSMMU_IDR5_VALUE;
    s->aidr = VSMMU_AIDR_VALUE;
}

/*
 * Allocate a Realm vIOMMU on the nesting parent HWPT of @hiodi. @data describes
 * a vSMMU visible to the guest. Without @data, the kernel activates the pSMMU
 * alone for the Realm.
 */
static SMMURealmViommu *
smmuv3_realm_viommu_alloc(HostIOMMUDeviceIOMMUFD *hiodi, void *data,
                          uint32_t data_len, Error **errp)
{
    SMMURealmViommu *viommu;
    uint32_t viommu_id;

    if (!iommufd_backend_alloc_viommu(hiodi->iommufd, hiodi->devid,
                                      IOMMU_VIOMMU_TYPE_ARM_REALM_SMMUV3,
                                      hiodi->hwpt_id, data, data_len,
                                      &viommu_id, errp)) {
        return NULL;
    }

    viommu = g_new0(SMMURealmViommu, 1);
    viommu->core.iommufd = hiodi->iommufd;
    viommu->core.viommu_id = viommu_id;
    viommu->core.s2_hwpt_id = hiodi->hwpt_id;
    return viommu;
}

static void smmuv3_realm_viommu_free(SMMURealmViommu *viommu)
{
    if (viommu->bypass_ats_hwpt_id) {
        iommufd_backend_free_id(viommu->core.iommufd,
                                viommu->bypass_ats_hwpt_id);
    }
    if (viommu->bypass_hwpt_id) {
        iommufd_backend_free_id(viommu->core.iommufd, viommu->bypass_hwpt_id);
    }
    iommufd_backend_free_id(viommu->core.iommufd, viommu->core.viommu_id);
    g_free(viommu);
}

/* PSMMU-only vIOMMUs of a Realm guest without a vSMMU, one per pSMMU */
static GList *smmuv3_realm_psmmu_viommus;
/* The PSMMU-only vIOMMU of each device, keyed by its HostIOMMUDevice */
static GHashTable *smmuv3_realm_psmmu_devices;

/*
 * Test if the device of @hiodi can share @viommu. The kernel allows a HWPT to
 * be nested on a vIOMMU only for a device behind the same pSMMU, so probe with
 * a bypass one. Also require the same nesting parent HWPT, which the vIOMMU is
 * holding, as VFIO frees that HWPT once all its devices are gone.
 */
static bool smmuv3_realm_viommu_match(SMMURealmViommu *viommu,
                                      HostIOMMUDeviceIOMMUFD *hiodi)
{
    struct iommu_hwpt_arm_smmuv3 bypass_data = {
        .ste = { SMMU_STE_CFG_BYPASS | SMMU_STE_VALID, 0x0ULL },
    };
    uint32_t hwpt_id;

    if (viommu->core.iommufd != hiodi->iommufd ||
        viommu->core.s2_hwpt_id != hiodi->hwpt_id) {
        return false;
    }

    if (!iommufd_backend_alloc_hwpt(hiodi->iommufd, hiodi->devid,
                                    viommu->core.viommu_id, 0,
                                    IOMMU_HWPT_DATA_ARM_SMMUV3,
                                    sizeof(bypass_data), &bypass_data,
                                    &hwpt_id, NULL)) {
        return false;
    }
    iommufd_backend_free_id(hiodi->iommufd, hwpt_id);
    return true;
}

bool smmuv3_realm_alloc_psmmu_viommu(HostIOMMUDevice *hiod,
                                     uint32_t *viommu_id, Error **errp)
{
    HostIOMMUDeviceIOMMUFD *hiodi = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    SMMURealmViommu *viommu;
    GList *l;

    if (!smmuv3_realm_psmmu_devices) {
        smmuv3_realm_psmmu_devices = g_hash_table_new(NULL, NULL);
    }

    viommu = g_hash_table_lookup(smmuv3_realm_psmmu_devices, hiod);
    if (!viommu) {
        for (l = smmuv3_realm_psmmu_viommus; l; l = l->next) {
            if (smmuv3_realm_viommu_match(l->data, hiodi)) {
                viommu = l->data;
                break;
            }
        }
        if (!viommu) {
            viommu = smmuv3_realm_viommu_alloc(hiodi, NULL, 0, errp);
            if (!viommu) {
                return false;
            }
            smmuv3_realm_psmmu_viommus =
                g_list_prepend(smmuv3_realm_psmmu_viommus, viommu);
        }
        viommu->users++;
        g_hash_table_insert(smmuv3_realm_psmmu_devices, hiod, viommu);
    }

    *viommu_id = viommu->core.viommu_id;
    return true;
}

void smmuv3_realm_free_psmmu_viommu(HostIOMMUDevice *hiod)
{
    SMMURealmViommu *viommu;

    if (!smmuv3_realm_psmmu_devices) {
        return;
    }

    viommu = g_hash_table_lookup(smmuv3_realm_psmmu_devices, hiod);
    if (!viommu) {
        return;
    }

    g_hash_table_remove(smmuv3_realm_psmmu_devices, hiod);
    if (--viommu->users) {
        return;
    }

    smmuv3_realm_psmmu_viommus =
        g_list_remove(smmuv3_realm_psmmu_viommus, viommu);
    smmuv3_realm_viommu_free(viommu);
}

static SMMUv3RealmDevice *smmuv3_realm_get_dev(SMMUState *bs,
                                                SMMUPciBus *sbus,
                                                PCIBus *bus, int devfn)
{
    SMMUDevice *sdev = sbus->pbdev[devfn];
    SMMUv3RealmDevice *realm_dev;

    if (sdev) {
        return container_of(sdev, SMMUv3RealmDevice, sdev);
    }

    realm_dev = g_new0(SMMUv3RealmDevice, 1);
    sdev = &realm_dev->sdev;
    sbus->pbdev[devfn] = sdev;
    smmu_init_sdev(bs, sdev, bus, devfn);
    return realm_dev;
}

static SMMUv3RealmDevice *smmuv3_realm_find_dev(SMMUState *bs, PCIBus *bus,
                                                 int devfn)
{
    SMMUPciBus *sbus = g_hash_table_lookup(bs->smmu_pcibus_by_busptr, bus);

    if (!sbus || !sbus->pbdev[devfn]) {
        return NULL;
    }
    return container_of(sbus->pbdev[devfn], SMMUv3RealmDevice, sdev);
}

/*
 * Devices behind a Realm vSMMU all share one vIOMMU that describes the vSMMU to
 * the kernel. The first device allocates it on top of its nesting parent HWPT.
 */
/*
 * Allocate a pair of bypass HWPTs that set the EATS field in the STE explicitly
 * per the ATS state of the device that the guest sees, as the guest must issue
 * ATC invalidations for an enabled ATS. Otherwise, the bypass STE is identical
 * to the one of the nesting parent HWPT.
 */
static bool smmuv3_realm_viommu_alloc_bypass(SMMURealmViommu *viommu,
                                             HostIOMMUDeviceIOMMUFD *hiodi,
                                             Error **errp)
{
    struct iommu_hwpt_arm_smmuv3 bypass_data = {
        .ste = { SMMU_STE_CFG_BYPASS | SMMU_STE_VALID, 0x0ULL },
        .flags = IOMMU_HWPT_ARM_SMMUV3_FLAGS_BYPASS_EATS_VALID,
    };
    struct iommu_hwpt_arm_smmuv3 bypass_ats_data = {
        .ste = {
            SMMU_STE_CFG_BYPASS | SMMU_STE_VALID,
            SMMU_STE_EATS_TRANS,
        },
        .flags = IOMMU_HWPT_ARM_SMMUV3_FLAGS_BYPASS_EATS_VALID,
    };

    return iommufd_backend_alloc_hwpt(hiodi->iommufd, hiodi->devid,
                                      viommu->core.viommu_id, 0,
                                      IOMMU_HWPT_DATA_ARM_SMMUV3,
                                      sizeof(bypass_data), &bypass_data,
                                      &viommu->bypass_hwpt_id, errp) &&
           iommufd_backend_alloc_hwpt(hiodi->iommufd, hiodi->devid,
                                      viommu->core.viommu_id, 0,
                                      IOMMU_HWPT_DATA_ARM_SMMUV3,
                                      sizeof(bypass_ats_data),
                                      &bypass_ats_data,
                                      &viommu->bypass_ats_hwpt_id, errp);
}

static bool smmuv3_realm_dev_get_viommu(SMMUv3RealmDevice *realm_dev,
                                         Error **errp)
{
    struct iommu_viommu_arm_realm_vsmmuv3 args = { 0 };
    HostIOMMUDeviceIOMMUFD *hiodi = realm_dev->hiodi;
    SMMUv3State *s = ARM_SMMUV3(realm_dev->sdev.smmu);
    SMMUv3RealmState *realm = s->s_realm;
    SMMURealmViommu *viommu = realm->viommu;

    if (realm_dev->viommu) {
        return true;
    }

    if (!viommu) {
        args.flags = IOMMU_VIOMMU_ARM_REALM_SMMUV3_FLAGS_VSMMU;
        args.reg_base = s->mmio_base;
        args.reg_top = s->mmio_base + 0x20000;
        args.aidr = VSMMU_AIDR_VALUE;
        args.idr[0] = VSMMU_IDR0_VALUE;
        args.idr[1] = VSMMU_IDR1_VALUE;
        args.idr[3] = VSMMU_IDR3_VALUE;
        args.idr[5] = VSMMU_IDR5_VALUE;

        viommu = smmuv3_realm_viommu_alloc(hiodi, &args, sizeof(args), errp);
        if (!viommu) {
            return false;
        }
        if (!smmuv3_realm_viommu_alloc_bypass(viommu, hiodi, errp)) {
            smmuv3_realm_viommu_free(viommu);
            return false;
        }
        realm->viommu = viommu;
    }

    /*
     * The guest cannot have enabled ATS yet, since this runs at VM start or at
     * a hotplug. smmuv3_realm_set_ats() switches the bypass HWPT later on.
     */
    if (!host_iommu_device_iommufd_attach_hwpt(hiodi, viommu->bypass_hwpt_id,
                                               errp)) {
        if (QLIST_EMPTY(&viommu->device_list)) {
            smmuv3_realm_viommu_free(viommu);
            realm->viommu = NULL;
        }
        return false;
    }

    realm_dev->viommu = viommu;
    QLIST_INSERT_HEAD(&viommu->device_list, realm_dev, next);
    return true;
}

static void smmuv3_realm_dev_put_viommu(SMMUv3RealmDevice *realm_dev)
{
    SMMUv3State *s = ARM_SMMUV3(realm_dev->sdev.smmu);
    HostIOMMUDeviceIOMMUFD *hiodi = realm_dev->hiodi;
    SMMURealmViommu *viommu = realm_dev->viommu;

    if (!viommu) {
        return;
    }

    /* Detach the device from the bypass HWPT before it might be freed */
    if (!host_iommu_device_iommufd_attach_hwpt(hiodi, hiodi->hwpt_id, NULL)) {
        error_report("Unable to attach device 0x%x to its default HWPT",
                     smmu_get_sid(&realm_dev->sdev));
    }

    QLIST_REMOVE(realm_dev, next);
    realm_dev->viommu = NULL;

    if (QLIST_EMPTY(&viommu->device_list)) {
        smmuv3_realm_viommu_free(viommu);
        s->s_realm->viommu = NULL;
    }
}

static bool smmuv3_realm_set_iommu_device(PCIBus *bus, void *opaque,
                                           int devfn,
                                           HostIOMMUDevice *hiod,
                                           Error **errp)
{
    HostIOMMUDeviceIOMMUFD *hiodi = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    SMMUState *bs = opaque;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3RealmDevice *realm_dev =
        smmuv3_realm_get_dev(bs, sbus, bus, devfn);

    if (!hiodi) {
        return true;
    }

    if (realm_dev->hiodi) {
        if (realm_dev->hiodi != hiodi) {
            error_setg(errp, "Device 0x%x already has an associated hiodi",
                       smmu_get_sid(&realm_dev->sdev));
            return false;
        }
        return true;
    }

    realm_dev->hiodi = hiodi;
    trace_smmuv3_realm_set_iommu_device(devfn,
                                        smmu_get_sid(&realm_dev->sdev));
    return true;
}

static void smmuv3_realm_unset_iommu_device(PCIBus *bus, void *opaque,
                                             int devfn)
{
    SMMUv3RealmDevice *realm_dev = smmuv3_realm_find_dev(opaque, bus, devfn);

    if (!realm_dev || !realm_dev->hiodi) {
        return;
    }

    smmuv3_realm_dev_put_viommu(realm_dev);
    realm_dev->hiodi = NULL;
    trace_smmuv3_realm_unset_iommu_device(devfn,
                                          smmu_get_sid(&realm_dev->sdev));
}

static bool smmuv3_realm_alloc_viommu(PCIBus *bus, void *opaque, int devfn,
                                      HostIOMMUDevice *hiod,
                                      uint32_t *viommu_id, Error **errp)
{
    SMMUv3RealmDevice *realm_dev = smmuv3_realm_find_dev(opaque, bus, devfn);

    if (!realm_dev || !realm_dev->hiodi ||
        HOST_IOMMU_DEVICE(realm_dev->hiodi) != hiod) {
        error_setg(errp, "Device 0x%x has no host IOMMU device set",
                   PCI_BUILD_BDF(pci_bus_num(bus), devfn));
        return false;
    }

    if (!smmuv3_realm_dev_get_viommu(realm_dev, errp)) {
        return false;
    }

    *viommu_id = realm_dev->viommu->core.viommu_id;
    return true;
}

static bool smmuv3_realm_set_ats(PCIBus *bus, void *opaque, int devfn,
                                 bool enabled, Error **errp)
{
    SMMUv3RealmDevice *realm_dev = smmuv3_realm_find_dev(opaque, bus, devfn);
    SMMURealmViommu *viommu;
    uint32_t hwpt_id;

    if (!realm_dev || !realm_dev->hiodi || !realm_dev->viommu) {
        error_setg(errp, "Device 0x%x has no Realm vIOMMU",
                   PCI_BUILD_BDF(pci_bus_num(bus), devfn));
        return false;
    }

    viommu = realm_dev->viommu;
    if (enabled) {
        hwpt_id = viommu->bypass_ats_hwpt_id;
    } else {
        hwpt_id = viommu->bypass_hwpt_id;
    }

    return host_iommu_device_iommufd_attach_hwpt(realm_dev->hiodi, hwpt_id,
                                                  errp);
}

static void smmuv3_realm_free_viommu(PCIBus *bus, void *opaque, int devfn,
                                     HostIOMMUDevice *hiod)
{
    SMMUv3RealmDevice *realm_dev = smmuv3_realm_find_dev(opaque, bus, devfn);

    if (realm_dev && realm_dev->hiodi) {
        smmuv3_realm_dev_put_viommu(realm_dev);
    }
}

static bool smmuv3_realm_pdev_allowed(PCIDevice *pdev, bool *vfio_pci)
{
    if (object_dynamic_cast(OBJECT(pdev), TYPE_PCI_BRIDGE) ||
        object_dynamic_cast(OBJECT(pdev), TYPE_PXB_PCIE_DEV) ||
        object_dynamic_cast(OBJECT(pdev), TYPE_GPEX_ROOT_DEVICE)) {
        return true;
    }

    if (object_dynamic_cast(OBJECT(pdev), TYPE_VFIO_PCI)) {
        *vfio_pci = true;
        return object_property_get_link(OBJECT(pdev), "iommufd", NULL) != NULL;
    }

    return false;
}

static bool smmuv3_realm_supports_as(PCIBus *bus, void *opaque, int devfn,
                                     Error **errp)
{
    PCIDevice *pdev = pci_find_device(bus, pci_bus_num(bus), devfn);
    bool vfio_pci = false;

    if (!pdev || smmuv3_realm_pdev_allowed(pdev, &vfio_pci)) {
        return true;
    }

    if (vfio_pci) {
        error_setg(errp, "vfio-pci endpoint devices without an iommufd "
                   "backend are not allowed with arm-smmuv3,realm=on");
    } else {
        error_setg(errp, "Emulated endpoint devices are not allowed with "
                   "arm-smmuv3,realm=on");
    }
    return false;
}

static AddressSpace *smmuv3_realm_find_add_as(PCIBus *bus, void *opaque,
                                               int devfn)
{
    PCIDevice *pdev = pci_find_device(bus, pci_bus_num(bus), devfn);
    SMMUState *bs = opaque;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3RealmDevice *realm_dev =
        smmuv3_realm_get_dev(bs, sbus, bus, devfn);
    bool vfio_pci = false;

    if (pdev && !smmuv3_realm_pdev_allowed(pdev, &vfio_pci)) {
        g_assert_not_reached();
    }

    /*
     * An untrusted Realm device can only access the shared, upper half of the
     * IPA space. The RME DMA address space tracks shared RAM by those aliases,
     * so the nesting parent HWPT holds only the mappings a T=0 device may use.
     * Once the device becomes trusted, RMM programs its protected stage-2 from
     * the Realm RTT instead.
     */
    if (vfio_pci) {
        AddressSpace *dma_as = kvm_arm_rme_get_dma_as();

        g_assert(dma_as);
        return dma_as;
    } else {
        return &realm_dev->sdev.as;
    }
}

static uint64_t smmuv3_realm_get_msi_gpa(PCIBus *bus, void *opaque,
                                          int devfn)
{
    SMMUv3State *s = ARM_SMMUV3(opaque);

    g_assert(s->msi_gpa);
    return s->msi_gpa;
}

static uint64_t smmuv3_realm_get_viommu_flags(void *opaque)
{
    uint64_t flags = VIOMMU_FLAG_WANT_NESTING_PARENT;
    SMMUv3State *s = ARM_SMMUV3(opaque);

    if (smmuv3_pasid_supported(s)) {
        flags |= VIOMMU_FLAG_PASID_SUPPORTED;
    }
    return flags;
}

static const PCIIOMMUOps smmuv3_realm_ops = {
    .supports_address_space = smmuv3_realm_supports_as,
    .get_address_space = smmuv3_realm_find_add_as,
    .get_viommu_flags = smmuv3_realm_get_viommu_flags,
    .alloc_viommu = smmuv3_realm_alloc_viommu,
    .free_viommu = smmuv3_realm_free_viommu,
    .set_iommu_device = smmuv3_realm_set_iommu_device,
    .unset_iommu_device = smmuv3_realm_unset_iommu_device,
    .get_msi_direct_gpa = smmuv3_realm_get_msi_gpa,
    .set_ats = smmuv3_realm_set_ats,
};

bool smmuv3_realm_init(SMMUv3State *s, Error **errp)
{
    SMMUState *bs = ARM_SMMU(s);

    bs->iommu_ops = &smmuv3_realm_ops;
    s->s_realm = g_new0(SMMUv3RealmState, 1);
    return true;
}
