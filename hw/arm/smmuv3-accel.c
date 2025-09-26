/*
 * Copyright (c) 2025 Huawei Technologies R & D (UK) Ltd
 * Copyright (C) 2025 NVIDIA
 * Written by Nicolin Chen, Shameer Kolothum
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "trace.h"

#include "hw/arm/smmuv3.h"
#include "hw/iommu.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-host/gpex.h"
#include "hw/vfio/pci.h"

#include "smmuv3-accel.h"
#include "smmuv3-internal.h"

#define SMMU_STE_VALID      (1ULL << 0)
#define SMMU_STE_CFG_BYPASS (1ULL << 3)

#define STE0_V       MAKE_64BIT_MASK(0, 1)
#define STE0_CONFIG  MAKE_64BIT_MASK(1, 3)
#define STE0_S1FMT   MAKE_64BIT_MASK(4, 2)
#define STE0_CTXPTR  MAKE_64BIT_MASK(6, 50)
#define STE0_S1CDMAX MAKE_64BIT_MASK(59, 5)
#define STE0_MASK    (STE0_S1CDMAX | STE0_CTXPTR | STE0_S1FMT | STE0_CONFIG | \
                      STE0_V)

#define STE1_S1DSS    MAKE_64BIT_MASK(0, 2)
#define STE1_S1CIR    MAKE_64BIT_MASK(2, 2)
#define STE1_S1COR    MAKE_64BIT_MASK(4, 2)
#define STE1_S1CSH    MAKE_64BIT_MASK(6, 2)
#define STE1_S1STALLD MAKE_64BIT_MASK(27, 1)
#define STE1_ETS      MAKE_64BIT_MASK(28, 2)
#define STE1_MASK     (STE1_ETS | STE1_S1STALLD | STE1_S1CSH | STE1_S1COR | \
                       STE1_S1CIR | STE1_S1DSS)

static bool
smmuv3_accel_check_hw_compatible(SMMUv3State *s,
                                 struct iommu_hw_info_arm_smmuv3 *info,
                                 Error **errp)
{
    uint32_t val;

    /*
     * QEMU SMMUv3 supports both linear and 2-level stream tables.
     */
    val = FIELD_EX32(info->idr[0], IDR0, STLEVEL);
    if (val != FIELD_EX32(s->idr[0], IDR0, STLEVEL)) {
        s->idr[0] = FIELD_DP32(s->idr[0], IDR0, STLEVEL, val);
        error_setg(errp, "Host SUMMUv3 differs in Stream Table format");
        return false;
    }

    /* QEMU SMMUv3 supports only little-endian translation table walks */
    val = FIELD_EX32(info->idr[0], IDR0, TTENDIAN);
    if (!val && val > FIELD_EX32(s->idr[0], IDR0, TTENDIAN)) {
        error_setg(errp, "Host SUMMUv3 doesn't support Little-endian "
                   "translation table");
        return false;
    }

    /* QEMU SMMUv3 supports only AArch64 translation table format */
    val = FIELD_EX32(info->idr[0], IDR0, TTF);
    if (val < FIELD_EX32(s->idr[0], IDR0, TTF)) {
        error_setg(errp, "Host SUMMUv3 deosn't support Arch64 Translation "
                   "table format");
        return false;
    }

    /* QEMU SMMUv3 supports SIDSIZE 16 */
    val = FIELD_EX32(info->idr[1], IDR1, SIDSIZE);
    if (val < FIELD_EX32(s->idr[1], IDR1, SIDSIZE)) {
        error_setg(errp, "Host SUMMUv3 SIDSIZE not compatible");
        return false;
    }

    /* If user enables PASID support(pasid=on), QEMU sets SSIDSIZE to 16 */
    val = FIELD_EX32(info->idr[1], IDR1, SSIDSIZE);
    if (val < FIELD_EX32(s->idr[1], IDR1, SSIDSIZE)) {
        error_setg(errp, "Host SUMMUv3 SSIDSIZE not compatible");
        return false;
    }

    /* User can override QEMU SMMUv3 Range Invalidation support */
    val = FIELD_EX32(info->idr[3], IDR3, RIL);
    if (val != FIELD_EX32(s->idr[3], IDR3, RIL)) {
        error_setg(errp, "Host SUMMUv3 differs in Range Invalidation support");
        return false;
    }

    /*
     * ToDo: OAS is not something Linux kernel doc says meaningful for user.
     * But looks like OAS needs to be compatibe for accelerator support. Please
     * check.
     */
    val = FIELD_EX32(info->idr[5], IDR5, OAS);
    if (val < FIELD_EX32(s->idr[5], IDR5, OAS)) {
        error_setg(errp, "Host SUMMUv3 OAS not compatible");
        return false;
    }

    val = FIELD_EX32(info->idr[5], IDR5, GRAN4K);
    if (val != FIELD_EX32(s->idr[5], IDR5, GRAN4K)) {
        error_setg(errp, "Host SMMUv3 doesn't support 64K translation granule");
        return false;
    }
    val = FIELD_EX32(info->idr[5], IDR5, GRAN16K);
    if (val != FIELD_EX32(s->idr[5], IDR5, GRAN16K)) {
        error_setg(errp, "Host SMMUv3 doesn't support 16K translation granule");
        return false;
    }
    val = FIELD_EX32(info->idr[5], IDR5, GRAN64K);
    if (val != FIELD_EX32(s->idr[5], IDR5, GRAN64K)) {
        error_setg(errp, "Host SMMUv3 doesn't support 16K translation granule");
        return false;
    }
    return true;
}

