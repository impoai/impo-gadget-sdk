# SPDX-License-Identifier: MIT
"""The USB keyboard and mouse (main/usb_hid.c): text to key presses, moves
split into reports, and keyboard.type and mouse.move advertised and
dispatched under CONFIG_HOMEHUB_USB_HID."""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkUsbHidTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = (ROOT / "main/usb_hid.c").read_text()
        start = source.index("// ---- Text and moves to HID reports (host-tested)")
        (out / "usb_hid_reports.inc").write_text(source[start:source.index("// ---- USB device", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-I", str(out), str(ROOT / "tests/link_usb_hid_harness.c"), "-o", str(out / "usb_hid")]
        compiled = subprocess.run(cmd, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_keys_and_moves(self):
        result = subprocess.run([str(self.out / "usb_hid")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_commands_are_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_USB_HID\n    {\n        cJSON *type_required")
        block = noise[start:noise.index("#endif", start)]
        self.assertIn('add_command(commands, "keyboard.type"', block)
        self.assertIn('add_command(commands, "mouse.move"', block)
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_HOMEHUB_USB_HID"):]
        self.assertIn("usb_hid_command(command, params, request_id, session_generation)",
                      block[:block.index("#endif")])
        hid = (ROOT / "main/usb_hid.c").read_text()
        self.assertIn('strcmp(command, "keyboard.type")', hid)
        self.assertIn('strcmp(command, "mouse.move")', hid)

    def test_xiao_turns_it_on_with_one_hid_interface(self):
        overlay = (ROOT / "devices/sdkconfig.seeed-xiao-esp32s3").read_text().splitlines()
        self.assertIn("CONFIG_HOMEHUB_USB_HID=y", overlay)
        self.assertIn("CONFIG_TINYUSB_HID_COUNT=1", overlay)


if __name__ == "__main__":
    unittest.main()
