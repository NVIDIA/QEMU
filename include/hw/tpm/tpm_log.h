#ifndef QEMU_TPM_LOG_H
#define QEMU_TPM_LOG_H

#include "qom/object.h"
#include "system/tpm.h"

/*
 * Defined in: TCG Algorithm Registry
 * Family 2.0 Level 00 Revision 01.34
 *
 * (Here TCG stands for Trusted Computing Group)
 */
#define TCG_ALG_SHA256  0xB
#define TCG_ALG_SHA512  0xD

/* Size of a digest in bytes */
#define TCG_ALG_SHA256_DIGEST_SIZE      32
#define TCG_ALG_SHA512_DIGEST_SIZE      64

/*
 * Defined in: TCG PC Client Platform Firmware Profile Specification
 * Version 1.06 revision 52
 */
#define TCG_EV_NO_ACTION                        0x00000003
#define TCG_EV_EVENT_TAG                        0x00000006
#define TCG_EV_POST_CODE2                       0x00000013
#define TCG_EV_EFI_PLATFORM_FIRMWARE_BLOB2      0x8000000A

struct UefiPlatformFirmwareBlob2Head {
        uint8_t blob_description_size;
        uint8_t blob_description[];
} __attribute__((packed));

struct UefiPlatformFirmwareBlob2Tail {
        uint64_t blob_base;
        uint64_t blob_size;
} __attribute__((packed));

#define TYPE_TPM_LOG "tpm-log"

OBJECT_DECLARE_SIMPLE_TYPE(TpmLog, TPM_LOG)

/**
 * tpm_log_create - Create the event log
 * @log: the log object
 * @max_size: maximum size of the log. Adding an event past that size will
 *            return an error
 * @errp: pointer to a NULL-initialized error object
 *
 * Allocate the event log and create the initial entry (Spec ID Event03)
 * describing the log format.
 *
 * Returns: 0 on success, -1 on error
 */
int tpm_log_create(TpmLog *log, size_t max_size, Error **errp);

/**
 * tpm_log_add_event - Append an event to the log
 * @log: the log object
 * @event_type: the `eventType` field in TCG_PCR_EVENT2
 * @event: the `event` field in TCG_PCR_EVENT2
 * @event_size: the `eventSize` field in TCG_PCR_EVENT2
 * @data: content to be hashed into the event digest. May be NULL.
 * @data_size: size of @data. Should be zero when @data is NULL.
 * @errp: pointer to a NULL-initialized error object
 *
 * Add a TCG_PCR_EVENT2 event to the event log. Depending on the event type, a
 * data buffer may be hashed into the event digest (for example
 * TCG_EV_EFI_PLATFORM_FIRMWARE_BLOB2 contains a digest of the blob.)
 *
 * Returns: 0 on success, -1 on error
 */
int tpm_log_add_event(TpmLog *log, uint32_t event_type, const uint8_t *event,
                      size_t event_size, const uint8_t *data, size_t data_size,
                      Error **errp);

/**
 * tpm_log_write_and_close - Move the log to guest memory
 * @log: the log object
 * @errp: pointer to a NULL-initialized error object
 *
 * Write the log into memory, at the address set in the load-addr property.
 * After this operation, the log is not writable anymore.
 *
 * Return: 0 on success, -1 on error
 */
int tpm_log_write_and_close(TpmLog *log, Error **errp);

#endif
