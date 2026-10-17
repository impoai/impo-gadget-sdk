#!/usr/bin/env python3
"""Back up an unlocked, factory-layout X4; replace only app1 and its selector.

Run from an activated ESP-IDF environment. Never writes a bootloader, partition
table, NVS, SD card, eFuses, or app0. Refuses any other flash layout.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib


def select_app1(otadata):
    """Write the inactive OTA metadata sector, retaining the previous selector."""
    valid = []
    for sector in range(2):
        entry = otadata[sector * 4096:sector * 4096 + 32]
        seq, _, state, crc = struct.unpack("<I20sII", entry)
        if (seq not in (0, 0xffffffff) and state not in (3, 4) and
                crc == zlib.crc32(entry[:4], 0xffffffff)):
            valid.append((seq, sector))
    seq, active = max(valid, default=(0, 1))
    seq = (seq // 2 + 1) * 2  # (seq - 1) % 2 == 1 -> app1
    if seq >= 0xffffffff:
        raise ValueError("OTA sequence exhausted; refusing to change the selector")
    entry = struct.pack("<I20sII", seq, b"\xff" * 20, 2,
                        zlib.crc32(struct.pack("<I", seq), 0xffffffff))
    return 0xe000 + (1 - active) * 4096, entry + b"\xff" * (4096 - 32)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("build", type=Path)
    parser.add_argument("--select-only", action="store_true",
                        help="verify the existing app1 and repair its boot selector only")
    args = parser.parse_args()
    build = args.build.resolve()
    config = (build / "sdkconfig").read_text()
    if "CONFIG_HOMEHUB_LED_BACKEND_XTEINK_X4=y\n" not in config:
        raise ValueError("not an X4 build")
    for option in ("SECURE_BOOT", "SECURE_FLASH_ENC_ENABLED", "HOMEHUB_PAIRING_EFUSE_AUTH",
                   "HOMEHUB_NVS_ENCRYPTION", "NVS_ENCRYPTION", "HOMEHUB_OTA_ENABLED"):
        if f"CONFIG_{option}=y\n" in config:
            raise ValueError(f"unsafe X4 build: {option}")
    image = build / "impo-gadget.bin"
    if not 24 < image.stat().st_size <= 0x640000:
        raise ValueError("app does not fit app1")
    backup = build / "backups" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    backup.mkdir(parents=True, mode=0o700)
    base = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "-p", args.port]

    def run(*command, before="no-reset", after="no-reset"):
        result = subprocess.run(base + ["--before", before, "--after", after, *map(str, command)],
                                check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = result.stdout.decode(errors="replace")
        with (backup / "flash.log").open("a") as log:
            log.write(output)
        if result.returncode:
            # Preserve the real serial/verification error, including failures
            # before a write, instead of hiding it behind CalledProcessError.
            print(output, file=sys.stderr, flush=True)
            result.check_returncode()
        # Drop the hundreds of progress-bar updates from the terminal.
        for line in output.replace("\r", "\n").splitlines():
            if any(s in line for s in ("MAC:", "Disabled", "Verified", "verified",
                                      "digest matched", "Hash of data", "Wrote", "Read ")):
                print(line, flush=True)
        return output

    security = run("get-security-info", before="default-reset")
    if "Secure Boot: Disabled" not in security or "Flash Encryption: Disabled" not in security:
        raise ValueError("X4 is locked/encrypted; no flash was written")
    # Validate the actual device's full table, including names and checksums.
    table = backup / "partition-table.bin"
    run("read-flash", "0x8000", "0x1000", table)
    expected = (build / "partition_table/partition-table.bin").read_bytes()
    if table.read_bytes()[:len(expected)] != expected:
        raise ValueError("X4 factory partition table differs; no flash was written")
    full = backup / "full-flash.bin"
    print(f"Backing up 16 MB to {full}", flush=True)
    run("read-flash", "0", "0x1000000", full)
    data = full.read_bytes()
    if len(data) != 0x1000000:
        raise ValueError("incomplete backup")
    run("verify-flash", "0", full)
    digest = hashlib.sha256(data).hexdigest()
    (backup / "full-flash.sha256").write_text(f"{digest}  full-flash.bin\n")
    offset, selector = select_app1(data[0xe000:0x10000])
    selector_file = backup / "app1-selector.bin"
    selector_file.write_bytes(selector)
    # Write + verify app first; change boot selection only after it succeeds.
    if not args.select_only:
        run("write-flash", "0x650000", image)
    run("verify-flash", "0x650000", image)
    run("write-flash", hex(offset), selector_file)
    run("verify-flash", hex(offset), selector_file)
    (backup / "manifest.json").write_text(json.dumps({
        "backup_sha256": digest,
        "app_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "app_offset": "0x650000", "selector_offset": hex(offset),
        "app0_and_bootloader_preserved": True,
    }, indent=2) + "\n")
    run("get-security-info", after="hard-reset")
    print(f"X4 flashed. Recovery backup: {backup}")


if __name__ == "__main__":
    main()
