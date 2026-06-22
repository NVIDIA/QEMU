/*
 * iommufd container backend
 *
 * Copyright (C) 2023 Intel Corporation.
 * Copyright Red Hat, Inc. 2023
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *          Eric Auger <eric.auger@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <linux/iommufd.h>

#include "hw/vfio/vfio-device.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "qapi/error.h"
#include "system/iommufd.h"
#include "hw/core/qdev.h"
#include "system/kvm.h"
#include "hw/vfio/vfio-cpr.h"
#include "system/reset.h"
#include "qemu/cutils.h"
#include "qemu/chardev_open.h"
#include "migration/cpr.h"
#include "pci.h"
#include "vfio-iommufd.h"
#include "vfio-helpers.h"
#include "vfio-listener.h"
#include "linux-headers/linux/arm-smccc.h"
#include "linux-headers/linux/tsm.h"

#define TYPE_HOST_IOMMU_DEVICE_IOMMUFD_VFIO             \
            TYPE_HOST_IOMMU_DEVICE_IOMMUFD "-vfio"

/*
 * Arm SMMUv3 stream-table-entry bits used to build a stage-1 bypass STE for
 * the nested (VIOMMU) domain in the RME device-assignment flow.
 */
#define VFIO_STRTAB_STE_0_V          (1UL << 0)
#define VFIO_STRTAB_STE_0_CFG_BYPASS 4

int iommufd_tsm_bind(unsigned long vdev_id)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    struct iommu_vdevice_tsm_op tsm_op;

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
        return -ENODEV;
    }

    tsm_op.size = sizeof(struct iommu_vdevice_tsm_op);
    tsm_op.flags = 0;
    tsm_op.op = IOMMU_VDEVICE_TSM_BIND;
    tsm_op.vdevice_id = vbasedev->vdevice_id;

    if (ioctl(vbasedev->iommufd->fd, IOMMU_VDEVICE_TSM_OP, &tsm_op)) {
        warn_report("Failed to TSM bind vdevice: %d", errno);
        return -errno;
    }

    return 0;
}

int iommufd_tsm_unbind(unsigned long vdev_id)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    struct iommu_vdevice_tsm_op tsm_op;

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
        return -ENODEV;
    }

    tsm_op.size = sizeof(struct iommu_vdevice_tsm_op);
    tsm_op.flags = 0;
    tsm_op.op = IOMMU_VDEVICE_TSM_UNBIND;
    tsm_op.vdevice_id = vbasedev->vdevice_id;

    if (ioctl(vbasedev->iommufd->fd, IOMMU_VDEVICE_TSM_OP, &tsm_op)) {
        warn_report("Failed to TSM unbind vdevice: %d", errno);
        return -errno;
    }

    return 0;
}

static int iommufd_tsm_guest_request(VFIODevice *vbasedev,
                                     uint32_t vdevice_id, uint32_t scope,
                                     void *req, uint32_t req_len,
                                     void *resp, uint32_t resp_len,
                                     uint32_t *actual_resp_len)
{
    struct iommu_vdevice_tsm_guest_request guest_req = {
        .size = sizeof(guest_req),
        .vdevice_id = vdevice_id,
        .scope = scope,
        .req_uptr = (uintptr_t)req,
        .req_len = req_len,
        .resp_uptr = (uintptr_t)resp,
        .resp_len = resp_len,
    };
    int ret;

    ret = ioctl(vbasedev->iommufd->fd, IOMMU_VDEVICE_TSM_GUEST_REQUEST,
                &guest_req);
    if (ret < 0) {
        warn_report("IOMMU_VDEVICE_TSM_GUEST_REQUEST failed: %d", errno);
        return -errno;
    }

    /* return value is the residue */
    if (actual_resp_len) {
        *actual_resp_len = resp_len - ret;
    }

    return 0;
}

int iommufd_tsm_da_set_tdi_state_run(unsigned int vdev_id)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    struct arm64_vdev_set_tdi_state_guest_req req = {
        .req_type = __RHI_DA_VDEV_SET_TDI_STATE,
        .tdi_state = RHI_DA_TDI_CONFIG_RUN,
    };

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
        return -ENODEV;
    }

    return iommufd_tsm_guest_request(vbasedev, vbasedev->vdevice_id,
                                     PCI_TSM_REQ_STATE_CHANGE,
                                     &req, sizeof(req),
                                     NULL, 0, NULL);
}

int iommufd_tsm_get_da_object_size(unsigned int vdev_id,
       unsigned int object_type,
       unsigned int *object_size)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    struct arm64_vdev_object_size_guest_req req = {
        .req_type = __RHI_DA_OBJECT_SIZE,
        .object_type = object_type,
    };
    uint32_t resp_len = 0;
    int ret;

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
        return -ENODEV;
    }

    ret = iommufd_tsm_guest_request(vbasedev, vbasedev->vdevice_id,
                                    PCI_TSM_REQ_INFO,
                                    &req, sizeof(req),
                                    object_size, sizeof(*object_size),
                                    &resp_len);
    if (ret) {
        return ret;
    }
    if (resp_len != sizeof(int)) {
        return -EINVAL;
    }
    return 0;
}

int iommufd_tsm_da_object_read(unsigned int vdev_id,
       unsigned int object_type,
       unsigned long offset,
       void *buf,
       unsigned long max_len,
       unsigned int *resp_len)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    struct arm64_vdev_object_read_guest_req req = {
        .req_type = __RHI_DA_OBJECT_READ,
        .object_type = object_type,
        .offset = offset,
    };

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
        return -ENODEV;
    }

    return iommufd_tsm_guest_request(vbasedev, vbasedev->vdevice_id,
                                     PCI_TSM_REQ_INFO,
                                     &req, sizeof(req),
                                     buf, max_len, resp_len);
}

int iommufd_tsm_da_get_interface_report(unsigned int vdev_id)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    __u32 req_type;

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
        return -ENODEV;
    }

    req_type = __RHI_DA_VDEV_UPDATE_INTERFACE_REPORT;
    return iommufd_tsm_guest_request(vbasedev, vbasedev->vdevice_id,
                                     PCI_TSM_REQ_INFO,
                                     &req_type, sizeof(req_type),
                                     NULL, 0, NULL);
}

