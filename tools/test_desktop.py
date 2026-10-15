#!/usr/bin/env python3
"""
YesOS Desktop test (yesos.elf). For now the desktop is the YesOS core and its
serial console: it boots under its own name, the PMP is locked and proven,
files written from the console persist across a reboot, fsck is clean, and
there is nothing else - no NAS, vault, packages, DNS or gateway commands, and
nothing answering on the network.

Usage: python3 tools/test_desktop.py      (after `make`; non-zero exit on failure)
"""
import os
import shutil
import socket
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(1, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "core", "tools"))  # YesOS core tools
import yesos_harness as harness  # noqa: E402
import redsea_fsck  # noqa: E402
import redsea_injector  # noqa: E402

failures = []


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label, flush=True)
    if not cond:
        failures.append(label)
    return cond


def console(vm, line, wait=2):
    m = vm.send(line, wait)
    return vm.since(m)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    if not os.path.exists(harness.KERNEL):
        print(f"{harness.KERNEL} not found: run `make` first")
        sys.exit(2)
    tmp = tempfile.mkdtemp(prefix="yesos_desktop_")
    disk = os.path.join(tmp, "desktop.img")
    with open(os.devnull, "w") as null:
        old = sys.stdout
        sys.stdout = null
        try:
            redsea_injector.format_disk(disk, total_mb=64)
        finally:
            sys.stdout = old
    try:
        port = free_port()
        vm = harness.YesOS(disk, port)
        try:
            ok = vm.wait_for("YesOS [", 240)
            out = vm.output()
            check(ok and "[BOOT] Entering YesOS Desktop" in out and "YesOS Desktop // RISC-V M-mode" in out,
                  "boots as YesOS Desktop")
            check("Z3-proven layout" in out, "PMP locked with the Z3-proven layout")
            check("NAS SERVICE" not in out and "[DNS]" not in out and "[BRIDGE]" not in out and "[VAULT]" not in out,
                  "no NAS, DNS, bridge or vault at boot")
            h = console(vm, "help")
            check(all(w not in h for w in ("snapshot", "smb", "user [", "vault [", "pkg [", "dns ", "fw ")) and "pmp" in h,
                  "help lists only core commands")
            for cmd in ("nas status", "vault status", "pkg list", "dns", "fw"):
                check(f"Unknown command: '{cmd}'" in console(vm, cmd, 1), f"`{cmd}` does not exist")
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(2)
            s.sendto(struct.pack("<I", 0x59455343) + b"\0" * 300, ("127.0.0.1", port))
            try:
                s.recvfrom(2048)
                answered = True
            except socket.timeout:
                answered = False
            s.close()
            check(not answered, "nothing answers on the network")
            console(vm, "disk save notes.txt hello-from-the-desktop", 3)
            check("hello-from-the-desktop" in console(vm, "disk cat notes.txt", 2), "a file written from the console reads back")
            console(vm, "disk save notas.txt ¿Qué tal? Mañana, señor.", 3)
            check("¿Qué tal? Mañana, señor." in console(vm, "disk cat notas.txt", 2),
                  "UTF-8 text typed at the console is stored and read back")
            check("¿Qué tal?" in console(vm, "disk cat notas.txtñ\x7f", 2),
                  "backspace removes a whole UTF-8 character (ñ is two bytes)")
            check("CLEAN" in console(vm, "fsck", 6), "fsck clean")
            check("FATAL" not in vm.output() and "EXCEPTION" not in vm.output(), "no kernel faults")
        finally:
            vm.stop()
        vm = harness.YesOS(disk, free_port())
        try:
            check(vm.wait_for("YesOS [", 240) and "hello-from-the-desktop" in console(vm, "disk cat notes.txt", 2),
                  "the file is still there after a reboot")
        finally:
            vm.stop()
        r = redsea_fsck.fsck(disk)
        check(not r["errors"], f"host fsck clean ({r['files']} files)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
