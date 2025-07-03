/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-arm.h"


CcaCapability *qmp_query_cca_capabilities(Error **errp)
{
    error_setg(errp, "ARM CCA is not available on this target");
    return NULL;
}
