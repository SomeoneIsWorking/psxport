"""Hermetic checks for the setup action; no package manager, network, or build runs."""

from __future__ import annotations

import tempfile
import unittest
import subprocess
from pathlib import Path

from setup import PACKAGES, SetupPaths, export_environment, provision

ROOT = Path(__file__).resolve().parents[3]


class SetupTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = ROOT / "scratch" / "ci-setup-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        self.directory = self.enterContext(tempfile.TemporaryDirectory(dir=scratch))
        self.root = Path(self.directory)
        self.paths = SetupPaths.from_environment(
            {"GITHUB_WORKSPACE": str(self.root), "GITHUB_ENV": str(self.root / "environment"),
             "PSXPORT_CI_FRAMEWORK": ".", "PSXPORT_CI_DEPENDENCIES": "build/deps"}
        )
        self.commands: list[tuple[list[str], Path]] = []
        for relative in (
            "cmake/psxport.cmake", "build/deps/lightrec/lightrec.h",
            "build/deps/lightning/tools/build_lightning.py",
        ):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()

    def record(self, command: list[str], cwd: Path) -> None:
        self.commands.append((list(command), cwd))
        if command[0] == "uv":
            for relative in ("include/lightning.h", "lib/liblightning.a"):
                path = cwd / "build/install/linux-x86_64" / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()

    def test_shared_policy_and_prefix(self) -> None:
        values = provision(self.paths, self.record)
        self.assertEqual(len(self.commands), 4)
        self.assertEqual(self.commands[2][0], ["sudo", "apt-get", "install", "--yes", *PACKAGES])
        self.assertEqual(self.commands[3][1], self.root / "build/deps/lightning")
        self.assertIn("--frozen", self.commands[3][0])
        self.assertEqual(values["PSXPORT_LIGHTREC_DIR"], str(self.root / "build/deps/lightrec"))
        self.assertEqual(values["PSXPORT_LIGHTNING_PREFIX"], str(self.root / "build/deps/lightning/build/install/linux-x86_64"))

    def test_missing_checkout_refuses_before_mutation(self) -> None:
        (self.root / "build/deps/lightrec/lightrec.h").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing"):
            provision(self.paths, self.record)
        self.assertEqual(self.commands, [])

    def test_missing_build_output_refuses(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "did not produce"):
            provision(self.paths, lambda command, cwd: None)

    def test_failed_command_stops_provisioning(self) -> None:
        def fail(command: list[str], cwd: Path) -> None:
            self.commands.append((list(command), cwd))
            raise subprocess.CalledProcessError(1, command)

        with self.assertRaises(subprocess.CalledProcessError):
            provision(self.paths, fail)
        self.assertEqual(len(self.commands), 1)

    def test_dependency_root_escape_refuses(self) -> None:
        for destination in ("../deps", "scratch/deps", "build"):
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(RuntimeError, "build directory"):
                    SetupPaths.from_environment({
                        "GITHUB_WORKSPACE": str(self.root), "GITHUB_ENV": str(self.root / "env"),
                        "PSXPORT_CI_FRAMEWORK": ".", "PSXPORT_CI_DEPENDENCIES": destination,
                    })

    def test_environment_export_and_newline_refusal(self) -> None:
        export_environment(self.paths.environment_file, {"PSXPORT_LIGHTREC_DIR": "bounded/path"})
        self.assertEqual(self.paths.environment_file.read_text(), "PSXPORT_LIGHTREC_DIR=bounded/path\n")
        with self.assertRaisesRegex(RuntimeError, "line breaks"):
            export_environment(self.paths.environment_file, {"PSXPORT_LIGHTREC_DIR": "path\nBAD=1"})


if __name__ == "__main__":
    unittest.main()
