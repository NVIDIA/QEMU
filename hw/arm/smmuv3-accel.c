/*
 * Copyright (c) 2025 Huawei Technologies R & D (UK) Ltd
 * Copyright (C) 2025 NVIDIA
 * Written by Nicolin Chen, Shameer Kolothum
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "qemu/error-report.h"
#include <poll.h>

#include "hw/arm/smmuv3.h"
#include "hw/iommu.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-host/gpex.h"
#include "hw/vfio/pci.h"

#include "smmuv3-accel.h"

#include "smmuv3-internal.h"

#define SMMU_STE_VALID      (1ULL << 0)
#define SMMU_STE_CFG_BYPASS (1ULL << 3)

static int
smmuv3_accel_host_hw_info(SMMUv3AccelDevice *accel_dev, uint32_t *data_type,
                          uint32_t data_len, void *data)
{
    uint64_t caps;

    if (!accel_dev || !accel_dev->idev) {
        return -ENOENT;
    }

    return !iommufd_backend_get_device_info(accel_dev->idev->iommufd,
                                            accel_dev->idev->devid,
                                            data_type, data,
                                            data_len, &caps, NULL);
}

void smmuv3_accel_init_regs(SMMUv3State *s)
{
    SMMUv3AccelState *s_accel = s->s_accel;
    SMMUv3AccelDevice *accel_dev;
    uint32_t data_type = 0;
    uint32_t val;
    int ret;

    if (s_accel->info.idr[0]) {
        /* We already got this */
        goto init_regs;
    }

    if (!s_accel->viommu || QLIST_EMPTY(&s_accel->viommu->device_list)) {
        error_report("For arm-smmuv3,accel=on case, atleast one cold-plugged "
                     "vfio-pci dev needs to be assigned");
        goto out_err;
    }

    accel_dev = QLIST_FIRST(&s_accel->viommu->device_list);
    ret = smmuv3_accel_host_hw_info(accel_dev, &data_type,
                                    sizeof(s_accel->info), &s_accel->info);
    if (ret) {
        error_report("Failed to get Host SMMU device info");
        goto out_err;
    }

    if (data_type != IOMMU_HW_INFO_TYPE_ARM_SMMUV3) {
        error_report("Wrong data type (%d) for Host SMMU device info",
                     data_type);
        goto out_err;
    }

    if (s_accel->cmdqv) {
        data_type = IOMMU_HW_INFO_TYPE_TEGRA241_CMDQV;
        ret = smmuv3_accel_host_hw_info(accel_dev, &data_type,
                                        sizeof(s_accel->cmdqv_info),
                                        &s_accel->cmdqv_info);
        if (ret) {
            error_report("Failed to get Host CMDQV device info");
            goto out_err;
        }

        if (data_type != IOMMU_HW_INFO_TYPE_TEGRA241_CMDQV) {
            error_report("Wrong data type (%d) for Host CMDQV device info",
                         data_type);
            goto out_err;
        }
    }

