// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021-2024, NVIDIA CORPORATION & AFFILIATES
 *
 * NVIDIA Tegra241 CMDQ-Virtualization extension for SMMUv3
 *
 * Written by Nicolin Chen <nicolinc@nvidia.com>
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>

#include "cpu.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
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

#include "tegra241-cmdqv.h"
#include "hw/arm/smmuv3.h"

#define TYPE_TEGRA241_CMDQV "tegra241-cmdqv"
#define TEGRA241_CMDQV(obj) \
    OBJECT_CHECK(Tegra241CMDQV, (obj), TYPE_TEGRA241_CMDQV)

typedef struct Tegra241CMDQV Tegra241CMDQV;
struct Tegra241CMDQV {
    SysBusDevice parent_obj;
    DeviceState *smmu_dev;
    IOMMUFDViommu *viommu;
    IOMMUFDVqueue *vqueue[128];
    MemoryRegion mmio_cmdqv;
    MemoryRegion mmio_vcmdq_page;
    MemoryRegion mmio_vintf_page;
    qemu_irq irq[2];
    void *vcmdq_page0;

    /* Register Cache */
    uint32_t config;
    uint32_t param;
    uint32_t status;
    uint32_t vi_err_map[2];
    uint32_t vi_int_mask[2];
    uint32_t cmdq_err_map[2];
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
    int i;

    s->config = V_CONFIG_RESET;
    s->param = V_PARAM_RESET;
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

    case A_VINTF0_CMDQ_ERR_MAP_0 ... A_VINTF0_CMDQ_ERR_MAP_3:
        i = (offset - A_VINTF0_CMDQ_ERR_MAP_0) / 4;
        return s->vintf_cmdq_err_map[i];
    }

    qemu_log_mask(LOG_UNIMP, "%s unhandled read access at 0x%"PRIx64"\n",
                  __func__, offset);
    return 0;
}

/* Note that offset aligns down to 0x10000 */
static uint64_t tegra241_cmdqv_read_vcmdq(Tegra241CMDQV *s, hwaddr offset, int index)
{
    uint32_t *ptr;

    switch (offset) {
    case A_VCMDQ0_CONS_INDX:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        s->vcmdq_cons_indx[index] = *ptr;
        return s->vcmdq_cons_indx[index];

    case A_VCMDQ0_PROD_INDX:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        s->vcmdq_prod_indx[index] = *ptr;
        return s->vcmdq_prod_indx[index];

    case A_VCMDQ0_CONFIG:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        s->vcmdq_config[index] = *ptr;
        return s->vcmdq_config[index];

    case A_VCMDQ0_STATUS:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        s->vcmdq_status[index] = *ptr;
        return s->vcmdq_status[index];

    case A_VCMDQ0_GERROR:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        s->vcmdq_gerror[index] = *ptr;
        return s->vcmdq_gerror[index];

    case A_VCMDQ0_GERRORN:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        s->vcmdq_gerrorn[index] = *ptr;
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

    qemu_log_mask(LOG_UNIMP, "%s unhandled read access at 0x%"PRIx64"\n",
                  __func__, offset);
    return 0;
}

static int tegra241_cmdqv_init_vcmdq_page0(Tegra241CMDQV *s)
{
    char *name;

    s->vcmdq_page0 = smmu_iommu_get_shared_page(ARM_SMMU(s->smmu_dev),
                                                VCMDQ_REG_PAGE_SIZE, false);
    if (!s->vcmdq_page0) {
        error_report("failed to mmap VCMDQ PAGE0");
        return -EIO;
    }

    name = g_strdup_printf("%s vcmdq",
                           memory_region_name(&s->mmio_cmdqv));
    memory_region_init_ram_device_ptr(&s->mmio_vcmdq_page,
                                      memory_region_owner(&s->mmio_cmdqv),
                                      name, 0x10000, s->vcmdq_page0);
    memory_region_add_subregion_overlap(&s->mmio_cmdqv, 0x10000,
                                        &s->mmio_vcmdq_page, 1);
    g_free(name);

    name = g_strdup_printf("%s vintf",
                           memory_region_name(&s->mmio_cmdqv));
    memory_region_init_ram_device_ptr(&s->mmio_vintf_page,
                                      memory_region_owner(&s->mmio_cmdqv),
                                      name, 0x10000, s->vcmdq_page0);
    memory_region_add_subregion_overlap(&s->mmio_cmdqv, 0x30000,
                                        &s->mmio_vintf_page, 1);
    g_free(name);

    return 0;
}

static uint64_t tegra241_cmdqv_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra241CMDQV *s = (Tegra241CMDQV *) opaque;
    int index;

    if (!s->vcmdq_page0) {
        tegra241_cmdqv_init_vcmdq_page0(s);
    }

    if (offset > 0x50000) {
        qemu_log_mask(LOG_UNIMP, "%s offset 0x%"PRIx64" off limit (0x50000)\n",
                      __func__, offset);
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
        return s->vi_err_map[(offset - A_VI_ERR_MAP) / 2];

    case A_VI_INT_MASK ... A_VI_INT_MASK_1:
        return s->vi_int_mask[(offset - A_VI_INT_MASK) / 2];

    case A_CMDQ_ERR_MAP ... A_CMDQ_ERR_MAP_3:
        return s->cmdq_err_map[(offset - A_CMDQ_ERR_MAP) / 4];

    case A_CMDQ_ALLOC_MAP_0 ... A_CMDQ_ALLOC_MAP_127:
        return s->cmdq_alloc_map[(offset - A_CMDQ_ALLOC_MAP_0) / 128];

    case A_VINTF0_CONFIG ... A_VINTF0_CMDQ_ERR_MAP_3:
        return tegra241_cmdqv_read_vintf(s, offset);
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

    qemu_log_mask(LOG_UNIMP, "%s unhandled read access at 0x%"PRIx64"\n",
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
            s->vintf_status = R_VINTF0_STATUS_ENABLE_OK_MASK;
        } else {
            s->vintf_status &= ~R_VINTF0_STATUS_ENABLE_OK_MASK;
        }
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s unhandled write access at 0x%"PRIx64"\n",
                      __func__, offset);
        return;
    }
}

