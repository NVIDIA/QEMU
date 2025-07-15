/*
 * Copyright (c) 2025 Huawei Technologies R & D (UK) Ltd
 * Copyright (C) 2025 NVIDIA
 * Written by Nicolin Chen, Shameer Kolothum
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SMMUV3_ACCEL_H
#define HW_ARM_SMMUV3_ACCEL_H

#include "hw/arm/smmuv3.h"
#include "hw/arm/smmu-common.h"
#include "system/iommufd.h"
#include <linux/iommufd.h>
#include "smmuv3-internal.h"
#include CONFIG_DEVICES

typedef struct SMMUS2Hwpt {
    IOMMUFDBackend *iommufd;
    uint32_t hwpt_id;
} SMMUS2Hwpt;

typedef struct SMMUViommu {
    IOMMUFDBackend *iommufd;
    IOMMUFDViommu core;
    SMMUS2Hwpt *s2_hwpt;
    IOMMUFDVeventq *veventq;
    SMMUState *smmu;
    uint32_t bypass_hwpt_id;
    uint32_t abort_hwpt_id;
    QLIST_HEAD(, SMMUv3AccelDevice) device_list;
} SMMUViommu;

typedef struct SMMUS1Hwpt {
    IOMMUFDBackend *iommufd;
    uint32_t hwpt_id;
} SMMUS1Hwpt;

typedef struct SMMUv3AccelDevice {
    SMMUDevice  sdev;
    AddressSpace as_sysmem;
    HostIOMMUDeviceIOMMUFD *idev;
    SMMUS1Hwpt  *s1_hwpt;
    SMMUViommu *viommu;
    IOMMUFDVdev  *vdev;
    QLIST_ENTRY(SMMUv3AccelDevice) next;
} SMMUv3AccelDevice;

typedef struct SMMUv3AccelState {
    MemoryRegion root;
    MemoryRegion sysmem;
    SMMUViommu *viommu;
    QemuThread event_thread_id;
    bool event_thread_stop;
    QemuMutex event_thread_mutex;

    struct iommu_hw_info_arm_smmuv3 info;
} SMMUv3AccelState;

#if defined(CONFIG_ARM_SMMUV3) && defined(CONFIG_IOMMUFD)
void smmuv3_accel_init(SMMUv3State *s);
void smmuv3_accel_install_nested_ste(SMMUState *bs, SMMUDevice *sdev, int sid);
void smmuv3_accel_install_nested_ste_range(SMMUState *bs,
                                           SMMUSIDRange *range);
bool smmuv3_accel_issue_cmd_batch(SMMUState *bs, SMMUCommandBatch *batch);
void smmuv3_accel_batch_cmd(SMMUState *bs, SMMUDevice *sdev,
                           SMMUCommandBatch *batch, struct Cmd *cmd,
                           uint32_t *cons);
void smmuv3_accel_init_regs(SMMUv3State *s);
void smmu_realloc_veventq(SMMUState *bs, uint32_t log2size);
#else
static inline void smmuv3_accel_init(SMMUv3State *d)
{
}
static inline void
smmuv3_accel_install_nested_ste(SMMUState *bs, SMMUDevice *sdev, int sid)
{
}
static inline void
smmuv3_accel_install_nested_ste_range(SMMUState *bs, SMMUSIDRange *range)
{
}
static inline bool smmuv3_accel_issue_cmd_batch(SMMUState *bs,
                                               SMMUCommandBatch *batch)
{
    return true;
}
static inline void smmuv3_accel_batch_cmd(SMMUState *bs, SMMUDevice *sdev,
                                          SMMUCommandBatch *batch,
                                          struct Cmd *cmd, uint32_t *cons)
{
    return;
}
static inline void smmuv3_accel_init_regs(SMMUv3State *s)
{
}
static void smmu_realloc_veventq(SMMUState *bs, uint32_t log2size)
{
}
#endif

#endif /* HW_ARM_SMMUV3_ACCEL_H */