init_regs:
    trace_smmuv3_accel_host_hw_info(s_accel->info.idr[0], s_accel->info.idr[1],
                                    s_accel->info.idr[3], s_accel->info.idr[5]);
    /*
     * QEMU SMMUv3 supports both linear and 2-level stream tables. If host
     * SMMUv3 supports only linear stream table, report that to Guest.
     */
    val = FIELD_EX32(s_accel->info.idr[0], IDR0, STLEVEL);
    if (val < FIELD_EX32(s->idr[0], IDR0, STLEVEL)) {
        s->idr[0] = FIELD_DP32(s->idr[0], IDR0, STLEVEL, val);
    }

    /*
     * QEMU SMMUv3 supports little-endian support for translation table walks.
     * If host SMMUv3 supports only big-endian, report error.
     */
    val = FIELD_EX32(s_accel->info.idr[0], IDR0, TTENDIAN);
    if (val > FIELD_EX32(s->idr[0], IDR0, TTENDIAN)) {
        error_report("Host SUUMU device translation table walk endianess "
                     "not supported");
        goto out_err;
    }

    /*
     * QEMU SMMUv3 supports AArch64 Translation table format.
     * If host SMMUv3 supports only AArch32, report error.
     */
    val = FIELD_EX32(s_accel->info.idr[0], IDR0, TTF);
    if (val < FIELD_EX32(s->idr[0], IDR0, TTF)) {
        error_report("Host SMMU device Translation table format not supported");
        goto out_err;
    }

    val = FIELD_EX32(s_accel->info.idr[0], IDR0, STALL_MODEL);
    s->idr[0] = FIELD_DP32(s->idr[0], IDR0, STALL_MODEL, val);

    val = FIELD_EX32(s_accel->info.idr[0], IDR0, ATS);
    s->idr[0] = FIELD_DP32(s->idr[0], IDR0, ATS, val);

    val = FIELD_EX32(s_accel->info.idr[1], IDR1, SIDSIZE);
    s->idr[1] = FIELD_DP32(s->idr[1], IDR1, SIDSIZE, val);

    val = FIELD_EX32(s_accel->info.idr[1], IDR1, SSIDSIZE);
    s->idr[1] = FIELD_DP32(s->idr[1], IDR1, SSIDSIZE, val);

    val = FIELD_EX32(s_accel->info.idr[3], IDR3, HAD);
    s->idr[3] = FIELD_DP32(s->idr[3], IDR3, HAD, val);

    val = FIELD_EX32(s_accel->info.idr[3], IDR3, RIL);
    s->idr[3] = FIELD_DP32(s->idr[3], IDR3, RIL, val);

    val = FIELD_EX32(s_accel->info.idr[3], IDR3, BBML);
    s->idr[3] = FIELD_DP32(s->idr[3], IDR3, BBML, val);

    /*
     * QEMU SMMUv3 supports 4K/16K/64K translation granules. If host SMMUv3
     * does't support any of these, report the supported ones only to Guest.
     */
    val = FIELD_EX32(s_accel->info.idr[5], IDR5, GRAN4K);
    s->idr[5] = FIELD_DP32(s->idr[5], IDR5, GRAN4K, val);
    val = FIELD_EX32(s_accel->info.idr[5], IDR5, GRAN16K);
    s->idr[5] = FIELD_DP32(s->idr[5], IDR5, GRAN16K, val);
    val = FIELD_EX32(s_accel->info.idr[5], IDR5, GRAN64K);
    s->idr[5] = FIELD_DP32(s->idr[5], IDR5, GRAN64K, val);
    val = FIELD_EX32(s_accel->info.idr[5], IDR5, OAS);
    s->idr[5] = FIELD_DP32(s->idr[5], IDR5, OAS, val);
    return;

out_err:
    exit(1);
}

static void
smmuv3_accel_dev_uninstall_nested_ste(SMMUv3AccelDevice *accel_dev, bool abort)
{
    HostIOMMUDeviceIOMMUFD *idev = accel_dev->idev;
    SMMUS1Hwpt *s1_hwpt = accel_dev->s1_hwpt;
    uint32_t hwpt_id;

    if (!s1_hwpt || !accel_dev->viommu) {
        return;
    }

    if (abort) {
        hwpt_id = accel_dev->viommu->abort_hwpt_id;
    } else {
        hwpt_id = accel_dev->viommu->bypass_hwpt_id;
    }

    host_iommu_device_iommufd_attach_hwpt(idev, hwpt_id, &error_abort);
    iommufd_backend_free_id(s1_hwpt->iommufd, s1_hwpt->hwpt_id);
    accel_dev->s1_hwpt = NULL;
    g_free(s1_hwpt);
}

static int
smmuv3_accel_dev_install_nested_ste(SMMUv3AccelDevice *accel_dev,
                                    uint32_t data_type, uint32_t data_len,
                                    void *data)
{
    SMMUViommu *viommu = accel_dev->viommu;
    SMMUS1Hwpt *s1_hwpt = accel_dev->s1_hwpt;
    HostIOMMUDeviceIOMMUFD *idev = accel_dev->idev;
    uint32_t flags = 0;

    if (!idev || !viommu) {
        return -ENOENT;
    }

    if (s1_hwpt) {
        smmuv3_accel_dev_uninstall_nested_ste(accel_dev, true);
    }

    s1_hwpt = g_new0(SMMUS1Hwpt, 1);
    s1_hwpt->iommufd = idev->iommufd;
    iommufd_backend_alloc_hwpt(idev->iommufd, idev->devid,
                               viommu->core.viommu_id, flags, data_type,
                               data_len, data, &s1_hwpt->hwpt_id, &error_abort);
    host_iommu_device_iommufd_attach_hwpt(idev, s1_hwpt->hwpt_id, &error_abort);
    accel_dev->s1_hwpt = s1_hwpt;
    return 0;
}

