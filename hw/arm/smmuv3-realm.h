/*
 * Copyright (C) 2025-2026 NVIDIA
 * Written by Nicolin Chen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SMMUV3_REALM_H
#define HW_ARM_SMMUV3_REALM_H

#include "hw/arm/smmuv3.h"
#include "hw/arm/smmu-common.h"
#include "system/host_iommu_device.h"
#include "system/iommufd.h"
#ifdef CONFIG_LINUX
#include <linux/iommufd.h>
#endif

#define VSMMU_IDR0_VALUE \
    ((1 << 26) | /* terminate with RAZ/WI */ \
     (1 << 24) | /* no stalls supported */ \
     (2 << 21) | /* little endian */ \
     (1 << 12) | /* 16-bit ASID support */ \
     (1 << 10) | /* ATS support */ \
     (1 << 4) |  /* I/O-coherent access to queues and TTEs */ \
     (2 << 2) |  /* VMSA64 translation table format */ \
     (1 << 1))   /* stage 1 support */

#define VSMMU_IDR1_VALUE \
    ((16 << 0) |  /* 16-bit SID */ \
     (20 << 6) |  /* 20-bit SSID */ \
     (19 << 16) | /* maximum event queue size */ \
     (19 << 21))  /* maximum command queue size */

#define VSMMU_IDR3_VALUE \
    ((1 << 2) |  /* hierarchical attribute disable */ \
     (1 << 10) | /* range invalidation */ \
     (2 << 11))  /* break-before-make level 2 */

#define VSMMU_IDR5_VALUE \
    ((5 << 0) | /* 48-bit output address */ \
     (1 << 4))  /* 4K granule */

#define VSMMU_AIDR_VALUE 0x2U /* SMMUv3.2 */

typedef struct SMMURealmViommu {
    IOMMUFDViommu core;
    /* Number of devices sharing a PSMMU-only vIOMMU */
    unsigned int users;
    /* Devices sharing a vIOMMU that describes a vSMMU */
    QLIST_HEAD(, SMMUv3RealmDevice) device_list;
} SMMURealmViommu;

typedef struct SMMUv3RealmDevice {
    SMMUDevice sdev;
    HostIOMMUDeviceIOMMUFD *hiodi;
    SMMURealmViommu *viommu;
    QLIST_ENTRY(SMMUv3RealmDevice) next;
} SMMUv3RealmDevice;

typedef struct SMMUv3RealmState {
    SMMURealmViommu *viommu;
} SMMUv3RealmState;

bool smmuv3_realm_init(SMMUv3State *s, Error **errp);
void smmuv3_realm_idr_override(SMMUv3State *s);

bool smmuv3_realm_alloc_psmmu_viommu(HostIOMMUDevice *hiod,
                                     uint32_t *viommu_id, Error **errp);
void smmuv3_realm_free_psmmu_viommu(HostIOMMUDevice *hiod);

#endif /* HW_ARM_SMMUV3_REALM_H */