int iommufd_tsm_da_get_measurement(unsigned int vdev_id,
       struct rhi_vdev_measurement_params *param)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    struct arm64_vdev_device_measurement_guest_req req = {
        .req_type = __RHI_DA_VDEV_UPDATE_MEASUREMENTS,
        .flags = param->flags,
        .nonce = (uintptr_t)&param->nonce[0],
    };

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
        return -ENODEV;
    }

    return iommufd_tsm_guest_request(vbasedev, vbasedev->vdevice_id,
                                     PCI_TSM_REQ_INFO,
                                     &req, sizeof(req),
                                     NULL, 0, NULL);
}

bool iommufd_tsm_dev_memmap_exit(unsigned long vdev_id,
    unsigned long gpa_base, unsigned long gpa_top,
    unsigned long pa_base)
{
    VFIODevice *vbasedev = vfio_find_bdf(vdev_id);
    struct arm64_vdev_device_memmap_guest_req req = {
        .req_type = __REC_DA_VDEV_MAP,
        .gpa_base = gpa_base,
        .gpa_top = gpa_top,
        .pa_base = pa_base,
    };
    uint64_t range_size = gpa_top - gpa_base;
    bool ok;

    if (!vbasedev || !vbasedev->iommufd_vdevice) {
            return false;
    }

    /*
     * Mark the IPA window PRIVATE before the iommufd guest-request reaches
     * the host TSM/iommufd and the kernel installs ASSIGNED-DEV S2 entries
     * via realm_dev_mem_map(). This is the VDEV-side equivalent of the
     * RIPAS-change handshake used for RAM (rec_exit_ripas_change ->
     * KVM_EXIT_MEMORY_FAULT -> kvm_convert_memory) and gives the kernel's
     * realm_clamp_order() the neighbour signal it needs to refuse a 2 MiB
     * unprotected coalescing over VDEV-locked PAs.
     *
     * Set the attribute *before* the iommufd call: the
     * kvm_arch_post_set_memory_attributes() callback sweeps away any stale
     * NS S2 entries on those gfns (KVM_FILTER_SHARED) before the DEV
     * mapping lands, closing the race against other vCPUs faulting on the
     * unprotected alias in the window between VDEV_MAP exit and the host
     * actually installing the DEV S2.
     */
    if (kvm_set_memory_attributes_private(gpa_base, range_size)) {
        return false;
    }

    ok = iommufd_tsm_guest_request(vbasedev, vbasedev->vdevice_id,
                                   PCI_TSM_REQ_STATE_CHANGE,
                                   &req, sizeof(req),
                                   NULL, 0, NULL) == 0;
    if (!ok) {
        /*
         * Roll back the attribute change so the realm doesn't get stuck
         * with PRIVATE-locked gfns that have no backing DEV mapping.
         */
        kvm_set_memory_attributes_shared(gpa_base, range_size);
    }

    return ok;
}

static int iommufd_cdev_map(const VFIOContainer *bcontainer, hwaddr iova,
                            uint64_t size, void *vaddr, bool readonly,
                            MemoryRegion *mr)
{
    const VFIOIOMMUFDContainer *container = VFIO_IOMMU_IOMMUFD(bcontainer);

    return iommufd_backend_map_dma(container->be,
                                   container->ioas_id,
                                   iova, size, vaddr, readonly);
}

static int iommufd_cdev_map_file(const VFIOContainer *bcontainer,
                                 hwaddr iova, uint64_t size,
                                 int fd, unsigned long start, bool readonly)
{
    const VFIOIOMMUFDContainer *container = VFIO_IOMMU_IOMMUFD(bcontainer);

    return iommufd_backend_map_file_dma(container->be,
                                        container->ioas_id,
                                        iova, size, fd, start, readonly);
}

static int iommufd_cdev_unmap(const VFIOContainer *bcontainer,
                              hwaddr iova, uint64_t size,
                              IOMMUTLBEntry *iotlb, bool unmap_all)
{
    const VFIOIOMMUFDContainer *container = VFIO_IOMMU_IOMMUFD(bcontainer);
    IOMMUFDBackend *be = container->be;
    uint32_t ioas_id = container->ioas_id;
    bool need_dirty_sync = false;
    Error *local_err = NULL;
    int ret, unmap_ret;

    if (unmap_all) {
        size = UINT64_MAX;
    }

    if (iotlb && vfio_container_dirty_tracking_is_started(bcontainer)) {
        if (!vfio_container_devices_dirty_tracking_is_supported(bcontainer) &&
            bcontainer->dirty_pages_supported) {
            ret = vfio_container_query_dirty_bitmap(bcontainer, iova, size,
                                                    IOMMU_HWPT_GET_DIRTY_BITMAP_NO_CLEAR,
                                                    iotlb->translated_addr,
                                                    &local_err);
            if (ret) {
                error_report_err(local_err);
            }
            /* Unmap stale mapping even if query dirty bitmap fails */
            unmap_ret = iommufd_backend_unmap_dma(be, ioas_id, iova, size);

            /*
             * If dirty tracking fails, return the failure to VFIO core to
             * fail the migration, or else there will be dirty pages missed
             * to be migrated.
             */
            return unmap_ret ? : ret;
        }

        need_dirty_sync = true;
    }

    ret = iommufd_backend_unmap_dma(be, ioas_id, iova, size);
    if (ret) {
        return ret;
    }

    if (need_dirty_sync) {
        ret = vfio_container_query_dirty_bitmap(bcontainer, iova, size, 0,
                                                iotlb->translated_addr,
                                                &local_err);
        if (ret) {
            error_report_err(local_err);
            return ret;
        }
    }

    return 0;
}

static bool iommufd_cdev_kvm_device_add(VFIODevice *vbasedev, Error **errp)
{
    return !vfio_kvm_device_add_fd(vbasedev->fd, errp);
}

static void iommufd_cdev_kvm_device_del(VFIODevice *vbasedev)
{
    Error *err = NULL;

    if (vfio_kvm_device_del_fd(vbasedev->fd, &err)) {
        error_report_err(err);
    }
}

