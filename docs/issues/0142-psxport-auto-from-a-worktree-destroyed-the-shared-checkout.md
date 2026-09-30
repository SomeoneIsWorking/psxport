---
id: 142
title: "--auto from a title's linked worktree destroyed the SHARED framework checkout's beetle-psx"
status: fixed
symptom: "`~/repo/psx/psxport/vendor/beetle-psx` and `~/repo/psx/psxport/.git/modules/vendor/beetle-psx` were gone; ten titles build off that checkout"
tags: pin-tool,worktree,git,provenance,destructive
created: 2026-09-30
updated: 2026-09-30
---

## What happened

MEASURED 2026-09-30 16:34-16:35. An agent ran `tools/psxport_sync.py --auto` inside a title's LINKED git
worktree, `~/repo/psx/toystory2/scratch/wt/s002`. Afterwards the shared framework checkout
`~/repo/psx/psxport` had **no `vendor/beetle-psx` working tree and no `.git/modules/vendor/beetle-psx`** —
the gitlink was still recorded, the content was not. That is the one checkout all ten titles symlink at,
so this was a framework-wide outage caused by a routine "make my link" command.

## Cause 1 — discovery could not name the shared checkout from a worktree

`shared_candidates()` asked two places: `$PSX/psxport` and `REPO/../psxport`. In a linked worktree
`REPO` is `<title>/scratch/wt/<name>`, so the second is `<title>/scratch/wt/psxport`, which does not
exist, and `$PSX` was not set in that session. **Zero of the two candidates existed**, so `--auto` took
its other branch and CLONED a private framework into the worktree instead of linking. The worktree then
had a second, private copy of the framework — and the run against it is what turned cause 1 into damage.

## Cause 2 — a git command whose cwd resolved through a symlink this tool did not create

`do_clone()` cloned and ran `git submodule update --init ...` directly at `LINK`
(`external/psxport`). While that ran, the path was replaced by a symlink to the shared checkout. The
submodule update therefore executed **inside the shared checkout**, failed, and git's own failure
cleanup removed the shared checkout's submodule. The tool was writing through a path it had not verified
and could not have verified, because it never re-checked.

The two causes compose: cause 1 is what made the tool run at all in a place where the shared tree was
reachable; cause 2 is what made that run destructive.

## Fix

Three rules, all now the tool's contract. The second is a rule about the SHAPE of the operation rather
than about any one check, and the third is the one that changes what a build is against:

1. **The shared checkout is located from the MAIN checkout.** `git rev-parse --git-common-dir` names
   `.git` inside a normal checkout and the MAIN checkout's `.git` inside a linked worktree, so its
   parent is the main checkout root, and that directory's sibling `psxport` is the shared framework.
   Every worktree of a repository can answer that for itself. Order is `$PSX`, the main checkout's
   sibling, `REPO`'s sibling, deduplicated on the resolved path with `$PSX` still first.
2. **No git command runs with its cwd at `external/psxport`, or through any symlink this tool did not
   create.** Every git operation happens in a staging directory on the same filesystem, and the result
   is published by one `os.rename` that happens only if the path is still free. A path that appeared
   mid-flight is a REFUSAL, and only the tool's own staging directory is removed.
3. **`external/psxport` resolves to a checkout of the PINNED commit, never to the moving shared
   checkout.** This one is a SCOPE ADDITION to the incident fix, and it was asked for directly because
   the incident exposed what a shared checkout being one object for ten consumers actually costs: a
   framework commit landing under a running consumer changes what that consumer builds, with no edit to
   it and nothing in the title's tree to show for it.

## The pin: a detached worktree per commit, under the shared checkout's own `scratch/`

`<shared>/scratch/pins/<full-sha>/`, a detached `git worktree` of the shared repository. `scratch/` is
gitignored there, so the pins cost nothing to a `git status` in the shared checkout and are removed with
the rest of the workspace scratch. With a shared checkout present, `psxport_fetch.py`:

- reads `psxport.pin` and REUSES the worktree only if its HEAD is exactly that commit, it belongs to the
  shared repository (`--git-common-dir` matches), and `git status --porcelain --untracked-files=all` is
  EMPTY;
- otherwise REFUSES, naming which of the three failed and repairing nothing. A dirty pinned worktree is
  somebody's work in progress, and a `reset --hard` or a `git worktree remove --force` performed by a
  bootstrap tool to make a build work is exactly the incident again with a different target. A worktree
  at another commit, or one belonging to a different repository, is refused the same way;
- otherwise creates it in a temporary sibling (`<shared>/scratch/pins/.pin-<pid>`), runs the declared
  submodule git operations INSIDE that staging worktree — never in the shared checkout — and publishes it
  with one `os.rename`, which is atomic because it is the same directory and therefore the same
  filesystem;
- finally points `external/psxport` at it with an atomic symlink rename, and never replaces a
  non-symlink.

Two titles pinned to two commits get two worktrees, and a commit landing in the shared checkout changes
neither. The no-shared-checkout path is unchanged: a private clone checked out at the pin, staged and
published the same way, which is what a stranger's clone and CI take.

