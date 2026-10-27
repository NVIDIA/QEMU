#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate 1 GiB guest_memfd backing, guest integrity and stage-2 leaves."""

import argparse
import gzip
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time

HUGE_SIZE = 1 << 30
POOL = Path("/sys/kernel/mm/hugepages/hugepages-1048576kB")
MARKERS = (
    "PASS: guest booted with 4 KiB base pages",
    "PASS: guest verified 1 GiB of memory",
    "PASS: guest virtio block integrity",
    "PASS: guest_memfd HugeTLB guest validation",
)


def read_count(name):
    return int((POOL / name).read_text().strip())


def write_count(value):
    (POOL / "nr_hugepages").write_text(str(value) + "\n")


def status_text(returncode):
    if returncode is None:
        return "still running"
    if returncode < 0:
        number = -returncode
        return f"signal {number} ({signal.Signals(number).name})"
    return f"exit status {returncode}"


class QMP:
    def __init__(self, path, process, timeout=30):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.socket.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError) as exc:
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited before QMP: " +
                                       status_text(process.returncode)) from exc
                if time.monotonic() >= deadline:
                    raise RuntimeError("Timed out connecting to QMP") from exc
                time.sleep(0.05)
        self.file = self.socket.makefile("rwb", buffering=0)
        greeting = self.receive()
        if "QMP" not in greeting:
            raise RuntimeError(f"Unexpected QMP greeting: {greeting}")
        # This reply is required; a greeting alone can precede ROM reset.
        self.command("qmp_capabilities")

    def receive(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError("QMP connection closed")
        return json.loads(line)

    def command(self, execute):
        self.file.write((json.dumps({"execute": execute}) + "\n").encode())
        while True:
            response = self.receive()
            if "error" in response:
                raise RuntimeError(f"QMP {execute}: {response['error']}")
            if "return" in response:
                return response["return"]

    def close(self):
        self.file.close()
        self.socket.close()


def check_guest_memfd(pid):
    matches = []
    for fd in (Path("/proc") / str(pid) / "fd").iterdir():
        try:
            name = os.readlink(fd)
            if "kvm-gmem" in name:
                matches.append((name, os.stat(fd).st_blksize))
        except FileNotFoundError:
            continue
    if not any(size == HUGE_SIZE for _, size in matches):
        raise RuntimeError(f"No KVM guest_memfd with 1 GiB block size: {matches}")
    return matches


def cpio_entry(output, name, data, mode, ino):
    name = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0)
    header = b"070701" + "".join(f"{value:08x}" for value in fields).encode()
    output.write(header + name)
    output.write(b"\0" * (-(len(header) + len(name)) % 4))
    output.write(data)
    output.write(b"\0" * (-len(data) % 4))