static bool iommufd_cdev_connect_and_bind(VFIODevice *vbasedev, Error **errp)
{
    IOMMUFDBackend *iommufd = vbasedev->iommufd;
    struct vfio_device_bind_iommufd bind = {
        .argsz = sizeof(bind),
        .flags = 0,
    };

    if (!iommufd_backend_connect(iommufd, errp)) {
        return false;
    }

    /*
     * Add device to kvm-vfio to be prepared for the tracking
     * in KVM. Especially for some emulated devices, it requires
     * to have kvm information in the device open.
     */
    if (!iommufd_cdev_kvm_device_add(vbasedev, errp)) {
        goto err_kvm_device_add;
    }

    if (cpr_is_incoming()) {
        goto skip_bind;
    }

    /* Bind device to iommufd */
    bind.iommufd = iommufd->fd;
    if (ioctl(vbasedev->fd, VFIO_DEVICE_BIND_IOMMUFD, &bind)) {
        error_setg_errno(errp, errno, "error bind device fd=%d to iommufd=%d",
                         vbasedev->fd, bind.iommufd);
        goto err_bind;
    }

    vbasedev->devid = bind.out_devid;
    trace_iommufd_cdev_connect_and_bind(bind.iommufd, vbasedev->name,
                                        vbasedev->fd, vbasedev->devid);

skip_bind:
    return true;
err_bind:
    iommufd_cdev_kvm_device_del(vbasedev);
err_kvm_device_add:
    iommufd_backend_disconnect(iommufd);
    return false;
}

static void iommufd_cdev_unbind_and_disconnect(VFIODevice *vbasedev)
{
    /* Unbind is automatically conducted when device fd is closed */
    iommufd_cdev_kvm_device_del(vbasedev);
    iommufd_backend_disconnect(vbasedev->iommufd);
}

static bool iommufd_hwpt_dirty_tracking(VFIOIOASHwpt *hwpt)
{
    return hwpt && hwpt->hwpt_flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING;
}

static int iommufd_set_dirty_page_tracking(const VFIOContainer *bcontainer,
                                           bool start, Error **errp)
{
    const VFIOIOMMUFDContainer *container = VFIO_IOMMU_IOMMUFD(bcontainer);
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        if (!iommufd_hwpt_dirty_tracking(hwpt)) {
            continue;
        }

        if (!iommufd_backend_set_dirty_tracking(container->be,
                                                hwpt->hwpt_id, start, errp)) {
            goto err;
        }
    }

    return 0;

err:
    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        if (!iommufd_hwpt_dirty_tracking(hwpt)) {
            continue;
        }
        iommufd_backend_set_dirty_tracking(container->be,
                                           hwpt->hwpt_id, !start, NULL);
    }
    return -EINVAL;
}

static int iommufd_query_dirty_bitmap(const VFIOContainer *bcontainer,
                                      VFIOBitmap *vbmap, hwaddr iova,
                                      hwaddr size, uint64_t backend_flag,
                                      Error **errp)
{
    VFIOIOMMUFDContainer *container = VFIO_IOMMU_IOMMUFD(bcontainer);
    unsigned long page_size = qemu_real_host_page_size();
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        if (!iommufd_hwpt_dirty_tracking(hwpt)) {
            continue;
        }

        if (!iommufd_backend_get_dirty_bitmap(container->be, hwpt->hwpt_id,
                                              iova, size, page_size,
                                              (uint64_t *)vbmap->bitmap,
                                              backend_flag, errp)) {
            return -EINVAL;
        }
    }

    return 0;
}

static int iommufd_cdev_getfd(const char *sysfs_path, Error **errp)
{
    ERRP_GUARD();
    long int ret = -ENOTTY;
    g_autofree char *path = NULL;
    g_autofree char *vfio_dev_path = NULL;
    g_autofree char *vfio_path = NULL;
    DIR *dir = NULL;
    struct dirent *dent;
    g_autofree gchar *contents = NULL;
    gsize length;
    int major, minor;
    dev_t vfio_devt;

    path = g_strdup_printf("%s/vfio-dev", sysfs_path);
    dir = opendir(path);
    if (!dir) {
        error_setg_errno(errp, errno, "couldn't open directory %s", path);
        goto out;
    }

    while ((dent = readdir(dir))) {
        if (!strncmp(dent->d_name, "vfio", 4)) {
            vfio_dev_path = g_strdup_printf("%s/%s/dev", path, dent->d_name);
            break;
        }
    }

    if (!vfio_dev_path) {
        error_setg(errp, "failed to find vfio-dev/vfioX/dev");
        goto out_close_dir;
    }

    if (!g_file_get_contents(vfio_dev_path, &contents, &length, NULL)) {
        error_setg(errp,
                   "failed to load \"%s\""
                   " (is your kernel config missing CONFIG_VFIO_DEVICE_CDEV?)",
                   vfio_dev_path);
        goto out_close_dir;
    }

    if (sscanf(contents, "%d:%d", &major, &minor) != 2) {
        error_setg(errp, "failed to get major:minor for \"%s\"", vfio_dev_path);
        goto out_close_dir;
    }
    vfio_devt = makedev(major, minor);

    vfio_path = g_strdup_printf("/dev/vfio/devices/%s", dent->d_name);
    ret = open_cdev(vfio_path, vfio_devt);
    if (ret < 0) {
        error_setg(errp, "Failed to open %s", vfio_path);
    }

    trace_iommufd_cdev_getfd(vfio_path, ret);

out_close_dir:
    closedir(dir);
out:
    if (*errp) {
        error_prepend(errp, VFIO_MSG_PREFIX, path);
    }

    return ret;
}

static int iommufd_cdev_attach_ioas_hwpt(VFIODevice *vbasedev, uint32_t id,
                                         Error **errp)
{
    int iommufd = vbasedev->iommufd->fd;
    struct vfio_device_attach_iommufd_pt attach_data = {
        .argsz = sizeof(attach_data),
        .flags = 0,
        .pt_id = id,
    };

    /* Attach device to an IOAS or hwpt within iommufd */
    if (ioctl(vbasedev->fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data)) {
        error_setg_errno(errp, errno,
                         "[iommufd=%d] error attach %s (%d) to id=%d",
                         iommufd, vbasedev->name, vbasedev->fd, id);
        return -errno;
    }

    trace_iommufd_cdev_attach_ioas_hwpt(iommufd, vbasedev->name,
                                        vbasedev->fd, id);
    return 0;
}

