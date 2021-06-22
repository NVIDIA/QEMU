// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021-2024, NVIDIA CORPORATION & AFFILIATES
 *
 * NVIDIA Tegra241 CMDQ-Virtualization extension for SMMUv3
 *
 * Written by Nicolin Chen <nicolinc@nvidia.com>
 */

#include "qemu/osdep.h"
#include "trace.h"
#include <poll.h>
#include <sys/ioctl.h>

#include "hw/arm/smmuv3.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/pci/pci_bus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "cpu.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#include "smmuv3-accel.h"
#include "tegra241-cmdqv.h"

#define TEGRA241_CMDQV(obj) \
    OBJECT_CHECK(Tegra241CMDQV, (obj), TYPE_TEGRA241_CMDQV)

typedef struct Tegra241CMDQV Tegra241CMDQV;
struct Tegra241CMDQV {
    SysBusDevice parent_obj;
    SMMUv3State *smmu;
    SMMUViommu *viommu;
    IOMMUFDHWqueue *vcmdq[128];
    IOMMUFDVeventq *veventq;
    QemuThread irq_thread_id;
    MemoryRegion mmio_cmdqv;
    MemoryRegion mmio_vcmdq_page;
    MemoryRegion mmio_vintf_page;
    qemu_irq irq;
    void *vcmdq_page0;

    /* Register Cache */
    uint32_t config;
    uint32_t param;
    uint32_t status;
    uint32_t vi_err_map[2];
    uint32_t vi_int_mask[2];
    uint32_t cmdq_err_map[4];
    uint32_t cmdq_alloc_map[128];
    uint32_t vintf_config;
    uint32_t vintf_status;
    uint32_t vintf_cmdq_err_map[4];
    uint32_t vcmdq_cons_indx[128];
    uint32_t vcmdq_prod_indx[128];
    uint32_t vcmdq_config[128];
    uint32_t vcmdq_status[128];
    uint32_t vcmdq_gerror[128];
    uint32_t vcmdq_gerrorn[128];
    uint64_t vcmdq_base[128];
    uint64_t vcmdq_cons_indx_base[128];
};

static void cmdqv_init_regs(Tegra241CMDQV *s)
{
    SMMUv3State *smmu = s->smmu;
    SMMUv3AccelState *s_accel = smmu->s_accel;
    int i;

    s->config = V_CONFIG_RESET;
    s->param =
        FIELD_DP32(s->param, PARAM, CMDQV_VER, s_accel->cmdqv_info.version);
    s->param = FIELD_DP32(s->param, PARAM, CMDQV_NUM_CMDQ_LOG2,
                          s_accel->cmdqv_info.log2vcmdqs);
    s->param = FIELD_DP32(s->param, PARAM, CMDQV_NUM_SID_PER_VM_LOG2,
                          s_accel->cmdqv_info.log2vsids);
    trace_tegra241_cmdqv_init(s->param);
    s->status = R_STATUS_CMDQV_ENABLED_MASK;
    for (i = 0; i < 2; i++) {
        s->vi_err_map[i] = 0;
        s->vi_int_mask[i] = 0;
        s->cmdq_err_map[i] = 0;
    }
    s->vintf_config = 0;
    s->vintf_status = 0;
    for (i = 0; i < 4; i++) {
        s->vintf_cmdq_err_map[i] = 0;
    }
    for (i = 0; i < 128; i++) {
        s->cmdq_alloc_map[i] = 0;
        s->vcmdq_cons_indx[i] = 0;
        s->vcmdq_prod_indx[i] = 0;
        s->vcmdq_config[i] = 0;
        s->vcmdq_status[i] = 0;
        s->vcmdq_gerror[i] = 0;
        s->vcmdq_gerrorn[i] = 0;
        s->vcmdq_base[i] = 0;
        s->vcmdq_cons_indx_base[i] = 0;
    }
}

/* Note that offset aligns down to 0x1000 */
static uint64_t tegra241_cmdqv_read_vintf(Tegra241CMDQV *s, hwaddr offset)
{
    int i;

    switch (offset) {
    case A_VINTF0_CONFIG:
        return s->vintf_config;

    case A_VINTF0_STATUS:
        return s->vintf_status;

    case A_VINTF0_LVCMDQ_ERR_MAP_0 ... A_VINTF0_LVCMDQ_ERR_MAP_3:
        i = (offset - A_VINTF0_LVCMDQ_ERR_MAP_0) / 4;
        return s->vintf_cmdq_err_map[i];
    }

    qemu_log_mask(LOG_UNIMP, "%s unhandled read access at 0x%" PRIx64 "\n",
                  __func__, offset);
    return 0;
}