## Lightrec, pinned the same way, from one home for the revision

`shared/lightrec` has the identical hazard and the identical fix. `tools/psxport_fetch.py --lightrec`
reads `PSXPORT_LIGHTREC_REVISION` out of `cmake/lightrec_dependency.cmake` — the pin's ONE home, so the
tool and the resolver cannot disagree — and ensures the detached worktree at
`<shared-lightrec>/scratch/pins/<revision>/`. `psxport_configure_lightrec_dependency()` probes that path
BEFORE the plain checkout for every candidate, so an explicit `PSXPORT_LIGHTREC_DIR` still wins and
merely prefers that directory's own pinned tree.

Without this, a Lightrec commit landing under a running consumer turns the existing revision check into a
hard configure failure in a tree that was green a minute earlier — a false red with no change to blame.
The revision and cleanliness checks are KEPT: they are what makes a plain checkout usable at all, they
are what refuse a mismatched plain checkout, and they are what refuse a dirty pinned worktree. Both
refusals now say how to fix themselves (`python3 tools/psxport_fetch.py --lightrec`).

`psxport_sync.py --link` follows the same rule: it points at the pinned worktree and REFUSES when there
is none, naming `tools/psxport_fetch.py` as the thing that creates one, rather than linking to a moving
checkout under the same flag name the incident used.

## The split that fell out of it

The port needs one tool BEFORE it has the framework, and the pin check cannot be that tool: the check
reads `psxport.pin` and a build receipt, and the fetched framework is what produces the receipt. So:

- `tools/psxport_fetch.py` — the ONE file a port ships. Make `external/psxport` exist at the PINNED
  commit: a detached worktree of the shared checkout, else a private clone at the pin. Staged and
  published as above, refuses rather than repairs. stdlib only. `--lightrec` pins `shared/lightrec` the
  same way.
- `tools/psxport_sync.py` — the framework's own. Report, `--check`, `--bump`, and an explicit `--link`
  that points at a pinned worktree and refuses when there is none. Invoked from the pinned checkout
  against a title: `external/psxport/tools/psxport_sync.py --repo .`.
- `tools/check_port_pin_tools.py` now gates `psxport_fetch.py` (bytes + `--help` runs), because that is
  the copy that ships. It also NAMES every directory still carrying the retired `tools/psxport_sync.py`,
  so a port cannot drop out of the denominator silently while the migration is in flight.

## Evidence

`tests/test_psxport_fetch.py`, **17 cases** over real git repositories and no network — real linked
worktrees, real submodules, real byte digests of a working tree and of a `.git/modules` tree. Against
the pre-change code, run through a scratch harness that reuses those same fixtures
(`scratch/fetch-red/fetch-red.py`, not shipped), **7 of 12 properties fail**:

| case | property | pre-change | now |
|---|---|---|---|
| (a) linked worktree | status 0 | 2 (REFUSED: no usable psxport.pin) | 0 |
| (a) | `external/psxport` is a symlink | not a symlink, `external/` absent | symlink at the pin |
| (a) | the shared checkout is byte-unchanged | — | holds, working tree AND `.git/modules` |
| (a) | the candidate comes from the MAIN checkout | absent from the list | present, and asked before the one that cannot work |
| (b) the pin itself | resolves to the PINNED commit | **the moving shared checkout** | `scratch/pins/<sha>/` |
| (b) | a framework commit landing does not move it | **it does move it** | holds |
| (c) mid-clone symlink race | status is a refusal | 0, printed "cloned at pin" and stopped | 2, REFUSED |
| (c) | no git command's cwd inside the shared checkout | **ran at `<ws>/victim/vendor/beetle-psx`** | every cwd is a staging directory |
| (c) | the shared checkout is byte-unchanged | held in this fixture | holds, including `.git/modules` |
| (c) | the interfering symlink survives | held | holds |

The (b) rows are the strongest of the three: they are the one the incident did not cause, and they show
the old design answering "it works" — status 0, a symlink, the expected text — while the build follows
whatever the shared checkout is at.

WHAT THIS DOES **NOT** SHOW, stated because it is the interesting half: in this synthetic fixture the
victim's submodule was healthy, so git's failed update did not delete it and the byte-comparison
property passed against the old code too. What the fixture does measure is the mechanism — a git command
executing inside the shared checkout — which is what the incident's deletion came from. The shipped test
asserts both the invariant that makes the deletion impossible and the byte comparison, so a future git
that cleans up harder is caught rather than needed.

The pin path is refused rather than repaired in three named ways — dirty, wrong commit, foreign
repository — and each refusal asserts the pinned tree's own byte digest afterwards, so "refused" cannot
quietly become "reset".

`tests/test_lightrec_pinned_worktree.py`, **6 checks**, fixture-only: a throwaway two-commit Lightrec
repository and its own pin, three REAL `cmake` configures. A consumer resolves the pinned worktree even
though the shared checkout has moved past the pin; with no pinned worktree the moved checkout is REFUSED
by revision; and a dirty pinned worktree is not accepted. It never reads or touches the real
`shared/lightrec`.