int iommufd_vdevice_register(VFIODevice *vbasedev, Error **errp)
{
    IOMMUFDBackend *iommufd = vbasedev->iommufd;
    struct iommu_vdevice_alloc alloc_vdev;
    VFIOPCIDevice *vdev;
    int ret;

    if (vbasedev->type != VFIO_DEVICE_TYPE_PCI) {
        error_setg(errp, "vdevice registration is only supported for PCI");
        return -EINVAL;
    }

    vdev = container_of(vbasedev, VFIOPCIDevice, vbasedev);

    alloc_vdev.size = sizeof(alloc_vdev);
    alloc_vdev.viommu_id = vbasedev->hwpt->viommu_id;
    alloc_vdev.dev_id = vbasedev->devid;
    /* Guest-visible RID: segment (0) in bits [31:16], BDF in bits [15:0]. */
    alloc_vdev.virt_id = pci_get_bdf(&vdev->parent_obj);

    if (ioctl(iommufd->fd, IOMMU_VDEVICE_ALLOC, &alloc_vdev)) {
        ret = -errno;
        error_setg_errno(errp, errno, "failed to allocate vdevice");
        return ret;
    }

    ret = iommufd_cdev_attach_ioas_hwpt(vbasedev,
                                        vbasedev->hwpt->nested_hwpt_id, errp);
    if (ret) {
        iommufd_backend_free_id(iommufd, alloc_vdev.out_vdevice_id);
        return ret;
    }

    vbasedev->vdevice_id = alloc_vdev.out_vdevice_id;

    return 0;
}

static bool iommufd_cdev_detach_ioas_hwpt(VFIODevice *vbasedev, Error **errp)
{
    int iommufd = vbasedev->iommufd->fd;
    struct vfio_device_detach_iommufd_pt detach_data = {
        .argsz = sizeof(detach_data),
        .flags = 0,
    };

    if (ioctl(vbasedev->fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &detach_data)) {
        error_setg_errno(errp, errno, "detach %s failed", vbasedev->name);
        return false;
    }

    trace_iommufd_cdev_detach_ioas_hwpt(iommufd, vbasedev->name);
    return true;
}

/*
 * Allocate the nested-translation topology used for RME device assignment: a
 * stage-2 nesting-parent HWPT, a VIOMMU on top of it, and a stage-1 bypass
 * HWPT nested under the VIOMMU. Returns the parent VFIOIOASHwpt (with
 * nested_hwpt_id populated) on success, or NULL with @errp set on failure.
 */
static VFIOIOASHwpt *
iommufd_cdev_alloc_viommu_hwpt(VFIODevice *vbasedev,
                               VFIOIOMMUFDContainer *container,
                               Error **errp)
{
    IOMMUFDBackend *iommufd = vbasedev->iommufd;
    struct iommu_hwpt_arm_smmuv3 bypass_ste = {
        .ste = {
            VFIO_STRTAB_STE_0_V | (VFIO_STRTAB_STE_0_CFG_BYPASS << 1),
            0,
        },
    };
    struct iommu_viommu_alloc alloc_viommu = {
        .size = sizeof(alloc_viommu),
        .flags = 0,
        .type = IOMMU_VIOMMU_TYPE_ARM_REALM_SMMUV3,
        .dev_id = vbasedev->devid,
    };
    VFIOIOASHwpt *hwpt;
    uint32_t hwpt_id;
    int ret;

    if (!iommufd_backend_alloc_hwpt(iommufd, vbasedev->devid,
                                    container->ioas_id,
                                    IOMMU_HWPT_ALLOC_NEST_PARENT,
                                    IOMMU_HWPT_DATA_NONE, 0, NULL,
                                    &hwpt_id, errp)) {
        return NULL;
    }

    hwpt = g_malloc0(sizeof(*hwpt));
    hwpt->hwpt_id = hwpt_id;
    hwpt->hwpt_flags = IOMMU_HWPT_ALLOC_NEST_PARENT;
    QLIST_INIT(&hwpt->device_list);

    ret = iommufd_cdev_attach_ioas_hwpt(vbasedev, hwpt->hwpt_id, errp);
    if (ret) {
        goto err_free;
    }

    alloc_viommu.hwpt_id = hwpt->hwpt_id;
    if (ioctl(iommufd->fd, IOMMU_VIOMMU_ALLOC, &alloc_viommu)) {
        error_setg_errno(errp, errno, "failed to allocate VIOMMU");
        goto err_free;
    }

    if (!iommufd_backend_alloc_hwpt(iommufd, vbasedev->devid,
                                    alloc_viommu.out_viommu_id, 0,
                                    IOMMU_HWPT_DATA_ARM_SMMUV3,
                                    sizeof(bypass_ste), &bypass_ste,
                                    &hwpt_id, errp)) {
        goto err_free;
    }

    hwpt->viommu_id = alloc_viommu.out_viommu_id;
    hwpt->nested_hwpt_id = hwpt_id;

    return hwpt;

err_free:
    iommufd_backend_free_id(container->be, hwpt->hwpt_id);
    g_free(hwpt);
    return NULL;
}