void smmuv3_accel_install_nested_ste(SMMUState *bs, SMMUDevice *sdev, int sid)
{
    SMMUv3AccelDevice *accel_dev;
    SMMUEventInfo event = {.type = SMMU_EVT_NONE, .sid = sid,
                           .inval_ste_allowed = true};
    struct iommu_hwpt_arm_smmuv3 nested_data = {};
    uint32_t config;
    STE ste;
    int ret;

    if (!bs->accel) {
        return;
    }

    accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);
    if (!accel_dev->viommu) {
        return;
    }

    if (!accel_dev->vdev && accel_dev->idev) {
        IOMMUFDVdev *vdev;
        uint32_t vdev_id;
        SMMUViommu *viommu = accel_dev->viommu;

        iommufd_backend_alloc_vdev(viommu->core.iommufd, accel_dev->idev->devid,
                                   viommu->core.viommu_id, sid, &vdev_id,
                                   &error_abort);
        vdev = g_new(IOMMUFDVdev, 1);
        vdev->vdev_id = vdev_id;
        vdev->dev_id = sid;
        accel_dev->vdev = vdev;
        host_iommu_device_iommufd_attach_hwpt(accel_dev->idev,
                                              accel_dev->viommu->bypass_hwpt_id,
                                              &error_abort);
    }

    ret = smmu_find_ste(sdev->smmu, sid, &ste, &event);
    if (ret) {
        error_report("failed to find STE for sid 0x%x", sid);
        return;
    }

    config = STE_CONFIG(&ste);
    if (!STE_VALID(&ste) || !STE_CFG_S1_ENABLED(config)) {
        smmuv3_accel_dev_uninstall_nested_ste(accel_dev, STE_CFG_ABORT(config));
        smmuv3_flush_config(sdev);
        return;
    }

    nested_data.ste[0] = (uint64_t)ste.word[0] | (uint64_t)ste.word[1] << 32;
    nested_data.ste[1] = (uint64_t)ste.word[2] | (uint64_t)ste.word[3] << 32;
    /* V | CONFIG | S1FMT | S1CTXPTR | S1CDMAX */
    nested_data.ste[0] &= 0xf80fffffffffffffULL;
    /* S1DSS | S1CIR | S1COR | S1CSH | S1STALLD | EATS */
    nested_data.ste[1] &= 0x380000ffULL;
    ret = smmuv3_accel_dev_install_nested_ste(accel_dev,
                                              IOMMU_HWPT_DATA_ARM_SMMUV3,
                                              sizeof(nested_data),
                                              &nested_data);
    if (ret) {
        error_report("Unable to install nested STE=%16LX:%16LX, sid=0x%x,"
                      "ret=%d", nested_data.ste[1], nested_data.ste[0],
                      sid, ret);
    }

    trace_smmuv3_accel_install_nested_ste(sid, nested_data.ste[1],
                                          nested_data.ste[0]);
}

static void
smmuv3_accel_ste_range(SMMUDevice *sdev, SMMUSIDRange *sid_range)
{
    uint32_t sid = smmu_get_sid(sdev);

    if (sid >= sid_range->start && sid <= sid_range->end) {
        SMMUv3State *s = sdev->smmu;
        SMMUState *bs = &s->smmu_state;

        smmuv3_accel_install_nested_ste(bs, sdev, sid);
    }
}

void
smmuv3_accel_install_nested_ste_range(SMMUState *bs, SMMUSIDRange *sid_range)
{
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    SMMUv3AccelDevice *accel_dev;

    if (!bs->accel) {
        return;
    }

    QLIST_FOREACH(accel_dev, &s_accel->viommu->device_list, next) {
        if (smmu_get_sid(&accel_dev->sdev)) {
            smmuv3_accel_ste_range(&accel_dev->sdev, sid_range);
        }
    }
}

/* Update batch->ncmds to the number of execute cmds */
bool smmuv3_accel_issue_cmd_batch(SMMUState *bs, SMMUCommandBatch *batch)
{
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    uint32_t total = batch->ncmds;
    IOMMUFDViommu *viommu_core;
    int ret;

    if (!bs->accel) {
        return true;
    }

    if (!s_accel->viommu) {
        return true;
    }

    viommu_core = &s_accel->viommu->core;
    ret = iommufd_backend_invalidate_cache(viommu_core->iommufd,
                                           viommu_core->viommu_id,
                                           IOMMU_VIOMMU_INVALIDATE_DATA_ARM_SMMUV3,
                                           sizeof(Cmd), &batch->ncmds,
                                           batch->cmds, NULL);
    if (!ret || total != batch->ncmds) {
        error_report("%s failed: ret=%d, total=%d, done=%d",
                      __func__, ret, total, batch->ncmds);
        return ret;
    }

    batch->ncmds = 0;
    return ret;
}

