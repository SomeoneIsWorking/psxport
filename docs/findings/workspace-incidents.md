# FINDINGS: the workspace incidents the protocol rules were written from

`docs/workspace/PROTOCOL.md` keeps the RULE; this file keeps the MEASUREMENT under it. A rule whose
evidence is gone becomes folklore, so nothing here may be deleted without checking who relies on it —
but nothing here needs to be loaded into every session either. Each entry names what would falsify it.

---

## 2026-08-06 — a concurrent agent run silently reverted a submodule checkout mid-gate

Tomba2Engine was checked out onto psxport `9a08efca`, built and gated green — and the submodule reflog
then shows `checkout: moving from 9a08efca to 9890eaa8`, because an agent ran the game while the gate was
in flight. `run.sh` calls `scripts/sync-submodules.sh`, which syncs the submodule to the **recorded**
gitlink, so the sync ran *away* from the un-recorded checkout. The commit that followed recorded the OLD
pin, and only a `git ls-tree` check caught it.

**Rule it produced:** record the gitlink BEFORE you build (PROTOCOL, operator section).
**Falsifier:** `sync-submodules.sh` no longer syncing toward the recorded pin, or `run.sh` no longer
calling it.

## 2026-08-11 — `sync-submodules.sh` certified pins it never checked (FIXED 2026-08-13)

`git submodule status --recursive` aborts on beetle-psx's URL-less nested `deps/lightning/gnulib`, so it
never reached `vendor/lucent`; the script's `|| true` swallowed the non-zero exit and it printed "all at
recorded gitlinks" over a partial enumeration. It cost three broken builds in one day: `ot_attr.h` needs
vendored lucent >= `07c5836` and the stale `02ea34b` checkout failed to compile every time.

**THE CAUSE WAS NOT `|| true` — the check HAD NO DENOMINATOR**, so a short enumeration was
indistinguishable from a complete one. That is this workspace's recurring failure class, and it was
living inside the script written to prevent it.

**FIXED, verified 2026-08-13:** the script enumerates gitlinks directly (`ls-files -s` filtered to mode
160000) rather than trusting git's recursive walk, prints "checked N of M submodule(s)", and NAMES what
it cannot cover (the URL-less nested path git itself cannot sync). All seven trees carry it, md5
`535fd152dba6…`, and `tests/test_sync_submodules.cpp` passes. **Falsifier:** the script reporting "all
at recorded gitlinks" without an N-of-M count, or an N less than M presented as clean.

**Re-confirmed the same day by the same abort in a different tool:** creating the dev clone with
`git clone --recurse-submodules` died on that exact path *after* cloning beetle and *before* checking out
`vendor/lucent`, leaving lucent's worktree empty with every file staged deleted. Recursive submodule
operations on this tree do not fail loudly — they stop early. `scripts/bootstrap-workspace.sh` therefore
inits vendors ONE AT A TIME and non-recursively.

**Falsifier:** the gnulib entry gaining a URL, or the script enumerating without `--recursive`.

## 2026-08-06 — 9.6 GB of orphaned worktrees and agent clones, all from finished A/Bs

`git worktree list` in Tomba2Engine showed FOUR worktrees marked `prunable`, registered against a path
that no longer existed (`~/repo/Tomba2Engine/.claude/worktrees/…` — not even this workspace's copy of the
repo), plus **9.6 GB** of orphaned agent copies at the workspace root (`scratch-beamab`,
`scratch-lineclass-ab`, `scratch-plumeab`, `scratch-shockwave`). Nobody set out to leave those; every one
was a finished A/B nobody removed. A directory sitting next to `spyro/` and `spider1/` reads as a fourth
project, and a name nobody can attribute (`agent-aa1ae37`) is a name nobody dares delete.

**Rules it produced:** worktrees live under the repo's own `scratch/`, named after the WORK; removed in
the same task; `git worktree remove` THEN `git worktree prune`, because removing the directory alone
leaves the registration behind — which is exactly the state found here. Operator: `git worktree list` in
every game repo is end-of-session cleanup.

 Related, same shared-`.git` mechanism: a worktree `stash pop` has already grabbed another agent's stash
 in this workspace (`Tomba2Engine/CLAUDE.md`). `refs/stash` and `.git/modules` are common ground.

## 2026-08-06 — headless was unpaced and rendered at a different internal resolution (LANDED `80e3d203`)