static bool iommufd_cdev_autodomains_get(VFIODevice *vbasedev,
                                         VFIOIOMMUFDContainer *container,
                                         Error **errp)
{
    ERRP_GUARD();
    IOMMUFDBackend *iommufd = vbasedev->iommufd;
    VFIOContainer *bcontainer = VFIO_IOMMU(container);
    bool viommu_nesting, viommu_nesting_dirty;
    uint32_t type = IOMMU_HW_INFO_TYPE_DEFAULT, flags = 0;
    uint64_t hw_caps;
    VendorCaps caps;
    VFIOIOASHwpt *hwpt;
    uint32_t hwpt_id;
    int ret;

    /* Try to find a domain */
    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        if (!cpr_is_incoming()) {
            ret = iommufd_cdev_attach_ioas_hwpt(vbasedev, hwpt->hwpt_id, errp);
        } else if (vbasedev->cpr.hwpt_id == hwpt->hwpt_id) {
            ret = 0;
        } else {
            continue;
        }

        if (ret) {
            /* -EINVAL means the domain is incompatible with the device. */
            if (ret == -EINVAL) {
                /*
                 * It is an expected failure and it just means we will try
                 * another domain, or create one if no existing compatible
                 * domain is found. Hence why the error is discarded below.
                 */
                error_free(*errp);
                *errp = NULL;
                continue;
            }

            return false;
        } else {
            vbasedev->hwpt = hwpt;
            vbasedev->cpr.hwpt_id = hwpt->hwpt_id;
            QLIST_INSERT_HEAD(&hwpt->device_list, vbasedev, hwpt_next);
            vbasedev->iommu_dirty_tracking = iommufd_hwpt_dirty_tracking(hwpt);
            return true;
        }
    }

    if (vbasedev->iommufd_vdevice) {
        hwpt = iommufd_cdev_alloc_viommu_hwpt(vbasedev, container, errp);
        if (!hwpt) {
            return false;
        }
    } else {
        /*
         * This is quite early and VFIO Migration state isn't yet fully
         * initialized, thus rely only on IOMMU hardware capabilities as to
         * whether IOMMU dirty tracking is going to be requested. Later
         * vfio_migration_realize() may decide to use VF dirty tracking
         * instead.
         */
        if (!iommufd_backend_get_device_info(vbasedev->iommufd,
                                             vbasedev->devid, &type, &caps,
                                             sizeof(caps), &hw_caps, NULL,
                                             errp)) {
            return false;
        }

        viommu_nesting = vfio_device_get_viommu_flags_want_nesting(vbasedev);
        viommu_nesting_dirty =
            vfio_device_get_viommu_flags_want_nesting_dirty(vbasedev);

        if (hw_caps & IOMMU_HW_CAP_DIRTY_TRACKING) {
            if (!viommu_nesting || viommu_nesting_dirty) {
                flags |= IOMMU_HWPT_ALLOC_DIRTY_TRACKING;
            }
        }

        /*
         * If vIOMMU requests VFIO's cooperation to create nesting parent HWPT,
         * force to create it so that it could be reused by vIOMMU to create
         * nested HWPT.
         */
        if (viommu_nesting) {
            flags |= IOMMU_HWPT_ALLOC_NEST_PARENT;

            if (vfio_device_get_host_iommu_quirk_bypass_ro(vbasedev, type,
                                                           &caps,
                                                           sizeof(caps))) {
                bcontainer->bypass_ro = true;
            }
        }

        if (cpr_is_incoming()) {
            hwpt_id = vbasedev->cpr.hwpt_id;
            goto skip_alloc;
        }

        if (!iommufd_backend_alloc_hwpt(iommufd, vbasedev->devid,
                                        container->ioas_id, flags,
                                        IOMMU_HWPT_DATA_NONE, 0, NULL,
                                        &hwpt_id, errp)) {
            return false;
        }

        ret = iommufd_cdev_attach_ioas_hwpt(vbasedev, hwpt_id, errp);
        if (ret) {
            iommufd_backend_free_id(container->be, hwpt_id);
            return false;
        }

skip_alloc:
        hwpt = g_malloc0(sizeof(*hwpt));
        hwpt->hwpt_id = hwpt_id;
        hwpt->hwpt_flags = flags;
        QLIST_INIT(&hwpt->device_list);
    }
    vbasedev->hwpt = hwpt;
    vbasedev->cpr.hwpt_id = hwpt->hwpt_id;
    vbasedev->iommu_dirty_tracking = iommufd_hwpt_dirty_tracking(hwpt);
    QLIST_INSERT_HEAD(&hwpt->device_list, vbasedev, hwpt_next);
    QLIST_INSERT_HEAD(&container->hwpt_list, hwpt, next);
    bcontainer->dirty_pages_supported |=
                                vbasedev->iommu_dirty_tracking;
    if (bcontainer->dirty_pages_supported &&
        !vbasedev->iommu_dirty_tracking) {
        warn_report("IOMMU instance for device %s doesn't support dirty tracking",
                    vbasedev->name);
    }
    return true;
}

static void iommufd_cdev_autodomains_put(VFIODevice *vbasedev,
                                         VFIOIOMMUFDContainer *container)
{
    VFIOIOASHwpt *hwpt = vbasedev->hwpt;

    QLIST_REMOVE(vbasedev, hwpt_next);
    vbasedev->hwpt = NULL;

    if (QLIST_EMPTY(&hwpt->device_list)) {
        QLIST_REMOVE(hwpt, next);
        /*
         * Tear down the RME device-assignment nested topology (if any) in the
         * reverse order it was allocated: stage-1 bypass HWPT, then the VIOMMU,
         * then the stage-2 nesting-parent HWPT below.
         */
        if (hwpt->nested_hwpt_id) {
            iommufd_backend_free_id(container->be, hwpt->nested_hwpt_id);
        }
        if (hwpt->viommu_id) {
            iommufd_backend_free_id(container->be, hwpt->viommu_id);
        }
        iommufd_backend_free_id(container->be, hwpt->hwpt_id);
        g_free(hwpt);
    }
}

static bool iommufd_cdev_attach_container(VFIODevice *vbasedev,
                                          VFIOIOMMUFDContainer *container,
                                          Error **errp)
{
    /* mdevs aren't physical devices and will fail with auto domains */
    if (!vbasedev->mdev) {
        return iommufd_cdev_autodomains_get(vbasedev, container, errp);
    }

    /* If CPR, we are already attached to ioas_id. */
    return cpr_is_incoming() ||
           !iommufd_cdev_attach_ioas_hwpt(vbasedev, container->ioas_id, errp);
}

static void iommufd_cdev_detach_container(VFIODevice *vbasedev,
                                          VFIOIOMMUFDContainer *container)
{
    Error *err = NULL;

    if (!iommufd_cdev_detach_ioas_hwpt(vbasedev, &err)) {
        error_report_err(err);
    }

    /*
     * Destroy the VDEVICE before the VIOMMU it belongs to (freed in
     * iommufd_cdev_autodomains_put() below), as required by the iommufd UAPI.
     */
    if (vbasedev->iommufd_vdevice && vbasedev->vdevice_id) {
        iommufd_backend_free_id(container->be, vbasedev->vdevice_id);
        vbasedev->vdevice_id = 0;
    }

    if (vbasedev->hwpt) {
        iommufd_cdev_autodomains_put(vbasedev, container);
    }
}

