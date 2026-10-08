# Boards

One file per piece of hardware, each filling in an `impo_board_t`
(`../impo_board.h`): how to start the screen, open the audio codec, read the
buttons and the battery, turn the board off, and whatever else only it has.
Everything above the board (the avatar, captions, push-to-talk, settings,
Link, the commands) is shared, so a board port is small: the ESP-SparkBot's is
one file plus two helpers for its touch pads and motion sensor.

## Porting a board

1. Find the vendor's pin map and a working driver for the panel and codec;
   `../../../devices/AGENTS.md` lists where each supported board's came from.
   The closest existing board file is the template.
2. Fill in `impo_board_t`: `name`, the screen's size and shape, `init`,
   `display_start` (an LVGL display through `esp_lvgl_adapter`),
   `audio_init` (an `esp_codec_dev` for the speaker and the mic),
   `poll_buttons` / `wait_buttons`, `read_power` if there is a battery,
   `power_off`. Leave out what the hardware lacks: the capabilities the gadget
   registers follow from what is set (`../impo_commands.c`,
   `impo_commands_capabilities`).
3. Register a camera, if there is one, with `camera_register()` from
   `components/camera`: a `camera_dvp` backend for a parallel sensor, a
   `camera_sscma` one for a Himax module. `camera.capture` then works.
4. Put what the board has beyond the fixed capabilities in `features`
   (`"motion_sensor,locomotion,lights"`), and its own commands in `commands`,
   named as [`../../../../CAPABILITIES.md`](../../../../CAPABILITIES.md) says.
5. Add the board to `Kconfig`, `CMakeLists.txt`, `devices/sdkconfig.impo-<board>`,
   `tools/impo/board.sh` and `ports.py`, and the tables in `devices/README.md`
   and `README.md`.
6. Prove it on the hardware over USB (`tools/impo/chat.py`, the `>` console
   commands) and through the gateway's admin invoke before calling it done.

Keep the vendor's facts (pins, init sequences, quirks) in comments at the top
of the file with where they came from; the next person reads that first.
