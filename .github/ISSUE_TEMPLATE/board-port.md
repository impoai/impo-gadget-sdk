---
name: Board port
about: A board you want Impo Gadgets to support, or one you ported
title: "Board: <vendor> <model>"
labels: board
---

**Board**: vendor, model, chip (ESP32-S3/C6/...), flash, PSRAM, link to the product page.

**What it has** (tick what applies): screen (size, panel, touch?) · speaker · microphone · camera (sensor) · battery · buttons · motion sensor · lights · anything that moves.

**Where its facts come from**: the vendor's pin map, a working firmware (xiaozhi-esp32, the vendor's demo), the BSP. See `esp32/components/impo/boards/README.md`.

**Status**: wished for / port in progress (link the branch) / ported and tested over USB and through the gateway.
