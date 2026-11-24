#!/usr/bin/env python3
"""Prepare and launch a fourth-generation Wi-Fi iPad with the patched QEMU engine."""

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "Portable/firmware/firmware-catalog.json"
SHA256 = "fed2ecb8f0f530437247f755a5f1533a21517992a6a212af96d6a2adee4f0e4a"
BUILD = "p101ap-10B329"
KEYBAG = re.compile(r"A6NATIVEFORMAT (before-unmount|after-remount) bytes=(\d+).* bplist=1 tag=([0-9a-f]{8})")


def qemu_value(path):
    """QEMU doubles commas inside machine property values."""
    return str(path).replace(",", ",,")


def engine(args, boot, nand, display, log, *, restore_stream=None):
    machine = f"iPad-4-wifi,kboot={qemu_value(boot)},ppn=on,nand={qemu_value(nand)}"
    if restore_stream is not None:
        machine += f",restore-image={qemu_value(restore_stream)}"
    if display != "none":
        machine += ",display-probe=on,quiet-boot=on"
    return [
        str(args.qemu), "-L", str(args.bios), "-M", machine,
        "-m", "1G", "-display", display, "-monitor", "none",
        "-audio", "driver=none", "-no-reboot", "-serial", f"file:{log}",
    ]


def prepare(args):
    if args.timeout <= 0:
        raise ValueError("Restore timeout must be positive")
    with args.ipsw.open("rb") as ipsw:
        if hashlib.file_digest(ipsw, "sha256").hexdigest() != SHA256:
            raise ValueError("Expected iPad3,4 iOS 6.1.3 (10B329) restore firmware")
    if not args.initializer.is_file():
        raise ValueError("Build the engine's it_initialize helper with RESTORE_STOCK_DATA_ONLY=1")

    args.device.mkdir(parents=True)
    log = args.device / "restore.log"
    nand = args.device / "nand"

    def firmwarekit(*command):
        subprocess.run([str(args.firmwarekit), *map(str, command)], check=True)

    with tempfile.TemporaryDirectory(prefix="restore-", dir=args.device) as temporary:
        work = Path(temporary)
        decrypted = work / "decrypted"
        system = work / "system.hfs"
        ramdisk = work / "initialize.hfs"
        stream = work / "system.a6di"
        restore_boot = work / "initialize.kboot"
        firmwarekit("decrypt", "--catalog", CATALOG, "--id", BUILD,
                    "--ipsw", args.ipsw, "--out", decrypted)
        firmwarekit("extract-rootfs", decrypted / "rootfs.dmg", system)
        if args.offline_setup:
            firmwarekit("hfs-grow", system, system.stat().st_size + (32 << 20))
            firmwarekit("seed-data", "--system", system, "--offline-setup")
        if args.silent_audio:
            firmwarekit("hfs-edit", system,
                        "mv:/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin="
                        "/Library/Audio/Plug-Ins/VirtualAudio.plugin")
        firmwarekit("extract-rootfs", decrypted / "048-2666-005-ramdisk.dmg", ramdisk)
        firmwarekit("hfs-grow", ramdisk, 67108864)
        firmwarekit("hfs-edit", ramdisk, "rm:/usr/local/bin/restored_external",
                    f"write:/usr/local/bin/restored_external={args.initializer}",
                    "chmod:/usr/local/bin/restored_external=0755")
        firmwarekit("a6-nand", nand)
        firmwarekit("a6-stream", system, stream)
        firmwarekit("kboot", "--decrypted", decrypted, "--seed", BUILD, "--ramdisk", ramdisk,
                    "--boot-args", "-v serial=3 debug=0x8 nand-enable-reformat=1 "
                    "amfi_allow_any_signature=1 cs_enforcement_disable=1", "--out", restore_boot)
        print(f"Installing iOS through the emulated iPad's flash driver. See {log}.", flush=True)
        command = engine(args, restore_boot, nand, "none", log, restore_stream=stream)
        subprocess.run(command, check=True, timeout=args.timeout)
        report = log.read_text(encoding="utf-8", errors="replace")
        keybags = {stage: (size, tag) for stage, size, tag in KEYBAG.findall(report)}
        if ("A6RESTORE DONE " not in report or "after-remount" not in keybags
                or keybags["after-remount"] != keybags.get("before-unmount")
                or "effaceable formatted, system keybag created; halting" not in report
                or "pmu go stdby" not in report):
            raise ValueError(f"Restore did not verify both volumes and shut down cleanly. See {log}.")
        firmwarekit("kboot", "--decrypted", decrypted, "--seed", BUILD,
                    "--boot-args", "-v serial=3 debug=0x8 rd=disk0s1",
                    "--out", args.device / "normal.kboot")
    print(f"Prepared {args.device}")


def run(args):
    boot = args.device / "normal.kboot"
    nand = args.device / "nand"
    if not boot.is_file() or not (nand / "geometry.json").is_file():
        raise ValueError("The device directory needs a completed iPad 4 restore")
    subprocess.run(engine(args, boot, nand, args.display, args.device / "boot.log"), check=True)


def absolute(value):
    return Path(value).resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    prepare_args = subcommands.add_parser("prepare", help="Restore from the original IPSW to a new directory")
    run_args = subcommands.add_parser("run", help="Start a prepared iPad 4")
    for command in (prepare_args, run_args):
        command.add_argument("--device", type=absolute, required=True)
        command.add_argument("--qemu", type=absolute, required=True)
        command.add_argument("--bios", type=absolute, required=True)
    default_firmwarekit = "firmwarekit.exe" if sys.platform == "win32" else "firmwarekit"
    prepare_args.add_argument("--firmwarekit", type=absolute, default=ROOT / "target/debug" / default_firmwarekit)
    prepare_args.add_argument("--ipsw", type=absolute, required=True)
    prepare_args.add_argument("--initializer", type=absolute, required=True)
    prepare_args.add_argument("--offline-setup", action="store_true",
                              help="Prepare the virtual iPad for local use with Setup Assistant completed")
    prepare_args.add_argument("--silent-audio", action="store_true",
                              help="Keep the iOS VirtualAudio plug-in outside the guest's audio plug-in directory")
    prepare_args.add_argument("--timeout", type=int, default=1200, help="Restore timeout in seconds")
    run_args.add_argument("--display", choices=("none", "sdl", "cocoa"), default="sdl")
    args = parser.parse_args()
    try:
        (prepare if args.command == "prepare" else run)(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
