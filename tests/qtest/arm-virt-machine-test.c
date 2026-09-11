/*
 * Arm virt machine tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"

#define VIRT_FLASH_BANK_SIZE (64 * MiB)

static void test_pflash_property(gconstpointer data)
{
    const char *property = data;
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("qtest-arm-virt-pflash-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, VIRT_FLASH_BANK_SIZE), ==, 0);
    close(fd);

    qts = qtest_initf("-machine virt,%s=flash -accel qtest -nodefaults "
                      "-drive if=none,id=flash,format=raw,file=%s",
                      property, path);
    qtest_quit(qts);

    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("virt")) {
        qtest_add_data_func("/arm/virt/pflash0-property", "pflash0",
                            test_pflash_property);
        qtest_add_data_func("/arm/virt/pflash1-property", "pflash1",
                            test_pflash_property);
    }

    return g_test_run();
}
