"""psxport_fetch.py — the file a port SHIPS — against real git repositories, with no network.

WHAT IS BEING PROVEN, and why these cases are the ones that matter
------------------------------------------------------------------
MEASURED 2026-09-30 16:34: `--auto` run from a title's LINKED git worktree destroyed the SHARED framework
checkout. `vendor/beetle-psx` and `.git/modules/vendor/beetle-psx` were both deleted from a tree ten
titles build against. Two causes: discovery that could not name the shared checkout from a worktree, and
a `git submodule update` whose cwd resolved through a symlink to that checkout.

The tool's contract is now stronger than "link it": **external/psxport resolves to a checkout of the
PINNED commit**, a detached worktree under the shared checkout's own gitignored `scratch/pins/<sha>/`,
reused while clean and REFUSED — never repaired — when dirty or at another commit. So the cases are:

  (a) a linked worktree still finds the shared checkout and gets a PINNED tree, not the moving one;
  (b) no shared checkout anywhere -> a private clone at the same pin (the fresh-clone control);
  (c) the pinned worktree is created at the right sha, reused when clean, refused when dirty or at
      another sha, and two titles pinned to different shas get two worktrees;
  (d) a symlink that appears mid-clone refuses, with the shared checkout's bytes unchanged.

Every fixture is a real git repository in a temp directory: a real main checkout, a real linked worktree,
a real submodule, a real byte digest. A mocked `subprocess` cannot show that a `git` command ran through
the wrong path, which is the entire defect.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

# The canonical text. This file is not shipped any more — `psxport_fetch.py` is — so there is no second
# copy to hold it to the canonical one.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import psxport_fetch  # noqa: E402

GIT_ENV = {
    "GIT_AUTHOR_NAME": "fetch selftest",
    "GIT_AUTHOR_EMAIL": "fetch@selftest.invalid",
    "GIT_COMMITTER_NAME": "fetch selftest",
    "GIT_COMMITTER_EMAIL": "fetch@selftest.invalid",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
}


def git(*args: str, cwd: str) -> str:
    return subprocess.run(["git", *args], cwd=cwd, env=dict(os.environ, **GIT_ENV),
                          check=True, capture_output=True, text=True).stdout.strip()


@contextlib.contextmanager
def no_psx_env():
    """$PSX and $SHARED_DIR unset: the fixtures below are the whole world, and either inherited from the
    machine would silently point discovery at the real workspace."""
    saved = {k: os.environ.pop(k) for k in ("PSX", "SHARED_DIR") if k in os.environ}
    try:
        yield
    finally:
        os.environ.update(saved)


def worktree_digest(root):
    """Every path under the checkout's WORKING TREE and the SHA-256 of its bytes, `.git` excluded.

    `.git` and `scratch` are excluded deliberately, and it is not a convenience: creating a pinned
    worktree legitimately adds `<shared>/.git/worktrees/` metadata and `<shared>/scratch/pins/<sha>/`
    content, so a byte comparison that included either would fail on correct behaviour. What must never
    change is the tracked working tree — which is what the incident destroyed, `vendor/beetle-psx`
    included — and, checked separately, the shared checkout's `.git/modules`. Both are gitignored by
    design; everything else is not.
    """
    out = {}
    for path in sorted(Path(root).rglob("*")):
        if set(path.relative_to(root).parts[:1]) & {".git", "scratch"}:
            continue
        if path.is_symlink():
            out[str(path.relative_to(root))] = "symlink"
        elif path.is_file():
            out[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def module_digest(repo):
    """The shared checkout's main `.git/modules` tree — the other half of what the incident deleted."""
    modules = Path(repo, ".git", "modules")
    if not modules.is_dir():
        return {}
    return worktree_digest(modules)


