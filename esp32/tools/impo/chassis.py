#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Send a bounded SparkBot chassis diagnostic directly over USB."""
import argparse
import json
import time

from chat import Board, BoardError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB serial port, such as /dev/cu.usbmodem2101")
    parser.add_argument("direction", choices=("probe", "stop", "forward", "back", "left", "right"),
                        default="probe", nargs="?")
    args = parser.parse_args()
    with Board(args.port) as board:
        status = board.status()
        if not status or status.get("board") != "ESP-SparkBot":
            raise BoardError("The attached board did not identify as ESP-SparkBot.")
        board.drain()
        try:
            board.write_line("chassis=" + args.direction)
            result = board.frame(time.monotonic() + 3, "@chassis")
            if result is None:
                raise BoardError("No chassis diagnostic reply; movement outcome is unknown.")
            if result.get("direction") != args.direction:
                raise BoardError("The chassis diagnostic was rejected: " + str(result.get("error")))
            print(json.dumps(result))
            if not result.get("sent"):
                raise BoardError("The head could not transmit to the base.")
        except BaseException:
            # Firmware handles the 300 ms stop independently of this process.
            # Also attempt a stop if the host loses the diagnostic reply.
            try:
                board.write_line("chassis=stop")
            except Exception:
                pass
            raise


if __name__ == "__main__":
    try:
        main()
    except BoardError as error:
        raise SystemExit(str(error))

