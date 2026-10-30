.. _guest-memfd:

guest-memfd support
===================

Recent kernels allow for the creation of a guest-memfd file
descriptor, which can be used to back VMs in a similar manner as a
memfd file descriptor, but is intended specifically for this purpose
and allows for closer coordination between KVM and the management of
this memory to enable more advanced/VM-specific use-cases.

Initially this additional functionality centered around providing a
common/centralized place for managing kernel-side memory handing
requirements for various Confidential Guest architectures. (For more
on Confidential Guests, see :ref:`confidential-guest-support`).

guest_memfd has since evolved to become a more general-purpose way to
allocate/manage guest memory and potentially allow for things like
providing additional memory isolation within the kernel[1] and support
for persisting a guest's state across kexec to allow for live-updating
the host kernel with minimal guest downtime[2].

Usage
-----

For Confidential Guests, guest-memfd is currently utilized internally
by QEMU to handle private guest memory, independently of whatever
memory backend the user has configured for normal/non-private/shared
guest memory. To avoid doubling memory, QEMU discards memory in
response to the guest converting GPA ranges between shared/private.
(e.g. if GPA X is converted from private to shared, the guest-memfd FD
offset corresponding to GPA x will be truncated since the memory will
be provided by the memory backend the user configured for shared
memory, and vice-versa). This is handled automatically/internally for
Confidential Guests that rely on this handling and is not directly
exposed by QEMU command-line options.

For non-Confidential guests, guest-memfd can be used in a manner that
is somewhat interchangeable with a normal memfd. Currently, this is
handled by using the same memory-backend implementation as memfd, but
with an additional 'guest-memfd=on' option. E.g.::

    qemu ... \
      -object memory-backend-memfd,id=ID,size=SIZE,share=on,guest-memfd=on

Note that the share=on option is required for guest-memfd, since it
does not support anonymous memory allocations or COW-like semantics.

Also note that there are a couple of limitations in using guest_memfd
in this way compared to a normal memfd, which is why the option is
exposed as an experimental for the time being:

 * hugetlb=on requires an explicit hugetlbsize and a kernel that
   supports HugeTLB guest_memfd.  Confidential guests also require
   in-place conversion.
 * guest_memfd does not currently support Transparent Huge Pages

In-place conversion
-------------------

Newer kernels now support using a guest-memfd instance for both
private and shared memory at the same time by re-using the same
physical backing pages if a guest converts a GPA range between
shared and private. This is known as "in-place conversion".

For some confidential computing architectures, like SEV-SNP and
TDX, this mode offers better performance due to not needing to
deallocate/reallocate between 2 separate pools of shared vs. private
memory every time the guest converts memory between shared/private.
For other architectures, this is the only supported mode of
operation.

When in-place conversion is enabled, it is necessary to use the
memory-backend-memfd backend with guest-memfd=on as shown in the
"Usage" section. Because of this, guest-memfd=auto (which is the
default) will result in the parameter being automatically enabled
if the confidential guest type enables in-place conversion.

For ARM Realms, use the rme-guest object's convert-in-place=on property
and select that object with -machine confidential-guest-support=ID.
The default remains the existing separate shared/private backing mode.

On a 4 KiB ARM64 host with 1 GiB HugeTLB pages reserved, use a
1 GiB-aligned backend size and the memory-backend-memfd properties
share=on,hugetlb=on,hugetlbsize=1G.  guest-memfd=auto selects guest_memfd
when the Realm has convert-in-place=on.  The backend requests
GUEST_MEMFD_FLAG_MMAP and GUEST_MEMFD_FLAG_HUGETLB with page_order=18.
For a Realm, HugeTLB guest_memfd starts private and this path omits
GUEST_MEMFD_FLAG_INIT_SHARED.  The kernel must have in-place conversion
enabled for a Realm.  For an ordinary VM, QEMU additionally requests
GUEST_MEMFD_FLAG_INIT_SHARED so firmware and guest images can be loaded
through the host mapping.  The kernel must support this flag combination.
QEMU binds ordinary HugeTLB backing directly with KVM_MEM_GUEST_MEMFD;
migration and dirty logging are unsupported for these direct bindings.

The ARM64 integration test reserves two 1 GiB HugeTLB pages and restores
the original pool after the VM exits.  It builds a static validation
initramfs with gcc, boots the running host's kernel as a guest, verifies
1 GiB of guest memory and direct virtio block I/O, checks the host disk
contents, and asserts an actual 1 GiB stage-2 leaf from a trace instance
filtered to the QEMU threads::

    sudo tests/functional/aarch64/guest_memfd_hugetlb.py \
      --qemu build/qemu-system-aarch64 --output results-gmem

The host needs gcc with static libc, modprobe, and matching guest kernel
modules.  The default guest kernel is /boot/vmlinuz-RELEASE.  For a
different guest, use --kernel and --guest-release to select matching
installed modules, or --initrd to supply a validation initramfs containing
the accompanying guest_memfd_hugetlb_guest.c PID 1 program.  The test
records the command, guest console, QEMU diagnostics, stage-2 trace, and
HugeTLB accounting in the output directory.

For a quick paused-VM creation check using one HugeTLB page, add
--create-only.  That mode does not prove guest access or stage-2 leaves.

On a CCA host, add --realm to test the private Realm path.  The guest
kernel must support CCA.  Its image is staged in host buffers before ROM
reset can write private RAM, then populated through the RMM interface.
The test requires a private 1 GiB stage-2 leaf in Realm mode.

Shared backing may use order-0 folio descriptors after restructuring.
The allocator's original 1 GiB allocation and the actual stage-2 leaf
size are separate properties; an order-0 trace field does not imply a
4 KiB stage-2 leaf.  Host and guest base pages remain 4 KiB.


References
----------

- `[1] directmap removal <https://lore.kernel.org/kvm/20260317141031.514-1-kalyazin@amazon.com/>`__
- `[2] LUO <https://lore.kernel.org/kvm/20260728121138.1103610-1-tarunsahu@google.com/>`__
