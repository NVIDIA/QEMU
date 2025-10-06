/*
 * Copyright (c) 2025 Huawei Technologies R & D (UK) Ltd
 * Copyright (C) 2025 NVIDIA
 * Written by Nicolin Chen, Shameer Kolothum
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SMMUV3_ACCEL_H
#define HW_ARM_SMMUV3_ACCEL_H

#include "hw/arm/smmu-common.h"
#include "system/iommufd.h"
#include <linux/iommufd.h>
#include CONFIG_DEVICES

#define TYPE_TEGRA241_CMDQV "tegra241-cmdqv"
#define TEGRA241_CMDQV_VERSION 0x1
#define TEGRA241_CMDQV_NUM_CMDQ_LOG2 0x1
#define TEGRA241_CMDQV_NUM_SID_PER_VM_LOG2 0x4
typedef struct Tegra241CMDQV Tegra241CMDQV;

typedef struct SMMUViommu {
    IOMMUFDBackend *iommufd;
    IOMMUFDViommu core;
    IOMMUFDVeventq *veventq;
    uint32_t bypass_hwpt_id;
    uint32_t abort_hwpt_id;
    struct iommu_viommu_tegra241_cmdqv cmdqv_data;
    QLIST_HEAD(, SMMUv3AccelDevice) device_list;
} SMMUViommu;

typedef struct SMMUS1Hwpt {
    IOMMUFDBackend *iommufd;
    uint32_t hwpt_id;
} SMMUS1Hwpt;

typedef struct SMMUv3AccelDevice {
    SMMUDevice  sdev;
    HostIOMMUDeviceIOMMUFD *idev;
    SMMUS1Hwpt *s1_hwpt;
    IOMMUFDVdev *vdev;
    SMMUViommu *viommu;
    QLIST_ENTRY(SMMUv3AccelDevice) next;
} SMMUv3AccelDevice;

typedef struct SMMUv3AccelState {
    SMMUViommu *viommu;
    QemuThread event_thread_id;
    QemuMutex event_thread_mutex;
    bool event_thread_stop;
    struct iommu_hw_info_tegra241_cmdqv cmdqv_info;
    Tegra241CMDQV *cmdqv;
} SMMUv3AccelState;

#ifdef CONFIG_ARM_SMMUV3_ACCEL
void smmuv3_accel_init(SMMUv3State *s);
bool smmuv3_accel_install_nested_ste(SMMUv3State *s, SMMUDevice *sdev, int sid,
                                     Error **errp);
bool smmuv3_accel_install_nested_ste_range(SMMUv3State *s, SMMUSIDRange *range,
                                           Error **errp);
bool smmuv3_accel_issue_inv_cmd(SMMUv3State *s, void *cmd, SMMUDevice *sdev,
                                Error **errp);
void smmuv3_accel_attach_bypass_hwpt(SMMUv3State *s);
void smmuv3_accel_idr_override(SMMUv3State *s);
bool smmuv3_accel_realloc_veventq(SMMUv3State *s, uint32_t log2size,
                                  Error **errp);
#else
static inline void smmuv3_accel_init(SMMUv3State *s)
{
}
static inline bool
smmuv3_accel_install_nested_ste(SMMUv3State *s, SMMUDevice *sdev, int sid,
                                Error **errp)
{
    return true;
}
static inline bool
smmuv3_accel_install_nested_ste_range(SMMUv3State *s, SMMUSIDRange *range,
                                      Error **errp)
{
    return true;
}
static inline bool
smmuv3_accel_issue_inv_cmd(SMMUv3State *s, void *cmd, SMMUDevice *sdev,
                           Error **errp)
{
    return true;
}
static inline void smmuv3_accel_attach_bypass_hwpt(SMMUv3State *s)
{
}
static inline void smmuv3_accel_idr_override(SMMUv3State *s)
{
}
bool smmuv3_accel_realloc_veventq(SMMUv3State *s, uint32_t log2size,
                                  Error **errp)
{
    return true;
}
#endif

#if defined(CONFIG_TEGRA241_CMDQV)
void tegra241_cmdqv_init(SMMUv3State *s);
void tegra241_cmdqv_reset(SMMUv3State *s);
bool tegra241_cmdqv_hw_compatible(SMMUv3State *s, HostIOMMUDeviceIOMMUFD *idev,
                                  Error **errp);
#else
static inline void tegra241_cmdqv_init(SMMUv3State *s)
{
}
static inline void tegra241_cmdqv_reset(SMMUv3State *s)
{
}
static inline bool
tegra241_cmdqv_hw_compatible(SMMUv3State *s, HostIOMMUDeviceIOMMUFD *idev,
                             Error **errp)
{
    return true;
}
#endif

#endif /* HW_ARM_SMMUV3_ACCEL_H */
