# SPDX-License-Identifier: MIT
"""The Freenove 4WD car's commands, run on the host, and how the firmware wires them up."""

from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))
CAR = ROOT / "main/freenove_4wd_car.c"
OPTION = "#if CONFIG_HOMEHUB_LED_BACKEND_FREENOVE_4WD_CAR"


def block(text, start):
    """The text from `start` to its #endif."""
    i = text.index(start)
    return text[i:text.index("#endif", i)]


class FreenoveCarCommandsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = CAR.read_text()
        start = source.index("// ---- Commands (host-tested) ----")
        (out / "car_commands.inc").write_text(source[start:source.index("// ---- Hardware ----", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-DCJSON_NESTING_LIMIT=16", "-I", str(JSON), "-I", str(ROOT / "main"), "-I", str(out)]
        commands = [
            [*cc, *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, *flags, str(ROOT / "tests/freenove_4wd_car_harness.c"), str(ROOT / "main/command_params.c"),
             str(out / "cjson.o"), "-lm", "-o", str(out / "car")],
        ]
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_commands_on_fake_hardware(self):
        result = subprocess.run([str(self.out / "car")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ok", result.stdout)

    def test_manifest_matches_the_firmware(self):
        tool = ROOT / "tools/freenove_4wd_car_manifest.py"
        made = subprocess.run([sys.executable, str(tool)], capture_output=True, text=True)
        self.assertEqual(made.returncode, 0, made.stderr)
        manifest = json.loads(made.stdout)
        self.assertLessEqual(len(manifest["systemPrompt"]), 4000)
        names = [c["name"] for c in manifest["commands"]]
        source = CAR.read_text()
        table = source[source.index("s_commands[] = {"):source.index("void freenove_car_describe")]
        self.assertEqual(names[:len(re.findall(r'\{ "([a-z_.]+)",', table))],
                         re.findall(r'\{ "([a-z_.]+)",', table))
        self.assertIn("device.health", names)
        for c in manifest["commands"]:
            self.assertFalse(c["parameters"]["additionalProperties"], c["name"])
        # Only the keywords the platform accepts (contracts/gadget-identity.md, "Schema subset").
        allowed = {"type", "description", "properties", "required", "additionalProperties", "items", "enum",
                   "minimum", "maximum", "minLength", "maxLength", "minItems", "maxItems", "default"}
        for c in manifest["commands"]:
            self.assertLessEqual(len(c["description"]), 500, c["name"])
            for spec in [c["parameters"], *c["parameters"]["properties"].values()]:
                self.assertLessEqual(set(spec), allowed, c["name"])
        drive = next(c for c in manifest["commands"] if c["name"] == "chassis.drive")
        self.assertEqual(drive["parameters"]["properties"]["duration_ms"]["maximum"], 10000)


class FreenoveCarWiringTest(unittest.TestCase):
    def test_advertised_and_dispatched_with_the_backend(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        self.assertIn("freenove_car_describe(commands);", block(noise, OPTION + "\n    freenove_car_describe"))
        self.assertIn("freenove_car_capabilities(params);", block(noise, OPTION + "\n    freenove_car_capabilities"))
        self.assertIn("freenove_car_battery(", noise[noise.index("#elif CONFIG_HOMEHUB_LED_BACKEND_FREENOVE_4WD_CAR"):])
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        self.assertIn("freenove_car_command(command, params)", block(dispatch, OPTION))

    def test_stops_when_the_link_drops(self):
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static void on_ws_control_status(const char *status) {")
        self.assertIn('if (strcmp(status, "ws_connected") != 0) freenove_car_stop("link_lost");',
                      block(app[start:], OPTION))

    def test_motors_stop_first_at_boot(self):
        main = (ROOT / "main/main.c").read_text()
        body = main[main.index("void app_main(void) {"):]
        self.assertLess(body.index("freenove_car_init()"), body.index("app_run()"))
        self.assertLess(body.index("freenove_car_init()"), body.index("starting"))

    def test_build_files(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'if\(CONFIG_HOMEHUB_LED_BACKEND_FREENOVE_4WD_CAR\)\s*'
                                r'list\(APPEND GADGET_SRCS "freenove_4wd_car.c"\)')
        self.assertIn('"command_params.c"', cmake)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        self.assertRegex(kconfig, r'config HOMEHUB_LED_BACKEND_FREENOVE_4WD_CAR\n.*\n\s+depends on IDF_TARGET_ESP32\n')
        overlay = (ROOT / "devices/sdkconfig.freenove-4wd-car").read_text()
        for line in ('CONFIG_IDF_TARGET="esp32"', "CONFIG_HOMEHUB_LED_BACKEND_FREENOVE_4WD_CAR=y",
                     'CONFIG_GADGET_PRODUCT_ID="freenove-4wd-car"', 'CONFIG_ESPTOOLPY_FLASHSIZE="4MB"',
                     "CONFIG_ESP32_REV_MIN_3=y"):
            self.assertIn(line + "\n", overlay)
        self.assertRegex(overlay, r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"')
        table = re.search(r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', overlay).group(1)
        self.assertTrue((ROOT / table).exists())
        board = (ROOT / "tools/board.sh").read_text()
        self.assertIn("devices/sdkconfig.$BOARD", block_case(board, "freenove-4wd-car)"))

    def test_schema_checker_is_the_arcs_one(self):
        ours = (ROOT / "main/command_params.c").read_text().replace("command_params", "gadget_params")
        theirs = (ROOT.parent / "arcs/gadget/gadget_params.c").read_text()
        self.assertEqual(ours.split("\n", 1)[1], theirs)


def block_case(text, label):
    i = text.index(label)
    return text[i:text.index(";;", i)]


if __name__ == "__main__":
    unittest.main()