static void iommufd_cdev_container_destroy(VFIOIOMMUFDContainer *container)
{
    VFIOContainer *bcontainer = VFIO_IOMMU(container);

    if (!QLIST_EMPTY(&bcontainer->device_list)) {
        return;
    }
    vfio_iommufd_cpr_unregister_container(container);
    vfio_listener_unregister(bcontainer);
    iommufd_backend_free_id(container->be, container->ioas_id);
    object_unref(container);
}

static int iommufd_cdev_ram_block_discard_disable(bool state)
{
    /*
     * We support coordinated discarding of RAM via the RamDiscardManager.
     */
    return ram_block_uncoordinated_discard_disable(state);
}

static bool iommufd_cdev_get_info_iova_range(VFIOIOMMUFDContainer *container,
                                             uint32_t ioas_id, Error **errp)
{
    VFIOContainer *bcontainer = VFIO_IOMMU(container);
    g_autofree struct iommu_ioas_iova_ranges *info = NULL;
    struct iommu_iova_range *iova_ranges;
    int sz, fd = container->be->fd;

    info = g_malloc0(sizeof(*info));
    info->size = sizeof(*info);
    info->ioas_id = ioas_id;

    if (ioctl(fd, IOMMU_IOAS_IOVA_RANGES, info) && errno != EMSGSIZE) {
        goto error;
    }

    sz = info->num_iovas * sizeof(struct iommu_iova_range);
    info = g_realloc(info, sizeof(*info) + sz);
    info->allowed_iovas = (uintptr_t)(info + 1);

    if (ioctl(fd, IOMMU_IOAS_IOVA_RANGES, info)) {
        goto error;
    }

    iova_ranges = (struct iommu_iova_range *)(uintptr_t)info->allowed_iovas;

    for (int i = 0; i < info->num_iovas; i++) {
        Range *range = g_new(Range, 1);

        range_set_bounds(range, iova_ranges[i].start, iova_ranges[i].last);
        bcontainer->iova_ranges =
            range_list_insert(bcontainer->iova_ranges, range);
    }
    bcontainer->pgsizes = info->out_iova_alignment;

    return true;

error:
    error_setg_errno(errp, errno, "Cannot get IOVA ranges");
    return false;
}

static bool iommufd_cdev_attach(const char *name, VFIODevice *vbasedev,
                                AddressSpace *as, Error **errp)
{
    VFIOContainer *bcontainer;
    VFIOIOMMUFDContainer *container;
    VFIOAddressSpace *space;
    struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };
    int ret, devfd;
    bool res;
    uint32_t ioas_id;
    Error *err = NULL;
    const VFIOIOMMUClass *iommufd_vioc =
        VFIO_IOMMU_CLASS(object_class_by_name(TYPE_VFIO_IOMMU_IOMMUFD));

    vfio_cpr_load_device(vbasedev);

    if (vbasedev->fd < 0) {
        devfd = iommufd_cdev_getfd(vbasedev->sysfsdev, errp);
        if (devfd < 0) {
            return false;
        }
        vbasedev->fd = devfd;
    } else {
        devfd = vbasedev->fd;
    }

    if (!iommufd_cdev_connect_and_bind(vbasedev, errp)) {
        goto err_connect_bind;
    }

    space = vfio_address_space_get(as);

    /* try to attach to an existing container in this space */
    QLIST_FOREACH(bcontainer, &space->containers, next) {
        container = VFIO_IOMMU_IOMMUFD(bcontainer);
        if (VFIO_IOMMU_GET_CLASS(bcontainer) != iommufd_vioc ||
            vbasedev->iommufd != container->be) {
            continue;
        }

        if (!cpr_is_incoming() ||
            (vbasedev->cpr.ioas_id == container->ioas_id)) {
            res = iommufd_cdev_attach_container(vbasedev, container, &err);
        } else {
            continue;
        }

        if (!res) {
            const char *msg = error_get_pretty(err);

            trace_iommufd_cdev_fail_attach_existing_container(msg);
            error_free(err);
            err = NULL;
        } else {
            ret = iommufd_cdev_ram_block_discard_disable(true);
            if (ret) {
                error_setg_errno(errp, -ret,
                                 "Cannot set discarding of RAM broken");
                goto err_discard_disable;
            }
            goto found_container;
        }
    }

    if (cpr_is_incoming()) {
        ioas_id = vbasedev->cpr.ioas_id;
        goto skip_ioas_alloc;
    }

    /* Need to allocate a new dedicated container */
    if (!iommufd_backend_alloc_ioas(vbasedev->iommufd, &ioas_id, errp)) {
        goto err_alloc_ioas;
    }

    trace_iommufd_cdev_alloc_ioas(vbasedev->iommufd->fd, ioas_id);

skip_ioas_alloc:
    container = VFIO_IOMMU_IOMMUFD(object_new(TYPE_VFIO_IOMMU_IOMMUFD));
    container->be = vbasedev->iommufd;
    container->ioas_id = ioas_id;
    QLIST_INIT(&container->hwpt_list);

    bcontainer = VFIO_IOMMU(container);
    vfio_address_space_insert(space, bcontainer);

    if (!iommufd_cdev_attach_container(vbasedev, container, errp)) {
        goto err_attach_container;
    }

    ret = iommufd_cdev_ram_block_discard_disable(true);
    if (ret) {
        error_setg_errno(errp, -ret, "Cannot set discarding of RAM broken");
        goto err_discard_disable;
    }

    if (!iommufd_cdev_get_info_iova_range(container, ioas_id, &err)) {
        error_append_hint(&err,
                   "Fallback to default 64bit IOVA range and 4K page size\n");
        warn_report_err(err);
        err = NULL;
        bcontainer->pgsizes = qemu_real_host_page_size();
    }

    if (!vfio_listener_register(bcontainer, errp)) {
        goto err_listener_register;
    }

    if (!vfio_iommufd_cpr_register_container(container, errp)) {
        goto err_listener_register;
    }

    bcontainer->initialized = true;

