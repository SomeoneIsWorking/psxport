"""Real local-git regressions for the launcher's top-level submodule contract."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "sync_submodules.py"
SCRATCH = Path(__file__).resolve().parents[1] / "scratch" / "sync-submodules-tests"


class SyncSubmoduleTests(unittest.TestCase):
    def setUp(self) -> None:
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=SCRATCH)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.checkout = self.root / "checkout"
        self.environment = os.environ.copy()
        self.environment.update(
            {
                "GIT_ALLOW_PROTOCOL": "file",
                "GIT_CONFIG_GLOBAL": str(self.root / "gitconfig"),
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_TERMINAL_PROMPT": "0",
                "GIT_AUTHOR_NAME": "Fixture",
                "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
                "GIT_COMMITTER_NAME": "Fixture",
                "GIT_COMMITTER_EMAIL": "fixture@example.invalid",
            }
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def command(self, *arguments: str, cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            arguments,
            cwd=cwd,
            env=self.environment,
            text=True,
            capture_output=True,
            timeout=20,
            check=False,
        )
        if check and result.returncode:
            self.fail(f"{' '.join(arguments)} failed ({result.returncode}): {result.stdout}{result.stderr}")
        return result

    def git(self, repo: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self.command("git", "-C", str(repo), *arguments)

    def create_repo(self, name: str) -> Path:
        repo = self.source / name
        self.command("git", "init", "-q", "--initial-branch=main", str(repo))
        (repo / "README").write_text(name)
        self.git(repo, "add", "README")
        self.git(repo, "commit", "-qm", "initial")
        return repo

    def fixture(self, *, mapped_nested: bool = True, missing_top_level: bool = False) -> None:
        nested = self.create_repo("nested")
        beetle = self.create_repo("beetle")
        if mapped_nested:
            self.git(beetle, "submodule", "add", "-q", "../nested", "deps/lightning/gnulib")
        else:
            nested_sha = self.git(nested, "rev-parse", "HEAD").stdout.strip()
            self.git(beetle, "update-index", "--add", "--cacheinfo", f"160000,{nested_sha},deps/lightning/gnulib")
        self.git(beetle, "commit", "-qm", "nested gitlink")

        lucent = self.create_repo("lucent")
        (lucent / "README").write_text("second")
        self.git(lucent, "commit", "-qam", "second")

        framework = self.create_repo("framework")
        self.git(framework, "submodule", "add", "-q", "../beetle", "vendor/beetle")
        self.git(framework, "submodule", "add", "-q", "../lucent", "vendor/lucent")
        if missing_top_level:
            ghost = self.create_repo("ghost")
            self.git(framework, "submodule", "add", "-q", "../ghost", "vendor/ghost")
        self.git(framework, "commit", "-qam", "vendors")
        if missing_top_level:
            ghost.rename(self.source / "ghost-unavailable")

        self.command("git", "clone", "-q", str(framework), str(self.checkout))

    def launch_sync(self) -> subprocess.CompletedProcess[str]:
        return self.command(sys.executable, str(SCRIPT), cwd=self.checkout, check=False)

    def assert_top_level_ready(self, result: subprocess.CompletedProcess[str], count: int = 2) -> None:
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("beetle", "lucent"):
            self.assertTrue((self.checkout / "vendor" / name / ".git").exists(), name)
        self.assertFalse(
            (self.checkout / "vendor/beetle/deps/lightning/gnulib/.git").exists(),
            "launcher sync initialized a nested submodule",
        )
        self.assertIn(f"checked {count} of {count} submodule(s)", result.stdout)

    def test_cold_sync_initializes_only_declared_top_level_paths(self) -> None:
        self.fixture(mapped_nested=True)
        result = self.launch_sync()
        self.assert_top_level_ready(result)

    def test_warm_sync_updates_stale_top_level_pin_without_nested_clone(self) -> None:
        self.fixture(mapped_nested=True)
        self.assert_top_level_ready(self.launch_sync())
        lucent = self.checkout / "vendor/lucent"
        recorded = self.git(lucent, "rev-parse", "HEAD").stdout.strip()
        self.git(lucent, "checkout", "-q", "HEAD~1")
        self.assertNotEqual(self.git(lucent, "rev-parse", "HEAD").stdout.strip(), recorded)
        result = self.launch_sync()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("vendor/lucent", result.stdout)
        self.assertEqual(self.git(lucent, "rev-parse", "HEAD").stdout.strip(), recorded)
        self.assertFalse((self.checkout / "vendor/beetle/deps/lightning/gnulib/.git").exists())

    def test_unmapped_nested_gitlink_is_outside_managed_set(self) -> None:
        self.fixture(mapped_nested=False)
        result = self.launch_sync()
        self.assert_top_level_ready(result)
        self.assertIn("nested gitlink(s) outside this sync", result.stdout)
        self.assertIn("vendor/beetle/deps/lightning/gnulib", result.stdout)

    def test_missing_declared_top_level_path_is_named_and_refused(self) -> None:
        self.fixture(missing_top_level=True)
        result = self.launch_sync()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CANNOT SEE", result.stderr)
        self.assertIn("vendor/ghost", result.stderr)
        self.assertIn(" of 3 submodule(s)", result.stderr)

    def test_dirty_stale_checkout_is_not_rewound(self) -> None:
        self.fixture()
        self.assert_top_level_ready(self.launch_sync())
        lucent = self.checkout / "vendor/lucent"
        self.git(lucent, "checkout", "-q", "HEAD~1")
        stale = self.git(lucent, "rev-parse", "HEAD").stdout.strip()
        (lucent / "LOCAL_WORK").write_text("mine")
        result = self.launch_sync()
        self.assertIn("NOT syncing", result.stderr)
        self.assertEqual(self.git(lucent, "rev-parse", "HEAD").stdout.strip(), stale)
        self.assertTrue((lucent / "LOCAL_WORK").exists())

    def test_deliberately_advanced_checkout_is_not_rewound(self) -> None:
        self.fixture()
        self.assert_top_level_ready(self.launch_sync())
        source_lucent = self.source / "lucent"
        (source_lucent / "README").write_text("third")
        self.git(source_lucent, "commit", "-qam", "third")
        lucent = self.checkout / "vendor/lucent"
        self.git(lucent, "fetch", "-q", "origin")
        self.git(lucent, "checkout", "-q", "FETCH_HEAD")
        advanced = self.git(lucent, "rev-parse", "HEAD").stdout.strip()
        result = self.launch_sync()
        self.assertIn("AHEAD of the recorded gitlink", result.stderr)
        self.assertEqual(self.git(lucent, "rev-parse", "HEAD").stdout.strip(), advanced)


if __name__ == "__main__":
    unittest.main()
