/*
 * Copyright (C) 2025-2026 NVIDIA
 * Written by Nicolin Chen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SMMUV3_REALM_H
#define HW_ARM_SMMUV3_REALM_H

#include "system/host_iommu_device.h"
#include "system/iommufd.h"
#ifdef CONFIG_LINUX
#include <linux/iommufd.h>
#endif

typedef struct SMMURealmViommu {
    IOMMUFDViommu core;
    /* Number of devices sharing a PSMMU-only vIOMMU */
    unsigned int users;
} SMMURealmViommu;

bool smmuv3_realm_alloc_psmmu_viommu(HostIOMMUDevice *hiod,
                                     uint32_t *viommu_id, Error **errp);
void smmuv3_realm_free_psmmu_viommu(HostIOMMUDevice *hiod);

#endif /* HW_ARM_SMMUV3_REALM_H */
