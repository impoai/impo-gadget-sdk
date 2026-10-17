# AGENTS.md

How to build, test and flash the ARCS firmware. `README.md` has the overview,
`docs/BOARD_INTERFACE.md` the board interface.

## What this is

Impo Gadget firmware for ListenAI ARCS chips (the CP core's application). The
Link client is the ESP32 firmware's: `app/CMakeLists.txt` builds
`../esp32/main/noise_control.cpp`, `vm_api.c`, `stack_monitor.c` and
`../esp32/components/noise_core` unchanged, against the ESP-IDF compatibility
layer in `platform/arcs/esp_compat/`. Change those shared files for every
platform, never by copying them here; a new ESP-IDF call they make goes into
the compat layer, and a new `CONFIG_` they read into
`platform/arcs/esp_compat/include/sdkconfig.h`.

## Prerequisites

- The ListenAI ARCS SDK ([github.com/listenai/arcs-sdk](https://github.com/listenai/arcs-sdk), tested with 0.1.6) in `ARCS_BASE`.
- The ARCS toolchain: `brew install listenai/tap/arcs-toolchain` on macOS,
  the SDK's `source env.sh` on Linux, or the `ghcr.io/listenai/arcs-builder` image.
- `adb`.

## Build and test

```sh
export ARCS_BASE=/path/to/arcs-sdk NUCLEI_TOOLCHAIN_PATH=/opt/homebrew/opt/arcs-toolchain
export PATH=$NUCLEI_TOOLCHAIN_PATH/bin:$PATH
./build.sh -S ./app -B build -DBOARD=arcs_mini_v2_1     # add -DCONFIG_FILES=local.conf for build-time Wi-Fi
ARCS_BASE=$ARCS_BASE tests/host/run.sh                   # host tests, no hardware
```

Check the RAM use after a change: internal SRAM is nearly full. Large buffers
go to PSRAM through `malloc` (anything over 1 KB lands there); static arrays
land in SRAM.

## Flash and check

`README.md` > Flash. Only the app partition (0x600000); never flash the boot.
Then `SERIAL=<adb serial> tools/device_check.sh` (the dog moves and talks).

`app/local.conf` (from `local.conf.example`) holds build-time Wi-Fi and
pairing for bring-up. It is ignored by git: never commit it, and never commit
device logs, which carry tokens, VM ids and network names.
