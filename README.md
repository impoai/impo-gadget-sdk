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

# Impo Gadgets

> **Impo fork.** This is [Impo](https://impo.ai)'s fork of Meta's
> [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk),
> the open-source Muse Gadgets SDK. It differs from upstream in these ways:
>
> - Gadgets connect to `https://gadgets.impo.ai` by default, instead of
>   `https://api.muse.ai` and `hatch.metaaivm.com`. A gadget still follows
>   `api_url_v2` and `noise_host` when setup provides them.
> - Every "Muse" name in the code, files and documentation is now "Impo",
>   including the BLE name prefix (`ImpoGadget`), the Linux package and
>   command (`impogadget`) and the ESP32 component (`components/impo`). The
>   wire protocol is unchanged. "Impo Home Link" in the firmware and skills
>   refers to the same reference design upstream calls Muse Home Link.
> - The default avatar is Robin, an original Apache-licensed character.
> - The ESP32 firmware verifies cross-signed certificate chains, which the
>   Impo gateway's TLS certificate needs.
> - It adds a board: the Espressif ESP-SparkBot.
> - Boards with the full UI declare extra commands (`display.show_text`,
>   `avatar.cheer`, `speaker.set_volume`, `speaker.beep`), and a board can add
>   its own through `impo_board_t.commands`; see
>   [`esp32/components/impo/impo_commands.h`](esp32/components/impo/impo_commands.h).
>
> See [gadgets.impo.ai](https://gadgets.impo.ai) for what the Impo gateway
> supports today. Gadgets pair from the Impo app (Settings > Gadgets) over
> BLE; the Impo gateway neither issues nor requires SDK tokens, so skip the
> token steps and ignore the token warnings. Muse is a trademark of Meta
> Platforms, Inc.; Impo is not affiliated with or endorsed by Meta.

Impo gadgets are open source devices you build yourself. Program an
off-the-shelf ESP32 board or set up a Raspberry Pi with our device SDKs, then
connect Impo to your displays, buttons, sensors, actuators, and whatever else
you've got lying on your workbench.

We open sourced the SDKs and firmware here. It's built by hackers, for hackers,
just for fun. Side effects of tinkering may include bricked boards, voided
warranties, brownouts, or bankruptcies. Proceed at your own risk!

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | Connect your ESP32 board to Impo through our open source SDK. Throw in a screen to show images, add audio in and out, or wire up other sensors. |
| [**Linux Device SDK**](linux) | Turn that spare Raspberry Pi or Linux box into a Impo gadget. Hack in your own commands to let Impo handle sysadmin chores or your Home Assistant setup. |

ESP32 and Linux gadgets pair with the Impo app on iOS and Android, via
Settings > Devices. Turn on Developer mode there first, then look for devices
prefixed with "ImpoGadget".
Each directory has a `README.md` to get started and an `AGENTS.md` for coding
agents like [Muse Code](https://developer.meta.com/ai/lp/muse-code/).

## How it fits together

[`ARCHITECTURE.md`](ARCHITECTURE.md) is the full account: components, what
trusts what, the protocol surfaces, the four flows (a command, a voice turn,
an event, a sound) as sequence diagrams, where state lives and where to
extend. The short version:

Three layers, each adapted by different people:

| Layer | Where | Who adapts it |
|---|---|---|
| **Boards**: one file per piece of hardware, filling in the board interface (screen, audio, buttons, power, camera, extras) | [`esp32/components/impo/boards/`](esp32/components/impo/boards), [`linux/`](linux) | Whoever has the board, usually with a coding agent reading `AGENTS.md`. The [ESP-SparkBot](esp32/components/impo/boards/board_espressif_sparkbot.c) is the worked example. |
| **Capabilities**: a fixed vocabulary of commands (`display.*`, `speaker.*`, `camera.*`, `imu.*`, `chassis.*`, `sensors.*`, `device.*`) with their parameters and results, plus a summary of what the gadget has | [`CAPABILITIES.md`](CAPABILITIES.md) | This project. A board registers the capabilities it implements; the agent and the app rely on the contract. |
| **Ecosystem**: other people's devices on the home network, reached through a gadget's tunnel | [`skills/`](skills), one guide per product | The agent, following the skill; nothing in the firmware knows the product. |

Between the agent and the boards, [`docs/GADGET-AGENTS.md`](docs/GADGET-AGENTS.md)
proposes a layer of small agents a person makes in plain language and binds
to their gadgets: triggers from the physical world, allowed actions, a
little state, run by the platform. [`docs/architecture.html`](docs/architecture.html)
draws the whole stack.

Speech to text, text to speech and seeing pictures are the Impo server's,
not the gadget's: a gadget sends voice notes and gets spoken replies through
`gadgets.impo.ai` with its own device token, and no third-party key is ever
on a gadget.

## Community

Meet other hackers who are building and customizing Impo gadgets in our
community [Discord](https://discord.gg/84ZYn3xcGV). Get inspired, support each
other, and share what you make.

## License

Impo Gadgets is licensed under the Apache License, Version 2.0, found in
[`LICENSE`](LICENSE), except for these third-party files, which keep their
upstream licenses:

| Path | Upstream | License |
|---|---|---|
| [`esp32/components/minimp3/include/minimp3.h`](esp32/components/minimp3) | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0-1.0, see [`LICENSE`](esp32/components/minimp3/LICENSE) |
| [`esp32/components/bmi270/src/bmi270.c`](esp32/components/bmi270) and its headers | [espressif/esp-bsp](https://github.com/espressif/esp-bsp/tree/master/components/sensors/bmi270) | Apache-2.0, see [`LICENSE`](esp32/components/bmi270/LICENSE) |
| [`esp32/components/bmi270/src/bmi260_config.h`](esp32/components/bmi270/src/bmi260_config.h) | Bosch Sensortec, via [ChromiumOS EC](https://chromium.googlesource.com/chromiumos/platform/ec/+/HEAD/third_party/bmi260/) | BSD-3-Clause, see [`LICENSE.bmi260`](esp32/components/bmi270/LICENSE.bmi260) |
| [`esp32/main/pixel_font.c`](esp32/main/pixel_font.c) | Adafruit GFX `glcdfont.c` | BSD-2-Clause, in the file header |

Dependencies fetched at build time are under their own licenses: ESP-IDF
components (into `esp32/managed_components/`), and the simulator's LVGL and
SDL (listed in [`esp32/simulator/THIRD_PARTY.md`](esp32/simulator/THIRD_PARTY.md)).

The default avatar, Robin, in [`esp32/avatar`](esp32/avatar) is original to this fork and is covered by the Apache License.
