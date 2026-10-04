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

# Your avatar on your board

A board's default avatar is drawn by `avatar/impo_pixel.c`: a
procedural 64x64 pixel-art renderer with an animation for each state (boot,
idle, listening, thinking, speaking, error, off, and a happy reaction when
you pet it). Your Impo has its own avatar, and it can redraw that avatar as
a new `impo_pixel.c`.

This recipe starts from the default avatar's code, but your avatar should be
your own, with its own name and look. The default avatar is Meta's character,
and the Apache License doesn't grant rights to it.

Your avatar goes in `components/impo/avatar/impo_pixel.c`. When that file
exists, the firmware build and the preview tools use it in place of the default avatar.
The directory is gitignored, so your avatar stays on your machine. Delete
the file to go back to the default avatar.

## The quick way

Plug in the board (the S3 or AIPI Lite), then run:

```sh
cd esp32
python3 tools/impo/avatar.py
```

It asks your Impo for your avatar through the board, so your computer needs
no token. Then it checks the file, builds the firmware and flashes it. That
takes a few minutes, most of it waiting for Impo. To change the avatar
afterwards, describe what to change:

```sh
python3 tools/impo/avatar.py --edit "make the ears bigger and the eyes green"
```

What it does, step by step:

1. Finds the board on USB and asks it for its state (`>status` on its
   console). It stops if the board isn't on Wi-Fi or can't reach your Impo
   (it needs to be paired in the Impo app, or have a device token), and says
   what to set up.
2. Sends your Impo `tools/impo/avatar_prompt.md` with the current renderer
   attached, as a typed message through the board (`tools/impo/chat.py`).
   Impo replies with a new `impo_pixel.c`.
3. Saves the reply as `components/impo/avatar/last_reply.md` and the file
   as `components/impo/avatar/impo_pixel.c`. The file it replaces becomes
   `impo_pixel.c.prev`. The first comment in the file says what Impo drew,
   and the tool prints it.
4. Builds the file on your computer with warnings as errors, then runs every
   animation with the address sanitizer. If either fails, it sends the errors
   back to Impo and asks for a fixed file, twice at most. It also renders one
   GIF per animation to `components/impo/avatar/gifs/`.
5. Builds the firmware with `tools/impo/board.sh build` and flashes it.

Options: `--no-flash` stops after the build, `--port` picks the board when
more than one is plugged in, and `--reply FILE` skips asking Impo and uses a
reply you saved yourself.

It needs Python 3 with `pyserial` and `pillow`, a C compiler, and ESP-IDF
(see `../../AGENTS.md`). An agent in a sandbox needs the sandbox off to open
the serial port.

### What the exit status means

| Status | Meaning |
|---|---|
| 0 | Done |
| 1 | Impo's file didn't work, or the build or flash failed. The message says which |
| 2 | No board found, the board doesn't answer, or it can't chat over USB |
| 3 | The board isn't on Wi-Fi or isn't connected to your Impo |

When the board doesn't answer, it's usually running firmware from before
serial chat. Add `--board s3` (or `--board aipi`, `--board sticks3`,
`--board stopwatch`, `--board cores3`, `--board watcher`) and the tool flashes current firmware
first, then carries on.

The SenseCAP Watcher chats on its CH342 port ending in `3`, with firmware
that has `IMPO_CONSOLE_UART`. That bridge drops bytes from whole packets, so
`tools/impo/chat.py` writes to it 64 bytes at a time, and sends a newline or
two first to wake it from light sleep. Sending the prompt takes a few seconds
longer than on the other boards.

## By hand

The C6 has no PSRAM, so it can't chat over USB. On it, or if you'd rather do
it yourself:

1. Paste `tools/impo/avatar_prompt.md` into Impo and attach
   `avatar/impo_pixel.c`, which Impo starts from.
2. Save the whole reply to a file, then check it and build it:

   ```sh
   python3 tools/impo/avatar.py --reply reply.md --board c6
   ```

   This saves the file, checks it, builds the firmware and flashes it. With
   `--no-flash` and no board plugged in, it only checks and builds. The file
   can hold the whole reply or just the C file.

You can also save the C file straight to `components/impo/avatar/impo_pixel.c`
and build as usual with `tools/impo/board.sh build <board>`. The build log
says `Custom avatar: components/impo/avatar/impo_pixel.c` when it picks up your file.

## Checking the result

- Preview without a board:
  `python3 tools/impo/make_gifs.py /tmp/avatar_gifs` draws your avatar (or
  the default avatar, with `--default`). There's one GIF for each of boot, idle,
  listening, thinking, speaking, happy, off and error. Check each against the
  animation beats in `avatar_prompt.md`.
- If a state looks wrong, run `avatar.py --edit` with the GIF's name and what
  to change. Impo then edits its file rather than starting over.
- Timing on the device: the UI logs nothing per frame, so if the art is
  heavy, time `impo_pixel_render` on the C6 with `esp_timer_get_time()`. It
  should stay under 40 ms (under 10 ms on the S3).

## Talking to Impo through the board

`tools/impo/chat.py` sends any message to your Impo through the board and
prints the reply as it streams in:

```sh
python3 tools/impo/chat.py "What does your avatar look like?"
python3 tools/impo/chat.py --status     # the board's state, as JSON
```

Lines that start with `>` on the board's USB console are commands. `status`
prints the board's state. `chat+=TEXT` adds a line of a message, and
`chat=TEXT` adds the last line and sends it (with `\n`, `\t` and `\\`
escapes). `chat.cancel` drops the message or the turn in progress. The board
acknowledges each line and streams the reply back as `@chat {...}` JSON
lines, which `impo_hatch_text_turn` in `components/impo/impo_chat.h`
describes. A typed turn isn't spoken aloud, and pressing the talk button
cancels it. `power` prints the battery meter as `@power {...}` and
`power.reset` starts it over; `tools/impo/power.py` turns it into a report.
