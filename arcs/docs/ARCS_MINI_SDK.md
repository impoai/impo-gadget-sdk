# ARCS Mini product and application SDK

The `arcs_mini_voice` target adapts a stationary ARCS Mini with one 240 x 240
ST7789P3 screen, PB04 function/power key, PA01 amplifier and the codec's left
microphone. It is a separate target from the V2.1 robot dog. Confirm the
[wiring](../boards/arcs_mini_voice/README.md) and AP image for another variant.

Hardware belongs in the board adapter; product identity and optional acoustic
resources belong in product JSON. Applications extend the existing
`gadget_command_t` and `gadget_event_t` contracts. Pairing, Wi-Fi, verified TLS,
Noise and native Impo dialogue remain in the official SDK. The shared Link
sources remain under `esp32/`, compiled directly against ARCS compatibility
headers rather than copied into a second protocol implementation.

| Path under `arcs/` | Purpose |
| --- | --- |
| `components/impo_arcs_mini/include/` | Public C/C++ product and application API |
| `components/impo_arcs_mini/src/` | Official API delegation and read-only resource validation |
| `boards/arcs_mini_voice/` | Pins, button, screen, amplifier and AP wake engine |
| `products/arcs_mini.json` | Generic button-voice product, without acoustic dependencies |
| `examples/button_companion/` | Complete custom application and product |
| `tools/product.py`, `tools/wake_bundle.py` | Product validation and external resource descriptors |

## Build

Install ListenAI ARCS SDK **0.1.6** and the toolchain in the
[ARCS README](../README.md). SDK 0.1.8 has incompatible IPC APIs. From `arcs/`:

```sh
export ARCS_BASE=/path/to/arcs-sdk-0.1.6
./build.sh -S app -B build-mini -t mkhdr -DBOARD=arcs_mini_voice
python3 tools/check_image.py build-mini --no-wake
```

The image is `build-mini/impo-gadget.bin`. Before loading Kconfig, CMake validates
the product and writes its header, Kconfig, font and metadata under
`build-mini/impo_product/`. Product, wake descriptor and optional font changes
rerun configuration. No source-overlay installer or nested SDK clone is needed.
Building does not flash a device.

Build the custom application example:

```sh
./build.sh -S app -B build-mini-example -t mkhdr -DBOARD=arcs_mini_voice \
  -DIMPO_PRODUCT_FILE="$PWD/examples/button_companion/product.json" \
  -DIMPO_APP_DIR="$PWD/examples/button_companion"
python3 tools/check_image.py build-mini-example --no-wake
```

Copy the example to an external project for another product on this wiring.
`IMPO_APP_DIR` must contain `CMakeLists.txt` and implement
`impo_arcs_mini_application()`. Leave it empty for the built-in empty application.
The SDK continues to own `main()` and the protocol.

## Product configuration

JSON uses `schema_version: 1`. Required fields are `id`, `display_name`,
`model_id` and `node_prefix`. Machine identifiers use 1–32 lowercase letters,
numbers or hyphens. Local `id` is distinct from platform `product_id`.

| Optional field | Contract |
| --- | --- |
| `screen_title` | Defaults to the name; must fit 216 px |
| `product_id` | Impo-assigned lowercase UUID, or `null` before registration |
| `instructions` | At most 1500 UTF-8 bytes; generated if omitted |
| `continuous_dialogue` | Default `true`; wait for each listening prompt |
| `china_relay` | Default `true`; official mainland route and direct fallback |
| `wake` | `{"enabled":false}`, or enabled plus a relative `bundle` descriptor path |

Unknown fields are rejected. Network credentials and pairing do not belong in
product JSON. Pair each unit with its owner's account through official BLE
developer setup. A CP build does not update its cloud product definition.

## Application API

Include `impo_arcs_mini.h`. Descriptors, tables and callbacks have static lifetime.

- `impo_arcs_mini_product()` returns the compiled, read-only product.
- `impo_arcs_mini_begin_conversation()` requests an official voice turn; returns
  `-1` when offline, busy or already in a continuous conversation.