/*
 * Note: sdev can be NULL for certain invalidation commands
 * e.g., SMMU_CMD_TLBI_NH_ASID, SMMU_CMD_TLBI_NH_VA etc.
 */
void smmuv3_accel_batch_cmd(SMMUState *bs, SMMUDevice *sdev,
                           SMMUCommandBatch *batch, Cmd *cmd,
                           uint32_t *cons)
{
    if (!bs->accel) {
        return;
    }

   /*
    * We may end up here for any emulated PCI bridge or root port type
    * devices. The batching of commands only matters for vfio-pci endpoint
    * devices with Guest S1 translation enabled. Hence check that, if
    * sdev is available.
    */
    if (sdev) {
        SMMUv3AccelDevice *accel_dev;
        accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);

        if (!accel_dev->s1_hwpt) {
            return;
        }
    }

    batch->cmds[batch->ncmds] = *cmd;
    batch->cons[batch->ncmds++] = *cons;
    return;
}

static void *smmuv3_nested_event_thread(void *arg)
{
    struct iommu_vevent_arm_smmuv3 *vevent;
    struct iommufd_vevent_header *hdr;
    ssize_t readsz = sizeof(*hdr) + sizeof(*vevent);
    struct pollfd pollfd = {};
    SMMUViommu *viommu = arg;
    SMMUState *bs = viommu->smmu;
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    MemTxResult r;
    ssize_t bytes;
    Evt evt = {};
    void *buf;
    int ret;

    if (!viommu->veventq) {
        return NULL;
    }
    buf = g_malloc0(readsz);
    pollfd.events = POLLIN;
    pollfd.fd = viommu->veventq->veventq_fd;

    while (1) {
        qemu_mutex_lock(&s_accel->event_thread_mutex);
        if (s_accel->event_thread_stop) {
            qemu_mutex_unlock(&s_accel->event_thread_mutex);
            break;
        }
        qemu_mutex_unlock(&s_accel->event_thread_mutex);

        ret = poll(&pollfd, 1, 100);
        if (ret < 0) {
            error_report("%s: poll failed: %d", __func__, ret);
            goto out_free;
        }

        bytes = read(pollfd.fd, buf, readsz);
        if (bytes == 0) {
            continue;
        }
        if (bytes < 0) {
            error_report("%s: read failed: %d", __func__, ret);
            goto out_free;
        }
        hdr = buf;
        vevent = buf + sizeof(*hdr);
        if (hdr->flags & IOMMU_VEVENTQ_FLAG_LOST_EVENTS) {
            error_report("%s: vEVENTQ has lost events", __func__);
            goto out_free;
        }

        memcpy(&evt, vevent, sizeof(evt));
        r = smmuv3_write_eventq(s, &evt);
        if (r != MEMTX_OK) {
            smmuv3_trigger_irq(s, SMMU_IRQ_GERROR,
                               R_GERROR_EVENTQ_ABT_ERR_MASK);
        }
    }
out_free:
    g_free(buf);
    close(pollfd.fd);
    return NULL;
}

void smmu_realloc_veventq(SMMUState *bs, uint32_t log2size)
{
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    SMMUViommu *viommu = s_accel->viommu;

    if (!viommu)
        return;
    if (viommu->veventq) {
        qemu_mutex_lock(&s_accel->event_thread_mutex);
        s_accel->event_thread_stop = true;
        qemu_mutex_unlock(&s_accel->event_thread_mutex);
        qemu_thread_join(&s_accel->event_thread_id);
        iommufd_backend_free_id(viommu->iommufd, viommu->veventq->veventq_id);
        g_free(viommu->veventq);
    }
    viommu->veventq = iommufd_viommu_alloc_eventq(
        &viommu->core, IOMMU_VEVENTQ_TYPE_ARM_SMMUV3, 1 << log2size);
    if (!viommu->veventq) {
        error_report(
            "failed to allocate SMMUV3 veventq, errors will be ignored");
        return;
    }

    qemu_mutex_lock(&s_accel->event_thread_mutex);
    s_accel->event_thread_stop = false;
    qemu_mutex_unlock(&s_accel->event_thread_mutex);
    qemu_thread_create(&s_accel->event_thread_id, "irq/event",
                       smmuv3_nested_event_thread, viommu,
                       QEMU_THREAD_JOINABLE);
}

