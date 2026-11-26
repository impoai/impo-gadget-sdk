#!/usr/bin/env python3
"""Product identity, generated build inputs and external wake resource contracts."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ARCS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ARCS / "tools"))
from product import DEFAULT_PRODUCT, generated_files, load_product
from wake_bundle import describe


class ProductTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.product = json.loads(DEFAULT_PRODUCT.read_text())
        self.bundle = {
            "schema_version": 1, "id": "synthetic-test", "label": "Hello Mini",
            "keywords": ["hello mini"],
            "model": {"file": "mini.bin", "sha256": hashlib.sha256(b"model").hexdigest()},
            "wrap": {"file": "mini.json", "sha256": hashlib.sha256(b"wrap").hexdigest()},
        }

    def load(self, product=None, bundle=None):
        (self.root / "wake.json").write_text(json.dumps(self.bundle if bundle is None else bundle))
        path = self.root / "product.json"
        path.write_text(json.dumps(self.product if product is None else product))
        return load_product(path)

    def enable_wake(self):
        self.product["wake"] = {"enabled": True, "bundle": "wake.json"}

    def test_generic_default_has_no_acoustic_dependency(self):
        p = load_product()
        self.assertEqual(p["display_name"], "ARCS Mini")
        self.assertFalse(p["wake"]["enabled"])
        files = generated_files(p)
        self.assertIn(b"CONFIG_IMPO_CHINA_RELAY=y", files["product.conf"])
        self.assertIn(b"IMPO_PLATFORM_PRODUCT_ID NULL", files["impo_product_config.h"])
        self.assertNotIn("Say ", p["instructions"])

    def test_platform_product_id_is_lowercase_uuid(self):
        for value in ("arcs-mini", "", 123, "AAAAAAAA-0000-0000-0000-000000000000"):
            p = copy.deepcopy(self.product)
            p["product_id"] = value
            with self.assertRaises(ValueError):
                self.load(p)
        self.product["product_id"] = "12345678-1234-1234-1234-123456789abc"
        self.assertIn(self.product["product_id"], generated_files(self.load())["impo_product_config.h"].decode())

    def test_custom_wake_uses_exact_engine_results(self):
        self.enable_wake()
        p = self.load()
        self.assertEqual(p["wake"]["keywords"], ["hello mini"])
        header = generated_files(p)["impo_product_config.h"].decode()
        self.assertIn('#define IMPO_WAKE_KEYWORDS {"hello mini"}', header)
        self.assertEqual(len(p["_inputs"]), 2)
        self.assertNotIn("_inputs", json.loads(generated_files(p)["product.manifest.json"])["product"])

    def test_text_alone_cannot_change_model(self):
        self.product["wake"] = {"enabled": True, "label": "another phrase"}
        with self.assertRaises(ValueError):
            self.load()
        self.product["wake"] = {"enabled": True}
        with self.assertRaises(ValueError):
            self.load()

    def test_resource_contract(self):
        self.enable_wake()
        for edit in (lambda b: b.update(keywords=[]), lambda b: b.update(keywords=["x", "x"]),
                     lambda b: b["model"].update(sha256="0" * 64),
                     lambda b: b["model"].update(file="../model.bin"),
                     lambda b: b["model"].update(file="mini.json")):
            b = copy.deepcopy(self.bundle)
            edit(b)
            with self.assertRaises(ValueError):
                self.load(bundle=b)

    def test_names_types_escaping_and_credentials(self):
        for key, value in (("id", "../escape"), ("continuous_dialogue", "yes"),
                           ("screen_title", "😀"), ("display_name", "bad\0"),
                           ("wifi_password", "synthetic-test-only")):
            p = copy.deepcopy(self.product)
            p[key] = value
            with self.assertRaises(ValueError):
                self.load(p)
        self.product["instructions"] = 'He said "hello".\nSecond line.'
        self.assertIn(b'He said \\"hello\\".\\nSecond line.', generated_files(self.load())["impo_product_config.h"])

    def test_missing_glyph_is_explicit(self):
        self.product["screen_title"] = "喵"
        with self.assertRaisesRegex(ValueError, "--font"):
            generated_files(self.load())

    def test_descriptor_hashes_real_bytes_and_rejects_incompatible_wrap(self):
        model, wrap = self.root / "mini.bin", self.root / "mini.json"
        model.write_bytes(b"ALOG_RES_HV10001\0synthetic-test-fixture")
        wrap.write_text('{"cae":{"chns":{"mic":1,"ref":1}}}')
        b = describe("test", "Hello Mini", ["hello mini"], model, wrap)
        self.assertEqual(b["model"]["sha256"], hashlib.sha256(model.read_bytes()).hexdigest())
        self.enable_wake()
        self.load(bundle=b)
        wrap.write_text('{"cae":{"chns":{"mic":2,"ref":1}}}')
        with self.assertRaises(ValueError):
            describe("test", "Hello Mini", ["hello mini"], model, wrap)
        model.write_text("text cannot train a wake model")
        with self.assertRaises(ValueError):
            describe("test", "Hello Mini", ["hello mini"], model, wrap)

    def test_generation_is_repeatable_and_invalid_input_preserves_output(self):
        p = self.root / "product.json"
        p.write_text(json.dumps(self.product))
        out = self.root / "generated"
        cmd = [sys.executable, str(ARCS / "tools/product.py"), "--product", str(p), "--output", str(out)]
        subprocess.run(cmd, check=True)
        before = {f.name: (f.read_bytes(), f.stat().st_mtime_ns) for f in out.iterdir() if f.name != "inputs.cmake"}
        subprocess.run(cmd, check=True)
        for name, (data, stamp) in before.items():
            self.assertEqual((out / name).read_bytes(), data)
            self.assertEqual((out / name).stat().st_mtime_ns, stamp)
        p.write_text('{"wifi_password":"synthetic-test-only"}')
        self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
        for name, (data, _) in before.items():
            self.assertEqual((out / name).read_bytes(), data)

    def test_manifest_matches_application_and_keeps_robot_dog_tool(self):
        result = subprocess.check_output([sys.executable, str(ARCS / "tools/gen_arcs_mini_manifest.py"),
            "--product", str(ARCS / "examples/button_companion/product.json"),
            "--commands", str(ARCS / "examples/button_companion/app.c") + ":commands"])
        manifest = json.loads(result)
        names = [c["name"] for c in manifest["commands"]]
        self.assertIn("companion.info", names)
        self.assertFalse(any(n.startswith(("motor.", "dog.", "camera.")) for n in names))
        self.assertEqual(manifest["events"], [])
        subprocess.run([sys.executable, str(ARCS / "tools/gen_manifest.py")], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
