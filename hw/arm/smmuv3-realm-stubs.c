/*
 * Stubs for Realm SMMUv3 backed by an iommufd vIOMMU object.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/smmuv3-realm.h"

bool smmuv3_realm_init(SMMUv3State *s, Error **errp)
{
    error_setg(errp, "realm=on support not compiled in");
    return false;
}

void smmuv3_realm_idr_override(SMMUv3State *s)
{
}

bool smmuv3_realm_alloc_psmmu_viommu(HostIOMMUDevice *hiod,
                                     uint32_t *viommu_id, Error **errp)
{
    error_setg(errp, "Realm vIOMMU support not compiled in");
    return false;
}

void smmuv3_realm_free_psmmu_viommu(HostIOMMUDevice *hiod)
{
}
