/*
 * tpm_log.c - Event log as described by the Trusted Computing Group (TCG)
 *
 * Copyright (c) 2024 Linaro Ltd.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * Create an event log in the format specified by:
 *
 *  TCG PC Client Platform Firmware Profile Specification
 *  Level 00 Version 1.06 Revision 52
 *  Family “2.0”
 */

#include "qemu/osdep.h"

#include "crypto/hash.h"
#include "hw/tpm/tpm_log.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qom/object_interfaces.h"
#include "system/address-spaces.h"
#include "system/memory.h"

/*
 * Legacy structure used only in the first event in the log, for compatibility
 */
struct TcgPcClientPcrEvent {
        uint32_t pcr_index;
        uint32_t event_type;
        uint8_t  digest[20];
        uint32_t event_data_size;
        uint8_t  event[];
} QEMU_PACKED;

struct TcgEfiSpecIdEvent {
        uint8_t  signature[16];
        uint32_t platform_class;
        uint8_t  family_version_minor;
        uint8_t  family_version_major;
        uint8_t  spec_revision;
        uint8_t  uintn_size;
        uint32_t number_of_algorithms; /* 1 */
        /*
         * For now we declare a single algo, but if we want UEFI to reuse this
         * header then we'd need to add entries here for all algos supported by
         * UEFI (and expand the digest field for EV_NO_ACTION).
         */
        uint16_t algorithm_id;
        uint16_t digest_size;
        uint8_t  vendor_info_size;
        uint8_t  vendor_info[];
} QEMU_PACKED;

struct TcgPcrEvent2Head {
        uint32_t pcr_index;
        uint32_t event_type;
        /* variable-sized digests */
        uint8_t  digests[];
} QEMU_PACKED;

struct TcgPcrEvent2Tail {
        uint32_t event_size;
        uint8_t  event[];
} QEMU_PACKED;

struct TpmlDigestValues {
        uint32_t count;     /* 1 */
        uint16_t hash_alg;
        uint8_t  digest[];
} QEMU_PACKED;

struct TpmLog {
    Object parent_obj;

    TpmLogDigestAlgo digest_algo;
    size_t max_size;
    uint64_t load_addr;

    uint16_t tcg_algo;
    GByteArray *content;
    uint8_t *digest;
    size_t digest_size;
};

OBJECT_DEFINE_SIMPLE_TYPE(TpmLog, tpm_log, TPM_LOG, OBJECT)

static void tpm_log_init(Object *obj)
{
    TpmLog *log = TPM_LOG(obj);

    log->digest_algo = TPM_LOG_DIGEST_ALGO_SHA256;
}

static void tpm_log_destroy(TpmLog *log)
{
    if (!log->content) {
        return;
    }
    g_free(log->digest);
    log->digest = NULL;
    g_byte_array_free(log->content, /* free_segment */ true);
    log->content = NULL;
}

static void tpm_log_finalize(Object *obj)
{
    tpm_log_destroy(TPM_LOG(obj));
}

static int tpm_log_get_digest_algo(Object *obj, Error **errp)
{
    TpmLog *log = TPM_LOG(obj);

    return log->digest_algo;
}

static void tpm_log_set_digest_algo(Object *obj, int algo, Error **errp)
{
    TpmLog *log = TPM_LOG(obj);

    if (log->content != NULL) {
        error_setg(errp, "cannot set digest algo after log creation");
        return;
    }

    log->digest_algo = algo;
}

static void tpm_log_get_max_size(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    TpmLog *log = TPM_LOG(obj);
    uint64_t value = log->max_size;

    visit_type_uint64(v, name, &value, errp);
}

static void tpm_log_get_load_addr(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    TpmLog *log = TPM_LOG(obj);
    uint64_t value = log->load_addr;

    visit_type_uint64(v, name, &value, errp);
}