found_container:
    vbasedev->cpr.ioas_id = container->ioas_id;

    ret = ioctl(devfd, VFIO_DEVICE_GET_INFO, &dev_info);
    if (ret) {
        error_setg_errno(errp, errno, "error getting device info");
        goto err_listener_register;
    }

    /*
     * Do not move this code before attachment! The nested IOMMU support
     * needs device and hwpt id which are generated only after attachment.
     */
    if (!vfio_device_hiod_create_and_realize(vbasedev,
                     TYPE_HOST_IOMMU_DEVICE_IOMMUFD_VFIO, errp)) {
        goto err_listener_register;
    }

    /*
     * TODO: examine RAM_BLOCK_DISCARD stuff, should we do group level
     * for discarding incompatibility check as well?
     */
    if (vbasedev->ram_block_discard_allowed) {
        iommufd_cdev_ram_block_discard_disable(false);
    }

    vfio_device_prepare(vbasedev, bcontainer, &dev_info);
    vfio_iommufd_cpr_register_device(vbasedev);

    trace_iommufd_cdev_device_info(vbasedev->name, devfd, vbasedev->num_irqs,
                                   vbasedev->num_initial_regions,
                                   vbasedev->flags);
    return true;

err_listener_register:
    iommufd_cdev_ram_block_discard_disable(false);
err_discard_disable:
    iommufd_cdev_detach_container(vbasedev, container);
err_attach_container:
    iommufd_cdev_container_destroy(container);
err_alloc_ioas:
    vfio_address_space_put(space);
    iommufd_cdev_unbind_and_disconnect(vbasedev);
err_connect_bind:
    close(vbasedev->fd);
    return false;
}

static void iommufd_cdev_detach(VFIODevice *vbasedev)
{
    VFIOContainer *bcontainer = vbasedev->bcontainer;
    VFIOAddressSpace *space = bcontainer->space;
    VFIOIOMMUFDContainer *container = VFIO_IOMMU_IOMMUFD(bcontainer);

    vfio_device_unprepare(vbasedev);

    if (!vbasedev->ram_block_discard_allowed) {
        iommufd_cdev_ram_block_discard_disable(false);
    }

    object_unref(vbasedev->hiod);
    iommufd_cdev_detach_container(vbasedev, container);
    iommufd_cdev_container_destroy(container);
    vfio_address_space_put(space);

    vfio_iommufd_cpr_unregister_device(vbasedev);
    iommufd_cdev_unbind_and_disconnect(vbasedev);
    close(vbasedev->fd);
}

static VFIODevice *iommufd_cdev_pci_find_by_devid(__u32 devid)
{
    VFIODevice *vbasedev_iter;
    const VFIOIOMMUClass *iommufd_vioc =
        VFIO_IOMMU_CLASS(object_class_by_name(TYPE_VFIO_IOMMU_IOMMUFD));

    QLIST_FOREACH(vbasedev_iter, &vfio_device_list, global_next) {
        if (VFIO_IOMMU_GET_CLASS(vbasedev_iter->bcontainer) != iommufd_vioc) {
            continue;
        }
        if (devid == vbasedev_iter->devid) {
            return vbasedev_iter;
        }
    }
    return NULL;
}

static VFIOPCIDevice *
iommufd_cdev_dep_get_realized_vpdev(struct vfio_pci_dependent_device *dep_dev,
                                    VFIODevice *reset_dev)
{
    VFIODevice *vbasedev_tmp;

    if (dep_dev->devid == reset_dev->devid ||
        dep_dev->devid == VFIO_PCI_DEVID_OWNED) {
        return NULL;
    }

    vbasedev_tmp = iommufd_cdev_pci_find_by_devid(dep_dev->devid);
    if (!vfio_pci_from_vfio_device(vbasedev_tmp) ||
        !vbasedev_tmp->dev->realized) {
        return NULL;
    }

    return container_of(vbasedev_tmp, VFIOPCIDevice, vbasedev);
}

static int iommufd_cdev_pci_hot_reset(VFIODevice *vbasedev, bool single)
{
    VFIOPCIDevice *vdev = container_of(vbasedev, VFIOPCIDevice, vbasedev);
    struct vfio_pci_hot_reset_info *info = NULL;
    struct vfio_pci_dependent_device *devices;
    struct vfio_pci_hot_reset *reset;
    int ret, i;
    bool multi = false;

    trace_vfio_pci_hot_reset(vdev->vbasedev.name, single ? "one" : "multi");

    if (!single) {
        vfio_pci_pre_reset(vdev);
    }
    vdev->vbasedev.needs_reset = false;

    ret = vfio_pci_get_pci_hot_reset_info(vdev, &info);

    if (ret) {
        goto out_single;
    }

    assert(info->flags & VFIO_PCI_HOT_RESET_FLAG_DEV_ID);

    devices = &info->devices[0];

    if (!(info->flags & VFIO_PCI_HOT_RESET_FLAG_DEV_ID_OWNED)) {
        if (!vdev->has_pm_reset) {
            for (i = 0; i < info->count; i++) {
                if (devices[i].devid == VFIO_PCI_DEVID_NOT_OWNED) {
                    error_report("vfio: Cannot reset device %s, "
                                 "depends on device %04x:%02x:%02x.%x "
                                 "which is not owned.",
                                 vdev->vbasedev.name, devices[i].segment,
                                 devices[i].bus, PCI_SLOT(devices[i].devfn),
                                 PCI_FUNC(devices[i].devfn));
                }
            }
        }
        ret = -EPERM;
        goto out_single;
    }

    trace_vfio_pci_hot_reset_has_dep_devices(vdev->vbasedev.name);

    for (i = 0; i < info->count; i++) {
        VFIOPCIDevice *tmp;

        trace_iommufd_cdev_pci_hot_reset_dep_devices(devices[i].segment,
                                                     devices[i].bus,
                                                     PCI_SLOT(devices[i].devfn),
                                                     PCI_FUNC(devices[i].devfn),
                                                     devices[i].devid);

        /*
         * If a VFIO cdev device is resettable, all the dependent devices
         * are either bound to same iommufd or within same iommu_groups as
         * one of the iommufd bound devices.
         */
        assert(devices[i].devid != VFIO_PCI_DEVID_NOT_OWNED);

        tmp = iommufd_cdev_dep_get_realized_vpdev(&devices[i], &vdev->vbasedev);
        if (!tmp) {
            continue;
        }

        if (single) {
            ret = -EINVAL;
            goto out_single;
        }
        vfio_pci_pre_reset(tmp);
        tmp->vbasedev.needs_reset = false;
        multi = true;
    }

    if (!single && !multi) {
        ret = -EINVAL;
        goto out_single;
    }

    /* Use zero length array for hot reset with iommufd backend */
    reset = g_malloc0(sizeof(*reset));
    reset->argsz = sizeof(*reset);

     /* Bus reset! */
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_PCI_HOT_RESET, reset);
    g_free(reset);
    if (ret) {
        ret = -errno;
    }

    trace_vfio_pci_hot_reset_result(vdev->vbasedev.name,
                                    ret ? strerror(errno) : "Success");

    /* Re-enable INTx on affected devices */
    for (i = 0; i < info->count; i++) {
        VFIOPCIDevice *tmp;

        tmp = iommufd_cdev_dep_get_realized_vpdev(&devices[i], &vdev->vbasedev);
        if (!tmp) {
            continue;
        }
        vfio_pci_post_reset(tmp);
    }
