#!/usr/bin/env python3
"""Builds the Impo product manifest (contracts/gadget-identity.md, ManifestInput)
for the ARCS robot dog from the firmware's own command tables, so the platform
describes exactly what the firmware registers.

usage: tools/gen_manifest.py [--min-firmware 0.1.0] > manifest.json
"""
import argparse, json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([{}(),;])|([A-Za-z_][A-Za-z0-9_]*)|(/\*.*?\*/|//[^\n]*)|(\s+)|(.)', re.S)


def c_string(body):
    return json.loads('"' + body.replace("\\'", "'") + '"')


def table(path, name):
    """The elements of `name[] = { {...}, ... };` as lists of fields (str, None or identifier)."""
    src = path.read_text()
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
            field = None if ident == 'NULL' else ('@' + ident)
    sys.exit(f"{name}: unterminated table")


def instructions(path):
    src = path.read_text()
    m = re.search(r's_instructions\[\]\s*=(.*?);\n', src, re.S)
    return ''.join(c_string(x) for x in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))


def command(row):
    name, desc, req, opt = row[0], row[1], row[2], row[3]
    req = json.loads(req) if req else {}
    opt = json.loads(opt) if opt else {}
    props = {**req, **opt}
    schema = {"type": "object", "properties": props}
    if req:
        schema["required"] = list(req)
    schema["additionalProperties"] = False
    assert re.fullmatch(r'[a-z][a-z0-9_]*(\.[a-z0-9_]+){0,3}', name), name
    assert 1 <= len(desc) <= 500, (name, len(desc))
    return {"name": name, "description": desc, "parameters": schema}


ORIENTATION = {"type": "string", "description": "Which way up it is now.",
               "enum": ["upright", "upside_down", "face_up", "face_down", "on_left_side", "on_right_side", "tilted"]}
EVENT_DATA = {
    "picked_up": {"type": "object", "properties": {"orientation": ORIENTATION}},
    "put_down": {"type": "object", "properties": {"orientation": ORIENTATION}},
    "shaken": {"type": "object", "properties": {
        "peak_g": {"type": "number", "description": "Strongest acceleration in g."},
        "strength": {"type": "string", "enum": ["gentle", "hard"]}}},
    "upside_down": {"type": "object", "properties": {"orientation": ORIENTATION}},
}

BARK = ("To bark, play one of these with speaker.play_url, varying them: a short yap "
        "https://bigsoundbank.com/UPLOAD/mp3/0682.mp3, barking https://bigsoundbank.com/UPLOAD/mp3/2955.mp3, "
        "a howl https://bigsoundbank.com/UPLOAD/mp3/2451.mp3 (CC0 recordings from BigSoundBank).")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-firmware", default="0.1.0")
    ap.add_argument("--no-bark", action="store_true", help="leave the bark sounds out of the system prompt")
    a = ap.parse_args()
    core = ROOT / "gadget/gadget_core.c"
    dog = ROOT / "boards/arcs_mini_v2_1/gadget/dog_commands.c"
    rows = table(core, "s_standard") + table(core, "s_speaker_cmds") + table(core, "s_light_cmds") + table(dog, "dog_commands")
    commands = [command(r) for r in rows]
    events = []
    for name, desc in table(dog, "dog_events"):
        assert re.fullmatch(r'[a-z_]{1,32}', name) and 1 <= len(desc) <= 200, name
        events.append({"name": name, "description": desc, "data": EVENT_DATA.get(name)})
    prompt = instructions(ROOT / "boards/arcs_mini_v2_1/gadget/board_robot_dog.c")
    if not a.no_bark:
        prompt += " " + BARK
    assert len(prompt) <= 4000, len(prompt)
    names = [c["name"] for c in commands]
    assert len(names) == len(set(names)) and len(commands) <= 64 and len(events) <= 32
    json.dump({"minFirmware": a.min_firmware, "systemPrompt": prompt, "commands": commands, "events": events},
              sys.stdout, ensure_ascii=False, indent=2)
    print()


if __name__ == "__main__":
    main()
