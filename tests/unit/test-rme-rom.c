/*
 * Realm ROM range classification without KVM or CCA hardware.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "../../target/arm/kvm-rme-rom.h"

typedef struct TestRegion {
    MemoryRegion mr;
    hwaddr base;
    hwaddr size;
    bool guest_memfd;
} TestRegion;

static MemoryRegion root;
static TestRegion regions[4];
static unsigned int nr_regions;
static unsigned int references;

MemoryRegion *get_system_memory(void)
{
    return &root;
}

/* Return the first intersecting range, as memory_region_find() does. */
MemoryRegionSection memory_region_find(MemoryRegion *mr, hwaddr base,
                                       uint64_t size)
{
    MemoryRegionSection section = { 0 };
    unsigned int i;

    g_assert(mr == &root);
    for (i = 0; i < nr_regions; i++) {
        TestRegion *region = &regions[i];
        hwaddr start = MAX(base, region->base);
        hwaddr end = MIN(base + size, region->base + region->size);

        if (start < end) {
            section.mr = &region->mr;
            section.offset_within_region = start - region->base;
            section.offset_within_address_space = start;
            section.size = int128_make64(end - start);
            references++;
            break;
        }
    }
    return section;
}

void memory_region_unref(MemoryRegion *mr)
{
    if (mr) {
        g_assert_cmpuint(references, >, 0);
        references--;
    }
}

bool memory_region_has_guest_memfd(const MemoryRegion *mr)
{
    unsigned int i;

    for (i = 0; i < nr_regions; i++) {
        if (mr == &regions[i].mr) {
            return regions[i].guest_memfd;
        }
    }
    g_assert_not_reached();
}

static void add_region(hwaddr base, hwaddr size, bool ram, bool guest_memfd)
{
    TestRegion *region = &regions[nr_regions++];

    g_assert_cmpuint(nr_regions, <=, G_N_ELEMENTS(regions));
    *region = (TestRegion) {
        .mr.ram = ram,
        .base = base,
        .size = size,
        .guest_memfd = guest_memfd,
    };
}

static void check_range(hwaddr base, hwaddr size, int expected,
                        bool expected_private)
{
    Error *err = NULL;
    bool private_ram = false;
    int ret = rme_rom_private_range(base, size, &private_ram, &err);

    g_assert_cmpint(ret, ==, expected);
    if (expected) {
        g_assert_nonnull(err);
        error_free(err);
    } else {
        g_assert_null(err);
        g_assert_cmpint(private_ram, ==, expected_private);
    }
    g_assert_cmpuint(references, ==, 0);
    nr_regions = 0;
}

static void test_private(void)
{
    add_region(0x1000, 0x3000, true, true);
    check_range(0x1800, 0x2000, 0, true);
}

static void test_adjacent_private(void)
{
    add_region(0x1000, 0x1000, true, true);
    add_region(0x2000, 0x2000, true, true);
    check_range(0x1800, 0x2000, 0, true);
}

static void test_other(void)
{
    add_region(0x1000, 0x1000, false, false);
    add_region(0x2000, 0x1000, true, false);
    check_range(0x1000, 0x2000, 0, false);
}

static void test_other_then_private(void)
{
    add_region(0x1000, 0x1000, false, false);
    add_region(0x2000, 0x1000, true, true);
    check_range(0x1000, 0x2000, -EINVAL, false);
}

static void test_private_then_other(void)
{
    add_region(0x1000, 0x1000, true, true);
    add_region(0x2000, 0x1000, true, false);
    check_range(0x1000, 0x2000, -EINVAL, false);
}

static void test_leading_hole(void)
{
    add_region(0x2000, 0x1000, true, true);
    check_range(0x1000, 0x2000, -EINVAL, false);
}

static void test_trailing_hole(void)
{
    add_region(0x1000, 0x1000, true, true);
    check_range(0x1000, 0x2000, -EINVAL, false);
}

static void test_internal_hole(void)
{
    add_region(0x1000, 0x1000, true, true);
    add_region(0x3000, 0x1000, true, true);
    check_range(0x1000, 0x3000, -EINVAL, false);
}

static void test_zero_tail(void)
{
    /*
     * Only the first page contains image data; the zero-filled tail is
     * still part of the ROM range and must be checked before host writes.
     */
    add_region(0x1000, 0x1000, false, false);
    add_region(0x2000, 0x1000, true, true);
    check_range(0x1000, 0x2000, -EINVAL, false);
}

static void test_unmapped(void)
{
    check_range(0x1000, 0x2000, 0, false);
}

static void test_overflow(void)
{
    check_range(HWADDR_MAX - 0x1000, 0x2000, -EINVAL, false);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/rme-rom/private", test_private);
    g_test_add_func("/rme-rom/adjacent-private", test_adjacent_private);
    g_test_add_func("/rme-rom/other", test_other);
    g_test_add_func("/rme-rom/other-then-private", test_other_then_private);
    g_test_add_func("/rme-rom/private-then-other", test_private_then_other);
    g_test_add_func("/rme-rom/leading-hole", test_leading_hole);
    g_test_add_func("/rme-rom/trailing-hole", test_trailing_hole);
    g_test_add_func("/rme-rom/internal-hole", test_internal_hole);
    g_test_add_func("/rme-rom/zero-tail", test_zero_tail);
    g_test_add_func("/rme-rom/unmapped", test_unmapped);
    g_test_add_func("/rme-rom/overflow", test_overflow);
    return g_test_run();
}
