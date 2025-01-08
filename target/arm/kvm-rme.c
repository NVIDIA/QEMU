/*
 * QEMU Arm RME support
 *
 * Copyright Linaro 2024
 */

#include "qemu/osdep.h"

#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "hw/loader.h"
#include "hw/pci/pci.h"
#include "kvm_arm.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/base64.h"
#include "qemu/error-report.h"
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
 * For DMA, devices generally target the shared half (top) of the guest address
 * space. Only the devices trusted by the guest (using mechanisms like TDISP for
 * device authentication) can access the bottom half.
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

typedef struct RealmRamDiscardListener {
    MemoryRegion *mr;
    hwaddr offset_within_address_space;
    uint64_t granularity;
    RamDiscardListener listener;
    QLIST_ENTRY(RealmRamDiscardListener) rrdl_next;
} RealmRamDiscardListener;

typedef struct {
    hwaddr base;
    hwaddr size;
} RmeRamRegion;

struct RmeGuest {
    ConfidentialGuestSupport parent_obj;
    Notifier rom_load_notifier;
    GSList *ram_regions;

    char *personalization_value_str;
    uint8_t personalization_value[ARM_RME_CONFIG_RPV_SIZE];
    RmeGuestMeasurementAlgorithm measurement_algo;

    RmeRamRegion init_ram;
    uint8_t ipa_bits;

    RealmDmaRegion *dma_region;
    QLIST_HEAD(, RealmRamDiscardListener) ram_discard_list;
    MemoryListener memory_listener;
    AddressSpace dma_as;
};

OBJECT_DEFINE_SIMPLE_TYPE_WITH_INTERFACES(RmeGuest, rme_guest, RME_GUEST,
                                          CONFIDENTIAL_GUEST_SUPPORT,
                                          { TYPE_USER_CREATABLE }, { })

static RmeGuest *rme_guest;

static int rme_configure_one(RmeGuest *guest, uint32_t cfg, Error **errp)
{
    int ret;
    const char *cfg_str;
    struct arm_rme_config args = {
        .cfg = cfg,
    };

    switch (cfg) {
    case ARM_RME_CONFIG_RPV:
        memcpy(args.rpv, guest->personalization_value, ARM_RME_CONFIG_RPV_SIZE);
        cfg_str = "personalization value";
        break;
    case ARM_RME_CONFIG_HASH_ALGO:
        switch (guest->measurement_algo) {
        case RME_GUEST_MEASUREMENT_ALGORITHM_SHA256:
            args.hash_algo = ARM_RME_CONFIG_HASH_ALGO_SHA256;
            break;
        case RME_GUEST_MEASUREMENT_ALGORITHM_SHA512:
            args.hash_algo = ARM_RME_CONFIG_HASH_ALGO_SHA512;
            break;
        default:
            g_assert_not_reached();
        }
        cfg_str = "hash algorithm";
        break;
    default:
        g_assert_not_reached();
    }

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_CONFIG_REALM, (intptr_t)&args);
    if (ret) {
        error_setg_errno(errp, -ret, "failed to configure %s", cfg_str);
    }
    return ret;
}

static int rme_configure(Error **errp)
{
    int ret;
    size_t option;
    const uint32_t config_options[] = {
        ARM_RME_CONFIG_RPV,
        ARM_RME_CONFIG_HASH_ALGO,
    };

    for (option = 0; option < ARRAY_SIZE(config_options); option++) {
        ret = rme_configure_one(rme_guest, config_options[option], errp);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static int rme_init_ram(RmeRamRegion *ram, Error **errp)
{
    int ret;
    hwaddr start = QEMU_ALIGN_DOWN(ram->base, RME_PAGE_SIZE);
    hwaddr end = QEMU_ALIGN_UP(ram->base + ram->size, RME_PAGE_SIZE);
    struct arm_rme_init_ripas init_args = {
        .base = start,
        .size = end - start,
    };

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_INIT_RIPAS_REALM,
                            (intptr_t)&init_args);
    if (ret) {
        error_setg_errno(errp, -ret,
                         "failed to init RAM [0x%"HWADDR_PRIx", 0x%"HWADDR_PRIx")",
                         start, end);
    }

    return ret;
}

static int rme_populate_range(hwaddr base, size_t size, bool measure,
                              Error **errp)
{
    int ret;
    hwaddr start = QEMU_ALIGN_DOWN(base, RME_PAGE_SIZE);
    hwaddr end = QEMU_ALIGN_UP(base + size, RME_PAGE_SIZE);
    struct arm_rme_populate_realm populate_args = {
        .base = start,
        .size = end - start,
        .flags = measure ? KVM_ARM_RME_POPULATE_FLAGS_MEASURE : 0,
    };

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_POPULATE_REALM,
                            (intptr_t)&populate_args);
    if (ret) {
        error_setg_errno(errp, -ret,
                   "failed to populate realm [0x%"HWADDR_PRIx", 0x%"HWADDR_PRIx")",
                   start, end);
    }
    return ret;
}

