/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Arm RME device-assignment (RHI DA) definitions.
 *
 * WARNING: unlike the rest of linux-headers/, this file is NOT produced by
 * scripts/update-linux-headers.sh -- it is maintained by hand and mirrors
 * arch/arm64/include/uapi/asm/rmi-da.h.  Running the update script will not
 * refresh it.  It lives here only so that the RME device-assignment series
 * stays self-contained; it must be replaced by a proper header import (and
 * the SMCCC function IDs moved out of linux-headers/linux/arm-smccc.h) before
 * this work is posted upstream.
 */
#ifndef TSM_H_
#define TSM_H_

#include <linux/types.h>

#define PCI_TSM_REQ_INFO 0
#define PCI_TSM_REQ_STATE_CHANGE 1

#define RHI_DA_SUCCESS				0x0
#define RHI_DA_INCOMPLETE			0x1
#define RHI_DA_ERROR_DATA_NOT_AVAILABLE		0x2
#define RHI_DA_ERROR_INVALID_VDEV_ID		0x3
#define RHI_DA_ERROR_INVALID_OBJECT		0x4
#define RHI_DA_ERROR_INPUT			0x5
#define RHI_DA_ERROR_DEVICE			0x6
#define RHI_DA_ERROR_INVALID_OFFSET		0x7
#define RHI_DA_ERROR_ACCESS_FAILED		0x8
#define RHI_DA_ERROR_BUSY			0x9

#define RHI_DA_TDI_CONFIG_UNLOCKED		0x0
#define RHI_DA_TDI_CONFIG_LOCKED		0x1
#define RHI_DA_TDI_CONFIG_RUN			0x2

/*
 * Guest request operation numbers. The values must match the kernel UAPI
 * in arch/arm64/include/uapi/asm/rmi-da.h; the request payload structs
 * below mirror that header layout one-for-one.
 */
#define __RHI_DA_OBJECT_SIZE			0x1
#define __RHI_DA_OBJECT_READ			0x2
#define __RHI_DA_VDEV_UPDATE_INTERFACE_REPORT	0x3
#define __RHI_DA_VDEV_UPDATE_MEASUREMENTS	0x4
#define __REC_DA_VDEV_MAP			0x5
#define __RHI_DA_VDEV_SET_TDI_STATE		0x6

struct arm64_vdev_object_size_guest_req {
	__u32 req_type;
	__u32 object_type;
};

struct arm64_vdev_object_read_guest_req {
	__u32 req_type;
	__u32 object_type;
	__aligned_u64 offset;
};

struct arm64_vdev_device_measurement_guest_req {
	__u32 req_type;
	__u32 reserved;
	__aligned_u64 flags;
	__aligned_u64 nonce;
};

struct arm64_vdev_device_memmap_guest_req {
	__u32 req_type;
	__u32 reserved;
	__aligned_u64 gpa_base;
	__aligned_u64 gpa_top;
	__aligned_u64 pa_base;
};

struct arm64_vdev_set_tdi_state_guest_req {
	__u32 req_type;
	__u32 tdi_state;
};

#endif /* TSM_H_ */