static void tpm_log_set_load_addr(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    TpmLog *log = TPM_LOG(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    log->load_addr = value;
}


static void tpm_log_class_init(ObjectClass *oc, const void *data)
{
    object_class_property_add_enum(oc, "digest-algo",
                                   "TpmLogDigestAlgo",
                                   &TpmLogDigestAlgo_lookup,
                                   tpm_log_get_digest_algo,
                                   tpm_log_set_digest_algo);
    object_class_property_set_description(oc, "digest-algo",
            "Algorithm used to hash blobs added as events ('sha256', 'sha512')");

    /* max_size is set while allocating the log in tpm_log_create */
    object_class_property_add(oc, "max-size", "uint64", tpm_log_get_max_size,
                              NULL, NULL, NULL);
    object_class_property_set_description(oc, "max-size",
            "Maximum size of the log, reserved in guest memory");

    object_class_property_add(oc, "load-addr", "uint64", tpm_log_get_load_addr,
                              tpm_log_set_load_addr, NULL, NULL);
    object_class_property_set_description(oc, "load-addr",
            "Base address of the log in guest memory");
}

int tpm_log_create(TpmLog *log, size_t max_size, Error **errp)
{
    struct TcgEfiSpecIdEvent event;
    struct TcgPcClientPcrEvent header = {
        .pcr_index = 0,
        .event_type = cpu_to_le32(TCG_EV_NO_ACTION),
        .digest = {0},
        .event_data_size = cpu_to_le32(sizeof(event)),
    };

    log->content = g_byte_array_sized_new(max_size);
    log->max_size = max_size;

    switch (log->digest_algo) {
    case TPM_LOG_DIGEST_ALGO_SHA256:
        log->tcg_algo = TCG_ALG_SHA256;
        log->digest_size = TCG_ALG_SHA256_DIGEST_SIZE;
        break;
    case TPM_LOG_DIGEST_ALGO_SHA512:
        log->tcg_algo = TCG_ALG_SHA512;
        log->digest_size = TCG_ALG_SHA512_DIGEST_SIZE;
        break;
    default:
        g_assert_not_reached();
    }

    log->digest = g_malloc0(log->digest_size);

    event = (struct TcgEfiSpecIdEvent) {
        .signature = "Spec ID Event03",
        .platform_class = 0,
        .family_version_minor = 0,
        .family_version_major = 2,
        .spec_revision = 106,
        .uintn_size = 2, /* UINT64 */
        .number_of_algorithms = cpu_to_le32(1),
        .algorithm_id = cpu_to_le16(log->tcg_algo),
        .digest_size = cpu_to_le16(log->digest_size),
        .vendor_info_size = 0,
    };

    g_byte_array_append(log->content, (guint8 *)&header, sizeof(header));
    g_byte_array_append(log->content, (guint8 *)&event, sizeof(event));
    return 0;
}

int tpm_log_add_event(TpmLog *log, uint32_t event_type, const uint8_t *event,
                      size_t event_size, const uint8_t *data, size_t data_size,
                      Error **errp)
{
    int digests = 0;
    size_t rollback_len;
    struct TcgPcrEvent2Head header = {
        .pcr_index = 0,
        .event_type = cpu_to_le32(event_type),
    };
    struct TpmlDigestValues digest_header = {0};
    struct TcgPcrEvent2Tail tail = {
        .event_size = cpu_to_le32(event_size),
    };

    if (log->content == NULL) {
        error_setg(errp, "event log is not initialized");
        return -EINVAL;
    }
    rollback_len = log->content->len;

    g_byte_array_append(log->content, (guint8 *)&header, sizeof(header));

    if (data) {
        QCryptoHashAlgo qc_algo;

        digest_header.hash_alg = cpu_to_le16(log->tcg_algo);
        switch (log->digest_algo) {
        case TPM_LOG_DIGEST_ALGO_SHA256:
            qc_algo = QCRYPTO_HASH_ALGO_SHA256;
            break;
        case TPM_LOG_DIGEST_ALGO_SHA512:
            qc_algo = QCRYPTO_HASH_ALGO_SHA512;
            break;
        default:
            g_assert_not_reached();
        }
        if (qcrypto_hash_bytes(qc_algo, (const char *)data, data_size,
                               &log->digest, &log->digest_size, errp)) {
            goto err_rollback;
        }
        digests = 1;
    } else if (event_type == TCG_EV_NO_ACTION) {
        /* EV_NO_ACTION contains empty digests for each supported algo */
        memset(log->digest, 0, log->digest_size);
        digest_header.hash_alg = 0;
        digests = 1;
    }

    if (digests) {
        digest_header.count = cpu_to_le32(digests);
        g_byte_array_append(log->content, (guint8 *)&digest_header,
                            sizeof(digest_header));
        g_byte_array_append(log->content, log->digest, log->digest_size);
    } else {
        /* Add an empty digests list */
        g_byte_array_append(log->content, (guint8 *)&digest_header.count,
                            sizeof(digest_header.count));
    }

    g_byte_array_append(log->content, (guint8 *)&tail, sizeof(tail));
    g_byte_array_append(log->content, event, event_size);

    if (log->content->len > log->max_size) {
        error_setg(errp, "event log exceeds max size");
        goto err_rollback;
    }

    return 0;

err_rollback:
    g_byte_array_set_size(log->content, rollback_len);
    return -1;
}

int tpm_log_write_and_close(TpmLog *log, Error **errp)
{
    int ret;

    if (!log->content) {
        error_setg(errp, "event log is not initialized");
        return -1;
    }

    ret = address_space_write_rom(&address_space_memory, log->load_addr,
                                  MEMTXATTRS_UNSPECIFIED, log->content->data,
                                  log->content->len);
    if (ret) {
        error_setg(errp, "cannot load log into memory");
        return -1;
    }

    tpm_log_destroy(log);
    return ret;
}
