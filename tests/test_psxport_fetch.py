#!/usr/bin/env python3
"""test_psxport_fetch.py — the file a port SHIPS, against real git repositories and no network.

There is no pin any more: the contract is (1) a sibling framework checkout becomes a relative symlink
that every port follows live, (2) a machine with no sibling checkout still gets a usable framework by
cloning, (3) a real directory at `external/psxport` is never destroyed, and (4) the sibling checkout's
working tree and `.git/modules` stay byte-unchanged, because a git cwd that resolves through the
`external/psxport` symlink is what deleted them on 2026-09-30 (issue 0142).
"""

import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "psxport_fetch.py")


def git(args, cwd):
    done = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError(f"git {args} in {cwd} failed: {done.stderr}")
    return done.stdout.strip()


def seed_framework(path):
    """A directory that is a psxport checkout by the same two-file test the tool uses."""
    os.makedirs(os.path.join(path, "cmake"), exist_ok=True)
    for name in ("cmake/psxport.cmake", "CMakeLists.txt"):
        with open(os.path.join(path, name), "w", encoding="utf-8") as handle:
            handle.write("# fixture\n")


class FetchContract(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.mkdtemp(prefix="fetch-")
        self.repo = os.path.join(self.workspace, "spyro")
        self.framework = os.path.join(self.workspace, "psxport")
        os.makedirs(self.repo)
        seed_framework(self.framework)

    def run_tool(self, repo=None, env=None):
        environment = dict(os.environ)
        environment.pop("PSX", None)
        environment.update(env or {})
        done = subprocess.run([sys.executable, TOOL, "--auto", "--repo", repo or self.repo],
                              capture_output=True, text=True, env=environment)
        return done.returncode, done.stdout + done.stderr

    def link(self, repo=None):
        return os.path.join(repo or self.repo, "external", "psxport")

    def test_sibling_checkout_becomes_a_live_relative_symlink(self):
        code, out = self.run_tool()
        self.assertEqual(code, 0, out)
        link = self.link()
        self.assertTrue(os.path.islink(link))
        self.assertEqual(os.readlink(link), os.path.join("..", "..", "psxport"))
        self.assertEqual(os.path.realpath(link), os.path.realpath(self.framework))

    def test_an_existing_link_to_the_same_checkout_is_left_alone(self):
        os.makedirs(os.path.dirname(self.link()))
        os.symlink(os.path.join("..", "..", "psxport"), self.link())
        code, out = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertEqual(os.path.realpath(self.link()), os.path.realpath(self.framework))

    def test_a_link_to_a_checkout_this_machine_does_not_name_is_left_alone(self):
        # A linked worktree, or a workspace the sibling moved: discovery finds no candidate from here,
        # and the only framework this port has is the one it already links to.
        elsewhere = tempfile.mkdtemp(prefix="fetch-elsewhere-")
        seed_framework(elsewhere)
        os.remove(os.path.join(self.framework, "CMakeLists.txt"))
        os.makedirs(os.path.dirname(self.link()))
        os.symlink(elsewhere, self.link())
        code, out = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertEqual(os.path.realpath(self.link()), os.path.realpath(elsewhere))

    def test_a_real_clone_at_the_link_is_never_destroyed(self):
        os.makedirs(os.path.dirname(self.link()))
        os.makedirs(os.path.join(self.link(), ".git"))
        with open(os.path.join(self.link(), "keep.txt"), "w", encoding="utf-8") as handle:
            handle.write("somebody's work\n")
        code, out = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("REFUSED", out)
        self.assertTrue(os.path.isfile(os.path.join(self.link(), "keep.txt")))

    def test_no_sibling_checkout_clones_a_framework_in_place(self):
        origin = os.path.join(self.workspace, "origin.git")
        work = os.path.join(self.workspace, "origin-work")
        seed_framework(work)
        git(["init", "--initial-branch", "main"], work)
        git(["config", "user.email", "fixture@example.invalid"], work)
        git(["config", "user.name", "fixture"], work)
        git(["add", "-A"], work)
        git(["commit", "-m", "framework fixture"], work)
        git(["clone", "--bare", work, origin], self.workspace)
        moved = tempfile.mkdtemp(prefix="fetch-lonely-")
        repo = os.path.join(moved, "tomba")
        os.makedirs(repo)
        code, out = self.run_tool(repo=repo, env={"PSXPORT_FRAMEWORK_URL": origin})
        self.assertEqual(code, 0, out)
        link = self.link(repo)
        self.assertFalse(os.path.islink(link))
        self.assertTrue(os.path.isfile(os.path.join(link, "cmake", "psxport.cmake")))
        self.assertEqual(git(["rev-parse", "HEAD"], link), git(["rev-parse", "HEAD"], origin))

    def test_the_sibling_checkout_survives_the_clone_path_unchanged(self):
        # Issue 0142: the destruction came from a git cwd resolving THROUGH the symlink into the shared
        # checkout. Force that path — no sibling is discoverable, so the tool clones — and prove the
        # shared checkout's working tree and .git/modules are byte-identical afterwards.
        git(["init", "--initial-branch", "main"], self.framework)
        git(["config", "user.email", "fixture@example.invalid"], self.framework)
        git(["config", "user.name", "fixture"], self.framework)
        seed_framework(self.framework)
        git(["add", "-A"], self.framework)
        git(["commit", "-m", "shared checkout"], self.framework)
        vendor = os.path.join(self.framework, "vendor", "beetle-psx")
        os.makedirs(os.path.join(vendor, "deps"))
        seed_framework(vendor)
        git(["-C", vendor, "init", "--initial-branch", "main"], self.workspace)
        git(["-C", vendor, "config", "user.email", "fixture@example.invalid"], self.workspace)
        git(["-C", vendor, "config", "user.name", "fixture"], self.workspace)
        git(["-C", vendor, "add", "-A"], self.workspace)
        git(["-C", vendor, "commit", "-m", "beetle"], self.workspace)
        git(["-C", self.framework, "add", "-A"], self.workspace)
        git(["-C", self.framework, "commit", "-m", "vendor"], self.workspace)

        def snapshot():
            state = {}
            for base, _, names in os.walk(self.framework):
                if ".git" in os.path.relpath(base, self.framework).split(os.sep):
                    continue
                for name in names:
                    path = os.path.join(base, name)
                    with open(path, "rb") as handle:
                        state[os.path.relpath(path, self.framework)] = handle.read()
            return state, git(["status", "--porcelain", "--untracked-files=all"], self.framework)

        before = snapshot()
        lonely = tempfile.mkdtemp(prefix="fetch-lonely-")
        repo = os.path.join(lonely, "tekken3")
        os.makedirs(repo)
        origin = os.path.join(self.workspace, "origin.git")
        git(["clone", "--bare", self.framework, origin], self.workspace)
        code, out = self.run_tool(repo=repo, env={"PSXPORT_FRAMEWORK_URL": origin})
        self.assertEqual(code, 0, out)
        self.assertEqual(snapshot(), before)


if __name__ == "__main__":
    unittest.main()