"""psxport_sync.py, as a port runs it: `external/psxport/tools/psxport_sync.py --repo <title>`.

WHAT CHANGED AND WHY THIS FILE WAS REWRITTEN AGAIN. It used to call the internals with `read_pin` and
`describe_link` mocked and the receipt's `dir` pointing at a literal string. That was adequate while
`--check` compared only `psxport_resolved.txt` against `psxport.pin`, and it stopped being adequate the
moment the check grew a staleness guard that ALSO asks whether the framework tree is at the commit the
receipt names and is clean: those two inputs are `head_of(dir)` and `dirty(dir)`, and a fixture with no
live tree returns None for both, so the guard fired and the "matching" cases failed. The test had been
asserting the old contract.

So this file builds REAL repositories in a temp directory — a real framework, a real pushed remote, a
real title repository with a real `psxport.pin` and a real `external/psxport` symlink, a real receipt —
and invokes the tool as a COMMAND, the way a port's gate does. Nothing is mocked except the process's
working directory in the one case that exists to pin the default. Mocking `open` away cannot test a
guard whose whole purpose is comparing a recorded snapshot against the live tree.

TWO CASES LEFT THIS FILE FOR THE PORT-SIDE ONE. The old file skipped them when a port's `verify_ci` was
absent, which is the shape of a skip that rots into a pass. They are gone instead of skipped: this tool
is no longer installed into a port (that is `psxport_fetch.py`'s job), so no port runs this file, and
the framework has no `verify_ci` to drive. Nothing here is conditional.
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import psxport_sync  # noqa: E402

GIT_ENV = {
    "GIT_AUTHOR_NAME": "pin selftest",
    "GIT_AUTHOR_EMAIL": "pin@selftest.invalid",
    "GIT_COMMITTER_NAME": "pin selftest",
    "GIT_COMMITTER_EMAIL": "pin@selftest.invalid",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
}


def git(*args: str, cwd: str) -> str:
    return subprocess.run(["git", *args], cwd=cwd, env=dict(os.environ, **GIT_ENV),
                          check=True, capture_output=True, text=True).stdout.strip()


class _TitleFixture:
    """A real framework checkout, a real title that links it, and a real build receipt."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="pin-selftest-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        # Named `psxport` so it is also the shared checkout `crash`'s sibling, which is the shape a
        # workspace has and the only one --link can answer.
        self.framework = self._framework("psxport")
        self.first = self.head_of(self.framework)
        self.head = self._commit_framework("second framework commit")
        self.title = self._title("crash")
        self.write_pin(self.head)
        self.build = os.path.join(self.tmp, "build")
        os.makedirs(self.build)
        self.write_receipt(self.framework, self.head)

    def _framework(self, name):
        path = os.path.join(self.tmp, name)
        os.makedirs(os.path.join(path, "cmake"))
        git("init", "-q", "-b", "main", cwd=path)
        Path(path, "cmake", "psxport.cmake").write_text("# framework marker\n", encoding="utf-8")
        Path(path, "core.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        git("add", "-A", cwd=path)
        git("commit", "-q", "-m", "framework", cwd=path)
        # A pushed remote, because `do_bump` refuses a pin a fresh clone could not fetch. That refusal
        # is only meaningful if a fetchable case exists to contrast with.
        remote = path + ".remote.git"
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", remote], check=True,
                       env=dict(os.environ, **GIT_ENV))
        git("remote", "add", "origin", remote, cwd=path)
        git("push", "-q", "origin", "main", cwd=path)
        return path

    def _commit_framework(self, message):
        Path(self.framework, "core.cpp").write_text(f"int main() {{ return {len(message)}; }}\n",
                                                    encoding="utf-8")
        git("add", "-A", cwd=self.framework)
        git("commit", "-q", "-m", message, cwd=self.framework)
        git("push", "-q", "origin", "main", cwd=self.framework)
        return self.head_of(self.framework)

    def _title(self, name):
        path = os.path.join(self.tmp, name)
        os.makedirs(os.path.join(path, "external"))
        git("init", "-q", "-b", "main", cwd=path)
        Path(path, "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        git("add", "-A", cwd=path)
        git("commit", "-q", "-m", "title", cwd=path)
        os.symlink(self.framework, os.path.join(path, "external", "psxport"))
        return path

    def write_pin_at(self, repo, commit=None):
        Path(repo, "psxport.pin").write_text(
            f"# psxport framework pin\nurl = {self.framework}.remote.git\n"
            f"commit = {commit or self.head}\n", encoding="utf-8")

    def write_pin(self, commit):
        Path(self.title, "psxport.pin").write_text(
            f"# psxport framework pin\nurl = {self.framework}.remote.git\ncommit = {commit}\n",
            encoding="utf-8")

    def write_receipt(self, directory, commit):
        Path(self.build, "psxport_resolved.txt").write_text(
            f"dir = {directory}\ncommit = {commit}\n", encoding="utf-8")

    def head_of(self, path):
        return git("rev-parse", "HEAD", cwd=path)

    def run_sync(self, *args, repo=None):
        """Invoke the tool as a port does, with the title named explicitly."""
        argv = list(args) + ["--build", self.build]
        if repo is not None:
            argv += ["--repo", repo]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = psxport_sync.main(argv)
        return status, output.getvalue()


class CheckTests(_TitleFixture, unittest.TestCase):
    def test_matching_receipt_passes(self) -> None:
        status, output = self.run_sync("--check", repo=self.title)
        self.assertEqual(status, 0, output)
        self.assertIn("check OK", output)

    def test_repo_defaults_to_the_working_directory(self) -> None:
        """`--repo` defaults to the cwd, so a port can say `--repo .` or say nothing."""
        previous = os.getcwd()
        os.chdir(self.title)
        self.addCleanup(os.chdir, previous)
        status, output = self.run_sync("--check")
        self.assertEqual(status, 0, output)
        self.assertIn("check OK", output)

    def test_absent_receipt_refuses(self) -> None:
        os.unlink(os.path.join(self.build, "psxport_resolved.txt"))
        status, output = self.run_sync("--check", repo=self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("REFUSED", output)

    def test_recorded_pin_does_not_match_receipt_fails(self) -> None:
        self.write_pin("f" * 40)
        status, output = self.run_sync("--check", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("check FAILED", output)

    def test_a_missing_pin_refuses_rather_than_asserting_nothing(self) -> None:
        os.unlink(os.path.join(self.title, "psxport.pin"))
        status, output = self.run_sync("--check", repo=self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("asserted NOTHING", output)

    # --- the staleness guard -------------------------------------------------------------
    # Each of these has the pin MATCHING the receipt, so the pin comparison alone would pass. The only
    # thing that can refuse is the guard, which is what makes them the guard's own tests.

    def test_framework_advanced_since_configure_fails(self) -> None:
        Path(self.framework, "core.cpp").write_text("int main() { return 1; }\n", encoding="utf-8")
        git("commit", "-q", "-am", "newer framework", cwd=self.framework)
        self.assertNotEqual(self.head_of(self.framework), self.head)
        status, output = self.run_sync("--check", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("changed since configure", output)

    def test_dirty_framework_fails(self) -> None:
        Path(self.framework, "core.cpp").write_text("int main() { return 2; }\n", encoding="utf-8")
        status, output = self.run_sync("--check", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("changed since configure", output)

    def test_receipt_naming_a_missing_framework_fails(self) -> None:
        """A receipt whose `dir` does not exist must not read as a match: `head_of` returns None for a
        missing directory, and "the tree is gone" must never be reported as "the pin matches"."""
        self.write_receipt(os.path.join(self.tmp, "not-a-tree"), self.head)
        status, output = self.run_sync("--check", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("check FAILED", output)


class BumpTests(_TitleFixture, unittest.TestCase):
    def test_bump_records_the_receipts_commit(self) -> None:
        self.write_pin("a" * 40)
        status, output = self.run_sync("--bump", repo=self.title)
        self.assertEqual(status, 0, output)
        self.assertIn(f"pin aaaaaaaa -> {self.head[:8]}", output)
        self.assertIn("not from the framework's current HEAD", output)
        self.assertEqual(psxport_sync.read_pin(os.path.join(self.title, "psxport.pin"))[1], self.head)

    def test_bump_refuses_when_the_framework_advanced_since_configure(self) -> None:
        """THE INCIDENT. Pin and framework head would both look fine; only the receipt is stale."""
        Path(self.framework, "core.cpp").write_text("int main() { return 3; }\n", encoding="utf-8")
        git("commit", "-q", "-am", "newer framework", cwd=self.framework)
        git("push", "-q", "origin", "main", cwd=self.framework)
        status, output = self.run_sync("--bump", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("stale", output)
        self.assertEqual(psxport_sync.read_pin(os.path.join(self.title, "psxport.pin"))[1], self.head,
                         "a refused bump must not write a pin")

    def test_bump_refuses_with_no_receipt(self) -> None:
        os.unlink(os.path.join(self.build, "psxport_resolved.txt"))
        status, output = self.run_sync("--bump", repo=self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("Reconfigure and build FIRST", output)
        self.assertEqual(psxport_sync.read_pin(os.path.join(self.title, "psxport.pin"))[1], self.head)

    def test_bump_refuses_a_dirty_framework(self) -> None:
        Path(self.framework, "core.cpp").write_text("int main() { return 4; }\n", encoding="utf-8")
        status, output = self.run_sync("--bump", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("stale", output)
        self.assertEqual(psxport_sync.read_pin(os.path.join(self.title, "psxport.pin"))[1], self.head)

    def test_bump_refuses_a_receipt_naming_another_tree(self) -> None:
        """A receipt for a different framework than the one this port links is not this port's build."""
        other = os.path.join(self.tmp, "other")
        os.makedirs(other)
        git("init", "-q", "-b", "main", cwd=other)
        Path(other, "x.cpp").write_text("int x;\n", encoding="utf-8")
        git("add", "-A", cwd=other)
        git("commit", "-q", "-m", "other", cwd=other)
        self.write_receipt(other, self.head_of(other))
        status, output = self.run_sync("--bump", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("does not consume", output)
        self.assertEqual(psxport_sync.read_pin(os.path.join(self.title, "psxport.pin"))[1], self.head)

    def test_bump_refuses_an_unpushed_commit(self) -> None:
        Path(self.framework, "core.cpp").write_text("int main() { return 5; }\n", encoding="utf-8")
        git("commit", "-q", "-am", "unpushed framework", cwd=self.framework)
        self.write_receipt(self.framework, self.head_of(self.framework))
        status, output = self.run_sync("--bump", repo=self.title)
        self.assertEqual(status, 1, output)
        self.assertIn("not on any remote branch", output)
        self.assertEqual(psxport_sync.read_pin(os.path.join(self.title, "psxport.pin"))[1], self.head)


class ReportAndLinkTests(_TitleFixture, unittest.TestCase):
    def test_report_names_the_link_the_pin_and_sync_state(self) -> None:
        status, output = self.run_sync(repo=self.title)
        self.assertEqual(status, 0, output)
        self.assertIn("external/psxport : symlink", output)
        self.assertIn(self.head, output)
        self.assertIn("IN SYNC", output)

    def test_report_names_drift_with_a_commit_count(self) -> None:
        self.write_pin(self.first)
        status, output = self.run_sync(repo=self.title)
        self.assertEqual(status, 0, output)
        self.assertIn("DRIFT", output)
        self.assertIn("1 commit(s)", output)

    def test_link_refuses_to_replace_a_real_clone_without_force(self) -> None:
        link = os.path.join(self.title, "external", "psxport")
        os.unlink(link)
        subprocess.run(["git", "clone", "-q", self.framework + ".remote.git", link], check=True,
                       env=dict(os.environ, **GIT_ENV))
        marker = Path(link, "unpushed.txt")
        marker.write_text("work in progress\n", encoding="utf-8")
        status, output = self.run_sync("--link", repo=self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("REFUSED", output)
        self.assertTrue(marker.is_file(), "the refusal must not have touched the clone")

    def test_link_points_at_the_pinned_worktree_when_the_path_is_free(self) -> None:
        """A link never resolves to the MOVING shared checkout, and never to a tree that is not there."""
        target = os.path.join(self.title, "external", "psxport")
        os.unlink(target)
        status, output = self.run_sync("--link", repo=self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("Run tools/psxport_fetch.py", output)
        pinned = os.path.join(self.framework, "scratch", "pins", self.head)
        os.makedirs(os.path.join(pinned, "cmake"), exist_ok=True)
        Path(pinned, "cmake", "psxport.cmake").write_text("# marker\n", encoding="utf-8")
        status, output = self.run_sync("--link", repo=self.title)
        self.assertEqual(status, 0, output)
        self.assertTrue(os.path.islink(target))
        self.assertEqual(os.path.realpath(target), os.path.realpath(pinned))
        self.assertNotEqual(os.path.realpath(target), os.path.realpath(self.framework),
                            "the link must not resolve to the moving shared checkout")
        self.assertIn("NOT live here", output)

    def test_link_refuses_without_a_pin_before_it_looks_anywhere(self) -> None:
        os.unlink(os.path.join(self.title, "psxport.pin"))
        status, output = self.run_sync("--link", repo=self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("no commit to link to", output)

    def test_link_refuses_when_no_shared_checkout_exists(self) -> None:
        lonely = os.path.join(self.tmp, "nowhere", "title")
        os.makedirs(os.path.join(lonely, "external"))
        git("init", "-q", "-b", "main", cwd=lonely)
        Path(lonely, "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        git("add", "-A", cwd=lonely)
        git("commit", "-q", "-m", "title", cwd=lonely)
        self.write_pin_at(lonely)
        previous = os.environ.pop("PSX", None)
        try:
            status, output = self.run_sync("--link", repo=lonely)
        finally:
            if previous is not None:
                os.environ["PSX"] = previous
        self.assertEqual(status, 2, output)
        self.assertIn("no shared framework checkout found", output)
        self.assertIn("psxport_fetch.py", output, "the refusal must name what does clone instead")


if __name__ == "__main__":
    unittest.main()