static bool
smmuv3_accel_hw_compatible(SMMUv3State *s, HostIOMMUDeviceIOMMUFD *idev,
                           Error **errp)
{
    struct iommu_hw_info_arm_smmuv3 info;
    uint32_t data_type;
    uint64_t caps;

    if (!iommufd_backend_get_device_info(idev->iommufd, idev->devid, &data_type,
                                         &info, sizeof(info), &caps, NULL,
                                         errp)) {
        return false;
    }

    if (data_type != IOMMU_HW_INFO_TYPE_ARM_SMMUV3) {
        error_setg(errp, "Wrong data type (%d) for Host SMMUv3 device info",
                     data_type);
        return false;
    }

    if (!smmuv3_accel_check_hw_compatible(s, &info, errp)) {
        return false;
    }
    return true;
}

static bool
smmuv3_accel_alloc_vdev(SMMUv3AccelDevice *accel_dev, int sid, Error **errp)
{
    SMMUViommu *viommu = accel_dev->viommu;
    IOMMUFDVdev *vdev;
    uint32_t vdev_id;

    if (!accel_dev->idev || accel_dev->vdev) {
        return true;
    }

    if (!iommufd_backend_alloc_vdev(viommu->iommufd, accel_dev->idev->devid,
                                   viommu->core.viommu_id, sid,
                                   &vdev_id, errp)) {
            return false;
    }
    if (!host_iommu_device_iommufd_attach_hwpt(accel_dev->idev,
                                               viommu->bypass_hwpt_id, errp)) {
        iommufd_backend_free_id(viommu->iommufd, vdev_id);
        return false;
    }

    vdev = g_new(IOMMUFDVdev, 1);
    vdev->vdev_id = vdev_id;
    vdev->dev_id = sid;
    accel_dev->vdev = vdev;
    return true;
}