/* Note that offset aligns down to 0x10000 */
static uint64_t tegra241_cmdqv_read_vcmdq(Tegra241CMDQV *s, hwaddr offset,
                                          int index)
{
    uint32_t *ptr;

    switch (offset) {
    case A_VCMDQ0_CONS_INDX:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            s->vcmdq_cons_indx[index] = *ptr;
        }
        return s->vcmdq_cons_indx[index];

    case A_VCMDQ0_PROD_INDX:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            s->vcmdq_prod_indx[index] = *ptr;
        }
        return s->vcmdq_prod_indx[index];

    case A_VCMDQ0_CONFIG:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            s->vcmdq_config[index] = *ptr;
        }
        return s->vcmdq_config[index];

    case A_VCMDQ0_STATUS:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            s->vcmdq_status[index] = *ptr;
        }
        return s->vcmdq_status[index];

    case A_VCMDQ0_GERROR:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            s->vcmdq_gerror[index] = *ptr;
        }
        return s->vcmdq_gerror[index];

    case A_VCMDQ0_GERRORN:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            s->vcmdq_gerrorn[index] = *ptr;
        }
        return s->vcmdq_gerrorn[index];

    case A_VCMDQ0_BASE_L:
        return s->vcmdq_base[index];

    case A_VCMDQ0_BASE_H:
        return s->vcmdq_base[index] >> 32;

    case A_VCMDQ0_CONS_INDX_BASE_DRAM_L:
        return s->vcmdq_cons_indx_base[index];

    case A_VCMDQ0_CONS_INDX_BASE_DRAM_H:
        return s->vcmdq_cons_indx_base[index] >> 32;
    }

    qemu_log_mask(LOG_UNIMP, "%s unhandled read access at 0x%" PRIx64 "\n",
                  __func__, offset);
    return 0;
}

static void *tegra241_cmdqv_irq_thread(void *arg)
{
    struct iommu_vevent_tegra241_cmdqv *vevent;
    struct iommufd_vevent_header *hdr;
    struct pollfd pollfd = {};
    Tegra241CMDQV *s = arg;
    ssize_t readsz = sizeof(*hdr) + sizeof(*vevent);
    ssize_t bytes;
    int i, ret;
    void *buf;

    if (!s->viommu || !s->veventq) {
        return NULL;
    }
    buf = g_malloc0(readsz);
    pollfd.events = POLLIN;
    pollfd.fd = s->veventq->veventq_fd;

    while (1) {
        ret = poll(&pollfd, 1, -1);
        if (ret < 0) {
            error_report("%s: poll failed: %d", __func__, ret);
            return NULL;
        }

        bytes = read(pollfd.fd, buf, readsz);
        if (bytes <= 0) {
            error_report("%s: read failed: %d", __func__, ret);
            return NULL;
        }
        hdr = buf;
        vevent = buf + sizeof(*hdr);
        if (hdr->flags & IOMMU_VEVENTQ_FLAG_LOST_EVENTS) {
            error_report("%s: vEVENTQ has lost events", __func__);
            goto out_free;
        }

        if (vevent->lvcmdq_err_map[0] || vevent->lvcmdq_err_map[1]) {
            s->vintf_cmdq_err_map[0] = vevent->lvcmdq_err_map[0] & 0xffffffff;
            s->vintf_cmdq_err_map[1] =
                (vevent->lvcmdq_err_map[0] >> 32) & 0xffffffff;
            s->vintf_cmdq_err_map[2] = vevent->lvcmdq_err_map[1] & 0xffffffff;
            s->vintf_cmdq_err_map[3] =
                (vevent->lvcmdq_err_map[1] >> 32) & 0xffffffff;
            for (i = 0; i < 4; i++) {
                s->cmdq_err_map[i] = s->vintf_cmdq_err_map[i];
            }
            s->vi_err_map[0] |= 0x1;
            qemu_irq_pulse(s->irq);
            trace_tegra241_cmdqv_err_map(
                s->vintf_cmdq_err_map[3], s->vintf_cmdq_err_map[2],
                s->vintf_cmdq_err_map[1], s->vintf_cmdq_err_map[0]);
        }
    }
out_free:
    g_free(buf);
    return NULL;
}

