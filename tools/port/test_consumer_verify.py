#!/usr/bin/env python3
"""Hermetic tests for the shared PSXPort consumer verifier."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
SCRATCH = ROOT / "scratch" / "consumer-verify-policy"
sys.path.insert(0, str(TOOLS))

from automation.process import ToolError
from port.consumer_verify import ConsumerVerifier, ConsumerVerifyConfig


class RecordingRunner:
    def __init__(self) -> None:
        self.commands: list[list[str]] = []
        self.environments: list[dict[str, str]] = []

    def __call__(self, command: list[str], *, cwd: Path, environment: dict[str, str]) -> None:
        self.commands.append(command)
        self.environments.append(dict(environment))


class ConsumerVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        SCRATCH.mkdir(parents=True, exist_ok=True)

    def fixture(self, root: Path) -> ConsumerVerifyConfig:
        psxport = root / "external" / "psxport"
        for required in (
            psxport / "cmake" / "psxport.cmake",
            psxport / "tools" / "check_execution_boundary.py",
            root / "cmake" / "title.cmake",
        ):
            required.parent.mkdir(parents=True, exist_ok=True)
            required.write_text("fixture\n", encoding="utf-8")
        return ConsumerVerifyConfig(
            name="Fixture",
            root=root,
            build=root / "build" / "ci",
            psxport=psxport,
            product=root / "build" / "ci" / "bin" / "fixture",
            cmake_module=root / "cmake" / "title.cmake",
            test_regex=r"^fixture_",
            cmake_definitions=("-DBUILD_TESTING=ON",),
            build_targets=("cpp_policy",),
            python=root / "locked-python",
        )

    def test_sequence_uses_one_explicit_title_configuration(self) -> None:
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            config = self.fixture(Path(directory))
            lightning_prefix = config.root / "build" / "deps" / "lightning-install"
            lightning_header = lightning_prefix / "include" / "lightning.h"
            lightning_library = lightning_prefix / "lib" / "liblightning.a"
            lightning_header.parent.mkdir(parents=True)
            lightning_library.parent.mkdir(parents=True)
            lightning_header.write_text("fixture\n", encoding="utf-8")
            lightning_library.write_bytes(b"fixture")
            lightrec = config.root / "build" / "deps" / "lightrec"
            lightrec.mkdir(parents=True)
            (lightrec / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
            (lightrec / "lightrec.h").write_text("/* fixture */\n", encoding="utf-8")
            runner = RecordingRunner()

            ConsumerVerifier(config, runner).verify(
                {
                    "KEEP": "yes",
                    "PSXPORT_LIGHTNING_PREFIX": str(lightning_prefix),
                    "PSXPORT_LIGHTREC_DIR": str(lightrec),
                }
            )

            self.assertEqual(len(runner.commands), 6)
            self.assertIn("-G", runner.commands[0])
            self.assertIn("Ninja", runner.commands[0])
            self.assertIn("-DCMAKE_C_COMPILER=clang", runner.commands[0])
            self.assertIn("-DCMAKE_CXX_COMPILER=clang++", runner.commands[0])
            self.assertIn(f"-DPSXPORT_LIGHTREC_DIR={lightrec}", runner.commands[0])
            self.assertIn(
                f"-DLIBLIGHTNING_INCLUDE_DIR={lightning_prefix / 'include'}", runner.commands[0]
            )
            self.assertIn(f"-DLIBLIGHTNING={lightning_library}", runner.commands[0])
            self.assertIn("-DBUILD_TESTING=ON", runner.commands[0])
            self.assertEqual(runner.commands[2][-2:], ["--target", "cpp_policy"])
            self.assertEqual(runner.commands[3][-2:], ["-R", r"^fixture_"])
            self.assertEqual(runner.commands[4][-1], "--selftest")
            self.assertEqual(runner.commands[5][-2:], ["--binary", str(config.product)])
            self.assertTrue(all(env["KEEP"] == "yes" for env in runner.environments))
            self.assertTrue(
                all(env["PSXPORT_DIR"] == str(config.psxport.resolve()) for env in runner.environments)
            )

    def test_out_of_tree_build_is_refused_before_any_command(self) -> None:
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            root = Path(directory)
            config = self.fixture(root)
            invalid = ConsumerVerifyConfig(
                name=config.name,
                root=config.root,
                build=root / "elsewhere",
                psxport=config.psxport,
                product=root / "elsewhere" / "fixture",
                cmake_module=config.cmake_module,
                test_regex=config.test_regex,
                python=config.python,
            )
            runner = RecordingRunner()

            with self.assertRaisesRegex(ToolError, "build must stay under"):
                ConsumerVerifier(invalid, runner).verify({})
            self.assertEqual(runner.commands, [])

    def test_empty_test_regex_is_refused_instead_of_running_zero_tests(self) -> None:
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            config = self.fixture(Path(directory))
            invalid = ConsumerVerifyConfig(
                name=config.name,
                root=config.root,
                build=config.build,
                psxport=config.psxport,
                product=config.product,
                cmake_module=config.cmake_module,
                test_regex="",
                python=config.python,
            )

            with self.assertRaisesRegex(ToolError, "refuses an empty CTest regex"):
                ConsumerVerifier(invalid, RecordingRunner()).verify({})


if __name__ == "__main__":
    unittest.main()
