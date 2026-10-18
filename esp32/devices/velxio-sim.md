<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# velxio-sim: the firmware in an emulator

`velxio-sim` is a test board that exists only in
[Velxio](https://github.com/davidmonterocrespo24/velxio), an open-source
emulator that runs a classic ESP32 in QEMU. It runs the real firmware,
without hardware: it boots, joins Wi-Fi, connects to the gateway, registers
and answers commands, and draws its status screen.

The board is the ideaspark (classic ESP32, 16 MB flash, no PSRAM, 170x320
ST7789 on SPI2, BOOT on GPIO0). `sdkconfig.velxio-sim`, loaded after
`sdkconfig.ideaspark`, changes only what the emulator needs:

- It joins Velxio's emulated open access point, `Velxio-GUEST`, at boot.
- It advertises as `ImpoGadget-Sim-XXXXXX`.

Chip revision checks, signed apps and the PHY code stay as they ship.

## What the emulator covers

| Covered | Not covered |
|---|---|
| CPU, flash, GPIO, UART, SPI (the ST7789's bytes), I2C | Bluetooth: the board can't be set up over BLE, see below |
| Wi-Fi as a station, with NAT to the internet | PSRAM, I2S audio, touch, the DAC |
| TLS to the gateway, Noise, `link.register`, commands | ESP32-S3: its QEMU model has no Wi-Fi |

## What you need

- A Velxio Docker image (`docker compose build` in a Velxio checkout makes
  `velxio-velxio`).
- A QEMU in that image with three ESP32 fixes, which Velxio's QEMU doesn't
  have yet. Without the first two the firmware doesn't start:
  - eFuses report chip revision 3.0. Revision 0, QEMU's default, rejects apps
    signed with the RSA scheme.
  - `DPORT_WIFI_CLK_EN` is a register that reads back what was written. IDF 6
    asserts on its PHY clock bits.
  - `VELXIO_ESP32_MAC` sets the factory MAC, so each emulated board has its
    own node id.
- ESP-IDF v6, as for any other board.

## Run it

```sh
cd esp32
tools/board.sh velxio-sim build
python tools/velxio/make_flash.py build-velxio-sim flash.bin --tokens pairing.json
python3 tools/velxio/run.py flash.bin --seconds 120 --mac 10:01:00:5a:00:01 --spi spi.txt
python3 tools/velxio/screen.py spi.txt screen.png
```

`make_flash.py` runs in the ESP-IDF environment. It merges the build into
one 16 MB image, as the emulator boots a whole flash. `run.py` prints UART0
and exits after `--seconds`. `screen.py` rebuilds the screen from the SPI
bytes.

## Setting it up without Bluetooth

Without BLE, setup can't run on the board. `--tokens` writes what BLE setup
would have stored into the NVS partition, so the board starts set up:
`access_token`, `refresh_token`, `api_url_v2` and `noise_host` in the
`homehub` namespace, and `setup_complete` set to `1`. A gateway pairing
response holds these under `pairing` and can be passed as is. The file holds
live credentials: keep it out of the repository.

Without `--tokens`, the board waits for BLE setup and starts the Bluetooth
controller, which the emulator doesn't have. The controller init asserts and
the board reboots in a loop.

The node id comes from the MAC. Two boards with the same MAC on one VM are
one device to the gateway, and the second one's `link.register` fails with
`node_id_in_use`. Give each board its own `--mac`.