static int tegra241_cmdqv_init_vcmdq_page0(Tegra241CMDQV *s)
{
    SMMUv3State *smmu = s->smmu;
    SMMUv3AccelState *s_accel = smmu->s_accel;
    char *name;

    if (!s_accel->viommu) {
        return 0;
    }

    if (!s->viommu) {
        s->viommu = s_accel->viommu;
        s->veventq = iommufd_viommu_alloc_eventq(
            &s->viommu->core, IOMMU_VEVENTQ_TYPE_TEGRA241_CMDQV, 1 << 16);
        if (!s->veventq) {
            error_report(
                "failed to allocate CMDQV veventq, errors will be ignored");
        } else {
            qemu_thread_create(&s->irq_thread_id, "irq/cmdqv",
                               tegra241_cmdqv_irq_thread, s,
                               QEMU_THREAD_JOINABLE);
        }
    }

    if (!iommufd_viommu_mmap(&s_accel->viommu->core, VCMDQ_REG_PAGE_SIZE,
                             s_accel->viommu->cmdqv_data.out_vintf_mmap_offset,
                             &s->vcmdq_page0)) {
        error_report("failed to mmap VCMDQ PAGE0");
        s->vcmdq_page0 = NULL;
        return -EIO;
    }

    name = g_strdup_printf("%s vcmdq", memory_region_name(&s->mmio_cmdqv));
    memory_region_init_ram_device_ptr(&s->mmio_vcmdq_page,
                                      memory_region_owner(&s->mmio_cmdqv), name,
                                      0x10000, s->vcmdq_page0);
    memory_region_add_subregion_overlap(&s->mmio_cmdqv, 0x10000,
                                        &s->mmio_vcmdq_page, 1);
    g_free(name);

    name = g_strdup_printf("%s vintf", memory_region_name(&s->mmio_cmdqv));
    memory_region_init_ram_device_ptr(&s->mmio_vintf_page,
                                      memory_region_owner(&s->mmio_cmdqv), name,
                                      0x10000, s->vcmdq_page0);
    memory_region_add_subregion_overlap(&s->mmio_cmdqv, 0x30000,
                                        &s->mmio_vintf_page, 1);
    g_free(name);

    return 0;
}

