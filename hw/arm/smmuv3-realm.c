/*
 * Copyright (C) 2025-2026 NVIDIA
 * Written by Nicolin Chen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"

#include "smmuv3-realm.h"

#define SMMU_STE_VALID      (1ULL << 0)
#define SMMU_STE_CFG_BYPASS (1ULL << 3)

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
