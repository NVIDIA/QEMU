/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARM_KVM_RME_ROM_H
#define ARM_KVM_RME_ROM_H

#include "system/memory.h"

/* Reject mixed ROM backing before any direct host access to private RAM. */
int rme_rom_private_range(hwaddr base, hwaddr size, bool *private_ram,
                         Error **errp);

#endif