Two divergences, both bugs with addresses rather than constraints:

| where | what it did | why it was wrong |
|---|---|---|
| `gpu_native.cpp:1542` | `if (!gpu_has_window() \|\| cfg_on("PSXPORT_NOPACE")) return;` — headless was never paced | `PSXPORT_NOPACE` was ALREADY the independent switch for "run unpaced". The `!gpu_has_window()` term was redundant with it, and it is what made every headless timing number describe a program the user never runs |
| `gpu_vk.cpp:181,252` | headless took a different size path; ires derived from `win_h()/240.0` | headless rendered ires=1 where a window rendered ires=3, so headless captures were not the user's picture |

Also measured that day: two `spiderman_port` instances at 23.3% and 90.0% CPU, one of them timing
per-present spacing to decide whether a user-reported flicker was temporal — that number was a property of
the agent schedule, not of the port.

**Landed as psxport `80e3d203` (claim `pace-parity`).** The pacing decision lives in
`runtime/recomp/pace_plan.h`, which HAS NO WINDOW INPUT — leg-independence is structural, not asserted —
and the resolution decisions live in `runtime/recomp/video_plan.h`, taking the presentation SINK
(`sink_size()`, headless-default 960x720) instead of `win_w()/win_h()`. `gpu_has_window()` is DELETED: the
pace gate was its only caller. The pace interval is `paceQuota` display FIELDS at the game's own rate via
`gpu_field_rate_millihz()`, not a literal 60.000 Hz.

**Any headless timing number from before `80e3d203` is void** — it described an unpaced program at a
different internal resolution. **And the consequence, which is not a bug: a headless run now runs in REAL
TIME.** Every gate and tool whose intent is "as fast as possible" passes `PSXPORT_NOPACE=1`.

**The generalisation:** speed is orthogonal to windowing, and conflating them is how this got in — someone
wanted headless to be fast and tied *fast* to *no window* rather than to the switch that already existed.

## 2026-08-05/06 — a "fixed" black intro, verified by a mode that skips intro FMVs

A black intro was "fixed" and verified at 99.95% non-black, entirely under `PSXPORT_VK_HEADLESS=1` — a
mode that SKIPS INTRO FMVS BY CONSTRUCTION (`native_boot.cpp:612`). The numbers were true and answered a
question nobody asked; the USER, watching the window, still saw a black screen.

**Rules it produced:** reproduce in the user's conditions before fixing; write the test for the case
that would fail;
`cfg_on("PSXPORT_VK_HEADLESS")` reached anywhere except the final present/readback step is a defect.

## 2026-08-04/06 — the "vibrating" effect was manufactured entirely by the port

An effect was reported vibrating. The cause was reading `gte_read_ctrl(0..4)` and factoring the camera out
of an already-s16-quantised matrix while the display pass re-composed with the camera. `camᵀ`-then-`cam`
is identity only in exact arithmetic, so the residue was A FUNCTION OF THE CAMERA — **0.13 px with the
camera still, 1.53 px with 12/12 sign alternations while panning.** Nothing in the game makes it vibrate.

**Rule it produced:** resolve from what SUBMITS to the GTE, never from what the GTE produced. Source-level
frame interpolation can lerp FLOAT matrices the game itself computed before any hardware saw them; a
GTE-side matrix is s16 fixed point, so recovering a transform from it means inverting a quantised value and
the error is camera-dependent — that 1.53 px, not a rounding nit. The technique is fine; the SOURCE is what
differs, so interpolation is gated on the PC owning the code that COMPUTES the transform.

**A separate PSX fact, often confused with the above:** the PSX has no Z-buffer. RTPS yields screen XY plus
`OTZ`, an *averaged and shifted* Z used only to choose an ordering-table bucket — a bucket index, not a
distance. That is an argument for computing depth NATIVELY from game state (the framework's per-vertex
depth path already does), and it has nothing to do with the matrix rule. Do not cite "PSX depth is wrong"
as a reason for or against interpolation.

## 2026-08-12 — the shipped-value gate defect, found INDEPENDENTLY in four of five projects

