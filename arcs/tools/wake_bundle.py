#!/usr/bin/env python3
"""Describe existing compatible acoustic resources; does not train or flash them."""
import argparse
import hashlib
import json
from pathlib import Path

def describe(bundle_id, label, keywords, model, wrap):
    model, wrap = Path(model), Path(wrap)
    data = model.read_bytes()
    if len(data) < 16 or not data.startswith(b"ALOG_RES_HV10001"):
        raise ValueError("Expected an ACOMP-compatible acoustic model, not a keyword text file")
    raw = wrap.read_bytes()
    if not 2 <= len(raw) <= 4096:
        raise ValueError("wrap must contain 2-4096 bytes")
    j = json.loads(raw)
    channels = j.get("cae", {}).get("chns", {})
    if channels.get("mic") != 1 or channels.get("ref") != 1:
        raise ValueError("This hardware uses a one-microphone, one-reference wrap")
    return {"schema_version":1,"id":bundle_id,"label":label,"keywords":keywords,
            "model":{"file":model.name,"sha256":hashlib.sha256(data).hexdigest()},
            "wrap":{"file":wrap.name,"sha256":hashlib.sha256(raw).hexdigest()}}

if __name__ == "__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--id", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--keyword", action="append", required=True)
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--wrap", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    a=ap.parse_args()
    bundle=describe(a.id,a.label,a.keyword,a.model,a.wrap)
    # Reuse product validation before emitting a resource descriptor.
    from product import load_product
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp)
        (p/"wake.json").write_text(json.dumps(bundle,ensure_ascii=False))
        (p/"product.json").write_text(json.dumps({"schema_version":1,"id":"check","display_name":"Mini",
             "model_id":"arcs-mini","node_prefix":"mini","wake":{"enabled":True,"bundle":"wake.json"}}))
        load_product(p/"product.json")
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(bundle,ensure_ascii=False,indent=2)+"\n")
    print("Resource descriptor created; acoustic resources must be provisioned separately.")
