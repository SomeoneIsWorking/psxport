#!/usr/bin/env python3
"""Exercise the shipping RAM disassembler CLI with synthetic words and malformed inputs."""

from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
RAM_BYTES = 2 * 1024 * 1024


class DisassemblerCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = ROOT / "scratch" / "disasm-selftest"
        cls.directory.mkdir(parents=True, exist_ok=False)
        cls.ram = cls.directory / "ram.bin"
        data = bytearray(RAM_BYTES)
        struct.pack_into("<III", data, 0, 0x24020001, 0x03E00008, 0)
        cls.ram.write_bytes(data)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.directory)

    def cli(self, start="80000000", end="8000000c", path=None):
        return subprocess.run([sys.executable, str(ROOT / "tools" / "disasm.py"),
                               str(path or self.ram), start, end],
                              capture_output=True, text=True, timeout=10)

    def test_complete_range_and_ram_aliases(self):
        for base in (0, 0x80000000, 0xA0000000):
            result = self.cli(f"{base:x}", f"{base + 12:x}")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("addiu", result.stdout)
            self.assertIn("jr", result.stdout)
            self.assertIn(f"{base + 8:08X}  00000000  nop", result.stdout)
            self.assertIn("scanned 3/3 words; decoded 3/3 words; unknown 0; complete", result.stdout)
        result = self.cli("801ffffc", "80200000")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("scanned 1/1 words; decoded 1/1 words", result.stdout)

    def test_undecodable_words_do_not_hide_following_instructions(self):
        unknown = self.directory / "unknown.bin"
        data = bytearray(self.ram.read_bytes())
        # Invalid encoding and a PSX GTE command unsupported by Capstone are both explicit refusals.
        for word in (0xFFFFFFFF, 0x4A000001, 0x4842F800):
            struct.pack_into("<I", data, 4, word)
            unknown.write_bytes(data)
            result = self.cli(path=unknown)
            self.assertEqual(result.returncode, 1)
            self.assertIn(f"80000004  {struct.pack('<I', word).hex()}  UNKNOWN raw=0x{word:08X}",
                          result.stdout)
            self.assertIn("80000008  00000000  nop", result.stdout)
            self.assertIn("scanned 3/3 words; decoded 2/3 words; unknown 1; REFUSED", result.stdout)

    def test_invalid_ranges_refuse_without_scanning(self):
        ranges = (("0", "0"), ("8", "4"), ("1", "4"), ("0", "5"),
                  ("1ffffc", "200004"), ("80000000", "a0000004"),
                  ("40000000", "40000004"), ("-4", "4"))
        for start, end in ranges:
            result = self.cli(start, end)
            self.assertEqual(result.returncode, 1)
            self.assertIn("scanned 0 words; decoded 0 words", result.stderr)

    def test_missing_short_and_oversized_dump_refuse(self):
        missing = self.directory / "missing.bin"
        self.assertEqual(self.cli(path=missing).returncode, 1)
        invalid = self.directory / "invalid.bin"
        for size in (0, RAM_BYTES - 1, RAM_BYTES + 1):
            invalid.write_bytes(bytes(size))
            result = self.cli(path=invalid)
            self.assertEqual(result.returncode, 1)
            self.assertIn("exactly 2097152 bytes", result.stderr)
            self.assertIn("scanned 0 words; decoded 0 words", result.stderr)


if __name__ == "__main__":
    unittest.main()
