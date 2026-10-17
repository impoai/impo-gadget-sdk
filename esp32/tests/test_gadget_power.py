"""Run the shared power state machine with host hardware/RTOS substitutes."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class GadgetPower(unittest.TestCase):
    def test_shutdown_and_gestures(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
""")
            exe = path / "power"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-DLINK_FAKE_CUSTOM_TASKS",
                "-I", str(path), "-I", str(ROOT / "tests/link_fakes"),
                "-I", str(ROOT / "components/gadget_power/include"),
                str(ROOT / "components/gadget_power/gadget_power.c"),
                str(ROOT / "tests/gadget_power_harness.c"), "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)
