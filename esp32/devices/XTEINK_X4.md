# Xteink X4 (experimental)

This port is for the **original ESP32-C3 X4 with an SSD1677 panel**, 16 MB
flash and no PSRAM. It is not for the X3, X4 Pro, X4 Classic, or newer
UltraChip panel batches. The device tested here reports ESP32-C3 revision
v0.4 and USB Serial/JTAG VID:PID `303a:1001`.

The board module is `main/xteink_x4.c`. Pins, ADC button thresholds and the
SSD1677 initialization/full-refresh sequence come from FreeInk SDK commit
`111fdcc7f0176c3ee38391a160ee296bf492dbd8`, the submodule pinned by CrossPoint
1.6.5. Its MIT notice is in `licenses/FreeInk-MIT.txt`.

## Behavior

- Shared Impo BLE pairing, Wi-Fi, device identity and encrypted server session.
- Physical **Confirm** key: tap to confirm a pending pairing; otherwise retry
  Wi-Fi, double-tap to rescan, or hold 5 seconds to reset pairing.
- Physical **Power** key: hold 1.5 seconds and release to shut down; press again
  to start. Allow several seconds for the e-paper goodbye page to refresh.
  Shared `gadget_power` stops the server session, Wi-Fi and BLE, then waits for
  the key to be released. X4 code supplies GPIO3 wake and GPIO13 battery-latch
  control. With USB attached, the ESP32 enters deep sleep. Saved Wi-Fi and
  pairing remain intact. A held wake key must be released before another
  long press can turn the device off.
- Portrait 480×800 black/white display; one 48,000-byte framebuffer in SRAM.
  Full refreshes use the panel's OTP waveform and power down the panel afterward.
- `display.show_text`: queues 1–200 ASCII characters. Use an image for Chinese
  text. `display.draw_url`: existing streamed JPEG/RGB565 image command, with
  monochrome dithering. Downloads can fail with `out_of_memory`, especially
  HTTPS, because the C3 has no PSRAM.
- `display.show_animation` restores the static connection-status page.
- Agent-controlled canvas: `display.get_info`, `display.clear`,
  `display.draw_text`, regional `display.draw_url` and `display.refresh`.
  Compose arbitrary image/text rectangles and refresh once; see
  [the canvas contract and example](XTEINK_X4_CANVAS.md).
- `display.set_power_off_image` saves the current complete canvas as a persistent
  shutdown page. Draw it first, wait until idle, then save with `{}`. Later
  drawing does not affect it. `reset:true` restores the default goodbye page.
- Advertises as `ImpoGadget-X4-XXXXXX` before pairing.

This is an always-connected gadget firmware, not an ebook reader. It does not
mount or modify the SD card. Audio, touch, battery telemetry, navigation keys
and automatic sleep are not implemented. It remains connected until manually
powered off; use USB power for extended operation. OTA is disabled. E-paper
keeps its last image without power, so the goodbye page remains visible.

## Build and flash

Activate ESP-IDF **v6.0.1** using the Python version for which its environment
was installed, then run from `esp32/`:

```sh
tools/board.sh xteink-x4 build
tools/board.sh xteink-x4 flash /dev/cu.usbmodem1101
```

Use the actual port on your computer. The flash helper verifies that the
device is an unlocked ESP32-C3 with the exact X4 factory partition table.
It reads and verifies a complete 16 MB backup before writing. The backup,
SHA-256, selector and log are saved privately under
`build-xteink-x4/backups/<timestamp>/`.

The helper writes **app1 at `0x650000`** and a verified OTA selector. It keeps
the original bootloader, partition table, app0, NVS and filesystem partitions.
The running application will store its own identity/pairing/Wi-Fi settings in
NVS. An existing CrossPoint in app1 is replaced, but remains in the backup.
Keep backups private: they can contain device credentials and Wi-Fi settings.

**Do not use generic `idf.py flash` or its generated `flash_args` for this
migration:** they write app0 and the bootloader. Do not enable Secure Boot,
flash encryption, eFuse pairing authentication or HMAC NVS provisioning.
The build guard rejects these development-profile changes.

After boot, open Impo's Add Gadget flow, select `ImpoGadget-X4-XXXXXX`, press
the X4 Confirm key when the screen requests it, and complete Wi-Fi setup.
The screen says `Online` only after the control session connects.

## Restore

For a full restore, use the verified backup from before the change:

```sh
python -m esptool --chip esp32c3 -p PORT --after no-reset \
  write-flash 0 BACKUP_DIRECTORY/full-flash.bin
python -m esptool --chip esp32c3 -p PORT --before no-reset --after hard-reset \
  verify-flash 0 BACKUP_DIRECTORY/full-flash.bin
```

USB ROM flashing was tested on this unit. This port does not implement
CrossPoint's SD recovery gesture. Manual ROM recovery on a device whose USB
interface no longer enumerates has not been physically tested.

## Validation

The host harness checks framebuffer rotation, every panel pixel, chunked
RGB565 conversion, boundary rejection and the Confirm ADC window with address
and undefined-behavior sanitizers. Selector tests check both active sectors,
failed-image states and sequence overflow. The shared power harness checks
long presses, held wake keys, shutdown order, release bounce, busy rejection,
duplicate shutdown and hardware-failure recovery. Build both X4 and the default
DevKit after changing shared code, and run `python -m unittest discover -s
tests -p 'test_*.py'`.

