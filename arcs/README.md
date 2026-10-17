# ARCS Device SDK

Impo Gadget firmware for the ListenAI **ARCS** SoC (LS26xx, dual-core RISC-V;
this is the CP core's application). The device pairs with the Impo app over
BLE and talks directly to the Impo gadget service (`gadgets.impo.ai`); Impo
does speech-to-text and text-to-speech.

It runs the same Impo link as the ESP32 firmware, built from `../esp32`
unchanged (see Layers), and follows the ESP32 model: the **SDK** gives every ARCS
gadget the same base (network, link, voice, speaker, standard commands,
power and status policy), and each piece of hardware is **one board file**
filling `gadget_board_t`. See `docs/BOARD_INTERFACE.md`.

First board: ARCS-MINI V2.1 robot dog (`boards/arcs_mini_v2_1`).

## Layers

| Layer | Path | Contents |
|---|---|---|
| L0 platform | `platform/arcs` | Wi-Fi, SNTP, TLS (mbedtls 2.28, embedded roots), HTTPS client, BLE, KV, USB ADB shell, speaker DAC and mic ADC, watchdog with task supervision, crash record |
| L1 link | `../esp32/main` + `link/` | the link every Impo gadget runs, compiled straight from the ESP32 tree: `vm_api.c` (tokens, `fetch_vms`) and `noise_control.cpp` (TLS, WebSocket, Noise via `../esp32/components/noise_core`, `link.register` / `invoke` / `result` / `event`, request streams, keepalive, reconnect). `link/` is the glue that runs it here (what `esp32/main/app.c` does around it, request streams for `/tts` and `/chat/*`, the `link` command) |
| ESP-IDF compat | `platform/arcs/esp_compat` | the ESP-IDF calls those files make (`esp_tls`, `esp_http_client`, `esp_log`, FreeRTOS, lwIP sockets...), on the platform layer; `include/sdkconfig.h` is the configuration they see. Seen by those files only |
| SDK | `gadget/` | the board interface, standard commands, speaker (`say`, `play_url`, beep, volume), voice turns, status light and battery policy, buttons |
| features | `features/pairing` | Impo BLE setup protocol v5 (community mode) |
| board | `boards/<board>/gadget` | one board file plus the drivers only that hardware has |

## What the robot dog does (CAPABILITIES.md)

| Area | Commands / behaviour | Layer |
|---|---|---|
| device | `device.health` (official fields), `device.set_name` | SDK |
| speaker | `speaker.say`, `speaker.play_url` (https MP3/WAV ≤ 1.5 MB), `speaker.beep`, `speaker.set_volume`, `speaker.stop`, `speaker.status` | SDK |
| voice | touch its back, speak, it ends after a pause, the agent's reply is spoken | SDK (trigger: the board's AUX button) |
| status light | booting, unpaired, connecting, online, listening, thinking, speaking, offline, low battery, charging; `display.show_animation` | SDK decides, board draws |
| battery | warns once at 15 %, powers off when empty (never on USB) | SDK |
| buttons | power key: hold 3 s to power off | SDK policy, board reads keys |
| locomotion | `chassis.drive`, `chassis.move`, `chassis.stop`, `chassis.dance`, `chassis.status` | board |
| lights | `chassis.set_light` on/blink/breathe_slow/breathe_fast/flowing/show/off/auto | board |
| motion sensor | `imu.read` (7 orientations); events `picked_up`, `put_down`, `shaken`, `upside_down` | board |
| tricks | `dog.pose` (stand, sit, lie_down, sleep), `dog.trick` (22 tricks), `dog.relax` | board |

Not on this board: screen, camera, environment sensors, home-network tunnel, OTA.

## Identity in Impo

