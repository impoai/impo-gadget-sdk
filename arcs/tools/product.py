#!/usr/bin/env python3
"""Validate a product and generate C/Kconfig inputs without device credentials."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PRODUCT = ROOT / "products/arcs_mini.json"

def text(value, field, limit, empty=False, multiline=False):
    if not isinstance(value, str) or (not value and not empty) or len(value.encode("utf-8")) > limit:
        raise ValueError(f"{field}: expected text of at most {limit} UTF-8 bytes")
    if any(ord(c) < 32 and not (multiline and c == "\n") for c in value):
        raise ValueError(f"{field}: control characters are not allowed")
    return value

def object_keys(value, allowed, field):
    if not isinstance(value, dict) or set(value) - set(allowed):
        raise ValueError(f"{field}: invalid fields; allowed: {', '.join(allowed)}")

def boolean(value, field):
    if type(value) is not bool:
        raise ValueError(f"{field}: expected true or false")
    return value

def screen_text(value, field):
    text(value, field, 96)
    if any(ord(c) > 0xffff for c in value):
        raise ValueError(f"{field}: screen font supports BMP characters only")
    if "\n" in value or sum(16 if ord(c) < 128 else 24 for c in value) > 216:
        raise ValueError(f"{field}: text does not fit the 240px screen")
    return value

def load_product(path=DEFAULT_PRODUCT):
    path = Path(path).resolve()
    p = json.loads(path.read_text())
    object_keys(p, ["schema_version", "id", "display_name", "screen_title", "model_id", "node_prefix",
                    "product_id", "continuous_dialogue", "china_relay", "instructions", "wake"], "product")
    if p.get("schema_version") != 1:
        raise ValueError("Unsupported product schema_version")
    for key in ["id", "model_id", "node_prefix"]:
        if not isinstance(p.get(key), str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,31}", p[key]):
            raise ValueError(f"{key}: use 1-32 lowercase letters, numbers or hyphens")
    text(p.get("display_name"), "display_name", 63)
    p["screen_title"] = screen_text(p.get("screen_title", p["display_name"]), "screen_title")
    if p.get("product_id") is not None and (not isinstance(p["product_id"], str) or not re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", p["product_id"])):
        raise ValueError("product_id: expected null or a registered lowercase UUID")
    p["product_id"] = p.get("product_id")
    for key in ["continuous_dialogue", "china_relay"]:
        p[key] = boolean(p.get(key, True), key)
    w = p.get("wake", {})
    object_keys(w, ["enabled", "bundle"], "wake")
    enabled = boolean(w.get("enabled", False), "wake.enabled")
    bundle = {"label": "", "keywords": [], "id": "disabled"}
    if enabled:
        if not isinstance(w.get("bundle"), str) or not w["bundle"]:
            raise ValueError("wake.bundle is required: text alone cannot change the acoustic model")
        bundle = json.loads((path.parent / w["bundle"]).read_text())
        object_keys(bundle, ["schema_version", "id", "label", "keywords", "model", "wrap"], "wake bundle")
        if bundle.get("schema_version") != 1:
            raise ValueError("Unsupported wake bundle schema_version")
        text(bundle.get("id"), "wake bundle id", 80)
        screen_text(bundle.get("label"), "wake label")
        words = bundle.get("keywords")
        if not isinstance(words, list) or not 1 <= len(words) <= 8:
            raise ValueError("wake keywords: expected 1-8 engine result strings")
        for word in words:
            text(word, "wake keyword", 96)
        if len(set(words)) != len(words):
            raise ValueError("Duplicate wake keywords")
        for key in ["model", "wrap"]:
            resource = bundle.get(key)
            object_keys(resource, ["file", "sha256"], "wake " + key)
            filename = resource.get("file", "")
            if not isinstance(filename, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", filename) or filename in (".", ".."):
                raise ValueError("wake resource file must be a ROMFS root filename")
            digest = resource.get("sha256", "")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest) or digest == "0" * 64:
                raise ValueError("wake resource needs the actual binary's SHA256")
        if bundle["model"]["file"] == bundle["wrap"]["file"]:
            raise ValueError("Model and wrap must be distinct files")
    elif "bundle" in w:
        raise ValueError("Remove wake.bundle when wake is disabled")
    p["wake"] = dict(bundle, enabled=enabled)
    p["_inputs"] = [str(path)] + ([str((path.parent / w["bundle"]).resolve())] if enabled else [])
    prompt = (f"This gadget is {p['display_name']}, a stationary ARCS Mini voice companion. "
              + (f"Say {bundle['label']} or short click the main function button to start a conversation. "
                 if enabled else "Short click the main function button to start a conversation. ")
              + ("After each answer wait for the listening prompt before speaking again. Five seconds of silence or a short click ends the conversation. "
                 if p["continuous_dialogue"] else "Each interaction records one question and speaks its answer. ")
              + "Replies are spoken by its own speaker. Reply in the user's language in one to three short sentences without Markdown. "
              "Its screen shows connection and voice status automatically. No camera, movement or motion sensor is advertised.")
    p["instructions"] = text(p.get("instructions", prompt), "instructions", 1500, multiline=True)
    return p

def generated_files(p, font=None):
    q = lambda s: json.dumps(s, ensure_ascii=False)
    w = p["wake"]
    macros = {"IMPO_PRODUCT_ID": q(p["id"]), "IMPO_DISPLAY_NAME": q(p["display_name"]),
              "IMPO_MODEL_ID": q(p["model_id"]), "IMPO_PLATFORM_PRODUCT_ID": q(p["product_id"]) if p["product_id"] else "NULL",
              "IMPO_SCREEN_TITLE": q(p["screen_title"]), "IMPO_INSTRUCTIONS": q(p["instructions"]),
              "IMPO_CONTINUOUS_DIALOGUE": str(p["continuous_dialogue"]).lower(),
              "IMPO_WAKE_ENABLED": str(w["enabled"]).lower(), "IMPO_WAKE_LABEL": q(w["label"]),
              "IMPO_WAKE_KEYWORDS": "{" + ",".join(map(q, w["keywords"])) + "}" if w["keywords"] else "{NULL}",
              "IMPO_WAKE_KEYWORD_COUNT": str(len(w["keywords"]))}
    for key in ["model", "wrap"]:
        r = w.get(key, {"file": "", "sha256": "00" * 32})
        macros["IMPO_WAKE_" + key.upper() + "_FILE"] = q(r["file"])
        macros["IMPO_WAKE_" + key.upper() + "_SHA256"] = "{" + ",".join("0x" + r["sha256"][i:i+2] for i in range(0,64,2)) + "}"
    header = "/* Generated by the Impo ARCS Mini SDK. Do not edit. */\n#pragma once\n" + "".join(f"#define {k} {v}\n" for k,v in macros.items())
    config = "# Generated product defaults; credentials belong in official device storage.\n"
    for key, field in [("NODE_PREFIX", "node_prefix"), ("DISPLAY_NAME", "display_name"), ("MODEL_ID", "model_id")]:
        config += f"CONFIG_GADGET_{key}={q(p[field])}\n"
    config += "CONFIG_IMPO_CHINA_RELAY=" + ("y" if p["china_relay"] else "n") + "\n"
    glyphs = (ROOT / "components/impo_arcs_mini/assets/status_font.h").read_text()
    need = set(p["screen_title"] + w["label"])
    have = {chr(int(cp,16)) for cp in re.findall(r"\{0x([0-9a-fA-F]+),", glyphs)}
    if not need <= have:
        if not font:
            raise ValueError("Missing screen glyphs " + repr("".join(sorted(need-have))) + "; supply --font with the documented Noto Sans SC source")
        from gen_screen_font import generate, TEXT
        glyphs = generate(Path(font), TEXT + "".join(sorted(need)))
    manifest = {"sdk_version":re.search(r'#define IMPO_ARCS_MINI_SDK_VERSION "([^"]+)"',(ROOT/"components/impo_arcs_mini/include/impo_arcs_mini.h").read_text()).group(1), "product":{k:v for k,v in p.items() if not k.startswith("_")}, "resource_provisioning":"external; CP builds do not install wake models"}
    return {"impo_product_config.h":header.encode(), "product.conf":config.encode(),
            "impo_product_font.h":glyphs.encode(), "product.manifest.json":(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n").encode()}


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--product', type=Path, default=DEFAULT_PRODUCT)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--font', type=Path)
    a = ap.parse_args()
    p = load_product(a.product)
    files = generated_files(p, a.font)
    # Validate every output before replacing any generated build input.
    a.output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        dest = a.output / name
        if not dest.exists() or dest.read_bytes() != data:
            dest.write_bytes(data)
    inputs = p['_inputs'] + ([str(a.font.resolve())] if a.font else [])
    def cmake_literal(value):
        marks = '='
        while ']' + marks + ']' in value:
            marks += '='
        return '[' + marks + '[' + value + ']' + marks + ']'
    (a.output / 'inputs.cmake').write_text('set(IMPO_PRODUCT_INPUTS\n' +
        ''.join('  ' + cmake_literal(v) + '\n' for v in inputs) + ')\n')

if __name__ == '__main__':
    main()
