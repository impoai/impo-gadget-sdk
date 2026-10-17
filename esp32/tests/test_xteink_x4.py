"""Exercise the real X4 framebuffer conversion under ASan/UBSan on the host."""
import importlib.util
import struct
import zlib
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class XteinkX4Pixels(unittest.TestCase):
    def test_framebuffer_and_confirm_ladder(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "x4"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                str(ROOT / "tests/xteink_x4_harness.c"), "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)


class XteinkX4BootSelector(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("x4_flash", ROOT / "tools/xteink_x4_flash.py")
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    @staticmethod
    def sector(seq, state=2):
        crc = zlib.crc32(struct.pack("<I", seq), 0xffffffff)
        return struct.pack("<I20sII", seq, b"\xff" * 20, state, crc) + b"\xff" * (4096 - 32)

    def test_preserves_active_metadata_and_selects_app1(self):
        for active_seq in (1, 2, 31, 32):
            for active_slot in (0, 1):
                sectors = [b"\xff" * 4096, b"\xff" * 4096]
                sectors[active_slot] = self.sector(active_seq)
                offset, new = self.module.select_app1(b"".join(sectors))
                self.assertEqual(offset, 0xe000 + (1 - active_slot) * 4096)
                seq, _, state, crc = struct.unpack("<I20sII", new[:32])
                self.assertGreater(seq, active_seq)
                self.assertEqual((seq - 1) % 2, 1)
                self.assertEqual(state, 2)
                self.assertEqual(crc, zlib.crc32(new[:4], 0xffffffff))

    def test_real_device_crc_vectors_and_repeated_update(self):
        # Captured valid factory/CrossPoint entries, independent of the writer.
        # ESP-IDF calls esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4).
        factory = bytes.fromhex("01000000") + b"\xff" * 20 + bytes.fromhex("ffffffff9a984347")
        app1 = bytes.fromhex("02000000") + b"\xff" * 20 + bytes.fromhex("020000007437f655")
        sectors = factory + b"\xff" * (4096 - 32) + app1 + b"\xff" * (4096 - 32)
        offset, updated = self.module.select_app1(sectors)
        self.assertEqual(offset, 0xe000)
        self.assertEqual(updated[:4], bytes.fromhex("04000000"))
        self.assertEqual(updated[28:32], bytes.fromhex("a8689d70"))
        offset2, updated2 = self.module.select_app1(updated + sectors[4096:])
        self.assertEqual(offset2, 0xf000)
        self.assertEqual(struct.unpack_from("<I", updated2)[0], 6)

    def test_ignores_failed_images_and_refuses_counter_overflow(self):
        offset, new = self.module.select_app1(self.sector(2) + self.sector(19, 4))
        self.assertEqual(offset, 0xf000)
        self.assertEqual(struct.unpack_from("<I", new)[0], 4)
        with self.assertRaises(ValueError):
            self.module.select_app1(self.sector(0xfffffffe) + b"\xff" * 4096)
