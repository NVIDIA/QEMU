/*
 * QEMU Arm RME support
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright Linaro 2026
 */

#include "qemu/osdep.h"

#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "hw/pci/pci.h"
#include "kvm_arm.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qom/object_interfaces.h"
#include "system/confidential-guest-support.h"
#include "system/kvm.h"
#include "system/runstate.h"

#define TYPE_RME_GUEST "rme-guest"
OBJECT_DECLARE_SIMPLE_TYPE(RmeGuest, RME_GUEST)

#define RME_PAGE_SIZE qemu_real_host_page_size()

/*
 * Realms have a split guest-physical address space: the bottom half is private
 * to the realm, and the top half is shared with the host. Within QEMU, we use a
 * merged view of both halves. Most of RAM is private to the guest and not
 * accessible to us, but the guest shares some pages with us.
 *
 * RealmDmaRegion performs remapping of top-half accesses to system memory.
 */
struct RealmDmaRegion {
    IOMMUMemoryRegion parent_obj;
};

#define TYPE_REALM_DMA_REGION "realm-dma-region"
OBJECT_DECLARE_SIMPLE_TYPE(RealmDmaRegion, REALM_DMA_REGION)
OBJECT_DEFINE_SIMPLE_TYPE(RealmDmaRegion, realm_dma_region,
                          REALM_DMA_REGION, IOMMU_MEMORY_REGION);

typedef struct {
    hwaddr base;
    hwaddr size;
    AddressSpace *as;
} RmeRamRegion;

struct RmeGuest {
    ConfidentialGuestSupport parent_obj;
    Notifier rom_load_notifier;
    VMChangeStateEntry *vm_state_handler;
    GSList *ram_regions;
    bool rom_load_notifier_registered;
    bool activated;
    uint8_t ipa_bits;

    RealmDmaRegion *dma_region;
    AddressSpace dma_as;
};

OBJECT_DEFINE_SIMPLE_TYPE_WITH_INTERFACES(RmeGuest, rme_guest, RME_GUEST,
                                          CONFIDENTIAL_GUEST_SUPPORT,
                                          { TYPE_USER_CREATABLE }, { })

static RmeGuest *rme_get_machine_guest(void)
{
    MachineState *machine = MACHINE(qdev_get_machine());

    if (!machine->cgs ||
        !object_dynamic_cast(OBJECT(machine->cgs), TYPE_RME_GUEST)) {
        return NULL;
    }

    return RME_GUEST(machine->cgs);
}

static int rme_populate_range(const RmeRamRegion *region, bool measure,
                              Error **errp)
{
    const hwaddr end = region->base + region->size;
    hwaddr base = region->base;

    if (!address_space_range_is_ram(region->as, region->base, region->size)) {
        error_setg(errp,
                   "cannot populate non-RAM Realm range "
                   "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")",
                   region->base, end);
        return -EINVAL;
    }

    while (base < end) {
        struct kvm_arm_rmi_populate populate_args;
        hwaddr mapped_size = end - base;
        void *host_ua;
        int ret;

        host_ua = address_space_map(region->as, base, &mapped_size, false,
                                    MEMTXATTRS_UNSPECIFIED);
        if (!host_ua) {
            error_setg(errp,
                       "failed to map Realm range "
                       "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")",
                       base, end);
            return -ENOMEM;
        }
        if (!QEMU_IS_ALIGNED(mapped_size, RME_PAGE_SIZE) ||
            !QEMU_PTR_IS_ALIGNED(host_ua, RME_PAGE_SIZE)) {
            error_setg(errp,
                       "Realm range [0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx
                       ") does not have a page-aligned host mapping",
                       base, base + mapped_size);
            address_space_unmap(region->as, host_ua, mapped_size, false, 0);
            return -EINVAL;
        }

        populate_args = (struct kvm_arm_rmi_populate) {
            .base = base,
            .size = mapped_size,
            .source_uaddr = (uintptr_t)host_ua,
            .flags = measure ? KVM_ARM_RMI_POPULATE_FLAGS_MEASURE : 0,
        };

        while (populate_args.size > 0) {
            hwaddr size = populate_args.size;

            ret = kvm_vm_ioctl(kvm_state, KVM_ARM_RMI_POPULATE,
                               &populate_args, 0);
            if (ret) {
                error_setg_errno(errp, -ret,
                    "failed to populate Realm "
                    "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")",
                    region->base, end);
                address_space_unmap(region->as, host_ua, mapped_size,
                                    false, 0);
                return ret;
            }
            if (populate_args.size >= size) {
                error_setg(errp,
                           "KVM made no progress populating Realm range "
                           "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")",
                           region->base, end);
                address_space_unmap(region->as, host_ua, mapped_size,
                                    false, 0);
                return -EIO;
            }
        }

        address_space_unmap(region->as, host_ua, mapped_size, false, 0);
        base += mapped_size;
    }

    return 0;
}