`tests/test_psxport_sync.py`, 20 cases, invoked the way a port invokes the tool, including that
`--link` refuses when no pinned worktree exists and refuses a non-symlink without `--force`.
`tools/check_port_pin_tools.py --selftest`, 19 checks.

## Follow-up (not done here, and it is the operator's)

No title repository was edited. Each needs: delete `tools/psxport_sync.py` and its test, add
`tools/psxport_fetch.py` from `tools/check_port_pin_tools.py --install`, and update the call sites below.
The pin gate cannot enforce that migration from inside psxport; it can only name the ports that have not
been migrated, which it now does — **10 of 10 named** on the real workspace at the time of writing.

Every title: `git rm tools/psxport_sync.py tests/test_psxport_sync.py` (or `tools/test_psxport_sync.py`
where that is where it lives), install the canonical `tools/psxport_fetch.py`, drop the CTest entry that
ran `tests/test_psxport_sync.py`, and rewrite the pin test's command from
`tools/psxport_sync.py --check|--bump` to
`external/psxport/tools/psxport_sync.py --repo <source-dir> --check|--bump`.

The FUNCTIONAL call sites, from `git grep psxport_sync` in each title (prose in `CLAUDE.md`, `README.md`,
`CMakeLists.txt` comments, `.gitignore`, `psxport.pin`, and probe/refusal message strings also name the
old command and should follow):

| title | `--auto` / `--clone` call sites | pin-tool call sites | port-side imports |
|---|---|---|---|
| `crash` | `.github/workflows/ci.yml:47`, `tools/run.py`, `tools/verify.py`, `tools/verify_executable.py` | `CMakeLists.txt:610` | — (asserts the command: `tests/test_run.py:117`) |
| `ctr` | `.github/workflows/ci.yml:47`, `tools/run.py:258`, `tools/verify.py:16` | `CMakeLists.txt:204,248,278` | — |
| `crashbash` | `.github/workflows/ci.yml:38`, `bootstrap.py:331`, `tools/verify.py:17` | `CMakeLists.txt:402` | — |
| `megamanx4` | `tools/run.py:259` | `CMakeLists.txt:100` | — |
| `spider1` | `.github/workflows/ci.yml:70` (**`--clone`**), `tools/run.py:551` | `CMakeLists.txt:331,357` | — (asserts the CI command: `tests/test_ci_workflow.py:10`) |
| `spyro` | `tools/run.py:220` | `CMakeLists.txt:517` | — (also `tools/verify.py:96` runs `--check`) |
| `tekken3` | `.github/workflows/ci.yml:47`, `tools/run.py:251` | `CMakeLists.txt:332,366` | — |
| `Tomba2Engine` | `.github/workflows/ci.yml:35` (**`--clone`**), `tools/run.py:349` | `CMakeLists.txt:306` | **`tools/verify_ci.py:11` and `tools/gate.py:44` IMPORT `psxport_sync`** — they must import the fetched framework's module (`external/psxport/tools/`), which is the one real dependency-order change here |
| `vagrant` | `tools/launcher/runtime_boundary.py:111`, `tools/verify.py:58` | `CMakeLists.txt:186` | — |
| `toystory2` | `.github/workflows/ci.yml:37` (**`--clone`**), `tools/run.py:298` | `CMakeLists.txt:168` | — (asserts the command: `tools/test_run.py:118,252`) |

`--clone` has no equivalent flag: `psxport_fetch.py`'s single action pins the repo — a detached worktree of
the shared checkout when one exists, a clone at the pin when one does not — which in CI, where there is
no shared checkout, is the clone the CI workflow wanted. Those three workflows lose a flag and keep the
behaviour.

### Consumers with their own pinned dependency

Titles that pin `shared/lightrec` reach it through `psxport_configure_lightrec_dependency()` and need no
call-site change at all: the resolver now probes `<checkout>/scratch/pins/<revision>/` on its own, and
the worktree behind it is created by `python3 tools/psxport_fetch.py --lightrec`, which every port can
run from its own `tools/psxport_fetch.py` (it reads the revision out of the framework's
`cmake/lightrec_dependency.cmake`, so there is no second pin to keep in step). A build that wants to be
strict about never resolving a moving checkout should run that once and treat the plain checkout as the
fallback it now is.


## Amendment 2026-10-01: a stale private clone is advanced, not refused

The fresh-clone path left a private `external/psxport` at the OLD pin after `psxport.pin` moved, and the
retired tool exited 0 silently (Spyro build failed on `cd_stock_read_completion.h`). `psxport_fetch.py`
first refused such a clone; it now advances a CLEAN one to the pin (fetching `origin` when the commit is
absent) and refuses, naming both commits and leaving every byte, when the clone has uncommitted or untracked
changes or the pin is not reachable from its origin. A symlink to a live framework is left alone when no
shared checkout is discoverable. Tests: `tests/test_psxport_fetch.py`, `PrivateCloneTests`.
