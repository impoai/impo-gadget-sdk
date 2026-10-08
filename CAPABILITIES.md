<!--
Copyright (c) Impo contributors.

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

# Gadget capabilities

What a gadget can do for the agent, and the words it uses to say so. This is
the contract between the three layers of the project:

1. **Boards** (`esp32/components/impo/boards/`, `linux/`): one file per piece of
   hardware, adapting its screen, speaker, buttons and extras to the board
   interface. A board implements whichever capabilities its hardware has.
2. **Capabilities** (this document): a fixed vocabulary of commands with their
   parameters and results, grouped by namespace. A gadget registers the ones it
   implements; the agent and the app rely on the contract, not on the board.
3. **Ecosystem** (`skills/`): other people's devices on the home network,
   reached through a gadget's tunnel and controlled by the agent following a
   skill. Nothing in the firmware knows about them.

Anything a gadget needs that isn't here goes in as a board command first
(`impo_board_t.commands`); once a second board wants it, it comes here with a
contract.

## How a gadget says what it has

`link.register` carries two things:

- `commands_v2`: every command the gadget runs, each with a description the
  agent reads and `required` / `optional` parameters (`{"type", "description"}`
  each). The agent calls a command with exactly these parameters; a result is
  `{"ok": true, "payload": {...}}` or `{"ok": false, "error": "..."}`.
- `capabilities`: a summary for apps and prompts, without listing commands:

```json
{
  "screen": {"width": 240, "height": 240, "color": true, "round": false, "touch": false},
  "avatar": true,
  "speaker": true, "microphone": true, "push_to_talk": true,
  "camera": true, "battery": true, "buttons": true,
  "features": ["motion_sensor", "touch_pads", "locomotion", "lights"]
}
```

`features` names what the fixed keys don't: a board lists its own in
`impo_board_t.features`. Known values: `motion_sensor` (`imu.*`), `locomotion`
(`chassis.*`), `lights` (lights the agent can set), `touch_pads`,
`environment_sensors` (`sensors.read`).

The Impo server shows `capabilities` with each gadget in `impo_list_gadgets`;
the gateway keeps it with the registration.

## The vocabulary

Every command runs on the gadget and answers within its `timeout_ms` (30 s
unless the command says otherwise). Parameters are JSON; text is UTF-8 unless
a command says ASCII. "Any board with the full UI" means the boards that run
the avatar, captions and settings (`CONFIG_IMPO_ENABLED`).

### `display.*` — the screen

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `display.show_text` | full UI | `text` (1–200 chars, ASCII shown as is; others replaced) | — |
| `display.draw_url` | boards that draw images | `url` (public https image), `row`? | — |
| `display.show_animation` | status-light boards | — | — |
| `avatar.cheer` | full UI | — | — |

Showing text wakes the screen. The caption stays until the next thing the
gadget shows.

### `speaker.*` — sound

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `speaker.set_volume` | any speaker | `percent` 0–100 | `{"percent"}` |
| `speaker.beep` | any speaker | — | — |
| `speaker.say` | any speaker with Hatch | `text` (1–1000 chars, any language) | `{"status": "fetching"}` |
| `speaker.play_url` | any speaker with PSRAM | `url` (public https MP3, ≤ 1.5 MB, ~1 min) | `{"status": "fetching"}` |
| `speaker.stop` | any speaker | — | — |
| `speaker.status` | any speaker | — | `{"status": idle/fetching/decoding/playing/failed, "error"?, "volume_percent", "speaker_on"}` |

`say` and `play_url` return at once and the sound starts a few seconds
later; `status` tells how it went. `say` is spoken in Impo's voice: the gadget
asks the gateway (`POST /tts`), which asks the Impo API's text to speech. No
speech service key is ever on a gadget. Replies to push-to-talk are spoken the
same way.

### `camera.*` — pictures

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `camera.capture` | boards with a camera | — | `{"format": "jpeg-base64", "data_base64"}` |

The Impo server turns the result into a file in the agent's sandbox and hands
the agent its id, so the model sees the picture rather than its bytes.
`timeout_ms` is 30000.

### `imu.*` — motion

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `imu.read` | `motion_sensor` | — | `{"orientation", "moving", "accel_g": {x,y,z}, "gyro_dps": {x,y,z}}` |

`orientation` is one of `upright`, `upside_down`, `face_up`, `face_down`,
`on_left_side`, `on_right_side`, `tilted`. `moving` is true while the gadget is
being shaken or turned.

### `chassis.*` — moving about

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `chassis.drive` | `locomotion` | `forward` −1..1, `turn` −1..1, `duration_ms` ≤ 10000 (500) | `{"base", "moving"}` |
| `chassis.move` | `locomotion` | `direction` forward/back/left/right/stop, `duration_ms`? | `{"base", "moving"}` |
| `chassis.stop` | `locomotion` | — | `{"base", "moving"}` |
| `chassis.dance` | `locomotion` | — | `{"base", "moving"}` |
| `chassis.set_light` | `lights` | `effect` on/blink/breathe_slow/breathe_fast/flowing/show/off | `{"base", "moving"}` |
| `chassis.status` | `locomotion` | — | `{"base", "moving"}` |

A drive stops by itself when its time is up, and whenever the gadget stops
hearing the base. `base` is `responding`, `silent` or `unknown`: whether the
base answered the gadget, which proves the link, not the movement.

### `sensors.*` — the surroundings

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `sensors.read` | `environment_sensors` | — | CO₂, tVOC, temperature, humidity, each with its age; null when absent |

### `device.*` — the gadget itself

| Command | Who has it | Parameters | Result payload |
|---|---|---|---|
| `device.health` | every gadget | — | `{"overall", "metrics": {uptime, heap, battery_pct, battery_mv, charging, usb_power}, "network_ssid", "version"}` |
| `device.discover` | gadgets with the tunnel | — | devices found on the home network |
| `device.ota` | gadgets with OTA on | `url`, `force`? | — (not offered to the agent) |

## Events (planned)

Today the agent calls the gadget; the gadget never calls the agent. A
`link.event` from the gadget (a pad touched, the gadget picked up or shaken,
the base bumping into something) is the next addition to the protocol, so the
agent can react to the world instead of only polling it.

## Adding to this

- A new board implements existing capabilities through `impo_board_t`
  (screen, audio, buttons, power, camera registration) and lists its extras in
  `features`; see `esp32/components/impo/boards/README.md`.
- A new command for one board goes in that board's `commands` table.
- A command two boards share moves to the common table in
  `esp32/components/impo/impo_commands.c` and gets a row here, with its
  parameters and result payload spelled out before anything depends on it.
- The Linux client (`linux/`) is the reference implementation of the common
  capabilities without hardware: a laptop's screen, speaker, microphone and
  camera.