Hardware boot, screen and pairing results should be recorded separately;
successful compilation alone does not establish end-to-end connectivity.

### Bench result (2026-10-16)

An original ESP32-C3 rev v0.4 / SSD1677 X4 was flashed into app1, keeping
its factory partition table and bootloader. Both app and selector read-back
checks passed. The display refreshed, the physical Confirm key completed
pairing, Wi-Fi obtained an address, and the encrypted Noise session received
HTTP 200 and `link.register` acknowledgement from the gateway. BLE shut down
after setup. The connected device had about 69 KiB free internal heap (22 KiB
largest block) after its first pairing. Full refresh took about 3.9 seconds,
up to 6.2 seconds while pairing/TLS work competed for the CPU.

A USB-triggered reboot restored the saved Wi-Fi and account automatically;
the server acknowledged registration about 7.9 seconds after boot. The next
45 seconds showed a stable connection without a panic/reset loop, with about
68 KiB free internal heap and a 52 KiB largest block. The user also confirmed
successful pairing and connection.

The power/canvas update adds shared shutdown with release debounce and the
agent-controlled rectangle API. X4, default C5 and BOX-3 builds passed; the
host suite ran 215 tests with 214 passing and one missing-host-PSA-library skip.
The power and canvas harnesses run with address/undefined-behavior sanitizers.
Hardware verification of this update is recorded with its flash/serial logs.

The cloud Agent then composed a page with two text rectangles and two RGB565
image rectangles (432×280 and 432×120), including Chinese rendered into an
image. Downloads returned the expected 241,920 and 103,680 bytes, and the panel
refreshed in about 3.9 seconds. The owner confirmed the visible page. An initial
end-to-end check found that command data outside `payload` was discarded by
the control transport; canvas replies now use the required `payload` envelope.
The final cloud test read width 480, height 800 and `busy:false`; clear/text
returned `buffered:true, queued:false`, and refresh returned `queued:true`.
The first poll after the panel refresh completed returned `busy:false`.
The owner also physically tested the Power key and confirmed both shutdown
and startup. Battery current has not been measured.

The repeat-flash check caught an OTA selector CRC bug: ESP-IDF uses
`esp_rom_crc32_le(UINT32_MAX, sequence, 4)`, equivalent to Python
`zlib.crc32(sequence_bytes, 0xffffffff)`. The incorrect initial implementation
was masked by the other valid selector on the first flash; on the second flash
the bootloader fell back to the preserved stock app0. The helper now uses the
correct seed, and tests include captured factory CRC vectors and two successive
updates. `python tools/xteink_x4_flash.py PORT build-xteink-x4 --select-only`
backs up the device, verifies the existing app1 matches the build, and repairs
only its selector. The app, stock bootloader, app0 and NVS are not rewritten.

The selector-only repair booted IMPO and restored the saved pairing. A subsequent
app1 update with the corrected selector also booted IMPO; server registration
was acknowledged about 7.9 seconds after boot, without re-pairing.

### Persistent image storage

The factory `coredump` data partition at `0xff0000` (64 KiB, erased on the tested
unit) stores the 48,000-byte monochrome framebuffer and a versioned, checksummed
header. This profile disables coredumps; a build guard rejects enabling them
alongside image storage. No application partition, factory partition table,
NVS, filesystem or SD content is repurposed. Flash write protection stays on.

Saving verifies the pixels before committing the header. There is space for
one complete image: if power fails during replacement, the firmware falls back
to the default goodbye page. It never displays a partially written image.
Normal app-only updates preserve the image; full-flash restore replaces it
with the backed-up state. Host tests cover interrupted erase/pixel/header
writes, CRC corruption, reboot, reset and unchanged saves. The store also
supports two slots on boards with sufficient dedicated data space.

### Image command contract verification (2026-10-16)

A cloud Agent reproduced both image failures on the X4: progressive JPEG and
single-component grayscale JPEG are unsupported by the ROM decoder. The
registered command now specifies RGB baseline JPEG, a Pillow conversion recipe
and direct-download URLs. Invalid-image errors explain conversion and prohibit
unchanged retries. HTML, PNG, WebP and GIF responses are rejected before drawing.
On hardware, HTML/PNG, progressive JPEG and grayscale JPEG each returned the
expected actionable error; a 480×800 RGB baseline JPEG (28,085 bytes) then
rendered successfully in about 4.7 seconds including panel refresh.

With storage in the data partition, the cloud Agent saved the canvas, reset it
to the default, and saved a replacement image; all three operations passed
flash readback verification without a reboot. A 480×800 HTTPS JPEG (274,818
bytes) rendered in 15.6 seconds. The Agent then changed the visible page to
POWER TEST without saving it and read `power_off_image.stored:true`,
`persistent:true` and `busy:false`.

A subsequent USB-triggered reboot loaded the saved image before connecting;
registration was acknowledged at 7.9 seconds without re-pairing. Physical
shutdown/startup was confirmed for the preceding power/canvas build. Visual
confirmation of the newly saved custom shutdown page is still pending; the
storage/reboot checks do not establish what the owner sees during shutdown.