static bool
smmuv3_accel_dev_uninstall_nested_ste(SMMUv3AccelDevice *accel_dev, bool abort,
                                      Error **errp)
{
    HostIOMMUDeviceIOMMUFD *idev = accel_dev->idev;
    SMMUS1Hwpt *s1_hwpt = accel_dev->s1_hwpt;
    uint32_t hwpt_id;

    if (!s1_hwpt || !accel_dev->viommu) {
        return true;
    }

    if (abort) {
        hwpt_id = accel_dev->viommu->abort_hwpt_id;
    } else {
        hwpt_id = accel_dev->viommu->bypass_hwpt_id;
    }

    if (!host_iommu_device_iommufd_attach_hwpt(idev, hwpt_id, errp)) {
        return false;
    }

    iommufd_backend_free_id(s1_hwpt->iommufd, s1_hwpt->hwpt_id);
    accel_dev->s1_hwpt = NULL;
    g_free(s1_hwpt);
    return true;
}

static bool
smmuv3_accel_dev_install_nested_ste(SMMUv3AccelDevice *accel_dev,
                                    uint32_t data_type, uint32_t data_len,
                                    void *data, Error **errp)
{
    SMMUViommu *viommu = accel_dev->viommu;
    SMMUS1Hwpt *s1_hwpt = accel_dev->s1_hwpt;
    HostIOMMUDeviceIOMMUFD *idev = accel_dev->idev;
    uint32_t flags = 0;

    if (!idev || !viommu) {
        error_setg(errp, "Device 0x%x has no associated IOMMU dev or vIOMMU",
                   smmu_get_sid(&accel_dev->sdev));
        return false;
    }

    if (s1_hwpt) {
        if (!smmuv3_accel_dev_uninstall_nested_ste(accel_dev, true, errp)) {
            return false;
        }
    }

    s1_hwpt = g_new0(SMMUS1Hwpt, 1);
    s1_hwpt->iommufd = idev->iommufd;
    if (!iommufd_backend_alloc_hwpt(idev->iommufd, idev->devid,
                                    viommu->core.viommu_id, flags, data_type,
                                    data_len, data, &s1_hwpt->hwpt_id, errp)) {
        return false;
    }

    if (!host_iommu_device_iommufd_attach_hwpt(idev, s1_hwpt->hwpt_id, errp)) {
        iommufd_backend_free_id(idev->iommufd, s1_hwpt->hwpt_id);
        return false;
    }
    accel_dev->s1_hwpt = s1_hwpt;
    return true;
}

bool
smmuv3_accel_install_nested_ste(SMMUv3State *s, SMMUDevice *sdev, int sid,
                                Error **errp)
{
    SMMUv3AccelDevice *accel_dev;
    SMMUEventInfo event = {.type = SMMU_EVT_NONE, .sid = sid,
                           .inval_ste_allowed = true};
    struct iommu_hwpt_arm_smmuv3 nested_data = {};
    uint64_t ste_0, ste_1;
    uint32_t config;
    STE ste;
    int ret;

    if (!s->accel) {
        return true;
    }

    accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);
    if (!accel_dev->viommu) {
        return true;
    }

    if (!smmuv3_accel_alloc_vdev(accel_dev, sid, errp)) {
        return false;
    }

    ret = smmu_find_ste(sdev->smmu, sid, &ste, &event);
    if (ret) {
        error_setg(errp, "Failed to find STE for Device 0x%x", sid);
        return true;
    }

    config = STE_CONFIG(&ste);
    if (!STE_VALID(&ste) || !STE_CFG_S1_ENABLED(config)) {
        if (!smmuv3_accel_dev_uninstall_nested_ste(accel_dev,
                                                   STE_CFG_ABORT(config),
                                                   errp)) {
            return false;
        }
        smmuv3_flush_config(sdev);
        return true;
    }

    ste_0 = (uint64_t)ste.word[0] | (uint64_t)ste.word[1] << 32;
    ste_1 = (uint64_t)ste.word[2] | (uint64_t)ste.word[3] << 32;
    nested_data.ste[0] = cpu_to_le64(ste_0 & STE0_MASK);
    nested_data.ste[1] = cpu_to_le64(ste_1 & STE1_MASK);

    if (!smmuv3_accel_dev_install_nested_ste(accel_dev,
                                             IOMMU_HWPT_DATA_ARM_SMMUV3,
                                             sizeof(nested_data),
                                             &nested_data, errp)) {
        error_setg(errp, "Unable to install nested STE=%16LX:%16LX, sid=0x%x,"
                   "ret=%d", nested_data.ste[1], nested_data.ste[0], sid, ret);
        return false;
    }
    trace_smmuv3_accel_install_nested_ste(sid, nested_data.ste[1],
                                          nested_data.ste[0]);
    return true;
}

