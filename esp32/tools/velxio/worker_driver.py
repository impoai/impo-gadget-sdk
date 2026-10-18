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

"""Boot a flash image on Velxio's ESP32 worker. Runs inside the Velxio image.

run.py starts this in a container. The worker (Velxio's esp32_worker.py)
loads libqemu-xtensa, takes one JSON config line on stdin and writes one JSON
event per line on stdout. This prints UART0 as text, other events as `## {...}`
lines, and saves SPI bytes for screen.py.

Usage: worker_driver.py FLASH SECONDS [SPI_OUT]

VELXIO_ESP32_MAC, if set, is the board's factory MAC (needs a QEMU with
that patch); the worker and the QEMU it loads inherit it.
"""

import base64
import json
import subprocess
import sys
import threading
import time

SERVICES = "/app/app/services"
# Pin and LED events are many and say little in a log.
QUIET = {"gpio_change", "gpio_dir", "gpio_pull", "pin_change", "ledc_update",
         "ledc_duty"}


def main():
    flash, seconds = sys.argv[1], float(sys.argv[2])
    spi = open(sys.argv[3], "w") if len(sys.argv) > 3 else None
    with open(flash, "rb") as f:
        firmware = base64.b64encode(f.read()).decode()
    cfg = {"lib_path": "/app/lib/libqemu-xtensa.so", "machine": "esp32-picsimlab",
           "wifi_enabled": True, "firmware_b64": firmware}
    worker = subprocess.Popen([sys.executable, "-u", f"{SERVICES}/esp32_worker.py"],
                              cwd=SERVICES, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    worker.stdin.write((json.dumps(cfg) + "\n").encode())
    worker.stdin.flush()

    def stderr():
        for raw in worker.stderr:
            sys.stderr.write("[worker] " + raw.decode(errors="replace"))
    threading.Thread(target=stderr, daemon=True).start()
    # The worker can go quiet; stop on time even then.
    threading.Timer(seconds, worker.kill).start()

    line = ""
    for raw in worker.stdout:
        try:
            ev = json.loads(raw)
        except ValueError:
            continue
        kind = ev.get("type")
        if kind == "spi_batch":
            if spi:
                spi.write(ev["b64"] + "\n")
        elif kind == "uart_tx" and ev.get("uart", 0) == 0:
            for b in ev.get("bytes") or [ev["byte"]]:
                c = chr(b)
                if c == "\n":
                    print(line, flush=True)
                    line = ""
                elif c != "\r":
                    line += c
        elif kind not in QUIET and kind != "uart_tx":
            print("##", json.dumps(ev)[:300], flush=True)
    if line:
        print(line, flush=True)
    worker.kill()


if __name__ == "__main__":
    main()