out_single:
    if (!single) {
        vfio_pci_post_reset(vdev);
    }
    g_free(info);

    return ret;
}

static void vfio_iommu_iommufd_class_init(ObjectClass *klass, const void *data)
{
    VFIOIOMMUClass *vioc = VFIO_IOMMU_CLASS(klass);

    vioc->dma_map = iommufd_cdev_map;
    vioc->dma_map_file = iommufd_cdev_map_file;
    vioc->dma_unmap = iommufd_cdev_unmap;
    vioc->attach_device = iommufd_cdev_attach;
    vioc->detach_device = iommufd_cdev_detach;
    vioc->pci_hot_reset = iommufd_cdev_pci_hot_reset;
    vioc->set_dirty_page_tracking = iommufd_set_dirty_page_tracking;
    vioc->query_dirty_bitmap = iommufd_query_dirty_bitmap;
};

static bool
host_iommu_device_iommufd_vfio_attach_hwpt(HostIOMMUDeviceIOMMUFD *hiodi,
                                           uint32_t hwpt_id, Error **errp)
{
    VFIODevice *vbasedev = HOST_IOMMU_DEVICE(hiodi)->agent;

    return !iommufd_cdev_attach_ioas_hwpt(vbasedev, hwpt_id, errp);
}

static bool
host_iommu_device_iommufd_vfio_detach_hwpt(HostIOMMUDeviceIOMMUFD *hiodi,
                                           Error **errp)
{
    VFIODevice *vbasedev = HOST_IOMMU_DEVICE(hiodi)->agent;

    return iommufd_cdev_detach_ioas_hwpt(vbasedev, errp);
}

static bool hiod_iommufd_vfio_realize(HostIOMMUDevice *hiod, void *opaque,
                                      Error **errp)
{
    VFIODevice *vdev = opaque;
    HostIOMMUDeviceIOMMUFD *hiodi;
    HostIOMMUDeviceCaps *caps = &hiod->caps;
    VendorCaps *vendor_caps = &caps->vendor_caps;
    uint32_t type = IOMMU_HW_INFO_TYPE_DEFAULT;
    uint8_t max_pasid_log2;
    uint64_t hw_caps;

    hiod->agent = opaque;

    if (!iommufd_backend_get_device_info(vdev->iommufd, vdev->devid, &type,
                                         vendor_caps, sizeof(*vendor_caps),
                                         &hw_caps, &max_pasid_log2, errp)) {
        return false;
    }

    hiod->name = g_strdup(vdev->name);
    caps->type = type;
    caps->hw_caps = hw_caps;
    caps->max_pasid_log2 = max_pasid_log2;

    hiodi = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    hiodi->iommufd = vdev->iommufd;
    hiodi->devid = vdev->devid;
    hiodi->hwpt_id = vdev->hwpt->hwpt_id;

    return true;
}

static GList *
hiod_iommufd_vfio_get_iova_ranges(HostIOMMUDevice *hiod)
{
    VFIODevice *vdev = hiod->agent;

    g_assert(vdev);
    return vfio_container_get_iova_ranges(vdev->bcontainer);
}

static uint64_t
hiod_iommufd_vfio_get_page_size_mask(HostIOMMUDevice *hiod)
{
    VFIODevice *vdev = hiod->agent;

    g_assert(vdev);
    return vfio_container_get_page_size_mask(vdev->bcontainer);
}


static void hiod_iommufd_vfio_class_init(ObjectClass *oc, const void *data)
{
    HostIOMMUDeviceClass *hiodc = HOST_IOMMU_DEVICE_CLASS(oc);
    HostIOMMUDeviceIOMMUFDClass *hiodic = HOST_IOMMU_DEVICE_IOMMUFD_CLASS(oc);

    hiodc->realize = hiod_iommufd_vfio_realize;
    hiodc->get_iova_ranges = hiod_iommufd_vfio_get_iova_ranges;
    hiodc->get_page_size_mask = hiod_iommufd_vfio_get_page_size_mask;

    hiodic->attach_hwpt = host_iommu_device_iommufd_vfio_attach_hwpt;
    hiodic->detach_hwpt = host_iommu_device_iommufd_vfio_detach_hwpt;
};

static const TypeInfo types[] = {
    {
        .name = TYPE_VFIO_IOMMU_IOMMUFD,
        .parent = TYPE_VFIO_IOMMU,
        .instance_size = sizeof(VFIOIOMMUFDContainer),
        .class_init = vfio_iommu_iommufd_class_init,
    }, {
        .name = TYPE_HOST_IOMMU_DEVICE_IOMMUFD_VFIO,
        .parent = TYPE_HOST_IOMMU_DEVICE_IOMMUFD,
        .class_init = hiod_iommufd_vfio_class_init,
    }
};

DEFINE_TYPES(types)
