/*
 * Arm RME device-assignment ABI used by QEMU
 *
 * Copyright (c) 2026 NVIDIA Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * These definitions mirror the RHI device-assignment interface and the
 * request payloads consumed by the Arm RME IOMMUFD implementation. Keep the
 * values and layouts synchronized with the corresponding Arm specifications
 * and kernel interfaces.
 */

#ifndef HW_ARM_RME_DA_H
#define HW_ARM_RME_DA_H

#define ARM_SMCCC_FAST_CALL                 1U
#define ARM_SMCCC_SMC_64                    1U
#define ARM_SMCCC_TYPE_SHIFT                31
#define ARM_SMCCC_CALL_CONV_SHIFT           30
#define ARM_SMCCC_OWNER_MASK                0x3fU
#define ARM_SMCCC_OWNER_SHIFT               24
#define ARM_SMCCC_FUNC_MASK                 0xffffU
#define ARM_SMCCC_OWNER_STANDARD_HYP        5U

#define ARM_SMCCC_CALL_VAL(type, calling_convention, owner, func_num) \
    (((type) << ARM_SMCCC_TYPE_SHIFT) |                              \
     ((calling_convention) << ARM_SMCCC_CALL_CONV_SHIFT) |           \
     (((owner) & ARM_SMCCC_OWNER_MASK) << ARM_SMCCC_OWNER_SHIFT) |   \
     ((func_num) & ARM_SMCCC_FUNC_MASK))

#define SMC_RHI_CALL(func)                                      \
    ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,   \
                       ARM_SMCCC_OWNER_STANDARD_HYP, (func))

#define RHI_DA_FEATURE_OBJECT_SIZE             0b000001
#define RHI_DA_FEATURE_OBJECT_READ             0b000010
#define RHI_DA_FEATURE_VDEV_CONTINUE           0b000100
#define RHI_DA_FEATURE_VDEV_GET_MEASUREMENT    0b001000
#define RHI_DA_FEATURE_VDEV_GET_INTF_REPORT    0b010000
#define RHI_DA_FEATURE_VDEV_SET_TDI_STATE      0b100000

#define RHI_DA_BASE_FEATURE \
    (RHI_DA_FEATURE_OBJECT_SIZE | RHI_DA_FEATURE_OBJECT_READ |       \
     RHI_DA_FEATURE_VDEV_GET_INTF_REPORT |                           \
     RHI_DA_FEATURE_VDEV_GET_MEASUREMENT |                           \
     RHI_DA_FEATURE_VDEV_SET_TDI_STATE)

#define RHI_DA_FEATURES                        SMC_RHI_CALL(0x004b)
#define RHI_DA_OBJECT_SIZE                     SMC_RHI_CALL(0x004c)
#define RHI_DA_OBJECT_READ                     SMC_RHI_CALL(0x004d)
#define RHI_DA_VDEV_GET_MEASUREMENTS           SMC_RHI_CALL(0x0052)
#define RHI_DA_VDEV_GET_INTERFACE_REPORT       SMC_RHI_CALL(0x0053)
#define RHI_DA_VDEV_SET_TDI_STATE              SMC_RHI_CALL(0x0054)

#define SMCCC_RET_NOT_SUPPORTED                -1

#define RHI_DA_SUCCESS                         0x0
#define RHI_DA_INCOMPLETE                      0x1
#define RHI_DA_ERROR_DATA_NOT_AVAILABLE        0x2
#define RHI_DA_ERROR_INVALID_VDEV_ID            0x3
#define RHI_DA_ERROR_INVALID_OBJECT             0x4
#define RHI_DA_ERROR_INPUT                      0x5
#define RHI_DA_ERROR_DEVICE                     0x6
#define RHI_DA_ERROR_INVALID_OFFSET             0x7
#define RHI_DA_ERROR_ACCESS_FAILED              0x8
#define RHI_DA_ERROR_BUSY                       0x9

#define RHI_DA_TDI_CONFIG_UNLOCKED              0x0
#define RHI_DA_TDI_CONFIG_LOCKED                0x1
#define RHI_DA_TDI_CONFIG_RUN                   0x2

#define PCI_TSM_REQ_INFO                        0
#define PCI_TSM_REQ_STATE_CHANGE                1

/* Guest request operation numbers from the RME device-assignment ABI. */
#define __RHI_DA_OBJECT_SIZE                    0x1
#define __RHI_DA_OBJECT_READ                    0x2
#define __RHI_DA_VDEV_UPDATE_INTERFACE_REPORT  0x3
#define __RHI_DA_VDEV_UPDATE_MEASUREMENTS       0x4
#define __REC_DA_VDEV_MAP                       0x5
#define __RHI_DA_VDEV_SET_TDI_STATE             0x6

struct rhi_vdev_measurement_params {
    union {
        uint64_t flags;
        uint8_t padding0[256];
    };
    uint8_t nonce[32];
};

struct arm64_vdev_object_size_guest_req {
    uint32_t req_type;
    uint32_t object_type;
};

struct arm64_vdev_object_read_guest_req {
    uint32_t req_type;
    uint32_t object_type;
    uint64_t offset QEMU_ALIGNED(8);
};

struct arm64_vdev_device_measurement_guest_req {
    uint32_t req_type;
    uint32_t reserved;
    uint64_t flags QEMU_ALIGNED(8);
    uint64_t nonce QEMU_ALIGNED(8);
};

struct arm64_vdev_device_memmap_guest_req {
    uint32_t req_type;
    uint32_t reserved;
    uint64_t gpa_base QEMU_ALIGNED(8);
    uint64_t gpa_top QEMU_ALIGNED(8);
    uint64_t pa_base QEMU_ALIGNED(8);
};

struct arm64_vdev_set_tdi_state_guest_req {
    uint32_t req_type;
    uint32_t tdi_state;
};

QEMU_BUILD_BUG_ON(sizeof(struct rhi_vdev_measurement_params) != 288);
QEMU_BUILD_BUG_ON(sizeof(struct arm64_vdev_object_size_guest_req) != 8);
QEMU_BUILD_BUG_ON(sizeof(struct arm64_vdev_object_read_guest_req) != 16);
QEMU_BUILD_BUG_ON(sizeof(struct arm64_vdev_device_measurement_guest_req) != 24);
QEMU_BUILD_BUG_ON(sizeof(struct arm64_vdev_device_memmap_guest_req) != 32);
QEMU_BUILD_BUG_ON(sizeof(struct arm64_vdev_set_tdi_state_guest_req) != 8);

#endif /* HW_ARM_RME_DA_H */
