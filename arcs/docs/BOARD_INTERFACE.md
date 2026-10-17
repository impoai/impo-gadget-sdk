# SDK and board: who provides what

The ARCS firmware follows the ESP32 model: the SDK is shared by every
ARCS gadget, and each piece of hardware is **one board file** that fills in
`gadget_board_t` (`gadget/gadget_board.h`, mirroring
`esp32/components/impo/impo_board.h`). Command, event and capability names come
from `../CAPABILITIES.md`.

ARCS boards all use the same SoC, so on-chip drivers (codec, ADC, PWM, I2C,
radios) are SDK code. A board file only says how its parts are wired and adds
what only it has.

## 1. Base capabilities: the SDK provides them

Every ARCS gadget gets these without writing code.

| Area | What the SDK does | Board input |
|---|---|---|
| Wi-Fi | scan, join, auto-reconnect, saved networks | — |
| BLE pairing | Impo app setup v5 (community), advertising, GATT | `ble_prefix` |
| Impo link | the link every gadget runs (`../esp32/main`, built unchanged): tokens, refresh, `fetch_vms`, Noise, `link.register`, `link.invoke` dispatch, `link.event` | — |
| Registration | builds `commands_v2` and `capabilities` from the board, identity (`platform`/`device_family` `arcs`, `model_id`), `product_id` | the board's tables, `model_id`, `product_id` |
| Time, TLS, storage | SNTP, verified TLS, KV | — |
| Audio | playback, mic capture, volume, beep, TTS and MP3 playback (`gadget_sound`, `gadget_play_url`) | `audio` (amp on/off hook) |
| Voice | touch/press to talk: end of speech on the gadget, voice note to `/chat/stream`, reply from `/chat/subscribe`, spoken via `/tts` (`gadget_voice`) | `audio`, AUX button |
| Power policy | `device.health` metrics, warning at 15 %, power-off when empty, never on USB (`gadget_status`, `gadget_policy`) | `read_power`, `power_off` |
| Buttons | main: hold 3 s powers off (the power-on press is ignored); AUX down: voice turn (`gadget_input`) | `poll_buttons` |
| Status | decides the state every 100 ms (`gadget_policy`) | `show_status` renders it, `show_animation` takes the light back |
| Standby | not yet | `standby` |
| System | watchdog, crash record, OTA, ADB shell, reboot | — |

**Standard commands the SDK registers** (CAPABILITIES.md), depending on what
the board filled in:

| Command | Registered when |
|---|---|
| `device.health`, `device.set_name` | always |
| `speaker.say`, `speaker.play_url`, `speaker.set_volume`, `speaker.beep`, `speaker.stop`, `speaker.status` | `audio.speaker` (PSRAM is on every ARCS board) |
| `display.show_animation` | `show_status` |

`capabilities.microphone` and `push_to_talk` are true when the board has a
speaker, a microphone and buttons (the AUX button starts a turn).

`capabilities` keys `speaker`, `microphone`, `push_to_talk`, `battery`,
`buttons` follow from `audio`, `read_power` and `poll_buttons`; `features`
comes from the board.

## 2. What a board provides

| Field | Meaning |
|---|---|
| `name`, `model_id`, `ble_prefix` | identity |
| `product_id` | the product created for this gadget on the Impo developer platform, 2 to 40 of `a-z`, `0-9`, `-`; sent in `link.register` so the agent gets that product's manifest. NULL or `""`: not set up yet (registers as custom without a product). An invalid one is logged and not sent |
| `init` | pins, buses, the board's own parts |
| `audio` | speaker / mic present, mic channel, gains, amp on/off |
| `poll_buttons` | raw button edges |
| `read_power`, `power_off` | battery reading, power latch |
| `show_status`, `show_animation` | how its light (if any) shows an SDK state; take the light back after a board command used it |
| `standby` | park and wake its actuators and sensors |
| `commands[]` | what only this hardware does, CAPABILITIES.md names first |
| `features` | `motion_sensor`, `locomotion`, `lights`, `touch_pads`, ... |
| `events[]` | what it reports, CAPABILITIES.md names first; for the product manifest, not sent in `link.register` |
| `instructions` | what the device is for, for the agent (≤ 1500 chars); for the product manifest, not sent in `link.register` |

Rules:
- A hook left NULL is hardware the board does not have; the SDK leaves out the
  matching capability and commands.
- A command needed by one board is a board command. When a second board needs
  it, it moves into the SDK with a contract, as CAPABILITIES.md says.
- Commands must not block the link task: start, return, offer a status command.
  The agent always waits 30 s at most.

## 3. The robot dog as a board (ARCS-MINI V2.1)

| Part | Field |
|---|---|
| Speaker (internal DAC, amp on PA01), 1 mic on ADC left | `audio` |
| Power key on PB04 | `poll_buttons` (main button) |
| Battery ADC PB05, charge PB08, USB detect PB00, latch PB03 | `read_power`, `power_off` |
| 5-pixel RGB strip on PA08 (eyes = pixels 2 and 4) | `show_status`, `chassis.set_light` |
| 4 leg servos on PA04–PA07, gait player | `standby` (fold legs), commands below |
| MPU6050 on I2C | `imu.read`, motion events |
| 2 touch pads PA09, PA13 | touch events |

`features`: `motion_sensor,touch_pads,locomotion,lights`.

Board commands (`dog_commands.c`):

| Command | Source |
|---|---|
| `chassis.drive` (`forward`, `turn` −1..1, `duration_ms`) | CAPABILITIES.md; the larger one picks walk/run/turn and its speed |
| `chassis.move` (forward/back/left/right/stop, `duration_ms`, `fast`) | CAPABILITIES.md, mapped to walk/turn cycles |
| `chassis.stop`, `chassis.dance`, `chassis.status` | CAPABILITIES.md; results carry `base` and `moving` |
| `chassis.set_light` (on/blink/breathe_slow/breathe_fast/flowing/show/off/auto, `color`, `brightness`) | CAPABILITIES.md |
| `imu.read` (upright, upside_down, face_up, face_down, on_left_side, on_right_side, tilted) | CAPABILITIES.md |
| `dog.pose` (stand, sit, lie_down, sleep) | board only |
| `dog.trick` (22 tricks), `dog.relax` | board only |

Board events: `picked_up`, `put_down`, `shaken` with `peak_g` and
`strength` (CAPABILITIES.md, as sparkbot_imu.c; none while its own gait
runs nor 2 s after); `upside_down` (board only). The touch pads are the AUX
button (voice), not an event.

The pure decisions (orientation, the event state machine, the drive mapping,
the light frames) are in `dog_logic.c` and host-tested.

Old-firmware autonomous behaviours (random idle tricks, touch → handshake,
upside-down → wiggle) are not carried over as reflexes: the board reports the
event and the agent decides, guided by `instructions` (once they reach the
agent through the product manifest).

`product_id` is `arcs-robot-dog`, the dog's product on the Impo developer
platform; it registers as a custom gadget of that product.

## 4. Layout

```
gadget/            SDK core: gadget_board.h, registration, command dispatch, standard commands
platform/arcs/     SDK on-chip drivers (Wi-Fi, BLE, TLS, audio, ADC, PWM...)
../esp32/main/     the link every gadget runs, built from there unchanged
platform/arcs/esp_compat/     the ESP-IDF calls it makes, on ARCS
link/              SDK glue that runs it (connection keeping, request streams)
features/          SDK features (pairing, voice, power policy, OTA)
boards/<board>/    one board file + helpers, the vendor's part
```
