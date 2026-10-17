"""Exercise the real command parser/renderer with an instrumented framebuffer."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DisplayCanvas(unittest.TestCase):
    def test_canvas(self):
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "canvas"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                "-I", str(cjson), str(cjson / "cJSON.c"),
                str(ROOT / "main/display_canvas.c"), str(ROOT / "main/pixel_font.c"),
                str(ROOT / "tests/display_canvas_harness.c"), "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)
