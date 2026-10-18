#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the Impo product manifest (contracts/gadget-identity.md, ManifestInput)
for the Freenove 4WD car from the firmware's own command table
(main/freenove_4wd_car.c), so the platform describes exactly what the firmware
registers and checks.

usage: tools/freenove_4wd_car_manifest.py [--min-firmware 0.1.0] > manifest.json
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "main/freenove_4wd_car.c"
TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([{}(),;])|([A-Za-z_][A-Za-z0-9_]*)|(/\*.*?\*/|//[^\n]*)|(\s+)|(.)', re.S)

SYSTEM_PROMPT = """\
You control a Freenove 4WD Car: a small four-wheeled robot car with an ESP32 on top and two \
18650 cells underneath. Its four wheels, an ultrasonic distance sensor and a camera on its \
head are connected so far: it has no line sensors, lights, sound, screen, speaker or \
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


def c_string(body):
    return json.loads('"' + body.replace("\\'", "'") + '"')


def table(path, name):
    """The rows of `name[] = { {...}, ... };`: strings joined (macros expanded), NULL as None."""
    src = path.read_text()
    macros = {m.group(1): m.group(2) for m in re.finditer(r'#define (\w+) ((?:"(?:[^"\\]|\\.)*"\s*\\?\s*)+)', src)}
    m = re.search(re.escape(name) + r'\[\]\s*=\s*\{', src)
    if not m:
        sys.exit(f"{name} not found in {path}")
    rows, row, field, depth = [], None, None, 1
    for t in TOKEN.finditer(src, m.end()):
        s, p, ident, comment, space, other = t.groups()
        if comment or space:
            continue
        if p == '{':
            depth += 1
            row, field = [], None
        elif p == '}':
            depth -= 1
            if depth == 0:
                return rows
            if row is not None:
                if field is not None:
                    row.append(field)
                rows.append(row)
                row, field = None, None
        elif p == ',' and row is not None:
            row.append(field)
            field = None
        elif s is not None and row is not None:
            field = (field or '') + c_string(s)
        elif ident is not None and row is not None:
            if ident in macros:
                field = (field or '') + ''.join(c_string(x) for x in re.findall(r'"((?:[^"\\]|\\.)*)"', macros[ident]))
            else:
                field = None if ident == 'NULL' else ('@' + ident)
    sys.exit(f"{name}: unterminated table")


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
    rows = table(SOURCE, "s_commands")
    commands = [command(r[0], r[1], r[2], r[3]) for r in rows] + [command(*c) for c in LINK]
    names = [c["name"] for c in commands]
    assert len(names) == len(set(names)) and len(commands) <= 64
    assert len(SYSTEM_PROMPT) <= 4000, len(SYSTEM_PROMPT)
    json.dump({"minFirmware": a.min_firmware, "systemPrompt": SYSTEM_PROMPT, "commands": commands, "events": []},
              sys.stdout, ensure_ascii=False, indent=2)
    print()


if __name__ == "__main__":
    main()