static void rme_populate_ram_region(gpointer data, gpointer err)
{
    Error **errp = err;
    const RmeRamRegion *region = data;

    if (*errp) {
        return;
    }

    rme_populate_range(region->base, region->size, /* measure */ true, errp);
}

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

    if (rme_configure(errp)) {
        return -1;
    }

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_ARM_RME, 0,
                            KVM_CAP_ARM_RME_CREATE_REALM);
    if (ret) {
        error_setg_errno(errp, -ret, "failed to create Realm Descriptor");
        return -1;
    }

    if (rme_init_ram(&rme_guest->init_ram, errp)) {
        return -1;
    }

    g_slist_foreach(rme_guest->ram_regions, rme_populate_ram_region, errp);
    g_slist_free_full(g_steal_pointer(&rme_guest->ram_regions), g_free);
    if (*errp) {
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

static char *rme_get_rpv(Object *obj, Error **errp)
{
    RmeGuest *guest = RME_GUEST(obj);

    return g_strdup(guest->personalization_value_str);
}

static void rme_set_rpv(Object *obj, const char *value, Error **errp)
{
    RmeGuest *guest = RME_GUEST(obj);
    g_autofree uint8_t *rpv;
    size_t len;

    rpv = qbase64_decode(value, -1, &len, errp);
    if (!rpv) {
        return;
    }

    if (len != sizeof(guest->personalization_value)) {
        error_setg(errp,
                   "expecting a Realm Personalization Value of size %zu, got %zu\n",
                   sizeof(guest->personalization_value), len);
        return;
    }
    memcpy(guest->personalization_value, rpv, len);

    /* Save the value so we don't need to encode it in the getter */
    g_free(guest->personalization_value_str);
    guest->personalization_value_str = g_strdup(value);
}

static int rme_get_measurement_algo(Object *obj, Error **errp)
{
    RmeGuest *guest = RME_GUEST(obj);

    return guest->measurement_algo;
}

static void rme_set_measurement_algo(Object *obj, int algo, Error **errp)
{
    RmeGuest *guest = RME_GUEST(obj);

    guest->measurement_algo = algo;
}

static void rme_guest_class_init(ObjectClass *oc, const void *data)
{
    object_class_property_add_str(oc, "personalization-value", rme_get_rpv,
                                  rme_set_rpv);
    object_class_property_set_description(oc, "personalization-value",
            "Realm personalization value (64 bytes encodede in base64)");

    object_class_property_add_enum(oc, "measurement-algorithm",
                                   "RmeGuestMeasurementAlgorithm",
                                   &RmeGuestMeasurementAlgorithm_lookup,
                                   rme_get_measurement_algo,
                                   rme_set_measurement_algo);
    object_class_property_set_description(oc, "measurement-algorithm",
            "Realm measurement algorithm ('sha256', 'sha512')");
}

static void rme_guest_init(Object *obj)
{
    if (rme_guest) {
        error_report("a single instance of RmeGuest is supported");
        exit(1);
    }
    rme_guest = RME_GUEST(obj);
    rme_guest->measurement_algo = RME_GUEST_MEASUREMENT_ALGORITHM_SHA512;
}

static void rme_guest_finalize(Object *obj)
{
    memory_listener_unregister(&rme_guest->memory_listener);
}

static gint rme_compare_ram_regions(gconstpointer a, gconstpointer b)
{
        const RmeRamRegion *ra = a;
        const RmeRamRegion *rb = b;

        g_assert(ra->base != rb->base);
        return ra->base < rb->base ? -1 : 1;
}

static void rme_rom_load_notify(Notifier *notifier, void *data)
{
    RmeRamRegion *region;
    RomLoaderNotifyData *rom = data;

    if (rom->addr == -1) {
        /*
         * These blobs (ACPI tables) are not loaded into guest RAM at reset.
         * Instead the firmware will load them via fw_cfg and measure them
         * itself.
         */
        return;
    }

    region = g_new0(RmeRamRegion, 1);
    region->base = rom->addr;
    region->size = rom->len;

    /*
     * The Realm Initial Measurement (RIM) depends on the order in which we
     * initialize and populate the RAM regions. To help a verifier
     * independently calculate the RIM, sort regions by GPA.
     */
    rme_guest->ram_regions = g_slist_insert_sorted(rme_guest->ram_regions,
                                                   region,
                                                   rme_compare_ram_regions);
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

    rme_guest->rom_load_notifier.notify = rme_rom_load_notify;
    rom_add_load_notifier(&rme_guest->rom_load_notifier);

    cgs->require_guest_memfd = true;
    cgs->ready = true;
    return 0;
}

void kvm_arm_rme_init_guest_ram(hwaddr base, size_t size)
{
    if (rme_guest) {
        rme_guest->init_ram.base = base;
        rme_guest->init_ram.size = size;
    }
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

static int rme_ram_discard_notify(RamDiscardListener *rdl,
                                  MemoryRegionSection *section,
                                  bool populate)
{
    hwaddr gpa, next;
    IOMMUTLBEvent event;
    const hwaddr end = section->offset_within_address_space +
                       int128_get64(section->size);
    const hwaddr address_mask = MAKE_64BIT_MASK(0, rme_guest->ipa_bits - 1);
    RealmRamDiscardListener *rrdl = container_of(rdl, RealmRamDiscardListener,
                                                 listener);

    assert(rme_guest->dma_region != NULL);

    event.type = populate ? IOMMU_NOTIFIER_MAP : IOMMU_NOTIFIER_UNMAP;
    event.entry.target_as = &address_space_memory;
    event.entry.perm = populate ? IOMMU_RW : IOMMU_NONE;
    event.entry.addr_mask = rrdl->granularity - 1;

    assert(end <= address_mask);

    /*
     * Create IOMMU mappings from the top half of the address space to the RAM
     * region.
     */
    for (gpa = section->offset_within_address_space; gpa < end; gpa = next) {
        event.entry.iova = gpa + address_mask + 1;
        event.entry.translated_addr = gpa;
        memory_region_notify_iommu(IOMMU_MEMORY_REGION(rme_guest->dma_region),
                                   0, event);

        next = ROUND_UP(gpa + 1, rrdl->granularity);
        next = MIN(next, end);
    }

    return 0;
}

static int rme_ram_discard_notify_populate(RamDiscardListener *rdl,
                                           MemoryRegionSection *section)
{
    return rme_ram_discard_notify(rdl, section, /* populate */ true);
}

static void rme_ram_discard_notify_discard(RamDiscardListener *rdl,
                                          MemoryRegionSection *section)
{
    rme_ram_discard_notify(rdl, section, /* populate */ false);
}

/* Install a RAM discard listener */
static void rme_listener_region_add(MemoryListener *listener,
                                    MemoryRegionSection *section)
{
    RealmRamDiscardListener *rrdl;
    RamDiscardManager *rdm = memory_region_get_ram_discard_manager(section->mr);

    if (!rdm) {
        return;
    }

    rrdl = g_new0(RealmRamDiscardListener, 1);
    rrdl->mr = section->mr;
    rrdl->offset_within_address_space = section->offset_within_address_space;
    rrdl->granularity = ram_discard_manager_get_min_granularity(rdm,
                                                                section->mr);
    QLIST_INSERT_HEAD(&rme_guest->ram_discard_list, rrdl, rrdl_next);

    ram_discard_listener_init(&rrdl->listener,
                              rme_ram_discard_notify_populate,
                              rme_ram_discard_notify_discard, true);
    ram_discard_manager_register_listener(rdm, &rrdl->listener, section);
}

static void rme_listener_region_del(MemoryListener *listener,
                                    MemoryRegionSection *section)
{
    RealmRamDiscardListener *rrdl;
    RamDiscardManager *rdm = memory_region_get_ram_discard_manager(section->mr);

    if (!rdm) {
        return;
    }

    QLIST_FOREACH(rrdl, &rme_guest->ram_discard_list, rrdl_next) {
        if (rrdl->mr == section->mr && rrdl->offset_within_address_space ==
            section->offset_within_address_space) {
            ram_discard_manager_unregister_listener(rdm, &rrdl->listener);
            g_free(rrdl);
            break;
        }
    }
}

static AddressSpace *rme_dma_get_address_space(PCIBus *bus, void *opaque,
                                               int devfn)
{
    return &rme_guest->dma_as;
}

static const PCIIOMMUOps rme_dma_ops = {
    .get_address_space = rme_dma_get_address_space,
};

void kvm_arm_rme_init_gpa_space(hwaddr highest_gpa, PCIBus *pci_bus)
{
    RealmDmaRegion *dma_region;
    const unsigned int ipa_bits = 64 - clz64(highest_gpa) + 1;

    if (!rme_guest) {
        return;
    }

    assert(ipa_bits < 64);

    /*
     * Setup a DMA translation from the shared top half of the guest-physical
     * address space to our merged view of RAM.
     */
    dma_region = g_new0(RealmDmaRegion, 1);

    memory_region_init_iommu(dma_region, sizeof(*dma_region),
                             TYPE_REALM_DMA_REGION, OBJECT(rme_guest),
                             "realm-dma-region", 1ULL << ipa_bits);
    address_space_init(&rme_guest->dma_as, MEMORY_REGION(dma_region),
                       TYPE_REALM_DMA_REGION);
    rme_guest->dma_region = dma_region;

    pci_setup_iommu(pci_bus, &rme_dma_ops, NULL);

    /*
     * Install notifiers to forward RAM discard changes to the IOMMU notifiers
     * (ie. tell VFIO to map shared pages and unmap private ones).
     */
    rme_guest->memory_listener = (MemoryListener) {
        .name = "rme",
        .region_add = rme_listener_region_add,
        .region_del = rme_listener_region_del,
    };
    memory_listener_register(&rme_guest->memory_listener,
                             &address_space_memory);

    rme_guest->ipa_bits = ipa_bits;
}

static void realm_dma_region_init(Object *obj)
{
}

static IOMMUTLBEntry realm_dma_region_translate(IOMMUMemoryRegion *mr,
                                                hwaddr addr,
                                                IOMMUAccessFlags flag,
                                                int iommu_idx)
{
    const hwaddr address_mask = MAKE_64BIT_MASK(0, rme_guest->ipa_bits - 1);
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