static void rme_populate_ram_region(gpointer data, gpointer err)
{
    Error **errp = err;
    const RmeRamRegion *region = data;

    if (*errp) {
        return;
    }

    rme_populate_range(region, /* measure */ true, errp);
}

static bool rme_coalesce_ram_regions(RmeGuest *guest, Error **errp)
{
    GSList *regions = g_steal_pointer(&guest->ram_regions);
    GSList *result = NULL;
    GSList **tail = &result;
    RmeRamRegion *previous = NULL;

    while (regions) {
        GSList *node = regions;
        RmeRamRegion *region = node->data;
        hwaddr region_end = region->base + region->size;

        regions = regions->next;
        node->next = NULL;

        if (previous && region->base < previous->base + previous->size) {
            if (region->as != previous->as) {
                error_setg(errp,
                           "overlapping Realm ranges use different address "
                           "spaces at GPA 0x%" HWADDR_PRIx,
                           region->base);
                g_slist_free_full(node, g_free);
                g_slist_free_full(regions, g_free);
                g_slist_free_full(result, g_free);
                return false;
            }
            previous->size = MAX(previous->base + previous->size,
                                 region_end) - previous->base;
            g_free(region);
            g_slist_free_1(node);
            continue;
        }

        if (previous && region->base == previous->base + previous->size &&
            region->as == previous->as) {
            previous->size += region->size;
            g_free(region);
            g_slist_free_1(node);
            continue;
        }

        *tail = node;
        tail = &node->next;
        previous = region;
    }

    guest->ram_regions = result;
    return true;
}

static void rme_vm_state_change(void *opaque, bool running, RunState state)
{
    RmeGuest *guest = opaque;
    Error *errp = NULL;

    if (!running || guest->activated) {
        return;
    }

    if (rme_coalesce_ram_regions(guest, &errp)) {
        g_slist_foreach(guest->ram_regions, rme_populate_ram_region, &errp);
    }
    g_slist_free_full(g_steal_pointer(&guest->ram_regions), g_free);
    if (errp) {
        error_report_err(errp);
        exit(EXIT_FAILURE);
    }

    guest->activated = true;
    kvm_mark_guest_state_protected();
}

static gint rme_compare_ram_regions(gconstpointer a, gconstpointer b)
{
    const RmeRamRegion *ra = a;
    const RmeRamRegion *rb = b;

    if (ra->base == rb->base) {
        return 0;
    }
    return ra->base < rb->base ? -1 : 1;
}

static void rme_rom_load_notify(Notifier *notifier, void *data)
{
    RmeGuest *guest = container_of(notifier, RmeGuest, rom_load_notifier);
    RmeRamRegion *region;
    RomLoaderNotifyData *rom = data;
    hwaddr end;

    if (rom->addr == -1) {
        /*
         * These blobs (ACPI tables) are not loaded into guest RAM at reset.
         * Instead the firmware will load them via fw_cfg and measure them
         * itself.
         */
        return;
    }
    if (!rom->len) {
        return;
    }
    if (rom->len > HWADDR_MAX - rom->addr ||
        rom->addr + rom->len > HWADDR_MAX - (RME_PAGE_SIZE - 1)) {
        error_report("Realm image at 0x%" HWADDR_PRIx " is too large",
                     rom->addr);
        exit(EXIT_FAILURE);
    }

    end = QEMU_ALIGN_UP(rom->addr + rom->len, RME_PAGE_SIZE);

    region = g_new0(RmeRamRegion, 1);
    region->base = QEMU_ALIGN_DOWN(rom->addr, RME_PAGE_SIZE);
    region->size = end - region->base;
    region->as = rom->as;

    /*
     * The Realm Initial Measurement (RIM) depends on the order in which we
     * initialize and populate the RAM regions. To help a verifier
     * independently calculate the RIM, sort regions by GPA.
     */
    guest->ram_regions = g_slist_insert_sorted(guest->ram_regions, region,
                                               rme_compare_ram_regions);
}

#define KVM_CAP_ARM_RMI_SYSFS_PATH "/sys/module/kvm/parameters/kvm_cap_arm_rmi"

/*
 * Returns the KVM CCA capability number for the running kernel.
 *
 * The capability number is not stable: it shifts whenever other KVM
 * capabilities land ahead of it, so the value in linux-headers only matches
 * hosts built from the same snapshot. NVIDIA kernels export the live value
 * as a module parameter; prefer it and fall back to the compile-time
 * constant.
 */
static unsigned int kvm_arm_rme_get_cap(void)
{
    static unsigned int rme_cap;
    static bool detected;

    if (!detected) {
        FILE *f;
        int cap;

        rme_cap = KVM_CAP_ARM_RMI;

        f = fopen(KVM_CAP_ARM_RMI_SYSFS_PATH, "r");
        if (f) {
            if (fscanf(f, "%d", &cap) == 1 && cap > 0) {
                rme_cap = cap;
            }
            fclose(f);
        }
        detected = true;
    }

    return rme_cap;
}