- `impo_arcs_mini_say(text)` starts asynchronous official TTS. Success means
  accepted, not playback complete.
- `impo_arcs_mini_set_wake_enabled(bool)` pauses/resumes an initialized engine;
  it cannot enable an absent model or failed engine.
- `impo_arcs_mini_wake_available()` reports runtime readiness.
- `impo_arcs_mini_send_event()` retains `gadget_send_event()`'s JSON ownership.

Use task context. `on_init` runs before audio/Link startup; `on_status` runs on
the SDK status task and must return promptly. Command handlers follow the
official nonblocking start/return/status contract; long work belongs in an
application task. The example adds `companion.info` through this interface.

## Optional local wake word

Editing a keyword string does not train a model. Obtain licensed resources
compatible with the installed AP and ACOMP's one-microphone/one-reference
interface. Use the exact `keyword` strings returned by that engine:

```sh
python3 tools/wake_bundle.py --id mini-wake-v1 --label 'Hello Mini' \
  --keyword 'hello mini' --model /private/resources/mini.bin \
  --wrap /private/resources/mini.json --output /path/to/product/wake.json
```

Set `"wake":{"enabled":true,"bundle":"wake.json"}` in product JSON.
The descriptor contains 1–8 result strings, ROMFS root filenames and SHA256
hashes computed from actual files. Resources must already exist in the retained
ROMFS; the CP build does not install them. Startup checks bounds, hashes, model
header and wrap channels before ACOMP initialization. Failure retains button
voice. Matching hashes establish identity, not recognition accuracy or AP
compatibility; accept new models on physical hardware separately.

The default product needs no model, Pillow or source font. Bundled glyphs cover
printable ASCII and status text. Missing title/wake glyphs cause an explicit
error. To add glyphs, install Pillow on the build host and pass
`-DIMPO_PRODUCT_FONT=/path/to/NotoSansSC-variable.ttf`; its SHA256 must be
`a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da`.
The Noto Sans SC glyphs from Google Fonts/Adobe are covered by
[OFL-1.1](../boards/arcs_mini_voice/LICENSE-OFL). The screen supports BMP
characters, not emoji.

## Manifest and validation

From `arcs/`:

```sh
python3 tools/gen_arcs_mini_manifest.py > /tmp/arcs-mini-manifest.json
python3 tools/gen_arcs_mini_manifest.py \
  --product examples/button_companion/product.json \
  --commands examples/button_companion/app.c:commands > /tmp/mini-example-manifest.json
ARCS_BASE=/path/to/arcs-sdk-0.1.6 sh tests/host/run_arcs_mini_sdk.sh
```

Repeat `--commands FILE:TABLE` and `--events FILE:TABLE` for static application
tables. The tool reuses the official parser and checks duplicates/count limits;
it cannot infer dynamically constructed C tables. The robot-dog tool is retained.

The host suite covers configuration failures, API delegation, button debounce,
screen bounds, buffered speech, complete capture, continuous-dialogue exit and
cancellation, microphone ownership, preferred Wi-Fi, TLS scheduling and route
identity. With `ARCS_BASE`, it also runs the existing ARCS BLE/crypto/logic suite.
Without it, external-SDK tests report an explicit skip; CI runs this subset.

Short TTS buffers complete PCM before playback; long responses start at a
bounded high-water mark. Voice captures a bounded note before slow upload.
Continuous dialogue is opt-in per board; the robot dog retains single-turn
behavior. Recording takes exclusive microphone priority over the idle wake
listener. Runtime wake readiness uses an optional shared-Link hook, defaulting
to `false` on ESP32.

Earlier firmware on this wiring was physically observed to show status, play
a complete answer and continue a conversation on the direct path. This reusable
SDK refactor and the current relay TLS-name change have not been flashed as
part of this contribution. Host tests and compilation remain separate from
physical acceptance and long-term reliability. See [cloud routing](CLOUD_ROUTE.md).
