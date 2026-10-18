#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Merge a velxio-sim build into one flash image for the Velxio emulator.

The emulator boots a whole flash image, not separate partitions. This writes
the bootloader, partition table, app and the other files `idf.py flash` would
write, at the same offsets, padded to the board's flash size.

The emulator has no Bluetooth, so the board can't be set up over BLE. With
--tokens, the device tokens go straight into the NVS partition and the board
starts set up. The JSON file holds what BLE setup would have stored:

    {"access_token": "...", "refresh_token": "...",
     "api_url_v2": "https://...", "noise_host": "..."}

A gateway pairing response, which nests these under "pairing", works as is.

Run inside an ESP-IDF v6 environment (esptool and the NVS generator).

Usage: make_flash.py BUILD_DIR OUT.bin [--tokens tokens.json]
"""

import argparse
import csv
import io
import json
import os
import subprocess
import sys
import tempfile

TOKEN_KEYS = ("access_token", "refresh_token", "api_url_v2", "noise_host")


def run(*cmd):
    subprocess.run(cmd, check=True)


def partitions(build):
    """The partition table of the build, as dicts with int offset and size."""
    gen = os.path.join(os.environ["IDF_PATH"], "components", "partition_table",
                       "gen_esp32part.py")
    table = os.path.join(build, "partition_table", "partition-table.bin")
    out = subprocess.run([sys.executable, gen, "-q", table], check=True,
                         capture_output=True, text=True).stdout
    rows = csv.reader(line for line in io.StringIO(out) if not line.startswith("#"))
    parts = []
    for name, ptype, subtype, offset, size, *_ in rows:
        size = size.strip()
        mult = {"K": 1024, "M": 1024 * 1024}.get(size[-1:], 1)
        parts.append({"name": name.strip(), "type": ptype.strip(),
                      "subtype": subtype.strip(), "offset": int(offset, 0),
                      "size": int(size.rstrip("KM"), 0) * mult})
    return parts


def nvs_image(tokens_path, size, tmp):
    with open(tokens_path) as f:
        tokens = json.load(f)
    tokens = tokens.get("pairing", tokens)
    missing = [k for k in TOKEN_KEYS if not tokens.get(k)]
    if missing:
        sys.exit(f"error: {tokens_path} lacks {', '.join(missing)}")
    rows = [["key", "type", "encoding", "value"], ["homehub", "namespace", "", ""]]
    rows += [[k, "data", "string", tokens[k]] for k in TOKEN_KEYS]
    rows.append(["setup_complete", "data", "string", "1"])
    src = os.path.join(tmp, "nvs.csv")
    with open(src, "w", newline="") as f:
        csv.writer(f).writerows(rows)
    out = os.path.join(tmp, "nvs.bin")
    run(sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", src, out,
        hex(size))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("build", help="build directory, e.g. build-velxio-sim")
    ap.add_argument("out", help="flash image to write")
    ap.add_argument("--tokens", help="JSON file with the device tokens")
    args = ap.parse_args()

    with open(os.path.join(args.build, "flasher_args.json")) as f:
        flasher = json.load(f)
    settings = flasher["flash_settings"]
    # flasher_args.json says "keep"; the image needs the real size to pad to.
    with open(os.path.join(args.build, "config", "sdkconfig.json")) as f:
        flash_size = json.load(f)["ESPTOOLPY_FLASHSIZE"]
    files = {int(off, 0): os.path.join(args.build, path)
             for off, path in flasher["flash_files"].items()}

    with tempfile.TemporaryDirectory() as tmp:
        if args.tokens:
            nvs = next(p for p in partitions(args.build)
                       if p["type"] == "data" and p["subtype"] == "nvs")
            files[nvs["offset"]] = nvs_image(args.tokens, nvs["size"], tmp)
        pairs = [x for off in sorted(files) for x in (hex(off), files[off])]
        run(sys.executable, "-m", "esptool", "--chip", flasher["extra_esptool_args"]["chip"],
            "merge-bin", "-o", args.out,
            "--flash-mode", settings["flash_mode"],
            "--flash-freq", settings["flash_freq"],
            "--flash-size", flash_size,
            "--pad-to-size", flash_size, *pairs)


if __name__ == "__main__":
    main()