bool smmuv3_accel_install_nested_ste_range(SMMUv3State *s, SMMUSIDRange *range,
                                           Error **errp)
{
    SMMUv3AccelState *s_accel = s->s_accel;
    SMMUv3AccelDevice *accel_dev;

    if (!s_accel || !s_accel->viommu) {
        return true;
    }

    QLIST_FOREACH(accel_dev, &s_accel->viommu->device_list, next) {
        uint32_t sid = smmu_get_sid(&accel_dev->sdev);

        if (sid >= range->start && sid <= range->end) {
            if (!smmuv3_accel_install_nested_ste(s, &accel_dev->sdev,
                                                 sid, errp)) {
                return false;
            }
        }
    }
    return true;
}

/*
 * This issues the invalidation cmd to the host SMMUv3.
 * Note: sdev can be NULL for certain invalidation commands
 * e.g., SMMU_CMD_TLBI_NH_ASID, SMMU_CMD_TLBI_NH_VA etc.
 */
bool smmuv3_accel_issue_inv_cmd(SMMUv3State *bs, void *cmd, SMMUDevice *sdev,
                                Error **errp)
{
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    IOMMUFDViommu *viommu_core;
    uint32_t entry_num = 1;

    if (!s->accel || !s_accel->viommu) {
        return true;
    }

   /*
    * We may end up here for any emulated PCI bridge or root port type devices.
    * However, passing invalidation commands with sid (eg: CFGI_CD) to host
    * SMMUv3 only matters for vfio-pci endpoint devices. Hence check that if
    * sdev is valid.
    */
    if (sdev) {
        SMMUv3AccelDevice *accel_dev = container_of(sdev, SMMUv3AccelDevice,
                                                    sdev);
        if (!accel_dev->vdev) {
            return true;
        }
    }

    viommu_core = &s_accel->viommu->core;
    return iommufd_backend_invalidate_cache(
                   viommu_core->iommufd, viommu_core->viommu_id,
                   IOMMU_VIOMMU_INVALIDATE_DATA_ARM_SMMUV3,
                   sizeof(Cmd), &entry_num, cmd, errp);
}

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

static bool
smmuv3_accel_dev_alloc_viommu(SMMUv3AccelDevice *accel_dev,
                              HostIOMMUDeviceIOMMUFD *idev, Error **errp)
{
    struct iommu_hwpt_arm_smmuv3 bypass_data = {
        .ste = { SMMU_STE_CFG_BYPASS | SMMU_STE_VALID, 0x0ULL },
    };
    struct iommu_hwpt_arm_smmuv3 abort_data = {
        .ste = { SMMU_STE_VALID, 0x0ULL },
    };
    SMMUDevice *sdev = &accel_dev->sdev;
    SMMUState *bs = sdev->smmu;
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    uint32_t s2_hwpt_id = idev->hwpt_id;
    SMMUViommu *viommu;
    uint32_t viommu_id;

    if (s_accel->viommu) {
        accel_dev->viommu = s_accel->viommu;
        return true;
    }

    if (!iommufd_backend_alloc_viommu(idev->iommufd, idev->devid,
                                      IOMMU_VIOMMU_TYPE_ARM_SMMUV3,
                                      s2_hwpt_id, &viommu_id, errp)) {
        return false;
    }

    viommu = g_new0(SMMUViommu, 1);
    viommu->core.viommu_id = viommu_id;
    viommu->core.s2_hwpt_id = s2_hwpt_id;
    viommu->core.iommufd = idev->iommufd;

    if (!iommufd_backend_alloc_hwpt(idev->iommufd, idev->devid,
                                    viommu->core.viommu_id, 0,
                                    IOMMU_HWPT_DATA_ARM_SMMUV3,
                                    sizeof(abort_data), &abort_data,
                                    &viommu->abort_hwpt_id, errp)) {
        goto free_viommu;
    }

    if (!iommufd_backend_alloc_hwpt(idev->iommufd, idev->devid,
                                    viommu->core.viommu_id, 0,
                                    IOMMU_HWPT_DATA_ARM_SMMUV3,
                                    sizeof(bypass_data), &bypass_data,
                                    &viommu->bypass_hwpt_id, errp)) {
        goto free_abort_hwpt;
    }

    viommu->iommufd = idev->iommufd;

    s_accel->viommu = viommu;
    accel_dev->viommu = viommu;
    return true;

free_abort_hwpt:
    iommufd_backend_free_id(idev->iommufd, viommu->abort_hwpt_id);
free_viommu:
    iommufd_backend_free_id(idev->iommufd, viommu->core.viommu_id);
    g_free(viommu);
    return false;
}