static int tegra241_cmdqv_setup_vcmdq(Tegra241CMDQV *s, int index)
{
    SMMUState *bs = ARM_SMMU(s->smmu_dev);
    uint64_t base_mask = (uint64_t)R_VCMDQ0_BASE_L_ADDR_MASK |
                         (uint64_t)R_VCMDQ0_BASE_H_ADDR_MASK << 32;
    struct iommu_vqueue_tegra241_cmdqv data = {
        .vcmdq_id = index,
        .vcmdq_log2size = s->vcmdq_base[index] &
                          R_VCMDQ0_BASE_L_LOG2SIZE_MASK,
        .vcmdq_base = s->vcmdq_base[index] & base_mask,
    };
    IOMMUFDVqueue *vqueue = s->vqueue[index];

    if (!bs->viommu) {
        return -ENODEV;
    }
    if (!data.vcmdq_log2size) {
        return -EINVAL;
    }
    if (!cpu_physical_memory_is_ram(data.vcmdq_base)) {
        return -EINVAL;
    }
    if (vqueue) {
        iommufd_backend_free_id(bs->viommu->iommufd, vqueue->vqueue_id);
        g_free(vqueue);
    }
    if (!s->viommu) {
        s->viommu = bs->viommu;
    }
    vqueue = iommufd_viommu_alloc_queue(bs->viommu,
                                        IOMMU_VQUEUE_DATA_TEGRA241_CMDQV,
                                        sizeof(data), (void *)&data);
    if (!vqueue) {
        error_report("failed to allocate VCMDQ%d, viommu_id=%d", index, bs->viommu->viommu_id);
        return -ENODEV;
    }
    s->vqueue[index] = vqueue;

    return 0;
}