The shape: an RE step measures values with a tool. The tool keeps its own copy (`FIXTURE_EXPECT`,
`X4_EXPECT`) and asserts the binary still matches it. The port keeps a SECOND copy (`constexpr kFoo = 0x…`
in `game_config.cpp`). **Nothing compares the two copies.** The tool verifies itself, the `static_assert`s
verify internal consistency, both gates pass — and the value that actually ships can be anything.
Verified in each repo by breaking the gate and requiring RED, not argued:

| repo | the break | both gates said |
|---|---|---|
| megamanx4 | `kCrt0GameMain` → `0x80999999`, `kCrt0LibcInit` → `0xDEADBEEF` | PASS |
| vagrant | `kHeapSizePtr` +4, `kLibcInit` → a real nop instruction | PASS |
| psxport | deleted the ORACLE/SBS suppression from `enh_gate()` entirely | selftest PASS, "0 disagreement(s)" |
| Tomba2Engine | replaced `_ref_time_from_git()` with a 1970 constant | selftest PASS while the tool's whole answer INVERTED |

The psxport row is the one to feel: with the suppression gone, a `PSXPORT_ORACLE` run enables every
enhancement, so the byte-compare oracle is contaminated — and the shipping selftest still certified it.

**And the selftest must exercise the SHIPPING path.** psxport's `selftest()` looped `compare_run_from()`
— the pure predicate — and never called `enh_gate()`, the function the game actually reaches. Tomba2's
injected its reference string, so the novel half had zero coverage. In both cases the tested code and the
shipping code were different code.

## 2026-08-12 — what BREAKING a gate found that 15 green classes never did

Evidence for "WRITE THE TEST FOR THE CASE THAT WOULD FAIL", and for why coverage is owed **per class**
rather than per tool. Read it as a record of what was MISSING from a test suite — each row below became a
new positive test — not as an argument for breaking a live tree, which USER 2026-08-12 retired after it
left broken code behind twice in one day.

Tomba2Engine `tools/producers.py` had 15 green selftest classes. Two real holes survived all of them, each
surfaced only by breaking the shipping path and requiring RED:

| the break | the tool said | what it hid |
|---|---|---|
| deleted `if fw > stamp: stamp = fw` — the framework half of the staleness reference | 0 FAILs | the real reference moved 9 hours |
| narrowed `>=` to `>` on the reference boundary | green | contradicted the tool's OWN printed contract |

Both are now gated (17 classes) and both re-proven RED.

psxport's crt0 audit (726d10c9) needed **six** breaks, each proven RED and then restored with md5 identity:
dropping an undeclared-bias refusal; storing a heap size through an unset pointer; not setting `a1`;
accepting an underflowed heap size; an audit not refusing a confirmed disagreement; an audit certifying
while resolving nothing. No single green run covers any of these — the count IS the finding: a class is
commissioned only by its own break.

**Falsifier:** the class counts above are LIVE — `python3 Tomba2Engine/tools/producers.py stale --selftest`
prints its own class count, and this section is falsified the moment that number disagrees with the 15->17
stated here. Re-read it from the tool rather than trusting this line; the sibling incident three sections
down ("a stale hand-copied count in a doc") is what this warning is for. Falsified also if a break listed
above stops turning its gate RED, which would mean the gate regressed after commissioning.

**The RESTORE half has already burned us.** A restore proof citing an md5 plus `git diff --numstat`
described a file ONE LINE different from the one that shipped — a leftover debug probe added after the
proof was taken. So the proof must be taken against the SHIPPING file AFTER the last edit, or it certifies
a program nobody ran.

## 2026-08-11 — a tool's headline answer was false because debt was never registered

`re_frontier.py hacks` reported "No hacks tracked. (Good — no-hacks rule holds.)" while one-slot module
pinning — proven unsound, and the reason a port reached no screen at all — was shipping. The frontier file
described it; nothing carried `status: hack`, so the headline answer was false.

Same class, different tool: `re_frontier.py`'s green-over-nothing bug was fixed FOUR times across THREE
diverged copies (890 / 443 / skill lines), and two of those copies were still reporting "OK" over a
zero-entry parse on 2026-08-11 — one of them the copy a CLAUDE.md tells you to run. Divergence also
produced the `RE_FRONTIER_ROADMAP` trap (spider1 needs `RE_FRONTIER_ROADMAP=docs/re-frontier.md` or the
tool silently parses an empty roadmap and reports OK) and the `codemap.md` vs `code-map.md` split.

