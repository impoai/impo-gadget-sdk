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

`link.register` carries four things, all written in the firmware. Nothing
about a gadget is configured on a server: what the agent knows of a device is
what the device said when it connected.

- `commands_v2`: every command the gadget runs, each with a description the
  agent reads and `required` / `optional` parameters (`{"type", "description"}`
  each). The agent calls a command with exactly these parameters; a result is
  `{"ok": true, "payload": {...}}` or `{"ok": false, "error": "..."}`.
- `events`: the events it sends (below), each with what it means on this
  device: `[{"name": "picked_up", "description": "Someone lifted the robot
  off the desk, most likely to talk to it."}]`. At most 16, descriptions up
  to 200 characters.
- `instructions`: the maker's words to the agent that owns the gadget, in
  plain language, up to 1500 characters: what the device is for and what to
  do when its events arrive ("When it is picked up, greet them and read their
  next calendar event. Everything you say is spoken aloud: three sentences,
  no markdown."). The agent carries them out with whatever the person has
  connected (mail, calendar, other gadgets) and says so when something is
  missing. They sit below the person's own wishes and the platform's rules:
  a gadget can ask, never override, and it has no access of its own. The
  gateway refuses a registration over these limits (`invalid_instructions`,
  `invalid_events`), so a firmware that writes too much finds out at once.
  A gadget's task answers one thing at a time: while the agent works on one
  event, the next waits. So ask for what one lookup can answer ("the subjects
  of the two newest unread mails"), not for what takes a tour ("how many
  unread mails"): a long turn leaves the gadget mute until it ends.
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

The Impo server shows `capabilities`, `events` and `instructions` with each
gadget in `impo_list_gadgets`; the gateway keeps them with the registration
and quotes the event's meaning and the instructions with every `link.event`
it passes on. A board fills them in `impo_board_t.events` and
`impo_board_t.instructions`.

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
| `device.set_name` | every gadget | `name` (1–32 characters) | — ; the gadget stores the name, reconnects under it, and the account's gadget list shows it from then on |
| `device.discover` | gadgets with the tunnel | — | devices found on the home network |
| `device.ota` | gadgets with OTA on | `url`, `force`? | — (not offered to the agent) |

## Events

The gadget can call the agent too: `link.event` on the control stream, with
`{"event": "<name>", "data": {...}, "uptime_ms"}`. The gateway posts it to the
gadget's own standing task ("[Gadget event] Desk Bot: shaken (peak_g=2.3,
strength=hard)", plus what the board says it means), never to the person's
conversation, and the agent's reply is spoken by the gadget. Events are
for things worth a remark, debounced on the gadget; the gateway ignores a
repeat of the same event from the same gadget within three seconds.

| Event | Who sends it | Data |
|---|---|---|
| `shaken` | `motion_sensor` | `peak_g`, `strength` gentle/hard |
| `picked_up` | `motion_sensor` | `orientation` it ended up in |
| `put_down` | `motion_sensor` | `orientation` (upright) |

A board sends one with `impo_link_send_event(name, data)` and describes it
in `impo_board_t.events`; a new event name gets a row here first.

What happens next is the agent's call, guided by the gadget's
`instructions`. A reaction that needs no judgement (a sound, a fixed phrase,
a light) belongs in the firmware itself: `speaker.say` through the platform's
TTS, or a local file through `impo_sound`; it is instant and works without
the agent. An event is for what needs the person's context.

## Memory

One ESP32-S3 holds Wi-Fi, BLE, TLS, the UI, the Link session and the voice
session before any capability gets a byte, and what is left (about 40 KB of
internal RAM in pieces, a few MB of PSRAM) is shared by all of them. So a
capability's memory is decided up front, not taken when used:

| Capability | Memory | When |
|---|---|---|
| Replies spoken (`tts_data` → `decode`) | 512 KB PSRAM ring + decoder | at start-up, once |
| `speaker.say`, `speaker.play_url` (`impo_sound`) | 160 KB in + 64 KB out, PSRAM; a 32 KB decoder task | at start-up, once |
| `camera.capture` | 2 × 614 KB frames in PSRAM, 8 KB DMA pieces in internal RAM, a 614 KB copy and a 150 KB JPEG | for the second a photo takes, then given back |
| `imu.read`, the motion watcher | a 3 KB task | at start-up |

Rules that keep it that way: stream, never buffer a whole file (a sound of
any length costs the same); release what a command took before it returns
(the camera stops after each photo); never let a worst-case constant size an
allocation; and tell memory failures apart from bad data in the error. The
numbers to watch are `heap_largest_internal` and `heap_largest_psram` in
`device.health`: the largest piece, not the total, is what a DMA buffer or
a frame needs.

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
