/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "kvm-rme-rom.h"

/*
 * Decide whether the complete ROM range is private before rom_reset() can
 * write any part of it through the host mapping. Adjacent guest_memfd RAM
 * regions are allowed; mixed backing and holes must not fall through to the
 * ordinary ROM writer.
 */
int rme_rom_private_range(hwaddr base, hwaddr size, bool *private_ram,
                                 Error **errp)
{
    hwaddr start = base;
    bool have_private = false;
    bool have_other = false;

    if (size > HWADDR_MAX - base) {
        error_setg(errp, "Realm image at 0x%" HWADDR_PRIx " is too large",
                   base);
        return -EINVAL;
    }

    while (size) {
        MemoryRegionSection section =
            memory_region_find(get_system_memory(), start, size);
        hwaddr section_size;
        hwaddr skipped;

        if (!section.mr) {
            have_other = true;
            break;
        }

        skipped = section.offset_within_address_space - start;
        section_size = int128_get64(section.size);
        assert(skipped <= size && section_size &&
               section_size <= size - skipped);
        have_other |= skipped != 0;

        if (memory_region_is_ram(section.mr) &&
            memory_region_has_guest_memfd(section.mr)) {
            have_private = true;
        } else {
            have_other = true;
        }
        memory_region_unref(section.mr);

        if (have_private && have_other) {
            break;
        }
        start += skipped + section_size;
        size -= skipped + section_size;
    }

    if (have_private && have_other) {
        error_setg(errp, "Realm image at 0x%" HWADDR_PRIx
                   " mixes guest_memfd RAM with other memory or holes", base);
        return -EINVAL;
    }

    *private_ram = have_private;
    return 0;
}
