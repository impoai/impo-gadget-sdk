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

# Linux Device SDK

This turns any Linux computer, like a Raspberry Pi, into a Impo gadget.
Install it, pair it with the Impo app, and Impo can run commands and move
files on the machine.

Then make it your own: wire up a sensor, bridge a webhook, or give Impo a new
command.

> **Note:** Built by hackers, for hackers, just for fun. Proceed at your own
> risk! Impo gets the same access to the machine as the account you install it
> for.

## What you need

- **A Raspberry Pi with Bluetooth**: a 3B+, 4, 5 or Zero 2 W. Other Linux
  computers work too, as long as they have Bluetooth LE.
- **Raspberry Pi OS Bullseye or later**, Debian 11 or later, or Ubuntu 22.04
  or later, already on your network. 32-bit and 64-bit both work.
- **An account with sudo** on the machine.
- **The Impo app** on your phone, to pair the device.

## Install

On the machine, as the account Impo should use:

```sh
curl -fsSL https://raw.githubusercontent.com/impoai/impo-gadget-sdk/main/linux/install.sh -o install.sh
less install.sh     # read it first
bash install.sh --sdk-token mgst_…
```

The installer checks your system, installs what it needs into
`/opt/impogadget`, and starts the `impogadget` service. Before it gives Impo
your account, it asks, and tells you if that account can use sudo. Then it
opens Bluetooth pairing.

## Set it up with Impo

When the installer says pairing is open, go to the Impo app:

1. Turn on **Settings > Devices > Developer mode**.
2. Add a device (**Settings > Devices > Add Device**, the **+** icon in the
   top right). It shows up as `ImpoGadgetXXXXXX`, with the name the
   installer printed.
3. Impo warns that this is a community device. Continue if it's yours.
4. When asked for Wi-Fi, pick the network shown. The machine is already
   online, so no password is needed.

That's it: the device connects to Impo and stays connected, across reboots.
Pairing is open for 10 minutes. To pair again later, run
`sudo impogadget pair`.

Every setup creates a fresh encrypted session, and pairing only opens when you
run the installer or `impogadget pair` on the machine itself. Because these are
community devices, pairing has no manufacturer verification and can't prevent
an active man-in-the-middle attack. Set it up on a network you trust.

## What Impo can do

| Command | What it does |
|---|---|
| `system.run` | Runs a shell command and returns its output and exit code |
| `file.read` | Reads a file, 64 KB at a time |
| `file.write` | Writes a file, 64 KB at a time, replacing it only when complete |
| `device.health` | Reports uptime, load, memory, disk and temperature |

Commands run as the account you installed for, with exactly that account's
permissions. If it can use sudo, so can Impo.

Ask Impo things like:

> What's using all the disk space on my Pi?

> Install Home Assistant on my Pi and tell me how to open it.

> Every morning at 7, check if my Pi's backups ran and tell me if they didn't.

## Hack and extend it

Programs on the machine can send messages to Impo, with no credentials of
their own:

```sh
impogadget send-user-msg "The garage door has been open for an hour."
impogadget send-user-msg --session-id 6f1c2d4e-0b7a-4c3e-9f5d-2a8b1e0c7d93 "Posted to a side chat"
```

`--session-id` posts into a side chat: a new id starts one, and reusing it
keeps later messages there. [`examples/pebble_ring_bridge.py`](examples/pebble_ring_bridge.py)
is a complete example: a webhook listener that sends every note from a Pebble
ring to its own Impo chat.

A few other ways to build on it:

- **Let Impo do it.** Impo can run commands on the machine, so you can ask it to
  set up the rest: "Write a service that tells me when the Pi gets too hot."
- **Add a command.** Commands live in [`src/impogadget/executor.py`](src/impogadget/executor.py):
  add a spec to `COMMAND_SPECS` and a branch in `Executor.run`.
  [`AGENTS.md`](AGENTS.md) walks through it.
- **Change the account.** `bash install.sh --run-as someone` gives Impo a
  different account, such as one without sudo.
- **Change the SDK token.** `bash install.sh --sdk-token mgst_…` replaces it.
  It's saved in `/var/lib/impogadget/sdk_token`, readable only by root.

## Manage it

```sh
sudo impogadget info                     # name, node id and pairing state
sudo systemctl status impogadget         # is it running?
sudo journalctl -u impogadget -f         # follow the log
sudo impogadget pair                     # pair again
bash install.sh --uninstall              # remove it (add --purge to forget the pairing)
```

## Develop

From this directory, with [uv](https://docs.astral.sh/uv/):

```sh
uv run --with pytest --with . pytest
```

The tests run anywhere, with no Bluetooth or Impo needed. To try a change on a
device, copy this directory to it and run `bash install.sh --from .`.

## Community

Meet other hackers who are building and customizing Impo gadgets in our
community [Discord](https://discord.gg/84ZYn3xcGV). Get inspired, support each
other, and share what you make.

## License

Apache 2.0. See [`LICENSE`](../LICENSE).