static bool smmuv3_accel_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                          HostIOMMUDevice *hiod, Error **errp)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    SMMUState *bs = opaque;
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3AccelDevice *accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    SMMUDevice *sdev = &accel_dev->sdev;
    uint16_t sid = smmu_get_sid(sdev);

    if (!idev) {
        return true;
    }

    if (accel_dev->idev) {
        if (accel_dev->idev != idev) {
            error_setg(errp, "Device 0x%x already has an associated IOMMU dev",
                       sid);
            return false;
        }
        return true;
    }

    /*
     * Check the host SMMUv3 associated with the dev is compatible with the
     * QEMU SMMUv3 accel.
     */
    if (!smmuv3_accel_hw_compatible(s, idev, errp)) {
        return false;
    }

    if (!smmuv3_accel_dev_alloc_viommu(accel_dev, idev, errp)) {
        error_setg(errp, "Device 0x%x: Unable to alloc viommu", sid);
        return false;
    }

    accel_dev->idev = idev;
    QLIST_INSERT_HEAD(&s_accel->viommu->device_list, accel_dev, next);
    trace_smmuv3_accel_set_iommu_device(devfn, sid);
    return true;
}

static void smmuv3_accel_unset_iommu_device(PCIBus *bus, void *opaque,
                                            int devfn)
{
    SMMUState *bs = opaque;
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUPciBus *sbus = g_hash_table_lookup(bs->smmu_pcibus_by_busptr, bus);
    SMMUv3AccelDevice *accel_dev;
    SMMUViommu *viommu;
    IOMMUFDVdev *vdev;
    SMMUDevice *sdev;
    uint16_t sid;

    if (!sbus) {
        return;
    }

    sdev = sbus->pbdev[devfn];
    if (!sdev) {
        return;
    }

    sid = smmu_get_sid(sdev);
    accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);
    if (!host_iommu_device_iommufd_attach_hwpt(accel_dev->idev,
                                               accel_dev->idev->hwpt_id,
                                               NULL)) {
        error_report("Unable to attach dev 0x%x to the default HW pagetable",
                     sid);
    }

    accel_dev->idev = NULL;
    QLIST_REMOVE(accel_dev, next);
    trace_smmuv3_accel_unset_iommu_device(devfn, sid);

    viommu = s->s_accel->viommu;
    vdev = accel_dev->vdev;
    if (vdev) {
        iommufd_backend_free_id(viommu->iommufd, vdev->vdev_id);
        g_free(vdev);
        accel_dev->vdev = NULL;
    }

    if (QLIST_EMPTY(&viommu->device_list)) {
        iommufd_backend_free_id(viommu->iommufd, viommu->bypass_hwpt_id);
        iommufd_backend_free_id(viommu->iommufd, viommu->abort_hwpt_id);
        iommufd_backend_free_id(viommu->iommufd, viommu->core.viommu_id);
        g_free(viommu);
        s->s_accel->viommu = NULL;
    }
}

