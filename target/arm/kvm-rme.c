/*
 * QEMU Arm RME support
 *
 * Copyright Linaro 2024
 */

#include "qemu/osdep.h"

#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "kvm_arm.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qom/object_interfaces.h"
#include "system/confidential-guest-support.h"
#include "system/kvm.h"
#include "system/runstate.h"

#define TYPE_RME_GUEST "rme-guest"
OBJECT_DECLARE_SIMPLE_TYPE(RmeGuest, RME_GUEST)

struct RmeGuest {
    ConfidentialGuestSupport parent_obj;
};

OBJECT_DEFINE_SIMPLE_TYPE_WITH_INTERFACES(RmeGuest, rme_guest, RME_GUEST,
                                          CONFIDENTIAL_GUEST_SUPPORT,
                                          { TYPE_USER_CREATABLE }, { })

static RmeGuest *rme_guest;

static int rme_init_cpus(Error **errp)
{
    int ret;
    CPUState *cs;

    /*
     * Now that do_cpu_reset() initialized the boot PC and
     * kvm_cpu_synchronize_post_reset() registered it, we can finalize the REC.
     */
    CPU_FOREACH(cs) {
        ret = kvm_arm_vcpu_finalize(ARM_CPU(cs), KVM_ARM_VCPU_REC);
        if (ret) {
            error_setg_errno(errp, -ret, "failed to finalize vCPU");
            return ret;
        }
    }
    return 0;
}

static int rme_create_realm(Error **errp)
{
    int ret;

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_CREATE_REALM);
    if (ret) {
        error_setg_errno(errp, -ret, "failed to create Realm Descriptor");
        return -1;
    }

    if (rme_init_cpus(errp)) {
        return -1;
    }

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_ACTIVATE_REALM);
    if (ret) {
        error_setg_errno(errp, -ret, "failed to activate realm");
        return -1;
    }

    kvm_mark_guest_state_protected();
    return 0;
}

static void rme_vm_state_change(void *opaque, bool running, RunState state)
{
    Error *err = NULL;

    if (!running) {
        return;
    }

    if (rme_create_realm(&err)) {
        error_propagate_prepend(&error_fatal, err, "RME: ");
    }
}

static void rme_guest_class_init(ObjectClass *oc, const void *data)
{
}

static void rme_guest_init(Object *obj)
{
    if (rme_guest) {
        error_report("a single instance of RmeGuest is supported");
        exit(1);
    }
    rme_guest = RME_GUEST(obj);
}

static void rme_guest_finalize(Object *obj)
{
}

int kvm_arm_rme_init(MachineState *ms)
{
    static Error *rme_mig_blocker;
    ConfidentialGuestSupport *cgs = ms->cgs;

    if (!rme_guest) {
        return 0;
    }

    if (!cgs) {
        error_report("missing -machine confidential-guest-support parameter");
        return -EINVAL;
    }

    if (!kvm_check_extension(kvm_state, KVM_CAP_ARM_RME)) {
        return -ENODEV;
    }

    error_setg(&rme_mig_blocker, "RME: migration is not implemented");
    migrate_add_blocker(&rme_mig_blocker, &error_fatal);

    /*
     * The realm activation is done last, when the VM starts, after all images
     * have been loaded and all vcpus finalized.
     */
    qemu_add_vm_change_state_handler(rme_vm_state_change, NULL);

    cgs->require_guest_memfd = true;
    cgs->ready = true;
    return 0;
}

int kvm_arm_rme_vcpu_init(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);

    if (rme_guest) {
        cpu->kvm_rme = true;
        cpu->kvm_init_features[0] |= (1 << KVM_ARM_VCPU_REC);
    }
    return 0;
}

int kvm_arm_rme_vm_type(MachineState *ms)
{
    if (rme_guest) {
        return KVM_VM_TYPE_ARM_REALM;
    }
    return 0;
}