def build_initrd(output, release):
    source = Path(__file__).with_name("guest_memfd_hugetlb_guest.c")
    binary = output / "guest-init"
    result = subprocess.run(
        ["gcc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
         str(source), "-o", str(binary)],
        capture_output=True, text=True, check=False)
    (output / "guest-build.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("Static guest build failed; see guest-build.log")

    module_paths = []
    for driver in ("virtio_mmio", "virtio_blk"):
        result = subprocess.run(
            ["modprobe", "--show-depends", "--set-version", release, driver],
            capture_output=True, text=True, check=False)
        if result.returncode:
            raise RuntimeError(f"Cannot resolve guest {driver}: {result.stderr}")
        for line in result.stdout.splitlines():
            if line.startswith("insmod "):
                path = Path(line.split()[1])
                if path not in module_paths:
                    module_paths.append(path)
    entries = []
    for i, path in enumerate(module_paths):
        if path.suffix == ".zst":
            data = subprocess.check_output(["zstd", "-q", "-d", "-c", str(path)])
        elif path.suffix == ".xz":
            import lzma
            data = lzma.decompress(path.read_bytes())
        elif path.suffix == ".gz":
            data = gzip.decompress(path.read_bytes())
        else:
            data = path.read_bytes()
        entries.append((f"modules/{i:03}.ko", data, stat.S_IFREG | 0o644))
    module_list = "".join("/" + name + "\n" for name, _, _ in entries)
    files = [
        ("dev", b"", stat.S_IFDIR | 0o755),
        ("proc", b"", stat.S_IFDIR | 0o755),
        ("sys", b"", stat.S_IFDIR | 0o755),
        ("modules", b"", stat.S_IFDIR | 0o755),
        ("init", binary.read_bytes(), stat.S_IFREG | 0o755),
        ("modules.list", module_list.encode(), stat.S_IFREG | 0o644),
    ] + entries
    archive = output / "guest-initramfs.cpio"
    with archive.open("wb") as stream:
        for ino, (name, data, mode) in enumerate(files, 1):
            cpio_entry(stream, name, data, mode, ino)
        cpio_entry(stream, "TRAILER!!!", b"", 0, len(files) + 1)
    return archive


def start_trace(pid):
    tracefs = Path("/sys/kernel/tracing")
    if not (tracefs / "events/kvm/kvm_gmem_map/enable").exists():
        tracefs = Path("/sys/kernel/debug/tracing")
    if not (tracefs / "events/kvm/kvm_gmem_map/enable").exists():
        raise RuntimeError("kvm_gmem_map tracepoint unavailable; mount tracefs")
    instance = tracefs / "instances" / f"qemu-gmem-{pid}"
    instance.mkdir()
    try:
        (instance / "tracing_on").write_text("0")
        (instance / "buffer_size_kb").write_text("8192")
        # All vCPUs already exist while the machine is paused.
        tids = sorted(path.name for path in
                      (Path("/proc") / str(pid) / "task").iterdir())
        (instance / "set_event_pid").write_text("\n".join(tids) + "\n")
        (instance / "events/kvm/kvm_gmem_map/enable").write_text("1")
        (instance / "tracing_on").write_text("1")
        return instance
    except BaseException:
        instance.rmdir()
        raise


def save_trace(instance, output):
    (instance / "tracing_on").write_text("0")
    text = (instance / "trace").read_text()
    (output / "stage2.trace").write_text(text)
    return text


def check_trace(text, realm):
    for line in text.splitlines():
        match = re.search(r"kvm_gmem_map:\s+(.*)", line)
        if not match:
            continue
        fields = dict(re.findall(r"(\w+)=(\S+)", match.group(1)))
        if (int(fields.get("leaf", "0"), 0) == HUGE_SIZE and
                int(fields.get("requested", "0"), 0) >= HUGE_SIZE and
                int(fields.get("realm", "-1"), 0) == int(realm) and
                (not realm or int(fields.get("private", "0"), 0) == 1)):
            return line.strip()
    raise RuntimeError("No actual 1 GiB stage-2 leaf for this QEMU; see stage2.trace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--realm", action="store_true", help="requires CCA")
    parser.add_argument("--create-only", action="store_true",
                        help="check creation/startup without guest execution")
    parser.add_argument("--kernel", help="guest kernel; default is running host kernel")
    parser.add_argument("--initrd", help="initramfs containing the validation PID 1")
    parser.add_argument("--guest-release", default=platform.release(),
                        help="module release used when building the guest initramfs")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if platform.machine() != "aarch64" or os.sysconf("SC_PAGE_SIZE") != 4096:
        raise RuntimeError("This test requires a 4 KiB ARM64 host")
    if os.geteuid() != 0:
        raise RuntimeError("Run as root to reserve/restore HugeTLB pages and trace KVM")
    if not Path("/dev/kvm").exists():
        raise RuntimeError("/dev/kvm is unavailable")
    output = (args.output or Path(
        f"results-gmem-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}")).resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "host.json").write_text(json.dumps({
        "uname": list(platform.uname()), "qemu": str(Path(args.qemu).resolve()),
        "realm": args.realm, "create_only": args.create_only,
        "kernel_build": Path("/proc/version").read_text(encoding="utf-8").strip(),
        "qemu_version": subprocess.check_output(
            [args.qemu, "--version"], text=True).strip(),
    }, indent=2) + "\n")
    proc = qmp = instance = None
    stdout = stderr = None
    original = {name: read_count(name) for name in
                ("nr_hugepages", "free_hugepages", "resv_hugepages")}
    passed = False
    try:
        pages = 1 if args.create_only else 2
        available = read_count("free_hugepages") - read_count("resv_hugepages")
        if available < pages:
            write_count(original["nr_hugepages"] + pages - available)
        if read_count("free_hugepages") - read_count("resv_hugepages") < pages:
            raise RuntimeError(f"Could not reserve {pages} unused 1 GiB HugeTLB pages")
        baseline = {name: read_count(name) for name in
                    ("free_hugepages", "resv_hugepages")}
        machine = "virt,accel=kvm,gic-version=3,memory-backend=ram0"
        size = "1G" if args.create_only else "2G"
        backend = (f"memory-backend-memfd,id=ram0,size={size},share=on,"
                   "hugetlb=on,hugetlbsize=1G")
        objects = [backend + (",guest-memfd=auto" if args.realm
                             else ",guest-memfd=on")]
        if args.realm:
            machine += ",confidential-guest-support=rme0"
            objects.append("rme-guest,id=rme0,convert-in-place=on")
        qmp_path = Path(f"/tmp/qemu-gmem-{os.getpid()}.qmp")
        command = [str(Path(args.qemu).resolve()), "-nodefaults", "-display",
                   "none", "-S", "-monitor", "none", "-qmp",
                   f"unix:{qmp_path},server=on,wait=off",
                   "-machine", machine, "-cpu", "host", "-m", size]
        for obj in objects:
            command += ["-object", obj]
        if not args.create_only:
            kernel = Path(args.kernel or f"/boot/vmlinuz-{platform.release()}")
            if not kernel.is_file():
                raise RuntimeError(f"Guest kernel unavailable: {kernel}")
            initrd = (Path(args.initrd).resolve() if args.initrd else
                      build_initrd(output, args.guest_release))
            disk = output / "virtio-test.raw"
            with disk.open("wb") as stream:
                stream.truncate(64 << 20)
            command += [
                "-smp", "2", "-kernel", str(kernel.resolve()),
                "-initrd", str(initrd),
                "-append", "console=ttyAMA0 rdinit=/init panic=-1 nokaslr",
                "-serial", f"file:{output / 'guest-console.log'}",
                "-drive", f"file={disk},if=none,format=raw,id=checkdisk",
                "-device", "virtio-blk-device,drive=checkdisk",
            ]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        stdout = (output / "qemu.stdout").open("w")
        stderr = (output / "qemu.stderr").open("w")
        proc = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        qmp = QMP(qmp_path, proc)
        fds = check_guest_memfd(proc.pid)
        (output / "guest-memfd.json").write_text(json.dumps(fds, indent=2) + "\n")
        if (read_count("resv_hugepages") <= baseline["resv_hugepages"] and
                read_count("free_hugepages") >= baseline["free_hugepages"]):
            raise RuntimeError("QEMU did not reserve or allocate HugeTLB backing")
        if args.create_only:
            qmp.command("quit")
            proc.wait(timeout=15)
        else:
            instance = start_trace(proc.pid)
            qmp.command("cont")
            proc.wait(timeout=args.timeout)
        if proc.returncode:
            raise RuntimeError("QEMU exited with " + status_text(proc.returncode))
        if not args.create_only:
            console = (output / "guest-console.log").read_text(errors="replace")
            if "FAIL: guest" in console:
                raise RuntimeError("Guest validation failed; see guest-console.log")
            for marker in MARKERS:
                if marker not in console:
                    raise RuntimeError(f"Guest did not report: {marker}")
            with disk.open("rb") as stream:
                actual = stream.read(4 << 20)
            expected = bytes((i * 17 + 0x5a) & 255 for i in range(256))
            if actual != expected * ((4 << 20) // 256):
                raise RuntimeError("Host disk contents do not match guest writes")
            leaf = check_trace(save_trace(instance, output), args.realm)
            print("Verified stage-2 mapping:", leaf)
        passed = True
    finally:
        try:
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            if instance:
                save_trace(instance, output)
                (instance / "events/kvm/kvm_gmem_map/enable").write_text("0")
                (instance / "set_event_pid").write_text("")
                instance.rmdir()
            if qmp:
                qmp.close()
            if stdout:
                stdout.close()
            if stderr:
                stderr.close()
            if proc and proc.returncode:
                diagnostic = (output / "qemu.stderr").read_text().strip()
                print("QEMU:", status_text(proc.returncode), diagnostic, file=sys.stderr)
            if proc:
                Path(f"/tmp/qemu-gmem-{os.getpid()}.qmp").unlink(missing_ok=True)
        finally:
            if read_count("nr_hugepages") != original["nr_hugepages"]:
                write_count(original["nr_hugepages"])
            deadline = time.monotonic() + 10
            while True:
                final = {name: read_count(name) for name in original}
                if final == original or time.monotonic() >= deadline:
                    break
                time.sleep(0.1)
            (output / "pool.json").write_text(json.dumps(
                {"before": original, "after": final}, indent=2) + "\n")
            if final != original:
                raise RuntimeError(f"HugeTLB pool did not restore: {final}, expected {original}")
            print("Results:", output)
    if passed:
        print("PASS: " + ("1 GiB guest_memfd creation and startup" if args.create_only
                          else "guest memory, virtio I/O and actual 1 GiB stage-2 leaves"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("FAIL: " + str(exc), file=sys.stderr)
        sys.exit(1)
