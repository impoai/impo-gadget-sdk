# SPDX-License-Identifier: MIT
"""Unit attestation fields in link.register (contracts/gadget-identity.md)."""

from __future__ import annotations

import base64
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HASH = bytes(range(0xA0, 0xC0))


class GadgetAttestTest(unittest.TestCase):
    def run_harness(self, *defines: str) -> dict[str, dict[str, str]]:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / "gadget_attest_harness"
            compiled = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 *defines,
                 # Ahead of link_fakes, which has an esp_efuse.h of its own.
                 "-I", str(ROOT / "tests/attest_fakes"),
                 "-I", str(ROOT / "tests/link_fakes"),
                 "-I", str(ROOT / "main"),
                 str(ROOT / "tests/gadget_attest_harness.c"),
                 str(ROOT / "main/gadget_attest.c"),
                 str(ROOT / "tests/link_fakes/cJSON.c"),
                 "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        cases = {}
        for line in ran.stdout.splitlines():
            name, *fields = line.split()
            cases[name] = dict(field.split("=", 1) for field in fields)
        return cases

    def test_keyed_unit_attests_with_the_handshake_hash(self) -> None:
        cases = self.run_harness()
        keyed = cases["keyed"]
        self.assertEqual(keyed["unit_id"], "70af096dc5a8")
        self.assertEqual(keyed["key"], "5")
        self.assertEqual(bytes.fromhex(keyed["message"]),
                         b"impo-gadget/attest/v1\x00" + HASH)
        expected = base64.urlsafe_b64encode(bytes(range(32))).rstrip(b"=").decode()
        self.assertEqual(keyed["attest"], expected)
        self.assertEqual(len(keyed["attest"]), 43)

    def test_sends_neither_field_without_a_proof(self) -> None:
        cases = self.run_harness()
        # No session hash and a failed HMAC send nothing, never a partial pair;
        # a block without the HMAC_UP purpose is never handed to the peripheral.
        for name in ("no_session", "short_hash", "hmac_fails", "no_key"):
            self.assertEqual(cases[name]["unit_id"], "-", name)
            self.assertEqual(cases[name]["attest"], "-", name)
        self.assertEqual(cases["no_key"]["key"], "-1")
        self.assertEqual(cases["no_session"]["key"], "-1")

    def test_classic_esp32_never_attests(self) -> None:
        cases = self.run_harness("-DFAKE_TARGET_ESP32")
        self.assertEqual(cases["no_peripheral"], {"unit_id": "-", "attest": "-"})


if __name__ == "__main__":
    unittest.main()
