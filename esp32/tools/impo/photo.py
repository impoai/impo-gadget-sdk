#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Take a photo with the board's camera over USB and save the JPEG."""
import argparse
import base64
import time

from chat import Board, BoardError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB serial port, such as /dev/cu.usbmodem101")
    parser.add_argument("out", nargs="?", default="photo.jpg")
    args = parser.parse_args()
    with Board(args.port) as board:
        board.drain()
        board.write_line("snap")
        result = board.frame(time.monotonic() + 20, "@snap")
        if result is None:
            raise BoardError("No reply to snap.")
        if "error" in result:
            raise BoardError("Camera: " + result["error"])
        data = base64.b64decode(result["jpeg_base64"])
        with open(args.out, "wb") as f:
            f.write(data)
        print(f"{args.out}: {len(data)} bytes")


if __name__ == "__main__":
    try:
        main()
    except BoardError as error:
        raise SystemExit(str(error))
