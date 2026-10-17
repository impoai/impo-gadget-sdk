"""Exercise the persistent image store against interruptible NOR flash."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DisplaySnapshot(unittest.TestCase):
    def test_snapshot_storage(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "snapshot"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "main/display_snapshot.c"),
                str(ROOT / "tests/display_snapshot_harness.c"), "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)