static uint64_t tegra241_cmdqv_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra241CMDQV *s = (Tegra241CMDQV *)opaque;
    int index;

    if (!s->vcmdq_page0) {
        tegra241_cmdqv_init_vcmdq_page0(s);
    }

    if (offset > 0x50000) {
        qemu_log_mask(LOG_UNIMP,
                      "%s offset 0x%" PRIx64 " off limit (0x50000)\n", __func__,
                      offset);
        return 0;
    }

    /* Fallback to cached register values */
    switch (offset) {
    case A_CONFIG:
        return s->config;

    case A_PARAM:
        return s->param;

    case A_STATUS:
        return s->status;

    case A_VI_ERR_MAP ... A_VI_ERR_MAP_1:
        return s->vi_err_map[(offset - A_VI_ERR_MAP) / 4];

    case A_VI_INT_MASK ... A_VI_INT_MASK_1:
        return s->vi_int_mask[(offset - A_VI_INT_MASK) / 4];

    case A_CMDQ_ERR_MAP ... A_CMDQ_ERR_MAP_3:
        return s->cmdq_err_map[(offset - A_CMDQ_ERR_MAP) / 4];

    case A_CMDQ_ALLOC_MAP_0 ... A_CMDQ_ALLOC_MAP_127:
        return s->cmdq_alloc_map[(offset - A_CMDQ_ALLOC_MAP_0) / 4];

    case A_VINTF0_CONFIG ... A_VINTF0_LVCMDQ_ERR_MAP_3:
        return tegra241_cmdqv_read_vintf(s, offset);

    case A_VI_VCMDQ0_CONS_INDX ... A_VI_VCMDQ127_GERRORN:
        offset -= 0x20000;
        QEMU_FALLTHROUGH;
    case A_VCMDQ0_CONS_INDX ... A_VCMDQ127_GERRORN:
        /*
         * Align offset down to 0x10000 while extracting the index:
         *   VCMDQ0_CONS_INDX  (0x10000) => 0x10000, 0
         *   VCMDQ1_CONS_INDX  (0x10080) => 0x10000, 1
         *   VCMDQ2_CONS_INDX  (0x10100) => 0x10000, 2
         *   ...
         *   VCMDQ127_CONS_INDX (0x13f80) => 0x10000, 127
         */
        index = (offset - 0x10000) / 0x80;
        return tegra241_cmdqv_read_vcmdq(s, offset - 0x80 * index, index);

    case A_VI_VCMDQ0_BASE_L ... A_VI_VCMDQ127_CONS_INDX_BASE_DRAM_H:
        offset -= 0x20000;
        QEMU_FALLTHROUGH;
    case A_VCMDQ0_BASE_L ... A_VCMDQ127_CONS_INDX_BASE_DRAM_H:
        /*
         * Align offset down to 0x20000 while extracting the index:
         *   VCMDQ0_BASE_L  (0x20000) => 0x20000, 0
         *   VCMDQ1_BASE_L  (0x20080) => 0x20000, 1
         *   VCMDQ2_BASE_L  (0x20100) => 0x20000, 2
         *   ...
         *   VCMDQ127_BASE_L (0x23f80) => 0x20000, 127
         */
        index = (offset - 0x20000) / 0x80;
        return tegra241_cmdqv_read_vcmdq(s, offset - 0x80 * index, index);
    }

    qemu_log_mask(LOG_UNIMP, "%s unhandled read access at 0x%" PRIx64 "\n",
                  __func__, offset);
    return 0;
}

/* Note that offset aligns down to 0x1000 */
static void tegra241_cmdqv_write_vintf(Tegra241CMDQV *s, hwaddr offset,
                                       uint64_t value, unsigned size)
{
    switch (offset) {
    case A_VINTF0_CONFIG:
        /* Strip off HYP_OWN setting from guest kernel */
        value &= ~R_VINTF0_CONFIG_HYP_OWN_MASK;

        s->vintf_config = value;
        if (value & R_VINTF0_CONFIG_ENABLE_MASK) {
            s->vintf_status |= R_VINTF0_STATUS_ENABLE_OK_MASK;
        } else {
            s->vintf_status &= ~R_VINTF0_STATUS_ENABLE_OK_MASK;
        }
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s unhandled write access at 0x%" PRIx64 "\n",
                      __func__, offset);
        return;
    }
}

static int tegra241_cmdqv_setup_vcmdq(Tegra241CMDQV *s, int index)
{
    SMMUv3State *smmu = s->smmu;
    SMMUv3AccelState *s_accel = smmu->s_accel;
    uint64_t base_mask = (uint64_t)R_VCMDQ0_BASE_L_ADDR_MASK |
                         (uint64_t)R_VCMDQ0_BASE_H_ADDR_MASK << 32;
    uint64_t addr = s->vcmdq_base[index] & base_mask;
    uint64_t shift = s->vcmdq_base[index] & R_VCMDQ0_BASE_L_LOG2SIZE_MASK;
    uint64_t size = 1 << (shift + 4);
    IOMMUFDHWqueue *vcmdq = s->vcmdq[index];

    if (!s_accel->viommu) {
        return -ENODEV;
    }
    if (!size) {
        return -EINVAL;
    }
    if (!cpu_physical_memory_is_ram(addr)) {
        return -EINVAL;
    }
    if (vcmdq) {
        iommufd_backend_free_id(s_accel->viommu->iommufd, vcmdq->hw_queue_id);
        s->vcmdq[index] = NULL;
        g_free(vcmdq);
    }
    if (!s->viommu) {
        s->viommu = s_accel->viommu;
        s->veventq = iommufd_viommu_alloc_eventq(
            &s->viommu->core, IOMMU_VEVENTQ_TYPE_TEGRA241_CMDQV, 1 << 16);
        if (!s->veventq) {
            error_report(
                "failed to allocate CMDQV veventq, errors will be ignored");
        } else {
            qemu_thread_create(&s->irq_thread_id, "irq/cmdqv",
                               tegra241_cmdqv_irq_thread, s,
                               QEMU_THREAD_JOINABLE);
        }
    }
    vcmdq = iommufd_viommu_alloc_hw_queue(&s->viommu->core,
                                          IOMMU_HW_QUEUE_TYPE_TEGRA241_CMDQV,
                                          index, addr, size);
    if (!vcmdq) {
        error_report("failed to allocate VCMDQ%d, viommu_id=%d", index,
                     s->viommu->core.viommu_id);
        return -ENODEV;
    }
    s->vcmdq[index] = vcmdq;

    return 0;
}

