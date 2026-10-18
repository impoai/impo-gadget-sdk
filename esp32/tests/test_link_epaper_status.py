# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkEpaperStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = (ROOT / "main/epaper_status.c").read_text()
        start = source.index("// ---- Pixels (host-tested)")
        (out / "epaper_pixels.inc").write_text(source[start:source.index("// ---- Panel", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(out),
               str(ROOT / "tests/link_epaper_status_harness.c"), "-o", str(out / "epaper")]
        compiled = subprocess.run(cmd, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_pixels(self):
        result = subprocess.run([str(self.out / "epaper")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_draw_url_waits_longer_for_six_colours(self):
        # What the screen shows (bit depth, the six inks) is in the product's
        # manifest; the firmware only gives the slow panel more time.
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_DISPLAY_COMMANDS")
        block = noise[start:noise.index('"display.show_animation"', start)]
        self.assertIn("led_status_display_bits() == 4 ? 120000 : 60000", block)
        driver = (ROOT / "main/epaper_status.c").read_text()
        self.assertIn("led_status_display_bits", driver)
        self.assertIn("return EPD_COLOR ? 4 : 1;", driver)

    def test_epaper_replaces_led_status_and_refreshes_after_each_image(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'if\(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179\s+'
                                r'OR CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_XTEINK_X4\)\s*'
                                r'list\(APPEND GADGET_SRCS "xteink_x4.c" "display_canvas.c" "display_snapshot.c"\)\s*else\(\)\s*'
                                r'list\(APPEND GADGET_SRCS "led_status.c"\)')
        # Both implementations provide the whole display interface.
        header = (ROOT / "main/led_status.h").read_text()
        for name in re.findall(r"^\w[\w ]*?\b(led_status_\w+)\(", header, re.M):
            if name in ("led_status_set_voice", "led_status_set_level", "led_status_show_volume"):
                continue
            for impl in ("led_status.c", "epaper_status.c", "xteink_x4.c"):
                self.assertRegex((ROOT / "main" / impl).read_text(), rf"\n\w[\w ]*\b{name}\(",
                                 f"{impl} lacks {name}")
        fetch = (ROOT / "main/image_fetch.c").read_text()
        task = fetch[fetch.index("static void fetch_task("):]
        self.assertLess(task.index("draw_raw(f, &rows)"), task.index("led_status_draw_done()"))


if __name__ == "__main__":
    unittest.main()