`link.register` follows contracts/gadget-identity.md: it carries `commands_v2`,
`capabilities` and the identity (`node_id`, `display_name`, `platform` and
`device_family` `arcs`, the board's `model_id`, `version`). What the gadget
is for and what its events mean come from the platform, from the manifest of
the product the gadget declares. ARCS has no unit key, so a gadget registers
as custom with `product_id` from its board (`gadget_board_t.product_id`); the
robot dog's is `arcs-robot-dog`. Its instructions and event descriptions stay
in the board file, the source for that product's manifest.

## Prerequisites

- **The ListenAI ARCS SDK**, outside this repository:
  [github.com/listenai/arcs-sdk](https://github.com/listenai/arcs-sdk)
  (documentation: [docs2.listenai.com/arcs-sdk](https://docs2.listenai.com/arcs-sdk/)).
  This firmware was built and tested against ARCS SDK 0.1.6. Point `ARCS_BASE` at it.
- **The ARCS toolchain** (Nuclei RISC-V GCC with newlib). macOS:
  `brew install listenai/tap/arcs-toolchain`. Linux: the SDK's `source env.sh`
  installs it. Anywhere else: the `ghcr.io/listenai/arcs-builder` Docker image
  has it preinstalled.
- `adb` for flashing and the device shell; a C compiler for the host tests.
- The application core image: the ARCS-MINI's AP core runs the vendor's
  algorithm firmware (`ap.bin`), which this repository does not include.
  Flashing only the app partition (below) leaves the one on the board in place.

## Build

From this directory (`arcs/`):

```bash
export ARCS_BASE=/path/to/arcs-sdk
export NUCLEI_TOOLCHAIN_PATH=/opt/homebrew/opt/arcs-toolchain
export PATH=$NUCLEI_TOOLCHAIN_PATH/bin:$PATH
cp app/local.conf.example app/local.conf   # optional build-time Wi-Fi for bring-up
./build.sh -S ./app -B build -DBOARD=arcs_mini_v2_1 -DCONFIG_FILES=local.conf
```

Output: `build/impo-gadget.bin` (app partition at 0x600000).

## Flash (app partition only)

```bash
ADB_CMD=$(which adb) BUILD_DIR=build bash adb_download.sh -S res/arcs_mini_v2_1 app
```

`tools/device_check.sh` and these notes refer to the dog's adb serial
(`adb devices`); set `SERIAL` to it.

Notes for the V2.1 dog's bootloader:
- If adb does not list the `BOOT-<id>` recovery device, restart the adb server.
- After a watchdog-type reset the boot restarts every ~6 s until the host opens
  `adb -s BOOT-<id> shell <id>` within 3 s; then `adb shell reboot` returns to the app.
- With another adb device attached (an Android emulator), `adb_download.sh` waits for it
  too; flash by hand: `shell recovery`, the handshake, `root;listenai;upgrade enter`,
  `push build/impo-gadget.bin /RAW/NAND/600000/<size in hex>`, `md5sum` that path, `reboot hard`.

## Tests

Host unit tests (no hardware), for every piece of pure logic:

```bash
ARCS_BASE=/path/to/arcs-sdk tests/host/run.sh
```

| Test | Covers |
|---|---|
| `test_pairing_crypto`, `test_ble_setup` | BLE setup v5 against the SDK's pairing vectors |
| `test_gadget_pcm` | streaming WAV, mono mix, resampling, base64, rms |
| `test_gadget_voice_logic` | end of speech, line splitting, reply tracking from `/chat/subscribe` |
| `test_gadget_policy` | status light choice, battery warning and shutdown |
| `test_dog_logic` | orientation, picked_up/put_down/shaken, `chassis.drive` mapping, light effects |
| `test_link_glue` | product id check, credential refresh and retry pacing, command results for the link, session history, redirect URLs of the `esp_http_client` compat |

On the device (the dog moves and talks; about 4 minutes):

```bash
SERIAL=<serial> tools/device_check.sh            # everything
SERIAL=<serial> tools/device_check.sh --no-motion --no-voice
```

The voice check is a loopback: the dog asks a question out loud into its own
microphone and must hear the agent's answer.

## Device shell (adb shell)

- `gadget <command> [json]`: run any registered command, as the agent would.
- `talk`: same as touching its back. `talk_test [text]`: a voice turn spoken into its own mic.
- `mic_test`, `mic_rate`, `touch_raw`: microphone and touch pad checks.
- `link_drop`: drop the Impo session (reconnect test). `wdt_test task|hang`: watchdog test (resets).
- `link`: link state, how long online and since the gateway last sent anything, and the last 10 session ends with why.
- `wifi`, `time [sync]`, `https <url> [bearer]`, `pair [...]`, `dog ...`, `version`, `reboot`, `recovery`.

Logs and `shellPrint` reach ADB; `printf` goes to UART only.

## Product manifest

`tools/gen_manifest.py` builds the product manifest for the Impo developer
platform (contracts/gadget-identity.md, ManifestInput) from the board's command
tables, events and instructions, so the platform describes exactly what the
firmware registers.

## Licensing

Original Impo code here is under the MIT License (`../LICENSE`). Files with a
header of their own keep it unchanged:

| Files | Header |
|---|---|
| `boards/arcs_mini_v2_1/board.c`, `pinmux.c`, `pinmux.h`, `Kconfig`, `CMakeLists.txt` | Copyright (c) 2026, LISTENAI, Apache-2.0 |
| `platform/arcs/power_manager.c`, `power_manager.h` | Copyright (C) 2025 ANHUI LISTENAI Co., Ltd. |
| `platform/arcs/shell_cfg_user.h` | (c) 2019 Letter (letter-shell) |
| `platform/arcs/tusb_config.h` | (c) 2019 Ha Thach (TinyUSB), MIT |

Other parts come from ListenAI's ARCS SDK and its ARCS-MINI V2.1 firmware
without a header of their own: the build and flash scripts (`build.sh`,
`adb_download.sh`, `tools/make_package.py`), the board and memory
configuration (`app/prj.conf`, `app/memap.h`, `boards/arcs_mini_v2_1/`),
`platform/arcs/app_usb_descriptors.c`, `bt_config.h`. The robot dog's motion
and sensor drivers (`boards/arcs_mini_v2_1/gadget/dog_actuators.*`,
`dog_gait.*`, `dog_gait_preflight_policy.*`, `dog_sensors.*`,
`dog_imu_interaction.*`, `dog_shake_detector.*`, `dog_rgb_layout.h`) were
written by Marshall for that firmware and adapted here. Both are published
here with permission. `../NOTICE` records this.

Building needs the ARCS SDK, which is not part of this repository and comes
under ListenAI's terms.
