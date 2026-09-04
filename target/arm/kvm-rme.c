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
#include "qemu/memalign.h"
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
    uint8_t *data;
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

static void rme_ram_region_free(gpointer opaque)
{
    RmeRamRegion *region = opaque;

    qemu_vfree(region->data);
    g_free(region);
}

static uint8_t *rme_alloc_page_buffer(hwaddr size, Error **errp)
{
    size_t buffer_size = size;
    uint8_t *buffer;

    if (buffer_size != size) {
        error_setg(errp, "Realm population range is too large");
        return NULL;
    }

    buffer = qemu_try_memalign(RME_PAGE_SIZE, buffer_size);
    if (!buffer) {
        error_setg_errno(errp, ENOMEM,
                         "failed to allocate Realm population buffer");
        return NULL;
    }
    memset(buffer, 0, buffer_size);
    return buffer;
}

static int rme_populate_range(const RmeRamRegion *region, bool measure,
                              Error **errp)
{
    const hwaddr end = region->base + region->size;
    struct kvm_arm_rmi_populate populate_args = {
        .base = region->base,
        .size = region->size,
        .source_uaddr = (uintptr_t)region->data,
        .flags = measure ? KVM_ARM_RMI_POPULATE_FLAGS_MEASURE : 0,
    };
    int ret;

    if (!QEMU_IS_ALIGNED(region->base, RME_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(region->size, RME_PAGE_SIZE) ||
        !QEMU_PTR_IS_ALIGNED(region->data, RME_PAGE_SIZE)) {
        error_setg(errp,
                   "Realm range [0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx
                   ") is not page-aligned",
                   region->base, end);
        return -EINVAL;
    }

    /*
     * Keep KVM's attributes, QEMU's RamDiscardManager state, and the shared
     * host mapping synchronized before handing the private copy to KVM.
     */
    ret = kvm_convert_memory(region->base, region->size, true);
    if (ret) {
        error_setg_errno(errp, -ret,
                         "failed to configure private Realm range "
                         "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")",
                         region->base, end);
        return ret;
    }

    while (populate_args.size > 0) {
        hwaddr size = populate_args.size;

        ret = kvm_vm_ioctl(kvm_state, KVM_ARM_RMI_POPULATE, &populate_args, 0);
        if (ret) {
            error_setg_errno(errp, -ret,
                             "failed to populate Realm "
                             "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")",
                             region->base, end);
            return ret;
        }
        if (populate_args.size >= size) {
            error_setg(errp,
                       "KVM made no progress populating Realm range "
                       "[0x%" HWADDR_PRIx ", 0x%" HWADDR_PRIx ")",
                       region->base, end);
            return -EIO;
        }
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
    RmeRamRegion *merged = NULL;
    hwaddr previous_end = 0;
    bool have_previous = false;

    while (regions) {
        GSList *node = regions;
        RmeRamRegion *region = node->data;
        RmeRamRegion *new_region;
        uint8_t *new_data;
        hwaddr region_end;
        hwaddr start;
        hwaddr end;

        regions = regions->next;
        g_slist_free_1(node);

        if (region->size > HWADDR_MAX - region->base) {
            error_setg(errp, "Realm image at 0x%" HWADDR_PRIx
                       " is too large", region->base);
            goto error;
        }
        region_end = region->base + region->size;
        if (region_end > HWADDR_MAX - (RME_PAGE_SIZE - 1)) {
            error_setg(errp, "Realm image at 0x%" HWADDR_PRIx
                       " cannot be page-aligned", region->base);
            goto error;
        }
        if (have_previous && region->base < previous_end) {
            error_setg(errp, "overlapping Realm images at GPA 0x%"
                       HWADDR_PRIx, region->base);
            goto error;
        }

        start = QEMU_ALIGN_DOWN(region->base, RME_PAGE_SIZE);
        end = QEMU_ALIGN_UP(region_end, RME_PAGE_SIZE);

        if (merged && start <= merged->base + merged->size) {
            hwaddr merged_end = merged->base + merged->size;

            if (end > merged_end) {
                new_data = rme_alloc_page_buffer(end - merged->base, errp);
                if (!new_data) {
                    goto error;
                }
                memcpy(new_data, merged->data, merged->size);
                qemu_vfree(merged->data);
                merged->data = new_data;
                merged->size = end - merged->base;
            }
            memcpy(merged->data + (region->base - merged->base),
                   region->data, region->size);
        } else {
            new_region = g_new0(RmeRamRegion, 1);
            new_region->base = start;
            new_region->size = end - start;
            new_region->data = rme_alloc_page_buffer(new_region->size, errp);
            if (!new_region->data) {
                g_free(new_region);
                goto error;
            }
            memcpy(new_region->data + (region->base - start),
                   region->data, region->size);
            result = g_slist_append(result, new_region);
            merged = new_region;
        }

        previous_end = region_end;
        have_previous = true;
        rme_ram_region_free(region);
        continue;

error:
        rme_ram_region_free(region);
        g_slist_free_full(regions, rme_ram_region_free);
        g_slist_free_full(result, rme_ram_region_free);
        return false;
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
    g_slist_free_full(g_steal_pointer(&guest->ram_regions),
                      rme_ram_region_free);
    if (errp) {
        error_report_err(errp);
        exit(EXIT_FAILURE);
    }

    guest->activated = true;
    if (guest->rom_load_notifier_registered) {
        notifier_remove(&guest->rom_load_notifier);
        guest->rom_load_notifier_registered = false;
    }
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
    GSList *entry;
    RmeRamRegion *region;
    RomLoaderNotifyData *rom = data;
    uint8_t *copy;

    if (guest->activated) {
        return;
    }

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
    if (!rom->data) {
        error_report("Realm image at 0x%" HWADDR_PRIx " has no data",
                     rom->addr);
        exit(EXIT_FAILURE);
    }
    if (rom->len > HWADDR_MAX - rom->addr) {
        error_report("Realm image at 0x%" HWADDR_PRIx " is too large",
                     rom->addr);
        exit(EXIT_FAILURE);
    }

    copy = qemu_try_memalign(RME_PAGE_SIZE, rom->len);
    if (!copy) {
        error_report("failed to copy Realm image at 0x%" HWADDR_PRIx,
                     rom->addr);
        exit(EXIT_FAILURE);
    }
    memcpy(copy, rom->data, rom->len);

    /*
     * rom_reset() notifies listeners on every reset. Before the Realm is
     * activated, replace a previous snapshot of the same ROM instead of
     * adding a duplicate which would later look like an overlapping image.
     */
    for (entry = guest->ram_regions; entry; entry = entry->next) {
        region = entry->data;
        if (region->base == rom->addr && region->size == rom->len) {
            qemu_vfree(region->data);
            region->data = copy;
            return;
        }
    }

    region = g_new0(RmeRamRegion, 1);
    region->base = rom->addr;
    region->size = rom->len;
    region->data = copy;

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
 *
 * FIXME: this is a downstream-only workaround and must be dropped before the
 * series is posted upstream, where KVM_CAP_ARM_RMI will have a fixed value.
 */
#define KVM_CAP_ARM_RMI_MAX 4095

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
            /*
             * Bound the value: a garbage module parameter would otherwise
             * turn into a wild KVM_CHECK_EXTENSION argument.
             */
            if (fscanf(f, "%d", &cap) == 1 &&
                cap > 0 && cap <= KVM_CAP_ARM_RMI_MAX) {
                rme_cap = cap;
            } else {
                warn_report("ignoring out-of-range %s, falling back to "
                            "KVM_CAP_ARM_RMI=%d",
                            KVM_CAP_ARM_RMI_SYSFS_PATH, KVM_CAP_ARM_RMI);
            }
            fclose(f);
        } else {
            warn_report("cannot read %s: %s; falling back to "
                        "KVM_CAP_ARM_RMI=%d",
                        KVM_CAP_ARM_RMI_SYSFS_PATH, strerror(errno),
                        KVM_CAP_ARM_RMI);
        }
        detected = true;
    }

    return rme_cap;
}

bool kvm_arm_rme_available(void)
{
    return kvm_enabled() &&
           kvm_vm_check_extension(kvm_state, kvm_arm_rme_get_cap());
}

static int kvm_arm_rme_init(ConfidentialGuestSupport *cgs, Error **errp)
{
    RmeGuest *guest = RME_GUEST(cgs);
    static Error *rme_mig_blocker;

    if (!kvm_arm_rme_available()) {
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
    cgs->assigned_device_memory = true;
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
    g_slist_free_full(guest->ram_regions, rme_ram_region_free);
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

AddressSpace *kvm_arm_rme_get_dma_as(void)
{
    RmeGuest *guest = rme_get_machine_guest();

    return guest && guest->dma_region ? &guest->dma_as : NULL;
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
        /*
         * Firmware can use the canonical IPA for a page that it has made
         * shared with the RMM, while Linux DMA addresses carry shared_bit.
         * Both forms refer to the same host-visible RAM alias.
         */
        .perm = IOMMU_RW,
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
