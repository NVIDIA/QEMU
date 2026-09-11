/*
 * Copyright (c) 2025 Oracle and/or its affiliates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "migration/cpr.h"
#include "migration/vmstate.h"
#include "system/iommufd.h"

const VMStateDescription vmstate_cpr_vfio_devices = {
    .name = CPR_STATE "/vfio devices",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Arm RME device assignment.  target/arm/kvm.c dispatches the RHI
 * device-assignment hypercalls unconditionally, but the implementations live
 * in iommufd.c which is only built for CONFIG_VFIO && CONFIG_IOMMUFD.  Without
 * IOMMUFD there can be no assigned device, so every lookup fails with -ENODEV
 * and the guest sees RHI_DA_ERROR_INVALID_VDEV_ID.
 */
int iommufd_vdevice_register(VFIODevice *vbasedev, Error **errp)
{
    error_setg(errp, "IOMMUFD support is not compiled in");
    return -ENOSYS;
}

int iommufd_tsm_bind(uint32_t rid)
{
    return -ENODEV;
}

int iommufd_tsm_unbind(uint32_t rid)
{
    return -ENODEV;
}

int iommufd_tsm_da_set_tdi_state_run(uint32_t rid)
{
    return -ENODEV;
}

int iommufd_tsm_get_da_object_size(uint32_t rid, uint32_t object_type,
                                   uint32_t *object_size)
{
    return -ENODEV;
}

int iommufd_tsm_da_object_read(uint32_t rid, uint32_t object_type,
                               uint64_t offset, void *buf, uint32_t max_len,
                               uint32_t *resp_len)
{
    return -ENODEV;
}

int iommufd_tsm_da_get_interface_report(uint32_t rid)
{
    return -ENODEV;
}

int iommufd_tsm_da_get_measurement(uint32_t rid,
                                   struct rhi_vdev_measurement_params *param)
{
    return -ENODEV;
}

bool iommufd_tsm_dev_memmap_exit(uint32_t rid, uint64_t gpa_base,
                                 uint64_t gpa_top, uint64_t pa_base)
{
    return false;
}