/* Note that offset aligns down to 0x10000 */
static void tegra241_cmdqv_write_vcmdq(Tegra241CMDQV *s, hwaddr offset,
                                       int index, uint64_t value, unsigned size)
{
    uint32_t *ptr;

    switch (offset) {
    case A_VCMDQ0_CONS_INDX:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            *ptr = value;
        }
        s->vcmdq_cons_indx[index] = value;
        return;

    case A_VCMDQ0_PROD_INDX:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            *ptr = value;
        }
        s->vcmdq_prod_indx[index] = value;
        return;

    case A_VCMDQ0_CONFIG:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            *ptr = value;
        } else {
            if (value & R_VCMDQ0_CONFIG_CMDQ_EN_MASK) {
                s->vcmdq_status[index] |= R_VCMDQ0_STATUS_CMDQ_EN_OK_MASK;
            } else {
                s->vcmdq_status[index] &= ~R_VCMDQ0_STATUS_CMDQ_EN_OK_MASK;
            }
        }
        s->vcmdq_config[index] = value;
        return;

    case A_VCMDQ0_GERRORN:
        if (s->vcmdq_page0) {
            ptr =
                (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
            *ptr = value;
        }
        s->vcmdq_gerrorn[index] = value;
        return;

    case A_VCMDQ0_BASE_L:
        if (size == 8) {
            s->vcmdq_base[index] = value;
        } else if (size == 4) {
            s->vcmdq_base[index] &= 0xffffffff00000000;
            s->vcmdq_base[index] |= value & 0xffffffff;
        }
        tegra241_cmdqv_setup_vcmdq(s, index);
        break;

    case A_VCMDQ0_BASE_H:
        s->vcmdq_base[index] &= (uint64_t)0xffffffff;
        s->vcmdq_base[index] |= (uint64_t)value << 32;
        tegra241_cmdqv_setup_vcmdq(s, index);
        break;

    case A_VCMDQ0_CONS_INDX_BASE_DRAM_L:
        if (size == 8) {
            s->vcmdq_cons_indx_base[index] = value;
        } else if (size == 4) {
            s->vcmdq_cons_indx_base[index] &= 0xffffffff00000000;
            s->vcmdq_cons_indx_base[index] |= value & 0xffffffff;
        }
        break;

    case A_VCMDQ0_CONS_INDX_BASE_DRAM_H:
        s->vcmdq_cons_indx_base[index] &= (uint64_t)0xffffffff;
        s->vcmdq_cons_indx_base[index] |= (uint64_t)value << 32;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s unhandled write access at 0x%" PRIx64 "\n",
                      __func__, offset);
        return;
    }
}

