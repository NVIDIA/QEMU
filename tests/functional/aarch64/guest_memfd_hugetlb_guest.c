/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Static initramfs PID 1: guest memory integrity and real virtio block I/O. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void fail(const char *what)
{
    printf("FAIL: guest %s: %s\n", what, strerror(errno));
    sync();
    reboot(RB_POWER_OFF);
    for (;;)
        pause();
}

static void modules(void)
{
    char path[256];
    FILE *list = fopen("/modules.list", "r");

    if (!list)
        fail("module list");
    while (fgets(path, sizeof(path), list)) {
        int fd;

        path[strcspn(path, "\n")] = 0;
        fd = open(path, O_RDONLY);
        if (fd < 0)
            fail("open module");
        if (syscall(SYS_finit_module, fd, "", 0) && errno != EEXIST)
            fail("load module");
        close(fd);
    }
    fclose(list);
}

int main(void)
{
    const size_t size = 1UL << 30;
    const size_t words = size / sizeof(uint64_t);
    const size_t io_size = 4UL << 20;
    volatile uint64_t *mem;
    unsigned char *write_buf, *read_buf;
    size_t i;
    int fd, tries;

    if (getpid() != 1) {
        fprintf(stderr, "This program must run as guest PID 1.\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    mkdir("/dev", 0755);
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
        mount("proc", "/proc", "proc", 0, NULL) ||
        mount("sysfs", "/sys", "sysfs", 0, NULL))
        fail("mount pseudo filesystems");
    if (sysconf(_SC_PAGESIZE) != 4096) {
        errno = EINVAL;
        fail("requires 4 KiB base pages");
    }
    printf("PASS: guest booted with 4 KiB base pages\n");
    modules();
    mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        fail("allocate memory");
    /* Prevent guest THP from obscuring the base-page guest test. */
    if (madvise((void *)mem, size, MADV_NOHUGEPAGE) && errno != EINVAL)
        fail("disable guest THP");
    for (i = 0; i < words; i++)
        mem[i] = (uint64_t)i ^ UINT64_C(0x5a3cc3a596699669);
    for (i = 0; i < words; i++) {
        if (mem[i] != ((uint64_t)i ^ UINT64_C(0x5a3cc3a596699669))) {
            errno = EIO;
            fail("memory integrity");
        }
    }
    munmap((void *)mem, size);
    printf("PASS: guest verified 1 GiB of memory\n");

    for (tries = 0; tries < 100; tries++) {
        fd = open("/dev/vda", O_RDWR | O_DIRECT | O_SYNC);
        if (fd >= 0)
            break;
        usleep(100000);
    }
    if (fd < 0)
        fail("open virtio test disk");
    if (posix_memalign((void **)&write_buf, 4096, io_size) ||
        posix_memalign((void **)&read_buf, 4096, io_size)) {
        errno = ENOMEM;
        fail("allocate I/O buffers");
    }
    for (i = 0; i < io_size; i++)
        write_buf[i] = (i * 17 + 0x5a) & 255;
    if (pwrite(fd, write_buf, io_size, 0) != io_size ||
        fsync(fd) ||
        pread(fd, read_buf, io_size, 0) != io_size ||
        memcmp(write_buf, read_buf, io_size)) {
        errno = EIO;
        fail("virtio block integrity");
    }
    close(fd);
    free(read_buf);
    free(write_buf);
    printf("PASS: guest virtio block integrity\n");
    printf("PASS: guest_memfd HugeTLB guest validation\n");
    sync();
    reboot(RB_POWER_OFF);
    fail("power off");
}