static SMMUv3AccelDevice *smmuv3_accel_get_dev(SMMUState *bs, SMMUPciBus *sbus,
                                                PCIBus *bus, int devfn)
{
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUDevice *sdev = sbus->pbdev[devfn];
    SMMUv3AccelDevice *accel_dev;

    if (sdev) {
        accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);
    } else {
        accel_dev = g_new0(SMMUv3AccelDevice, 1);
        sdev = &accel_dev->sdev;

        sbus->pbdev[devfn] = sdev;
        smmu_init_sdev(bs, sdev, bus, devfn);
        address_space_init(&accel_dev->as_sysmem, &s->s_accel->root,
                           "smmuv3-accel-sysmem");
    }

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
    uint32_t viommu_id = 0;
    SMMUS2Hwpt *s2_hwpt;
    SMMUViommu *viommu;

    if (s_accel->viommu) {
        accel_dev->viommu = s_accel->viommu;
        return true;
    }

    viommu = g_new0(SMMUViommu, 1);

    if (bs->has_cmdqv) {
        if (!iommufd_backend_alloc_viommu(
                idev->iommufd, idev->devid, IOMMU_VIOMMU_TYPE_TEGRA241_CMDQV,
                s2_hwpt_id, &viommu->cmdqv_data, sizeof(viommu->cmdqv_data),
                &viommu_id, errp)) {
            error_report("CMDQV is unsupported, falling back to nested smmuv3");
            bs->has_cmdqv = false;
        }
    }

    if (!viommu_id) {
        if (!iommufd_backend_alloc_viommu(
                idev->iommufd, idev->devid, IOMMU_VIOMMU_TYPE_ARM_SMMUV3,
                s2_hwpt_id, NULL, 0, &viommu_id, errp)) {
            goto free_viommu;
        }
    }

    viommu->core.viommu_id = viommu_id;
    viommu->core.s2_hwpt_id = s2_hwpt_id;
    viommu->core.iommufd = idev->iommufd;
    viommu->smmu = bs;

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

    s2_hwpt = g_new(SMMUS2Hwpt, 1);
    s2_hwpt->iommufd = idev->iommufd;
    s2_hwpt->hwpt_id = s2_hwpt_id;

    viommu->iommufd = idev->iommufd;
    viommu->s2_hwpt = s2_hwpt;

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

    if (!idev) {
        return true;
    }

    if (accel_dev->idev) {
        if (accel_dev->idev != idev) {
            error_report("Device 0x%x already has an associated idev",
                         smmu_get_sid(sdev));
            return false;
        } else {
            return true;
        }
    }

    if (!smmuv3_accel_dev_alloc_viommu(accel_dev, idev, errp)) {
        error_report("Device 0x%x: Unable to alloc viommu", smmu_get_sid(sdev));
        return false;
    }

    accel_dev->idev = idev;
    QLIST_INSERT_HEAD(&s_accel->viommu->device_list, accel_dev, next);
    trace_smmuv3_accel_set_iommu_device(devfn, smmu_get_sid(sdev));
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

    if (!sbus) {
        return;
    }

    sdev = sbus->pbdev[devfn];
    if (!sdev) {
        return;
    }

    accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);
    if (!host_iommu_device_iommufd_attach_hwpt(accel_dev->idev,
                                               accel_dev->idev->hwpt_id,
                                               NULL)) {
        error_report("Unable to attach dev to the default HW pagetable");
    }

    accel_dev->idev = NULL;
    QLIST_REMOVE(accel_dev, next);
    trace_smmuv3_accel_unset_iommu_device(devfn, smmu_get_sid(sdev));

    viommu = s->s_accel->viommu;
    vdev = accel_dev->vdev;
    if (vdev) {
        iommufd_backend_free_id(viommu->iommufd, vdev->vdev_id);
        g_free(vdev);
        accel_dev->vdev = NULL;
    }

    if (QLIST_EMPTY(&viommu->device_list)) {
        qemu_thread_join(&s->s_accel->event_thread_id);
        iommufd_backend_free_id(viommu->iommufd, viommu->bypass_hwpt_id);
        iommufd_backend_free_id(viommu->iommufd, viommu->abort_hwpt_id);
        iommufd_backend_free_id(viommu->iommufd, viommu->core.viommu_id);
        iommufd_backend_free_id(viommu->iommufd, viommu->s2_hwpt->hwpt_id);
        g_free(viommu->s2_hwpt);
        g_free(viommu);
        s->s_accel->viommu = NULL;
    }
}

