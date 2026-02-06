/*
 * Support QMP command for AARCH64
 *
 */

#include "qemu/osdep.h"
#include "kvm_arm.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-arm.h"
#include "system/kvm.h"

static CcaMeasurementAlgoList *cca_measurement_algo_sections(void)
{
    CcaMeasurementAlgoList *head = NULL, **tail = &head;
    CcaMeasurementAlgo *malgo;

    malgo = g_new0(CcaMeasurementAlgo, 1);
    malgo->measurement_algo = g_malloc(8);
    memcpy(malgo->measurement_algo, "sha256", 7);
    QAPI_LIST_APPEND(tail, malgo);

    malgo = g_new0(CcaMeasurementAlgo, 1);
    malgo->measurement_algo = g_malloc(8);
    memcpy(malgo->measurement_algo, "sha512", 7);
    QAPI_LIST_APPEND(tail, malgo);

    return head;
}

CcaCapability *qmp_query_cca_capabilities(Error **errp)
{
    CcaCapability *info = NULL;

    if (!kvm_enabled()) {
        error_setg(errp, "KVM not enabled");
        return NULL;
    }

    /*
     * Use kvm_arm_rme_get_cap() to get the KVM CCA capability value
     * from the kernel's sysfs module parameter, handling ABI changes
     * between kernel versions gracefully.
     */
    if (!kvm_check_extension(kvm_state, kvm_arm_rme_get_cap())) {
        error_setg(errp, "RME is not enabled in KVM");
        return NULL;
    }

    info = g_new0(CcaCapability, 1);
    info->sections = cca_measurement_algo_sections();

    return info;
}