class _FetchFixture:
    """A workspace holding a real shared framework, a real title, and a real linked worktree of it."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="fetch-selftest-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.ws = os.path.join(self.tmp, "ws")
        os.makedirs(self.ws)
        self.shared = self._framework("psxport", push=True)
        self.head = self.head_of(self.shared)
        self.second = self._commit_framework("second framework commit")
        self.title = self._title("toystory2")
        os.makedirs(os.path.join(self.title, "scratch", "wt"), exist_ok=True)
        self.worktree = os.path.join(self.title, "scratch", "wt", "s002")
        git("worktree", "add", "-q", "-b", "agent", self.worktree, cwd=self.title)

    def _framework(self, name, push=False):
        """A checkout that `is_framework` accepts — the marker is `cmake/psxport.cmake` — with a pushed
        remote, which is what makes a private clone at a pin possible."""
        path = os.path.join(self.ws, name)
        os.makedirs(os.path.join(path, "cmake"))
        git("init", "-q", "-b", "main", cwd=path)
        Path(path, "cmake", "psxport.cmake").write_text("# framework marker\n", encoding="utf-8")
        Path(path, "README.md").write_text(f"{name}\n", encoding="utf-8")
        git("add", "-A", cwd=path)
        git("commit", "-q", "-m", "framework", cwd=path)
        if push:
            remote = path + ".remote.git"
            subprocess.run(["git", "init", "-q", "--bare", "-b", "main", remote], check=True,
                           env=dict(os.environ, **GIT_ENV))
            git("remote", "add", "origin", remote, cwd=path)
            git("push", "-q", "origin", "main", cwd=path)
        return path

    def _commit_framework(self, message):
        Path(self.shared, "README.md").write_text(f"{message}\n", encoding="utf-8")
        git("add", "-A", cwd=self.shared)
        git("commit", "-q", "-m", message, cwd=self.shared)
        git("push", "-q", "origin", "main", cwd=self.shared)
        return self.head_of(self.shared)

    def _title(self, name):
        path = os.path.join(self.ws, name)
        os.makedirs(path)
        git("init", "-q", "-b", "main", cwd=path)
        Path(path, "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        git("add", "-A", cwd=path)
        git("commit", "-q", "-m", "title", cwd=path)
        return path

    def write_pin(self, repo, commit=None):
        commit = commit or self.head
        Path(repo, "psxport.pin").write_text(
            f"url = {self.shared}.remote.git\ncommit = {commit}\n", encoding="utf-8")
        return commit

    def head_of(self, path):
        return git("rev-parse", "HEAD", cwd=path)

    def link_of(self, repo):
        return os.path.join(repo, "external", "psxport")

    def run_fetch(self, repo, *extra):
        output = io.StringIO()
        with no_psx_env(), contextlib.redirect_stdout(output):
            status = psxport_fetch.main(["--repo", repo, *extra])
        return status, output.getvalue()


class PinnedWorktreeTests(_FetchFixture, unittest.TestCase):
    """(c) The pin itself. external/psxport must resolve to a checkout of the PINNED commit, so that a
    framework commit landing under a consumer cannot change what that consumer builds."""

    def test_the_pinned_worktree_is_created_at_the_recorded_sha(self) -> None:
        self.write_pin(self.title)
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 0, output)
        link = self.link_of(self.title)
        pinned = os.path.realpath(link)
        self.assertEqual(pinned, psxport_fetch.pin_worktree_path(self.shared, self.head))
        self.assertEqual(self.head_of(pinned), self.head, "the pinned tree must be AT the pin")
        self.assertIn("created", output)

    def test_the_link_is_detached_so_it_cannot_follow_the_moving_checkout(self) -> None:
        self.write_pin(self.title)
        self.run_fetch(self.title)
        pinned = os.path.realpath(self.link_of(self.title))
        self.assertNotEqual(os.path.realpath(pinned), os.path.realpath(self.shared),
                            "the link must not resolve to the moving shared checkout")
        branch = subprocess.run(["git", "symbolic-ref", "-q", "HEAD"], cwd=pinned,
                                env=dict(os.environ, **GIT_ENV), capture_output=True, text=True)
        self.assertEqual(branch.returncode, 1, f"the pinned worktree must be detached, got {branch.stdout!r}")

    def test_a_clean_pinned_worktree_is_reused_not_rebuilt(self) -> None:
        """Reuse is an inode, not a message: a rebuild would give the same text and a new directory."""
        self.write_pin(self.title)
        self.run_fetch(self.title)
        pinned = psxport_fetch.pin_worktree_path(self.shared, self.head)
        inode = os.stat(pinned).st_ino
        os.unlink(self.link_of(self.title))
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 0, output)
        self.assertIn("reused", output)
        self.assertEqual(os.stat(pinned).st_ino, inode, "reuse must not rebuild the pinned tree")

    def test_a_dirty_pinned_worktree_is_refused_and_never_repaired(self) -> None:
        self.write_pin(self.title)
        self.run_fetch(self.title)
        pinned = psxport_fetch.pin_worktree_path(self.shared, self.head)
        edit = Path(pinned, "README.md")
        edit.write_text("local framework edit\n", encoding="utf-8")
        os.unlink(self.link_of(self.title))
        before = worktree_digest(pinned)
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("REFUSED", output)
        self.assertIn("uncommitted", output)
        self.assertEqual(edit.read_text(encoding="utf-8"), "local framework edit\n",
                         "the refusal must not have repaired the tree")
        self.assertEqual(worktree_digest(pinned), before)
        self.assertFalse(os.path.lexists(self.link_of(self.title)), "a refusal must not link anything")

    def test_a_pinned_worktree_at_another_commit_is_refused(self) -> None:
        self.write_pin(self.title, self.second)
        self.run_fetch(self.title)
        pinned = psxport_fetch.pin_worktree_path(self.shared, self.second)
        git("checkout", "-q", self.head, cwd=pinned)      # now at a commit the pin does not name
        os.unlink(self.link_of(self.title))
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("not the pinned", output)
        self.assertEqual(self.head_of(pinned), self.head, "the refusal must not have moved it")

    def test_two_titles_pinned_to_different_commits_get_two_worktrees(self) -> None:
        other = os.path.join(self.ws, "crash")
        os.makedirs(other)
        git("init", "-q", "-b", "main", cwd=other)
        Path(other, "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        git("add", "-A", cwd=other)
        git("commit", "-q", "-m", "title", cwd=other)
        self.write_pin(self.title, self.head)
        self.write_pin(other, self.second)
        self.assertEqual(self.run_fetch(self.title)[0], 0)
        self.assertEqual(self.run_fetch(other)[0], 0)
        first, second = (os.path.realpath(self.link_of(self.title)),
                         os.path.realpath(self.link_of(other)))
        self.assertNotEqual(first, second)
        self.assertEqual(self.head_of(first), self.head)
        self.assertEqual(self.head_of(second), self.second)

    def test_the_shared_checkout_moves_and_the_pinned_title_does_not(self) -> None:
        """The point of the whole design: a framework commit landing must not change a title's build."""
        self.write_pin(self.title)
        self.run_fetch(self.title)
        pinned = os.path.realpath(self.link_of(self.title))
        landed = self._commit_framework("a commit that landed after the title was pinned")
        self.assertEqual(self.head_of(pinned), self.head, "the pinned tree moved with the checkout")
        self.assertEqual(self.head_of(self.shared), landed)
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 0, output)
        self.assertIn("reused", output)

    def test_two_titles_fetching_the_same_pin_at_once_leave_one_tree(self) -> None:
        """The race the `os.path.lexists` check cannot close: both saw the path free. The loser's rename
        must fail into a refusal with its staging cleaned up, not raise and not merge two trees."""
        self.write_pin(self.title)
        target = psxport_fetch.pin_worktree_path(self.shared, self.head)
        real_rename = os.rename

        def racing_rename(src, dst):
            if os.path.abspath(dst) == os.path.abspath(target):
                raise OSError(39, "Directory not empty")
            return real_rename(src, dst)

        with patch.object(psxport_fetch.os, "rename", side_effect=racing_rename):
            status, output = self.run_fetch(self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("another actor published it first", output)
        self.assertFalse(os.path.lexists(target), "the loser's tree must not have been published")
        self.assertFalse(os.path.lexists(self.link_of(self.title)), "a refusal links nothing")
        leftovers = [n for n in os.listdir(os.path.dirname(target)) if n != self.head]
        self.assertEqual(leftovers, [], f"the loser's staging worktree must be gone, found {leftovers}")

    def test_a_non_symlink_at_the_link_is_never_replaced(self) -> None:
        self.write_pin(self.title)
        os.makedirs(os.path.join(self.title, "external"), exist_ok=True)
        clone = self.link_of(self.title)
        os.makedirs(clone)
        Path(clone, "unpushed.txt").write_text("work in progress\n", encoding="utf-8")
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("REFUSED", output)
        self.assertTrue(Path(clone, "unpushed.txt").is_file())


class LinkFromWorktreeTests(_FetchFixture, unittest.TestCase):
    """(a) The incident's first cause. REPO is `<title>/scratch/wt/<name>`, so `REPO/../psxport` is
    `<title>/scratch/wt/psxport` — a directory that does not exist — and $PSX is unset. The shared
    checkout sits beside the worktree's MAIN checkout, so only the main-checkout candidate can find it.
    """

    def test_worktree_pins_the_shared_checkout_beside_its_main_checkout(self) -> None:
        self.write_pin(self.worktree)   # the pin is tracked, so the worktree carries its own copy
        before, modules_before = worktree_digest(self.shared), module_digest(self.shared)
        status, output = self.run_fetch(self.worktree)
        self.assertEqual(status, 0, output)
        link = self.link_of(self.worktree)
        self.assertTrue(os.path.islink(link), f"external/psxport must be a symlink, got {output}")
        pinned = os.path.realpath(link)
        self.assertEqual(pinned, psxport_fetch.pin_worktree_path(self.shared, self.head))
        self.assertEqual(self.head_of(pinned), self.head)
        self.assertEqual(worktree_digest(self.shared), before,
                         "pinning must not touch the shared checkout's working tree")
        self.assertEqual(module_digest(self.shared), modules_before,
                         "pinning must not touch the shared checkout's .git/modules")

    def test_the_candidate_is_derived_from_the_main_checkout_not_the_worktree(self) -> None:
        candidates = psxport_fetch.shared_candidates(self.worktree)
        self.assertIn(os.path.realpath(self.shared), [os.path.realpath(c) for c in candidates])
        # And the candidate that CANNOT work is still asked after it, not instead of it.
        self.assertIn(os.path.join(self.title, "scratch", "wt", "psxport"),
                      [os.path.abspath(c) for c in candidates])

    def test_psx_env_still_wins(self) -> None:
        """$PSX first is a contract, not an accident: two layouts, one explicit answer."""
        other = os.path.join(self.tmp, "elsewhere")
        os.makedirs(os.path.join(other, "psxport", "cmake"))
        Path(other, "psxport", "cmake", "psxport.cmake").write_text("# marker\n", encoding="utf-8")
        os.environ["PSX"] = other
        try:
            candidates = psxport_fetch.shared_candidates(self.title)
        finally:
            del os.environ["PSX"]
        self.assertEqual(os.path.realpath(candidates[0]), os.path.realpath(os.path.join(other,
                                                                                        "psxport")))


class PrivateCloneTests(_FetchFixture, unittest.TestCase):
    """(b) The control. No shared checkout anywhere: the tool must still produce a private clone at the
    recorded pin, because that is the fresh-clone path a stranger and CI take."""

    def setUp(self) -> None:
        super().setUp()
        # A title that shares no parent directory with a framework checkout, and $PSX unset.
        os.makedirs(os.path.join(self.tmp, "lonely"))
        self.lonely = os.path.join(self.tmp, "lonely", "title")
        shutil.copytree(self.title, self.lonely, symlinks=True,
                        ignore=shutil.ignore_patterns("scratch", ".git", "external"))
        git("init", "-q", "-b", "main", cwd=self.lonely)
        git("add", "-A", cwd=self.lonely)
        git("commit", "-q", "-m", "title", cwd=self.lonely)
        self.commit = self.write_pin(self.lonely)

    def test_no_shared_checkout_clones_privately_at_the_pin(self) -> None:
        self.assertFalse(psxport_fetch.is_framework(os.path.join(self.tmp, "lonely", "psxport")),
                         "fixture sanity: no framework may be reachable")
        status, output = self.run_fetch(self.lonely)
        self.assertEqual(status, 0, output)
        link = self.link_of(self.lonely)
        self.assertFalse(os.path.islink(link), f"expected a private clone, got {output}")
        self.assertTrue(os.path.isdir(os.path.join(link, ".git")))
        self.assertEqual(self.head_of(link), self.commit, "the clone must be AT the recorded pin")

    def clone_at_first_commit(self):
        self.assertEqual(self.run_fetch(self.lonely)[0], 0)
        link = self.link_of(self.lonely)
        first = self.head_of(link)
        return link, first

    def test_a_clean_private_clone_at_the_old_pin_is_advanced_to_the_new_one(self) -> None:
        """The reproduced defect: the pin moved, the tool exited 0 and left the clone behind."""
        link, first = self.clone_at_first_commit()
        self.write_pin(self.lonely, self.second)
        status, output = self.run_fetch(self.lonely)
        self.assertEqual(status, 0, output)
        self.assertNotEqual(first, self.second, "fixture sanity: two distinct commits")
        self.assertEqual(self.head_of(link), self.second, output)
        self.assertIn("advanced", output)

    def test_a_pin_the_clone_lacks_is_fetched_from_origin(self) -> None:
        link, _ = self.clone_at_first_commit()
        third = self._commit_framework("third framework commit, pushed after the clone was made")
        has = subprocess.run(["git", "cat-file", "-e", f"{third}^{{commit}}"], cwd=link,
                             capture_output=True, check=False)
        self.assertNotEqual(has.returncode, 0, "fixture sanity: the clone must not have the commit yet")
        self.write_pin(self.lonely, third)
        status, output = self.run_fetch(self.lonely)
        self.assertEqual(status, 0, output)
        self.assertEqual(self.head_of(link), third, output)

    def test_a_private_clone_already_at_the_pin_is_a_no_op(self) -> None:
        link, first = self.clone_at_first_commit()
        before = worktree_digest(link)
        status, output = self.run_fetch(self.lonely)
        self.assertEqual((status, output), (0, ""))
        self.assertEqual(self.head_of(link), first)
        self.assertEqual(worktree_digest(link), before)

    def test_a_dirty_private_clone_is_refused_naming_both_commits_and_keeps_its_changes(self) -> None:
        for change in ("tracked", "untracked"):
            with self.subTest(change=change):
                self.write_pin(self.lonely, self.head)
                link, first = self.clone_at_first_commit()
                if change == "tracked":
                    Path(link, "README.md").write_text("local edit\n", encoding="utf-8")
                else:
                    Path(link, "notes.txt").write_text("local file\n", encoding="utf-8")
                self.write_pin(self.lonely, self.second)
                before = worktree_digest(link)
                status, output = self.run_fetch(self.lonely)
                self.assertEqual(status, 2, output)
                self.assertIn(first[:12], output)
                self.assertIn(self.second[:12], output)
                self.assertEqual(self.head_of(link), first)
                self.assertEqual(worktree_digest(link), before, "the local changes must be intact")
                shutil.rmtree(link)

    def test_a_pin_origin_does_not_have_is_refused_naming_both_commits_and_moves_nothing(self) -> None:
        link, first = self.clone_at_first_commit()
        self.write_pin(self.lonely, "1" * 40)
        before = worktree_digest(link)
        status, output = self.run_fetch(self.lonely)
        self.assertEqual(status, 2, output)
        self.assertIn(first[:12], output)
        self.assertIn("1" * 12, output)
        self.assertEqual(self.head_of(link), first)
        self.assertEqual(worktree_digest(link), before)

    def test_a_symlink_to_a_live_framework_is_left_alone_when_no_shared_checkout_exists(self) -> None:
        link = self.link_of(self.lonely)
        os.makedirs(os.path.dirname(link))
        os.symlink(self.shared, link)
        self.write_pin(self.lonely, self.second)
        before = self.head_of(self.shared)
        status, output = self.run_fetch(self.lonely)
        self.assertEqual(status, 0, output)
        self.assertTrue(os.path.islink(link))
        self.assertEqual(os.path.realpath(link), os.path.realpath(self.shared))
        self.assertEqual(self.head_of(self.shared), before, "the live framework must not be moved")

    def test_a_missing_pin_refuses_rather_than_pinning_a_head(self) -> None:
        os.unlink(os.path.join(self.lonely, "psxport.pin"))
        status, output = self.run_fetch(self.lonely)
        self.assertEqual(status, 2, output)
        self.assertIn("REFUSED", output)
        self.assertFalse(os.path.lexists(self.link_of(self.lonely)),
                         "a refusal must leave the path absent, not half-built")

    def test_an_unreachable_pin_refuses_and_cleans_up_its_staging_directory(self) -> None:
        self.write_pin(self.lonely, "0" * 40)
        status, output = self.run_fetch(self.lonely)
        self.assertEqual(status, 1, output)
        self.assertIn("not reachable", output)
        leftovers = os.listdir(os.path.join(self.lonely, "external"))
        self.assertEqual(leftovers, [], f"staging must not survive a refusal, found {leftovers}")


class PublishedPinWithSubmoduleTests(_FetchFixture, unittest.TestCase):
    """The pinned tree is built under a staging name and renamed. Git's own metadata records PATHS (the
    worktree's `gitdir` link and every submodule's `core.worktree`), so a rename that does not repair them
    leaves a tree whose `git status` dies with `cannot chdir to .../.pin-XXXX/vendor/beetle-psx`. The
    other fixtures' frameworks have NO submodule, which is why that escaped."""

    def _framework(self, name, push=False):
        path = super()._framework(name, push=push)
        inner = os.path.join(self.tmp, f"{name}-beetle")
        os.makedirs(inner)
        git("init", "-q", "-b", "main", cwd=inner)
        Path(inner, "beetle.c").write_text("int beetle(void) { return 1; }\n", encoding="utf-8")
        git("add", "-A", cwd=inner)
        git("commit", "-q", "-m", "beetle", cwd=inner)
        git("-c", "protocol.file.allow=always", "submodule", "--quiet", "add", inner,
            "vendor/beetle-psx", cwd=path)
        git("commit", "-q", "-m", "add beetle", cwd=path)
        if push:
            git("push", "-q", "origin", "main", cwd=path)
        return path

    def run_fetch(self, repo, *extra):
        # The tool's `git submodule update` must be allowed to clone from a local path in this fixture.
        allow = {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "protocol.file.allow",
                 "GIT_CONFIG_VALUE_0": "always", **GIT_ENV}
        with patch.dict(os.environ, allow):
            return super().run_fetch(repo, *extra)

    def pinned(self):
        self.write_pin(self.title)
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 0, output)
        return psxport_fetch.pin_worktree_path(self.shared, self.head)

    def test_the_published_pin_can_report_status_and_submodule_status(self) -> None:
        pinned = self.pinned()
        self.assertTrue(os.path.isfile(os.path.join(pinned, "vendor", "beetle-psx", "beetle.c")),
                        "the fixture must really have initialised the submodule")
        self.assertEqual(git("status", "--porcelain", cwd=pinned), "")
        self.assertIn("vendor/beetle-psx", git("submodule", "status", cwd=pinned))
        self.assertEqual(git("status", "--porcelain", cwd=os.path.join(pinned, "vendor", "beetle-psx")), "")

    def test_no_git_metadata_still_names_the_staging_path(self) -> None:
        self.pinned()
        stale = []
        for root, _, files in os.walk(os.path.join(self.shared, ".git", "worktrees")):
            for name in files:
                if name in ("gitdir", "config"):
                    if ".pin-" in Path(root, name).read_text(encoding="utf-8"):
                        stale.append(os.path.join(root, name))
        self.assertEqual(stale, [], "metadata still names the staging path")

    def test_a_reused_pin_with_a_submodule_is_still_usable(self) -> None:
        self.pinned()
        os.unlink(self.link_of(self.title))
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 0, output)
        self.assertIn("reused", output)

    def test_the_concurrent_publish_refusal_still_holds_with_a_submodule(self) -> None:
        self.write_pin(self.title)
        target = psxport_fetch.pin_worktree_path(self.shared, self.head)
        real_rename = os.rename

        def racing_rename(src, dst):
            if os.path.abspath(dst) == os.path.abspath(target):
                raise OSError(39, "Directory not empty")
            return real_rename(src, dst)

        with patch.object(psxport_fetch.os, "rename", side_effect=racing_rename):
            status, output = self.run_fetch(self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("another actor published it first", output)
        self.assertFalse(os.path.lexists(target))
        self.assertEqual([n for n in os.listdir(os.path.dirname(target)) if n != self.head], [])

    def test_an_unusable_existing_pin_is_refused_untouched_and_the_git_error_is_reported(self) -> None:
        pinned = self.pinned()
        os.unlink(self.link_of(self.title))
        module_config = None
        for root, _, files in os.walk(os.path.join(self.shared, ".git", "worktrees")):
            if "config" in files and root.endswith(os.path.join("modules", "vendor", "beetle-psx")):
                module_config = os.path.join(root, "config")
        self.assertIsNotNone(module_config)
        broken = Path(module_config).read_text(encoding="utf-8").replace(
            "worktree = ", "worktree = ../missing/")
        Path(module_config).write_text(broken, encoding="utf-8")
        before = worktree_digest(pinned)
        status, output = self.run_fetch(self.title)
        self.assertEqual(status, 2, output)
        self.assertIn("REFUSED", output)
        self.assertIn("state cannot be read", output)
        self.assertIn("fatal:", output, "the refusal must carry git's own error text")
        self.assertEqual(Path(module_config).read_text(encoding="utf-8"), broken, "must not be repaired")
        self.assertEqual(worktree_digest(pinned), before)


class PublishRaceTests(_FetchFixture, unittest.TestCase):
    """(d) The incident's second cause, and the destructive one. While the clone is in flight another
    actor puts a symlink at external/psxport. The old code ran its `git submodule update` with cwd
    resolving THROUGH that symlink, and git's failure cleanup deleted the shared checkout's submodule.
    Here the shared checkout is a REAL repository with a REAL submodule and a REAL `.git/modules`, and
    both are asserted byte-unchanged afterwards."""

    def setUp(self) -> None:
        super().setUp()
        # A second checkout, structured like the shared one: a submodule plus its module gitdir.
        inner = os.path.join(self.tmp, "inner.git")
        work = os.path.join(self.tmp, "inner")
        os.makedirs(work)
        git("init", "-q", "-b", "main", cwd=work)
        Path(work, "beetle.c").write_text("int beetle(void) { return 1; }\n", encoding="utf-8")
        git("add", "-A", cwd=work)
        git("commit", "-q", "-m", "beetle", cwd=work)
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", inner], check=True,
                       env=dict(os.environ, **GIT_ENV))
        git("remote", "add", "origin", inner, cwd=work)
        git("push", "-q", "origin", "main", cwd=work)
        self.victim = os.path.join(self.ws, "victim")
        os.makedirs(self.victim)
        git("init", "-q", "-b", "main", cwd=self.victim)
        Path(self.victim, "cmake").mkdir()
        Path(self.victim, "cmake", "psxport.cmake").write_text("# marker\n", encoding="utf-8")
        git("add", "-A", cwd=self.victim)
        git("commit", "-q", "-m", "victim", cwd=self.victim)
        git("-c", "protocol.file.allow=always", "submodule", "--quiet", "add", inner,
            "vendor/beetle-psx", cwd=self.victim)
        git("commit", "-q", "-m", "add beetle", cwd=self.victim)
        # The title is far from any shared checkout, so the tool must take the CLONE path.
        os.makedirs(os.path.join(self.tmp, "race"))
        self.isolated = os.path.join(self.tmp, "race", "title")
        shutil.copytree(self.title, self.isolated, symlinks=True,
                        ignore=shutil.ignore_patterns("scratch", ".git", "external"))
        git("init", "-q", "-b", "main", cwd=self.isolated)
        git("add", "-A", cwd=self.isolated)
        git("commit", "-q", "-m", "title", cwd=self.isolated)
        self.write_pin(self.isolated)

    def test_symlink_appearing_mid_clone_refuses_and_leaves_the_shared_repo_untouched(self) -> None:
        link = self.link_of(self.isolated)
        before, modules_before = worktree_digest(self.victim), module_digest(self.victim)
        self.assertTrue(any(k.startswith("vendor/beetle-psx/") for k in before),
                        "fixture sanity: the victim must hold a submodule")
        self.assertTrue(any(k.startswith("vendor/beetle-psx/") for k in modules_before),
                        "fixture sanity: git must have a module gitdir to lose")
        real_git = psxport_fetch.git
        injected = []

        def interfering_git(args, cwd):
            # The interference lands exactly where the incident landed it: between the clone and the
            # submodule update, which is the first command the old code ran with cwd at the link.
            if args[:1] == ["submodule"] and not injected:
                injected.append(True)
                os.symlink(self.victim, link)
            return real_git(args, cwd)

        with patch.object(psxport_fetch, "git", side_effect=interfering_git):
            status, output = self.run_fetch(self.isolated)
        self.assertTrue(injected, "the fixture did not inject anything, so this proves nothing")
        self.assertEqual(status, 2, output)
        self.assertIn("REFUSED", output)
        self.assertTrue(os.path.islink(link), "the other actor's symlink must survive untouched")
        self.assertEqual(os.path.realpath(link), os.path.realpath(self.victim))
        self.assertEqual(worktree_digest(self.victim), before,
                         "the shared checkout's working tree changed: this is the incident")
        self.assertEqual(module_digest(self.victim), modules_before,
                         "the shared checkout's .git/modules changed: this is the incident")
        leftovers = [n for n in os.listdir(os.path.dirname(link)) if n != "psxport"]
        self.assertEqual(leftovers, [], f"staging must not survive the refusal, found {leftovers}")

    def test_no_git_command_ever_runs_with_its_cwd_at_the_link(self) -> None:
        """The general form of the race: whatever the path holds, no git command's cwd may be inside it.

        Every command's cwd is asserted to be absent, not the link itself and not anything under it —
        the second case is the one that destroyed the shared checkout, because a cwd inside a symlinked
        directory resolves through it.
        """
        link = self.link_of(self.isolated)
        cws = []
        real_git = psxport_fetch.git

        def recording_git(args, cwd):
            cws.append(os.path.realpath(cwd))
            return real_git(args, cwd)

        with patch.object(psxport_fetch, "git", side_effect=recording_git):
            self.run_fetch(self.isolated)
        self.assertTrue(cws, "no git command ran, so this asserts nothing")
        link_real = os.path.realpath(link)
        for cwd in cws:
            self.assertFalse(cwd == link_real or cwd.startswith(link_real + os.sep),
                             f"a git command ran with cwd inside external/psxport: {cwd}")

    def test_pinning_never_runs_a_git_command_inside_the_shared_checkout(self) -> None:
        """The same invariant on the PIN path: the shared checkout's own working tree is never a cwd,
        because that is where the incident's damage landed."""
        self.write_pin(self.title)
        cws = []
        real_git = psxport_fetch.git

        def recording_git(args, cwd):
            cws.append((tuple(args[:2]), os.path.realpath(cwd)))
            return real_git(args, cwd)

        with patch.object(psxport_fetch, "git", side_effect=recording_git):
            self.assertEqual(self.run_fetch(self.title)[0], 0)
        self.assertTrue(cws, "no git command ran, so this asserts nothing")
        shared_real = os.path.realpath(self.shared)
        # `git worktree add/remove` with cwd AT the repository is how a worktree is made and pruned, and
        # it touches only .git/worktrees metadata. What must never happen is a command that can modify
        # the shared WORKING TREE, which is what the incident deleted.
        staging = os.path.join(shared_real, "scratch") + os.sep
        for args, cwd in cws:
            inside = cwd.startswith(shared_real + os.sep) and not cwd.startswith(staging)
            self.assertFalse(inside, f"git {' '.join(args)} ran with cwd INSIDE the shared checkout: "
                                     f"{cwd}")
            if cwd == shared_real:
                # `git worktree add/remove` at the repository is how a worktree is made and pruned, and
                # it touches only .git/worktrees metadata. Anything else there can change the files a
                # consumer is reading.
                self.assertIn(args[0], ("worktree", "rev-parse", "status"),
                              f"git {args[0]} ran inside the shared checkout's working tree")


if __name__ == "__main__":
    unittest.main()