static AddressSpace *smmuv3_accel_find_msi_as(PCIBus *bus, void *opaque,
                                                  int devfn)
{
    SMMUState *bs = opaque;
    SMMUPciBus *sbus;
    SMMUv3AccelDevice *accel_dev;
    SMMUDevice *sdev;

    sbus = smmu_get_sbus(bs, bus);
    accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    sdev = &accel_dev->sdev;

    /*
     * If the assigned vfio-pci dev has S1 translation enabled by
     * Guest, return IOMMU address space for MSI translation.
     * Otherwise, return system address space.
     */
    if (accel_dev->s1_hwpt) {
        return &sdev->as;
    } else {
        return &accel_dev->as_sysmem;
    }
}

static bool smmuv3_accel_pdev_allowed(PCIDevice *pdev, bool *vfio_pci)
{

    if (object_dynamic_cast(OBJECT(pdev), TYPE_PCI_BRIDGE) ||
        object_dynamic_cast(OBJECT(pdev), "pxb-pcie") ||
        object_dynamic_cast(OBJECT(pdev), "gpex-root")) {
        return true;
    } else if ((object_dynamic_cast(OBJECT(pdev), TYPE_VFIO_PCI) &&
        object_property_find(OBJECT(pdev), "iommufd"))) {
        *vfio_pci = true;
        return true;
    }
    return false;
}

static AddressSpace *smmuv3_accel_find_add_as(PCIBus *bus, void *opaque,
                                              int devfn)
{
    PCIDevice *pdev = pci_find_device(bus, pci_bus_num(bus), devfn);
    SMMUState *bs = opaque;
    bool vfio_pci = false;
    SMMUPciBus *sbus;
    SMMUv3AccelDevice *accel_dev;
    SMMUDevice *sdev;

    if (pdev && !smmuv3_accel_pdev_allowed(pdev, &vfio_pci)) {
        error_report("Device(%s) not allowed. Only PCIe root complex devices "
                     "or PCI bridge devices or vfio-pci endpoint devices with "
                     "iommufd as backend is allowed with arm-smmuv3,accel=on",
                     pdev->name);
        exit(1);
    }
    sbus = smmu_get_sbus(bs, bus);
    accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    sdev = &accel_dev->sdev;

    if (vfio_pci) {
        return &accel_dev->as_sysmem;
    } else {
        return &sdev->as;
    }
}

static uint64_t smmuv3_accel_get_viommu_cap(void *opaque)
{
    /*
     * Accelerated smmuv3 support only allowes Guest S1
     * configuration. Hence report VIOMMU_CAP_STAGE1
     * so that VFIO can create nested parent domain.
     * The real nested support should be reported from host
     * SMMUv3 and if it doesn't, the nested parent allocation
     * will fail anyway.
     */
    return VIOMMU_CAP_STAGE1;
}

static const PCIIOMMUOps smmuv3_accel_ops = {
    .get_address_space = smmuv3_accel_find_add_as,
    .get_viommu_cap = smmuv3_accel_get_viommu_cap,
    .set_iommu_device = smmuv3_accel_set_iommu_device,
    .unset_iommu_device = smmuv3_accel_unset_iommu_device,
    .get_msi_address_space = smmuv3_accel_find_msi_as,
};

void smmuv3_accel_init(SMMUv3State *s)
{
    SMMUState *bs = ARM_SMMU(s);
    SMMUv3AccelState *s_accel;

    s->s_accel = s_accel = g_new0(SMMUv3AccelState, 1);
    memory_region_init(&s_accel->root, OBJECT(s), "root", UINT64_MAX);
    memory_region_init_alias(&s_accel->sysmem, OBJECT(s),
                             "smmuv3-accel-sysmem", get_system_memory(), 0,
                             memory_region_size(get_system_memory()));
    memory_region_add_subregion(&s_accel->root, 0, &s_accel->sysmem);
    qemu_mutex_init(&s_accel->event_thread_mutex);

    if (bs->has_cmdqv) {
        s_accel->cmdqv = tegra241_cmdqv_init(s);
    }
}

static void smmuv3_accel_class_init(ObjectClass *oc, const void *data)
{
    SMMUBaseClass *sbc = ARM_SMMU_CLASS(oc);

    sbc->iommu_ops = &smmuv3_accel_ops;
}

static const TypeInfo types[] = {
    {
        .name = TYPE_ARM_SMMUV3_ACCEL,
        .parent = TYPE_ARM_SMMUV3,
        .class_init = smmuv3_accel_class_init,
    }
};
DEFINE_TYPES(types)
