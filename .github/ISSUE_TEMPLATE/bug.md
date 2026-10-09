---
name: Bug
about: Something a gadget, the firmware or the Linux client does wrong
title: ""
labels: bug
---

**Board or client**: e.g. ESP-SparkBot, Linux client on a Pi 5.

**Firmware / package version**: from `>status` on the USB console or `impogadget --version`.

**What you did, what happened, what you expected.**

**Log**: the USB console or `journalctl` lines around it (`tools/impo/board.sh` and `tools/impo/monitor.py` for the ESP32). Redact SSIDs and tokens.