/* Note that offset aligns down to 0x10000 */
static void tegra241_cmdqv_write_vcmdq(Tegra241CMDQV *s, hwaddr offset,
                                       int index, uint64_t value, unsigned size)
{
    uint32_t *ptr;

    switch (offset) {
    case A_VCMDQ0_CONS_INDX:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        *ptr = s->vcmdq_cons_indx[index] = value;
        return;

    case A_VCMDQ0_PROD_INDX:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        *ptr = s->vcmdq_prod_indx[index] = value;
        return;

    case A_VCMDQ0_CONFIG:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        *ptr = s->vcmdq_config[index] = value;
        return;

    case A_VCMDQ0_GERRORN:
        ptr = (uint32_t *)(s->vcmdq_page0 + 0x80 * index + offset - 0x10000);
        *ptr = s->vcmdq_gerrorn[index] = value;
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
            s->vcmdq_cons_indx_base[index] &= 0xffffffff;
            s->vcmdq_cons_indx_base[index] |= value & 0xffffffff;
        }
        break;

    case A_VCMDQ0_CONS_INDX_BASE_DRAM_H:
        s->vcmdq_cons_indx_base[index] &= (uint64_t)0xffffffff << 32;
        s->vcmdq_cons_indx_base[index] |= (uint64_t)value << 32;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s unhandled write access at 0x%"PRIx64"\n",
                      __func__, offset);
        return;
    }
}

static void tegra241_cmdqv_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Tegra241CMDQV *s = (Tegra241CMDQV *) opaque;
    int index;

    if (!s->vcmdq_page0) {
        tegra241_cmdqv_init_vcmdq_page0(s);
    }

    if (offset > 0x50000) {
        qemu_log_mask(LOG_UNIMP, "%s offset 0x%"PRIx64" off limit (0x50000)\n",
                      __func__, offset);
        return;
    }

    switch (offset) {
    case A_CONFIG:
        s->config = value;
        break;

    case A_VI_INT_MASK ... A_VI_INT_MASK_1:
        s->vi_int_mask[(offset - A_VI_INT_MASK) / 2] = value;
        break;

    case A_CMDQ_ALLOC_MAP_0 ... A_CMDQ_ALLOC_MAP_127:
        s->cmdq_alloc_map[(offset - A_CMDQ_ALLOC_MAP_0) / 128] = value;
        break;

    case A_VINTF0_CONFIG ... A_VINTF0_CMDQ_ERR_MAP_3:
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
        tegra241_cmdqv_write_vcmdq(s, offset - 0x80 * index, index, value, size);
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
        tegra241_cmdqv_write_vcmdq(s, offset - 0x80 * index, index, value, size);
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s unhandled write access at 0x%"PRIx64"\n",
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
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(status, Tegra241CMDQV),
        VMSTATE_UINT32_ARRAY(vi_err_map, Tegra241CMDQV, 2),
        VMSTATE_END_OF_LIST(),
    },
};

static void cmdqv_initfn(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    Tegra241CMDQV *s = TEGRA241_CMDQV(dev);

    memory_region_init_io(&s->mmio_cmdqv, obj, &mmio_cmdqv_ops,
                          s, TYPE_TEGRA241_CMDQV, 0x50000);
    sysbus_init_mmio(sbd, &s->mmio_cmdqv);
}

static void cmdqv_reset(DeviceState *d)
{
    Tegra241CMDQV *s = TEGRA241_CMDQV(d);
    int i;

    for (i = 0; i < 128; i++) {
        if (s->vqueue[i]) {
            iommufd_backend_free_id(s->viommu->iommufd,
                                    s->vqueue[i]->vqueue_id);
            g_free(s->vqueue[i]);
            s->vqueue[i] = NULL;
        }
    }
    cmdqv_init_regs(s);
}

static void cmdqv_realize(DeviceState *d, Error **errp)
{
    cmdqv_reset(d);
}

static void cmdqv_unrealize(DeviceState *d)
{
    Tegra241CMDQV *s = TEGRA241_CMDQV(d);

    smmu_iommu_put_shared_page(ARM_SMMU(s->smmu_dev), s->vcmdq_page0,
                               VCMDQ_REG_PAGE_SIZE);
}

static Property cmdqv_properties[] = {
    DEFINE_PROP_LINK("smmuv3", Tegra241CMDQV, smmu_dev, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void cmdqv_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, cmdqv_properties);
    dc->desc = "Tegra241 Virtual CMDQ";
    dc->vmsd = &vmstate_cmdqv;
    dc->reset = cmdqv_reset;
    dc->realize = cmdqv_realize;
    dc->unrealize = cmdqv_unrealize;
}

static const TypeInfo cmdqv_type_info = {
    .name          = TYPE_TEGRA241_CMDQV,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra241CMDQV),
    .instance_init = cmdqv_initfn,
    .class_init    = cmdqv_class_init,
};

static void cmdqv_register_types(void)
{
    type_register_static(&cmdqv_type_info);
}

type_init(cmdqv_register_types)