static int kvm_arm_rme_init(ConfidentialGuestSupport *cgs, Error **errp)
{
    RmeGuest *guest = RME_GUEST(cgs);
    KVMState *s = KVM_STATE(current_accel());
    static Error *rme_mig_blocker;

    if (!kvm_vm_check_extension(s, kvm_arm_rme_get_cap())) {
        error_setg(errp, "VM doesn't support Realms");
        return -ENODEV;
    }

    error_setg(&rme_mig_blocker, "RME: migration is not implemented");
    migrate_add_blocker(&rme_mig_blocker, &error_fatal);

    guest->rom_load_notifier.notify = rme_rom_load_notify;
    rom_add_load_notifier(&guest->rom_load_notifier);
    guest->rom_load_notifier_registered = true;

    /*
     * The realm activation is done last, when the VM starts, after all images
     * have been loaded and all vcpus finalized.
     */
    guest->vm_state_handler =
        qemu_add_vm_change_state_handler(rme_vm_state_change, guest);

    cgs->require_guest_memfd = true;
    cgs->ready = true;
    return 0;
}

void kvm_arm_rme_vcpu_init(ARMCPU *cpu)
{
    if (!rme_get_machine_guest()) {
        return;
    }

    cpu->kvm_rme = true;
}

static void rme_guest_class_init(ObjectClass *oc, const void *data)
{
    ConfidentialGuestSupportClass *klass = CONFIDENTIAL_GUEST_SUPPORT_CLASS(oc);

    klass->kvm_init = kvm_arm_rme_init;
}

static void rme_guest_init(Object *obj)
{
}

static void rme_guest_finalize(Object *obj)
{
    RmeGuest *guest = RME_GUEST(obj);

    if (guest->rom_load_notifier_registered) {
        notifier_remove(&guest->rom_load_notifier);
    }
    if (guest->vm_state_handler) {
        qemu_del_vm_change_state_handler(guest->vm_state_handler);
    }
    g_slist_free_full(guest->ram_regions, g_free);
}

static AddressSpace *rme_dma_get_address_space(PCIBus *bus, void *opaque,
                                               int devfn)
{
    RmeGuest *guest = opaque;

    return &guest->dma_as;
}

static const PCIIOMMUOps rme_dma_ops = {
    .get_address_space = rme_dma_get_address_space,
};

void kvm_arm_rme_init_gpa_space(hwaddr highest_gpa, PCIBus *pci_bus)
{
    RmeGuest *guest = rme_get_machine_guest();
    RealmDmaRegion *dma_region;
    const unsigned int ipa_bits = 64 - clz64(highest_gpa) + 1;

    if (!guest) {
        return;
    }

    assert(ipa_bits < 64);

    /*
     * Setup a DMA translation from the shared top half of the guest-physical
     * address space to our merged view of RAM.
     */
    dma_region = g_new0(RealmDmaRegion, 1);

    memory_region_init_iommu(dma_region, sizeof(*dma_region),
                             TYPE_REALM_DMA_REGION, OBJECT(guest),
                             "realm-dma-region", 1ULL << ipa_bits);
    address_space_init(&guest->dma_as, MEMORY_REGION(dma_region),
                       TYPE_REALM_DMA_REGION);
    guest->dma_region = dma_region;
    guest->ipa_bits = ipa_bits;

    pci_setup_iommu(pci_bus, &rme_dma_ops, guest);
}

static void realm_dma_region_init(Object *obj)
{
}

static IOMMUTLBEntry realm_dma_region_translate(IOMMUMemoryRegion *mr,
                                                hwaddr addr,
                                                IOMMUAccessFlags flag,
                                                int iommu_idx)
{
    RmeGuest *guest = RME_GUEST(memory_region_owner(MEMORY_REGION(mr)));
    const hwaddr shared_bit = 1ULL << (guest->ipa_bits - 1);
    const hwaddr address_mask = shared_bit - 1;
    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr,
        .translated_addr = addr & address_mask,
        /*
         * Somewhat arbitrary granule for users that need one, such as
         * address_space_get_iotlb_entry(). Should be relatively large to
         * avoid frequent TLB misses. It can't be larger than memory region
         * alignment (eg. address_mask) because that would mask the whole
         * address, preventing vhost from finding the correct memory region.
         */
        .addr_mask = 4 * KiB - 1,
        .perm = addr & shared_bit ? IOMMU_RW : IOMMU_NONE,
    };

    return entry;
}

static void realm_dma_region_replay(IOMMUMemoryRegion *mr, IOMMUNotifier *n)
{
    /* Nothing is shared at boot */
}

static void realm_dma_region_finalize(Object *obj)
{
}

static void realm_dma_region_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = realm_dma_region_translate;
    imrc->replay = realm_dma_region_replay;
}
