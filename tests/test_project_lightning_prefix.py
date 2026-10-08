#!/usr/bin/env python3
"""Policy tests for the maintained-Lightning installed-prefix handoff."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
ROOT = TOOLS.parent
SCRATCH = ROOT / "scratch" / "lightning-prefix-policy"
sys.path.insert(0, str(TOOLS))

from automation.process import ToolError
from project import lightning_cmake_definitions


class LightningPrefixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        SCRATCH.mkdir(parents=True, exist_ok=True)

    def test_unconfigured_local_build_uses_normal_cmake_discovery(self) -> None:
        self.assertEqual(lightning_cmake_definitions({}), [])

    def test_complete_prefix_becomes_exact_lightrec_cmake_inputs(self) -> None:
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            prefix = Path(directory)
            header = prefix / "include" / "lightning.h"
            library = prefix / "lib" / "liblightning.a"
            header.parent.mkdir(parents=True)
            library.parent.mkdir(parents=True)
            header.write_text("fixture\n", encoding="utf-8")
            library.write_bytes(b"fixture")

            self.assertEqual(
                lightning_cmake_definitions({"PSXPORT_LIGHTNING_PREFIX": str(prefix)}),
                [
                    f"-DLIBLIGHTNING_INCLUDE_DIR={prefix / 'include'}",
                    f"-DLIBLIGHTNING={library}",
                ],
            )

    def test_incomplete_prefix_names_every_missing_required_artifact(self) -> None:
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            prefix = Path(directory)

            with self.assertRaises(ToolError) as raised:
                lightning_cmake_definitions({"PSXPORT_LIGHTNING_PREFIX": str(prefix)})

            message = str(raised.exception)
            self.assertIn(str(prefix / "include" / "lightning.h"), message)
            self.assertIn(str(prefix / "lib" / "liblightning.a"), message)


if __name__ == "__main__":
    unittest.main()
