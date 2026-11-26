# ARCS Mini device adaptation

`arcs_mini_voice` adapts a stationary, single-screen ARCS Mini using official
BLE setup, Wi-Fi, verified TLS, Noise, speaker commands and native Impo dialogue.
See the [product/application SDK guide](../../docs/ARCS_MINI_SDK.md).

| Part | Wiring/configuration |
| --- | --- |
| Amplifier | PA01, high enables |
| Microphone | Codec ADC left channel, 30 dB |
| Power latch / function key | PB03 / PB04, active low |
| Screen | ST7789P3, 240 x 240 RGB565, zero offsets |
| Backlight | PA21, PWM0 channel 1, 2 kHz, 70 percent |
| CS / DC / reset | PA22 / PA23 / PB09 |
| SPI0 MOSI / clock | PA24 / PA25, 20 MHz, DMA channel 0, no TE |

Confirm wiring and AP/boot compatibility for another unit. This board does not
register robot-dog motors, lights, camera or motion sensors. It reports USB
power without inventing a battery curve.

A debounced short click starts a conversation; another click ends it. With
continuous dialogue, wait for the next listening prompt. Five seconds of
silence ends the conversation. The power-on hold is ignored by the voice
trigger; the SDK retains its 3 s power-off policy.

The status screen runs on its own task with a PSRAM framebuffer. Display
failure does not prevent voice starting. `screen` and `wake` shell commands
provide local diagnostics.

Only update the CP app partition at **0x600000** after backup and verification.
Preserve AP, boot, ROMFS, KV, calibration and pairing/Wi-Fi state. This target
includes neither vendor images nor a universal flashing command.
