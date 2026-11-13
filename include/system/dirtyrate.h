/*
 * dirty page rate helper functions
 *
 * Copyright (c) 2022 CHINA TELECOM CO.,LTD.
 *
 * Authors:
 *  Hyman Huang(黄勇) <huangy81@chinatelecom.cn>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_DIRTYRATE_H
#define QEMU_DIRTYRATE_H

#include "qapi/qapi-types-migration.h"

typedef struct VcpuStat {
    int nvcpu; /* number of vcpu */
    DirtyRateVcpu *rates; /* array of dirty rate for each vcpu */
} VcpuStat;

int64_t vcpu_calculate_dirtyrate(int64_t calc_time_ms,
                                 VcpuStat *stat,
                                 unsigned int flag,
                                 bool one_shot);

/* Reject logging on guest_memfd bindings before starting a service. */
bool dirtyrate_check_logging_supported(Error **errp);

void global_dirty_log_change(unsigned int flag,
                             bool start);
#endif
