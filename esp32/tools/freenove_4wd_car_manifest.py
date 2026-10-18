#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the Impo product manifest (contracts/gadget-identity.md, ManifestInput)
for the Freenove 4WD car. The firmware registers only command names; everything
the agent reads about them is here, and the platform checks calls against these
schemas. Refuses to build when the car's commands differ from what
main/freenove_4wd_car.c registers.

usage: tools/freenove_4wd_car_manifest.py [--min-firmware 0.1.0] > manifest.json
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "main/freenove_4wd_car.c"

SYSTEM_PROMPT = """\
You control a Freenove 4WD Car: a small four-wheeled robot car with an ESP32 on top and two \
18650 cells underneath. Its four wheels and a head that turns (servo.set_angle), with an \
ultrasonic distance sensor and a camera, are connected so far: it has no line sensors, lights, sound, screen, speaker or \
microphone. You talk to the person in the app, and the car drives.

Moving: chassis.move (forward, back, left, right; left and right spin it in place) or \
chassis.drive (forward and turn, each -1 to 1) for duration_ms, at most 10 seconds per call. \
Every drive stops by itself when its time is up, so chain several short drives instead of \
one long one, and call chassis.stop the moment the person asks to stop. How far a drive goes \
depends on the floor and the battery: start with short moves (about 500 ms at speed 0.5), \
ask the person how far it went, and adjust. Drives return at once: the result says the car \
is moving, not that it arrived. Look before longer or faster moves (camera.capture, \
chassis.sense), and ask the person whether the way is clear when you are not sure. If the \
person says the car is on a table, do not drive it unless they ask, and then only in very \
short moves.

Distance: chassis.sense measures how far the nearest thing in front of its ultrasonic head is, \
in cm (null if nothing within 3 m). Check it before driving forward; it cannot see thin legs, \
soft fabric or a table edge, so it does not replace asking the person.

Power: the wheels run only on the two 18650 cells with the power switch on; USB powers the \
ESP32 alone. If a result carries a warning about the battery, tell the person (the cells \
need charging, or the switch is off) instead of retrying. chassis.status reports battery_v; \
device.health gives the battery percentage and voltage (null without a charged battery).

Be playful but careful: say what you did in one short sentence, and ask before long or \
fast moves."""


def registered(path, name):
    """The command names in the firmware's `name[] = { {"a.b", run}, ... };`."""
    src = path.read_text()
    m = re.search(re.escape(name) + r'\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    if not m:
        sys.exit(f"{name} not found in {path}")
    return re.findall(r'\{\s*"([^"]+)"', m.group(1))


def command(name, desc, req, opt):
    req = json.loads(req) if req else {}
    opt = json.loads(opt) if opt else {}
    schema = {"type": "object", "properties": {**req, **opt}}
    if req:
        schema["required"] = list(req)
    schema["additionalProperties"] = False
    assert re.fullmatch(r'[a-z][a-z0-9_]*(\.[a-z0-9_]+){0,3}', name), name
    assert 1 <= len(desc) <= 500, (name, len(desc))
    return {"name": name, "description": desc, "parameters": schema}


# The car's own commands (main/freenove_4wd_car.c runs them; it registers only
# their names, so what the agent reads about them is here).
CAR = [
    ("chassis.drive",
     "Drive the 4WD car for a while: forward speed and turn, each -1 to 1 (turn alone spins it in "
     "place). It stops by itself when the time is up. Returns at once.",
     None,
     '{"forward":{"type":"number","minimum":-1,"maximum":1,"description":"-1 (full speed back) to 1 (full speed forward); 0 if left out."},"turn":{"type":"number","minimum":-1,"maximum":1,"description":"-1 (left) to 1 (right); 0 if left out."},"duration_ms":{"type":"integer","minimum":1,"maximum":10000,"description":"How long, 1 to 10000; 500 if left out."}}'),
    ("chassis.move",
     "Drive the 4WD car in one direction for a while (left and right spin it in place), or stop it. "
     "Returns at once.",
     '{"direction":{"type":"string","enum":["forward","back","left","right","stop"],"description":"forward, back, left, right or stop."}}',
     '{"duration_ms":{"type":"integer","minimum":1,"maximum":10000,"description":"How long, 1 to 10000; 500 if left out."},"speed":{"type":"number","minimum":0.1,"maximum":1,"description":"0.1 to 1; 0.5 if left out."}}'),
    ("chassis.stop",
     "Stop the 4WD car's wheels at once.",
     None,
     None),
    ("chassis.status",
     "Whether the car's motor driver answers, whether it is moving (and for how much longer), why it "
     "last stopped, and the battery voltage.",
     None,
     None),
    ("chassis.sense",
     "Measure the distance in cm to what is in front of the car's ultrasonic head (null with no echo "
     "within 3 m).",
     None,
     None),
    ("servo.set_angle",
     "Turn the car's head, which carries the camera and the distance sensor: pan turns it left and "
     "right, tilt up and down. Give one or both. The servos need the battery switched on.",
     None,
     '{"pan":{"type":"integer","minimum":0,"maximum":180,"description":"0 (right) to 180 (left); 90 ahead."},"tilt":{"type":"integer","minimum":80,"maximum":180,"description":"80 to 180; 90 at start."}}'),
]


# Link's own commands (main/noise_control.cpp), which every ESP32 gadget registers.
LINK = [
    ("device.health",
     "Battery level (percent) and voltage of the car's two 18650 cells (null without a charged battery), "
     "uptime, memory and the Wi-Fi network.", None, None),
    ("device.set_name",
     "Rename the car: the name the person calls it by. 1 to 32 characters. It reconnects under the new "
     "name a moment later.",
     '{"name":{"type":"string","minLength":1,"maxLength":32,"description":"The new name, 1 to 32 characters."}}',
     None),
    ("camera.capture",
     "Take one photo with the camera on the car's head (an OV2640 or OV3660) of whatever is in front of "
     "it, as a JPEG. Returned only when this command is explicitly invoked.", None, None),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-firmware", default="0.1.0")
    a = ap.parse_args()
    ours = [c[0] for c in CAR]
    if ours != registered(SOURCE, "s_commands"):
        sys.exit(f"CAR {ours} differs from what {SOURCE.name} registers")
    commands = [command(*c) for c in CAR + LINK]
    names = [c["name"] for c in commands]
    assert len(names) == len(set(names)) and len(commands) <= 64
    assert len(SYSTEM_PROMPT) <= 4000, len(SYSTEM_PROMPT)
    json.dump({"minFirmware": a.min_firmware, "systemPrompt": SYSTEM_PROMPT, "commands": commands, "events": []},
              sys.stdout, ensure_ascii=False, indent=2)
    print()


if __name__ == "__main__":
    main()