**Decision it produced (2026-08-11): the tooling hoist happens ADDITIVELY.** The generic tool ENGINES
(`info.py`, `catalog.py`, `re_frontier.py`, `codemap.py`, `whatis.py`, `go_public.py`, …) move into
`psxport/tools/port/`; the DATA (`docs/info/`, `docs/issues/`, codemaps, roadmaps) stays per-game, and
every game already vendors psxport so the engines reach them all. Additively because a flag day is
unacceptable mid-job: psxport gains the engine as a new file, each game's existing tool keeps working
untouched, and a game switches to a 3-line shim one tool at a time when someone is already in that repo.
No window exists in which a repo has no working tool, and each step is independently revertible. Unify
`codemap.md`/`code-map.md` naming in the same pass as whichever repo switches that tool. Progress:
`tools/port/README.md`.

## FALSIFIED 2026-08-12 — "psxport has no wired test suite at all"

PROTOCOL.md asserted, as the justification for its TDD section, that *"psxport currently has no wired test
suite at all: `tests/` holds two files (`test_coro.cpp`, `test_leaf.c`) that `CMakeLists.txt` never
references. That is why a week of shared-framework work broke three games without anything catching it."*

**The first half no longer holds.** `tests/` holds 30+ `test_*.cpp` files, globbed by
`tests/CMakeLists.txt`, which the root `CMakeLists.txt` reaches via `add_subdirectory(tests)` at line 36 —
so `ctest --test-dir build` runs them. The claim was deleted from PROTOCOL rather than carried forward,
because a rule justified by a condition that has since been fixed reads as current fact. The TDD rule
itself is unchanged and stands on its own: a test never seen red proves nothing.

The second half — a week of shared-framework work broke three games with nothing catching it — is history
and is why the suite exists.

## 2026-08-05 — a stale hand-copied count in a doc

A doc claimed the Tomba2Engine codemap held "~350" native/address entries while the generated map held
**1009**. Live counts belong in the generated doc's own header, never hand-copied into a prose file. Same
session caught a claim reading `holds` a week after the code it described was fixed.

## Ownership: "the guest's `main()` never returns" is not a finding

Written about spider1 (claim 025) and treated as a wall to scope around. It is not a wall — it is a
statement that the guest currently owns the loop and the port must take it. Tomba!2 already has the right
shape (`native_step_frame` owns the frame and calls into the substrate for what is unported), and that is
why that port can be debugged at all.

## Method: `disas.py` by hand is the banned method, with a body count

Walking backwards through addresses by hand, or using the single-instruction spot-checkers to understand
behaviour rather than to confirm Ghidra, is how **five wrong attributions** got made in spider1's RE-16
saga.

## 2026-08-12 — the GLOBAL instruction file has no history, and a prune of it is therefore unauditable

`~/.claude/CLAUDE.md` is in no repository. When the instruction corpus was pruned this day, every other
file's before-image came from `git diff`; for this one the only record was a scratch copy the pruning agent
itself produced (`scratch/leanpass/global-CLAUDE.before.md`) — an image the auditor could not
independently verify, that was already an intermediate rather than the true original (it lacks the
`Get help when you are stuck` heading the original had), and that is gitignored so a scratch cleanup
destroys it.

**What that cost, measured.** The prune deleted from the global file, with NO relocation target because
the file is in no repo: the STANDING AUTHORIZATION permitting any agent at any depth to spawn a workflow
or call Fable without asking (plus the Fable/Opus/Sonnet difficulty tiers and `brief it fully`), and the
whole CLAIMS/INSTRUMENTS discipline (`--expires-on` and "a claim with no falsifier is a belief"; "can the
tool be trusted to show the OTHER answer"). `grep -ciE "instrument|claim|falsifi|expires-on"` on the
pruned file returned **0**. Both clusters were restored by hand the same day from a session-start
snapshot — which existed only by luck, in one conversation's context.

**The rule this yields:** a file with no history may be APPENDED to freely, but a deletion pass over it
needs a before-image outside the deleting agent's own control, captured before the pass starts. The
durable fix is to make the file tracked (a dotfiles repo, symlinked into `~/.claude/` the way
`$PSX/CLAUDE.md` is symlinked into `psxport/docs/workspace/WORKSPACE.md`); until that exists, treat any
prune of it as unauditable and prefer relocation-then-delete in two separate steps.
