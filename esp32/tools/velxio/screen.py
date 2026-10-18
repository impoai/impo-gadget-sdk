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

"""Rebuild the velxio-sim ST7789 screen from SPI bytes saved by run.py.

The emulator reports SPI writes without the panel's D/C line, so a write of
one byte is taken as a command: CASET (0x2A) and RASET (0x2B) set the window,
RAMWR (0x2C) starts RGB565 pixels, big-endian, as led_status.c sends them.
The last value written to each pixel wins.

Usage: screen.py spi.txt screen.png [--scale 2]
"""

import argparse
import base64
import struct
import zlib

CASET, RASET, RAMWR = 0x2A, 0x2B, 0x2C


def framebuffer(path):
    fb = {}
    x0 = x1 = y0 = y1 = cx = cy = 0
    mode, pending = None, b""
    with open(path) as f:
        for line in f:
            data = base64.b64decode(line.strip())
            if len(data) == 1 and data[0] in (CASET, RASET, RAMWR):
                mode, pending = data[0], b""
                if mode == RAMWR:
                    cx, cy = x0, y0
                continue
            if len(data) == 1 and mode != RAMWR:
                mode = None
                continue
            if mode == CASET and len(data) == 4:
                x0, x1 = data[0] << 8 | data[1], data[2] << 8 | data[3]
            elif mode == RASET and len(data) == 4:
                y0, y1 = data[0] << 8 | data[1], data[2] << 8 | data[3]
            elif mode == RAMWR:
                data = pending + data
                n = len(data) // 2 * 2
                pending = data[n:]
                for i in range(0, n, 2):
                    fb[(cx, cy)] = data[i] << 8 | data[i + 1]
                    cx += 1
                    if cx > x1:
                        cx, cy = x0, cy + 1
                    if cy > y1:
                        cy = y0
    return fb


def write_png(path, fb, scale):
    xs = [x for x, _ in fb]
    ys = [y for _, y in fb]
    left, top = min(xs), min(ys)
    w, h = max(xs) - left + 1, max(ys) - top + 1
    rows = bytearray()
    for y in range(h * scale):
        rows.append(0)
        for x in range(w * scale):
            v = fb.get((x // scale + left, y // scale + top), 0)
            rows += bytes(((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63,
                           (v & 31) * 255 // 31))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body)))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(rows)))
                + chunk(b"IEND", b""))
    return left, top, w, h


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("spi", help="SPI bytes from run.py --spi")
    ap.add_argument("png", help="image to write")
    ap.add_argument("--scale", type=int, default=2)
    args = ap.parse_args()
    fb = framebuffer(args.spi)
    if not fb:
        raise SystemExit("error: no pixels in " + args.spi)
    left, top, w, h = write_png(args.png, fb, args.scale)
    print(f"{len(fb)} pixels, panel area {w}x{h} at ({left}, {top})")


if __name__ == "__main__":
    main()
