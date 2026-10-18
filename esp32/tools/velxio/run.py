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

"""Run a velxio-sim flash image in the Velxio emulator, without a browser.

Starts a throwaway container from a Velxio image and boots the image on its
ESP32 worker (worker_driver.py). UART0 goes to stdout. With --spi, the SPI
bytes are saved so screen.py can rebuild the ST7789 screen.

The node id comes from the MAC. Give each emulated board its own --mac so
the gateway doesn't take two of them for one device.

Usage: run.py FLASH.bin [--seconds 120] [--spi spi.txt] [--mac MAC] [--image IMAGE]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("flash", help="image from make_flash.py")
    ap.add_argument("--seconds", type=float, default=120,
                    help="how long to run (default 120)")
    ap.add_argument("--spi", help="file to save SPI bytes to, for screen.py")
    ap.add_argument("--mac", help="factory MAC, e.g. 10:01:00:5a:00:01 "
                    "(default: the emulator's 10:01:00:c4:0a:24)")
    ap.add_argument("--image", default=os.environ.get("VELXIO_IMAGE", "velxio-velxio"),
                    help="Velxio Docker image (default velxio-velxio, or $VELXIO_IMAGE)")
    args = ap.parse_args()

    # Mount one directory holding the flash, the driver and the SPI output.
    with tempfile.TemporaryDirectory() as work:
        shutil.copy(args.flash, os.path.join(work, "flash.bin"))
        shutil.copy(os.path.join(HERE, "worker_driver.py"), work)
        cmd = ["docker", "run", "--rm", "-v", f"{work}:/fw", "--entrypoint", "python3"]
        if args.mac:
            cmd += ["-e", f"VELXIO_ESP32_MAC={args.mac}"]
        cmd += [args.image, "-u", "/fw/worker_driver.py", "/fw/flash.bin", str(args.seconds)]
        if args.spi:
            cmd.append("/fw/spi.txt")
        code = subprocess.call(cmd)
        if args.spi and os.path.exists(os.path.join(work, "spi.txt")):
            shutil.copy(os.path.join(work, "spi.txt"), args.spi)
    sys.exit(code)


if __name__ == "__main__":
    main()
