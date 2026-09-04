#!/usr/bin/env python3
"""Policy tests for the explicit exact-Lightrec checkout handoff."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
ROOT = TOOLS.parent
SCRATCH = ROOT / "scratch" / "lightrec-path-policy"
sys.path.insert(0, str(TOOLS))

from automation.process import ToolError
from project import lightrec_cmake_definitions


class LightrecPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        SCRATCH.mkdir(parents=True, exist_ok=True)

    def test_unconfigured_local_build_uses_normal_cmake_discovery(self) -> None:
        self.assertEqual(lightrec_cmake_definitions({}), [])

    def test_complete_checkout_becomes_an_explicit_cmake_input(self) -> None:
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            checkout = Path(directory)
            (checkout / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
            (checkout / "lightrec.h").write_text("/* fixture */\n", encoding="utf-8")

            self.assertEqual(
                lightrec_cmake_definitions({"PSXPORT_LIGHTREC_DIR": str(checkout)}),
                [f"-DPSXPORT_LIGHTREC_DIR={checkout}"],
            )

    def test_incomplete_checkout_names_every_missing_required_file(self) -> None:
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            checkout = Path(directory)

            with self.assertRaises(ToolError) as raised:
                lightrec_cmake_definitions({"PSXPORT_LIGHTREC_DIR": str(checkout)})

            message = str(raised.exception)
            self.assertIn(str(checkout / "CMakeLists.txt"), message)
            self.assertIn(str(checkout / "lightrec.h"), message)


if __name__ == "__main__":
    unittest.main()
