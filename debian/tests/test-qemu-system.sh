#!/bin/sh

set -e

for ARCH in x86_64 i386; do
    echo -n "Checking for pc in ${ARCH}..."
    qemu-system-${ARCH} -M help | grep -qs "pc\s\+Standard PC"
    echo "done."
done