static AddressSpace *smmuv3_accel_find_msi_as(PCIBus *bus, void *opaque,
                                              int devfn)
{
    SMMUState *bs = opaque;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3AccelDevice *accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    SMMUDevice *sdev = &accel_dev->sdev;

    /*
     * If the assigned vfio-pci dev has S1 translation enabled by
     * Guest, return IOMMU address space for MSI translation.
     * Otherwise, return system address space.
     */
    if (accel_dev->s1_hwpt) {
        return &sdev->as;
    } else {
        return &address_space_memory;
    }
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
    uint64_t flags = VIOMMU_FLAG_WANT_NESTING_PARENT;
    SMMUState *bs = opaque;
    SMMUv3State *s = ARM_SMMUV3(bs);

    if (s->pasid) {
        flags |= VIOMMU_FLAG_PASID_SUPPORTED;
    }
    return flags;
}

static const PCIIOMMUOps smmuv3_accel_ops = {
    .get_address_space = smmuv3_accel_find_add_as,
    .get_viommu_flags = smmuv3_accel_get_viommu_flags,
    .set_iommu_device = smmuv3_accel_set_iommu_device,
    .unset_iommu_device = smmuv3_accel_unset_iommu_device,
    .get_msi_address_space = smmuv3_accel_find_msi_as,
};

void smmuv3_accel_idr_override(SMMUv3State *s)
{
    if (!s->accel) {
        return;
    }

    /* By default QEMU SMMUv3 has RIL. Update IDR3 if user has disabled it */
    if (!s->ril) {
        s->idr[3] = FIELD_DP32(s->idr[3], IDR3, RIL, 0);
    }
    /* QEMU SMMUv3 has no ATS. Update IDR0 if user has enabled it */
    if (s->ats) {
        s->idr[0] = FIELD_DP32(s->idr[0], IDR0, ATS, 1); /* ATS */
    }
    /* QEMU SMMUv3 has oas set 44. Update IDR5 if user has it set to 48 bits*/
    if (s->oas == 48) {
        s->idr[5] = FIELD_DP32(s->idr[5], IDR5, OAS, SMMU_IDR5_OAS_48);
    }

    /*
     * By default QEMU SMMUv3 has no PASID(SSID) support. Update IDR1 if user
     * has enabled it.
     */
    if (s->pasid) {
        s->idr[1] = FIELD_DP32(s->idr[1], IDR1, SSIDSIZE, SMMU_IDR1_SSIDSIZE);
    }
}

/*
 * If the guest reboots and devices are configured for S1+S2, Stage1 must
 * be switched to bypass. Otherwise, QEMU/UEFI may fail when accessing a
 * device, e.g. when UEFI retrieves boot partition information from an
 * assigned vfio-pci NVMe device.
 */
void smmuv3_accel_attach_bypass_hwpt(SMMUv3State *s)
{
    SMMUv3AccelDevice *accel_dev;
    SMMUViommu *viommu;

    if (!s->accel || !s->s_accel->viommu) {
        return;
    }

    viommu = s->s_accel->viommu;
    QLIST_FOREACH(accel_dev, &viommu->device_list, next) {
        if (!accel_dev->vdev) {
            continue;
        }
        if (!host_iommu_device_iommufd_attach_hwpt(accel_dev->idev,
                                                   viommu->bypass_hwpt_id,
                                                   NULL)) {
            error_report("Failed to install bypass hwpt id %u for dev id %u",
                          viommu->bypass_hwpt_id, accel_dev->idev->devid);
        }
    }
}

void smmuv3_accel_init(SMMUv3State *s)
{
    SMMUState *bs = ARM_SMMU(s);

    bs->iommu_ops = &smmuv3_accel_ops;
    s->s_accel = g_new0(SMMUv3AccelState, 1);
}