static void tegra241_cmdqv_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    Tegra241CMDQV *s = (Tegra241CMDQV *)opaque;
    int index;

    if (!s->vcmdq_page0) {
        tegra241_cmdqv_init_vcmdq_page0(s);
    }

    if (offset > 0x50000) {
        qemu_log_mask(LOG_UNIMP,
                      "%s offset 0x%" PRIx64 " off limit (0x50000)\n", __func__,
                      offset);
        return;
    }

    switch (offset) {
    case A_CONFIG:
        s->config = value;
        if (value & R_CONFIG_CMDQV_EN_MASK) {
            s->status |= R_STATUS_CMDQV_ENABLED_MASK;
        } else {
            s->status &= ~R_STATUS_CMDQV_ENABLED_MASK;
        }
        break;

    case A_VI_INT_MASK ... A_VI_INT_MASK_1:
        s->vi_int_mask[(offset - A_VI_INT_MASK) / 4] = value;
        break;

    case A_CMDQ_ALLOC_MAP_0 ... A_CMDQ_ALLOC_MAP_127:
        s->cmdq_alloc_map[(offset - A_CMDQ_ALLOC_MAP_0) / 4] = value;
        break;

    case A_VINTF0_CONFIG ... A_VINTF0_LVCMDQ_ERR_MAP_3:
        tegra241_cmdqv_write_vintf(s, offset, value, size);
        break;

    case A_VI_VCMDQ0_CONS_INDX ... A_VI_VCMDQ127_GERRORN:
        offset -= 0x20000;
        QEMU_FALLTHROUGH;
    case A_VCMDQ0_CONS_INDX ... A_VCMDQ127_GERRORN:
        /*
         * Align offset down to 0x10000 while extracting the index:
         *   VCMDQ0_CONS_INDX  (0x10000) => 0x10000, 0
         *   VCMDQ1_CONS_INDX  (0x10080) => 0x10000, 1
         *   VCMDQ2_CONS_INDX  (0x10100) => 0x10000, 2
         *   ...
         *   VCMDQ127_CONS_INDX (0x13f80) => 0x10000, 127
         */
        index = (offset - 0x10000) / 0x80;
        tegra241_cmdqv_write_vcmdq(s, offset - 0x80 * index, index, value,
                                   size);
        break;

    case A_VI_VCMDQ0_BASE_L ... A_VI_VCMDQ127_CONS_INDX_BASE_DRAM_H:
        offset -= 0x20000;
        QEMU_FALLTHROUGH;
    case A_VCMDQ0_BASE_L ... A_VCMDQ127_CONS_INDX_BASE_DRAM_H:
        /*
         * Align offset down to 0x20000 while extracting the index:
         *   VCMDQ0_BASE_L  (0x20000) => 0x20000, 0
         *   VCMDQ1_BASE_L  (0x20080) => 0x20000, 1
         *   VCMDQ2_BASE_L  (0x20100) => 0x20000, 2
         *   ...
         *   VCMDQ127_BASE_L (0x23f80) => 0x20000, 127
         */
        index = (offset - 0x20000) / 0x80;
        tegra241_cmdqv_write_vcmdq(s, offset - 0x80 * index, index, value,
                                   size);
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s unhandled write access at 0x%" PRIx64 "\n",
                      __func__, offset);
        return;
    }
}

static const MemoryRegionOps mmio_cmdqv_ops = {
    .read = tegra241_cmdqv_read,
    .write = tegra241_cmdqv_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_cmdqv = {
    .name = "cmdqv",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT32(status, Tegra241CMDQV),
            VMSTATE_UINT32_ARRAY(vi_err_map, Tegra241CMDQV, 2),
            VMSTATE_END_OF_LIST(),
        },
};

struct Tegra241CMDQV *tegra241_cmdqv_init(SMMUv3State *s)
{
    Tegra241CMDQV *cmdqv = g_new0(Tegra241CMDQV, 1);
    SMMUState *bs = ARM_SMMU(s);
    SysBusDevice *sbd = &bs->dev;

    memory_region_init_io(&cmdqv->mmio_cmdqv, OBJECT(s), &mmio_cmdqv_ops, cmdqv,
                          TYPE_TEGRA241_CMDQV, 0x50000);
    sysbus_init_mmio(sbd, &cmdqv->mmio_cmdqv);
    sysbus_init_irq(sbd, &cmdqv->irq);
    cmdqv->smmu = s;
    return cmdqv;
}

void tegra241_cmdqv_reset(Tegra241CMDQV *s)
{
    int i;

    for (i = 127; i >= 0; i--) {
        if (s->vcmdq[i]) {
            iommufd_backend_free_id(s->viommu->iommufd,
                                    s->vcmdq[i]->hw_queue_id);
            g_free(s->vcmdq[i]);
            s->vcmdq[i] = NULL;
        }
    }
    cmdqv_init_regs(s);
}

static void cmdqv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Tegra241 CMDQ-Virtualization";
    dc->vmsd = &vmstate_cmdqv;
}

static const TypeInfo types[] = {
    {
        .name = TYPE_TEGRA241_CMDQV,
        .parent = TYPE_ARM_SMMUV3_ACCEL,
        .class_init = cmdqv_class_init,
    }
};
DEFINE_TYPES(types)
