# psxport change coordination — read before changing the framework

Three game repos share ONE framework, and there are FOUR checkouts of it — one writable, three not:

    $PSX/psxport                     THE DEV CLONE — the one WRITABLE framework checkout
    spider1/external/psxport         submodule — read-only pinned consumer
    spyro/external/psxport           submodule — read-only pinned consumer
    Tomba2Engine/external/psxport    submodule — read-only pinned consumer

Several agents work at once. Two agents fixing the same framework bug in two different checkouts is
the expensive failure: the duplication is invisible until merge time, and whichever lands second is
thrown away along with its testing.

## The rule: ONE WRITABLE CHECKOUT, and it is `$PSX/psxport`

**Framework edits happen in the dev clone. Never in a game's `external/psxport`** — no edit, no
commit, no push, no merge, no stash there. That directory is `git checkout <pin>` territory, and a
game's `run.sh` re-syncs it to the recorded gitlink on every run, so an edit made there is not just
against the rules, it is *liable to be silently reverted mid-gate*.

The mechanism that makes this workable — a game builds against the dev clone WITHOUT touching its
submodule:

    PSXPORT_DIR=$PSX/psxport ./run.sh            # or: cmake -S . -B build -DPSXPORT_DIR=$PSX/psxport

`PSXPORT_DIR` defaults to `external/psxport`, so a bare clone of a game still builds standalone.
`run.sh` prints which framework checkout the run was built from and whether it was dirty — read that
line before trusting a measurement.

**Two agents needing the framework at once: `git worktree add` off the dev clone** (USER, 2026-08-11 —
separate clones are unnecessary), one worktree per claim area, each agent's `PSXPORT_DIR` pointing at
its own worktree. The claim below decides who owns an AREA; the worktree keeps their FILES apart. Both
are needed — the lock alone does not stop two agents from stepping on one tree.

**Clean up after yourself: no dangling worktrees.** And know the one sharp edge, because it has cut
this workspace before: a worktree shares `.git`, so `refs/stash` and the vendors' repos under
`.git/modules` are common ground between the two worktrees. A worktree stash-pop has already grabbed
another agent's stash here (`Tomba2Engine/CLAUDE.md`). So in a worktree: **do not `git stash`, and do
not move a vendor pin** (`vendor/beetle-psx`, `vendor/lucent`) — leave your work dirty and report it,
which is what this protocol asks for anyway.

**Landing is still the operator's** (only the operator sees the whole tree and the other agents in
flight): commit + push in the dev clone, then bump the three gitlinks — recording each gitlink BEFORE
building or gating that tree. An agent's job ends at a verified change plus a report.

## Before you touch a framework file: CLAIM THE AREA

`mkdir` is atomic on POSIX, so it is the lock. No tool, no daemon, no race:

    mkdir $PSX/coord/claims/<area>

- **It succeeded** → the area is yours. Immediately write `claim.md` inside it (template below).
- **It failed with "File exists"** → someone else owns it. `cat` their `claim.md` and then:
  - their change already covers your need → say so in your report, build on it, claim nothing;
  - you need something different in the same area → **do not edit it anyway**. Record the conflict
    in your report and route around it, or stop and report the dependency. Two divergent edits to
    one framework area is the one outcome this protocol exists to prevent.

`<area>` is the framework concern, not a filename — one claim should cover every file a single fix
touches. Existing vocabulary: `render-queue`, `fmv-decode`, `mdec-dma`, `recomp-emitter`,
`gpu-vk-present`, `cd-path`, `logging-lucent`, `tests-harness`. Invent a new one if none fits; keep
it lowercase-kebab.

### `claim.md` template

```markdown
area: render-queue
agent: <your agent name>
game: Tomba2Engine
opened: <date>
files: runtime/recomp/render_queue.cpp, runtime/recomp/render_queue.h
why: resolveKeyOrder wedges the frame loop at ~f1819 (watchdog backtrace in scratch/logs/…)
shape: <one line on the intended change — enough that another agent can tell if it covers them>
```

Keep `shape:` current. Another agent reads it to decide whether to wait on you.

## When your framework change is ready

Leave the DEV CLONE dirty and report it — do not commit, and do not export a patch file (that workflow
existed because each agent's edits were stranded in its own game tree; with one writable checkout the
operator reads the diff in place):

    git -C $PSX/psxport diff --stat        # in your report
    git -C $PSX/psxport diff              # in your report, in full
    git -C $PSX/psxport status --porcelain # NEW files do not appear in a diff — paste their FULL CONTENTS

Then in `claims/<area>/claim.md` append:

```markdown
status: ready
test: <the exact command that FAILS before the patch and PASSES after>
verified: <what you actually ran, on what data, and the numbers>
```

A framework change with no failing-test-first is not ready — see TDD below.

A framework change with no `claims/<area>/` directory is an edit taken WITHOUT the lock: create the
claim, even retroactively.

A **lucent** change (`$PSX/psxport/vendor/lucent`) is the same shape one level down: it is its own
repo, vendored by psxport, so edit it in place, leave it dirty, report the diff, and let the operator
land it and bump psxport's gitlink for it. `$PSX/coord/patches*/` holds the patch files from the era
when each agent's framework edits were stranded in its own game tree; they are history, not a
workflow — nothing new goes there.

## OPERATOR: RECORD THE GITLINK **BEFORE** YOU BUILD, not after

Bumping a pin by checking the submodule out and building is not safe while anyone else is working in
that repo. `run.sh` calls `scripts/sync-submodules.sh`, which syncs the submodule to the **recorded**
gitlink — so any concurrent agent run silently reverts your checkout, and the build or gate that
follows measures a different framework than you think.

Measured 2026-08-06: Tomba2Engine was checked out onto psxport `9a08efca`, built and gated green — and
the submodule reflog then shows `checkout: moving from 9a08efca to 9890eaa8`, because an agent ran the
game while the gate was in flight. The commit that followed recorded the OLD pin, and only a
`git ls-tree` check caught it.

**Order: `git -C external/psxport checkout <sha>` → `git add external/psxport` → commit → THEN build
and gate.** Once the gitlink is recorded, `sync-submodules.sh` syncs *toward* your pin instead of away
from it. Verify with `git ls-tree HEAD external/psxport` against the submodule's own HEAD; they must
match before you trust any number from that tree.

## OPERATOR OBLIGATION: write the landing sha back into `claim.md`

**When you land a claim, replace its `status:` line with `status: LANDED as psxport <sha>`.** Same
commit, or immediately after. Landings squash several claims into one commit, so nothing else in the
tree records which claim a commit came from — without the write-back that mapping exists only in your
head, and `grep -c 'status: ready' claims/*/claim.md` stops meaning "what is outstanding".

- Landing N claims as one commit: write the SAME sha into all N, each naming the others it was
  squashed with.
- A claim you land NOTHING for (superseded, released, refuted) gets a status saying so, not silence.
- If you cannot map a claim to a commit, write `status: UNKNOWN — could not map to a commit`. An
  honestly unmapped claim is fine; a wrong sha is not.

## THE GPU INTERLOCK IS `gpuguard`, AND ITS CLASSIFIER IS NOT THE SAFETY PROPERTY

The interlock lives at `~/.claude/tools/gpuguard/` with a PreToolUse hook at
`~/.claude/hooks/gpu_guard_hook.py`. Both are MACHINE-LOCAL and are lost on a machine switch, which is
why this note is in the repo: on a fresh machine the interlock does not exist until it is reinstalled,
and a run there is UNGUARDED. The hook fails open by design (a guard that bricks every Bash call the
moment it has a bug is worse than the problem), so its absence is silent.

**A denial is a STOP.** Do not retry, do not reach for `GPUGUARD=off`, do not hand-roll a launch that
dodges the pattern. Diagnose statically and ask the user, whose machine it is.

**But do not mistake the classifier for the guarantee.** Fixed 2026-08-12 after it DENIED a `git commit`:
it searched the whole command string for a binary name, so a commit whose MESSAGE mentioned `psxport` and
`tomba2_port` was classified as a GPU launch. Reading that code found it was wrong in BOTH directions —
`spyro_port`, `spiderman_port`, `control_port` and `ffa` were not in its list at all, and it only examined
one statement, so `cd x && ./scratch/bin/spyro_port` was MISSED. Three of the four PSX ports were never
interlocked. It also had a blanket "the word gpuguard appears" exemption, which let
`gpuguard status && ./scratch/bin/tomba2_port …` through entirely.

It now matches in COMMAND POSITION only (the executable of each statement, after skipping `VAR=val` and
wrappers like `timeout`/`env`/`python3`), strips heredoc bodies as data, and treats `tools/gate.py` and
`tools/gate.sh` as launches because they drive the port binary. **Validate it in both directions before
trusting it** — `python3 ~/.claude/hooks/gpu_guard_hook.py --selftest` runs 20 launch and 14 non-launch
cases; the old matcher scores 6 false positives and 7 false negatives against that suite, which is what
makes the suite a discriminator rather than a decoration.

**Matching in command position is necessary but NOT SUFFICIENT if you cannot FIND the command position.**
The first pass at this fix still missed `for g in a b; do ./bin/spyro_port; done`, `if true; then
./bin/tomba2_port; fi`, `while read l; do …; done`, `time …` and `nohup …` — five real launch shapes, all
unguarded, because the statement began with a shell keyword that is not a wrapper so the classifier
returned the keyword as the executable. Found by PROBING the classifier with the command shapes actually
in use, not by re-reading it. Shell keywords are statement separators now. To prove the classifier reaches the interlock
rather than merely labelling correctly: `gpuguard latch …`, feed the hook a launch and a commit, confirm
DENY/ALLOW, then `gpuguard clear`.

**A build is not a launch.** `cmake --build --target <port>` must stay allowed, or a latched card blocks
all work rather than all runs.

## AGENTS NEVER RUN WINDOWED. USER RULE.

> *"Ideally agents should never do windowed runs and windowed and headless should be equal anyway, it
> shouldn't change anything in the game. headless just means no window and no audio"* — USER, 2026-08-06

**Headless means exactly two things: no window surface, and no audio device. NOTHING else may differ.**
Not pacing, not internal resolution, not the render path, not which frames the game plays. The window
is an output sink, not a mode.

So the answer to "this measurement needs a window" is never *open a window* — it is **fix the
divergence that made you think so.** A windowed agent run also fights the user for their own screen
and CPU (measured 2026-08-06: two `spiderman_port` instances at 23.3% and 90.0% CPU, one of them
timing per-present spacing to decide whether a user-reported flicker was temporal — that number was a
property of the agent schedule, not of the port).

**The known divergences. These are BUGS with addresses, not constraints to design around:**

| where | what it does | why it is wrong |
|---|---|---|
| `gpu_native.cpp:1542` | `if (!gpu_has_window() \|\| cfg_on("PSXPORT_NOPACE")) return;` — headless is never paced | `PSXPORT_NOPACE` is ALREADY the independent switch for "run unpaced". The `!gpu_has_window()` term is redundant with it, and it is what makes every headless timing number describe a program the user never runs. Delete the term. |
| `gpu_vk.cpp:181,252` | headless takes a different size path; ires derives from `win_h()/240.0` | Headless renders ires=1 where a window renders ires=3, so headless captures are not the user's picture. Resolution must be an EXPLICIT input defaulting to what the window would have been — never inferred from the presence of a window. |

**STATUS 2026-08-06 — LANDED as psxport `80e3d203` (claim `pace-parity`).** The rows above are now
HISTORY, kept because they name what every headless measurement taken BEFORE that commit was actually
measuring. The pacing decision lives in `runtime/recomp/pace_plan.h`, which HAS NO WINDOW INPUT —
leg-independence is structural, not asserted — and the resolution decisions live in
`runtime/recomp/video_plan.h`, which takes the presentation SINK (`sink_size()`, headless-default
960x720) instead of `win_w()/win_h()`. `gpu_has_window()` is DELETED: the pace gate was its only
caller. The pace interval is `paceQuota` display FIELDS at the game's own rate via
`gpu_field_rate_millihz()`, not a literal 60.000 Hz.
**Any headless timing number from before `80e3d203` is void** — it described an unpaced program at a
different internal resolution.
**AND THE CONSEQUENCE, which is not a bug: a headless run now runs in REAL TIME.** Every gate and tool
whose intent is "as fast as possible" passes `PSXPORT_NOPACE=1` — that is what the switch is for, and
it is now the only thing that says so. The boot-gate command below carries it.

**SPEED IS ORTHOGONAL TO WINDOWING**, and conflating them is how this got in: someone wanted headless
to be fast and tied *fast* to *no window* rather than to the switch that already existed. If you want
an unpaced run, ask for an unpaced run.

## THE PC OWNS AS MUCH EXECUTION AS FEASIBLE. USER RULE.

> *"PC should own as much execution as feasible to unblock whatever problem there is"* — USER, 2026-08-06

The port drives; the guest is what it has not taken over YET. Not the reverse. Tomba!2 already has
this shape — `native_step_frame` owns the frame and calls into the substrate for what is unported —
and it is the reason that port can be debugged at all.

**"The guest's `main()` never returns, so the hook is unreachable" is not a finding, it is the work.**
That sentence was written about spider1 (claim 025) and treated as a wall to scope around. It is not a
wall: it is a statement that the guest currently owns the loop and the port must take it. When
ownership is what blocks progress, TAKE OWNERSHIP — do not produce another dependency chain and stop.

- **Ownership is the unblocking move.** A problem that cannot be diagnosed because guest code owns the
  path is solved by owning the path, not by instrumenting around it forever.
- **Take the biggest slice you can PROVE.** The byte-exact gate is a quality bar on what you own, never
  a reason to own nothing. "I could not prove all of it" means own the part you can prove and say where
  the seam is — it does not mean leave it all with the guest.
- **This does NOT license a fake.** Owning a path means REIMPLEMENTING it from the RE, readably. It is
  the opposite of stubbing a return value to get past a screen. An honestly unported path that fails
  fast still beats a fabricated one — the no-hacks and no-taps rules are unchanged.
- **Report ownership as a fraction with its denominator:** what the PC now executes vs what the guest
  still does, and which specific thing is next.

## HOUSEKEEPING IS DONE, NOT ASKED ABOUT. USER RULE.

> *"Do not ask for permissions for things like this, do what you need, make this stick"* — USER,
> 2026-08-06, after being asked whether to clear 9.6 GB of orphaned agent clones instead of clearing them.

Deleting orphaned scratch directories, agent clones, stale worktrees and build debris is ordinary
work. Do it and say so in one line afterwards; do not open a question about it.
`.claude/settings.json` allowlists the scoped `rm -rf` and `git worktree` forms so they do not prompt —
if you hit a prompt for a cleanup path that is clearly debris, ADD THE PATTERN there rather than
asking a human to click through it.

**The two checks before deleting, both cheap and mechanical:**
1. is a process still using it (`ps -eo pid,etimes,args`, never `pkill` a shared binary name);
2. does it hold unique work — `git status --porcelain` and `git log origin/main..HEAD` for a clone.
Clean on both → remove it.

**What this does NOT license:** deleting the user's own files, tracked work, or anything
irreplaceable. That is still confirm-first. The rule is about DEBRIS.

## WORKTREES ARE ALLOWED — AND YOU CLEAN UP AFTER YOURSELF. USER RULE.

> *"they can work on worktrees ... just make sure to put rules that state worktrees must be cleaned
> up, I don't want dangling worktrees"* — USER, 2026-08-06

Worktrees are the right tool when several agents need to build the same repo at once without stomping
each other's build directory, or when an A/B needs two binaries from one tree. There is no ban.

**What "dangling" looks like, measured 2026-08-06 before this rule existed:** `git worktree list` in
Tomba2Engine showed FOUR worktrees marked `prunable`, registered against a path that no longer existed
(`~/repo/Tomba2Engine/.claude/worktrees/…` — not even this workspace's copy of the repo), plus **9.6 GB**
of orphaned agent copies at the workspace root (`scratch-beamab`, `scratch-lineclass-ab`,
`scratch-plumeab`, `scratch-shockwave`). Nobody set out to leave those; every one of them was a
finished A/B nobody removed.

**The rules:**

1. **Create it under your repo's own `scratch/`** (git-ignored) or via the harness's worktree support —
   never at the workspace root, never beside the repos. A directory that sits next to `spyro/` and
   `spider1/` reads as a fourth project.
2. **Name it after the WORK, not a hash** — `scratch/wt/shockwave-ab`, not `agent-aa1ae37`. A name
   nobody can attribute is a name nobody dares delete.
3. **REMOVE IT IN THE SAME TASK THAT CREATED IT.** `git worktree remove <path>` (add `--force` if it
   is dirty and you have already reported the diff), then `git worktree prune`. Removing the directory
   alone leaves the registration behind — that is exactly the state found above.
4. **Report it in your final report either way**: the path, and that you removed it. If you
   deliberately left one for the operator, SAY SO and say why — an intentional leftover is fine, a
   silent one is not.
5. **A worktree SHARES `.git`.** `refs/stash` and the submodule repos under `.git/modules` are COMMON
   GROUND: a worktree `stash pop` has already grabbed another agent's stash in this workspace. Do not
   stash in a worktree, and do not assume your submodule checkout is private.
6. **Operator: `git worktree list` in EVERY game repo is part of end-of-session cleanup.** Anything
   `prunable` gets pruned; anything unexplained gets attributed before it is removed.

## LICENSING IS NOT A CONSTRAINT — BUT COPYLEFT STAYS OUT OF `psxport`. USER RULE.

> *"Licensing isn't an issue, you can use whatever license needed"* — USER, 2026-08-12, when asked
> whether the AGPL-3.0 decomp `sozud/mmx4` could be used for Mega Man X4.

So do not deliberate about whether a reference may be used. Vendor it, cite it, take from it.

**The one engineering rule that survives that permission: copyleft-derived code lives INSIDE the
game repo that needs it, and NEVER in `psxport`.** The framework is shared by every port, so an
AGPL file landing there pulls Tomba!2, Spyro, Spider-Man, Vagrant Story and X4 into AGPL with it —
a decision about five titles, made by an edit to one. Per-repo licenses differ on purpose:

| tree | vendored reference | license | what that permits |
|---|---|---|---|
| `spyro/external/open-spyro` | Spyro 1 decomp | CC0-1.0 | code AND ideas, freely, both directions |
| `vagrant/external/rood-reverse` | Vagrant Story decomp | CC0-1.0 | same |
| `megamanx4/external/mmx4` | Mega Man X4 decomp | **AGPL-3.0** | usable, but **repo-local**. Never lift into `psxport` |
| `psxport/vendor/beetle-psx` | GTE/MDEC/SPU/CHD backends | GPL-2-**or-later** | verified 2026-08-12: 121 "any later version" headers, so it can be upgraded to AGPL-3. GPL-2-ONLY would have made the combination undistributable |

A reference whose license is UNVERIFIED is read-only until someone checks it. `mstan/psxrecomp` is
PolyForm Noncommercial — **read and learn only**; take the SHAPE, never the text.
`psxport/docs/prior-art.md` holds the full table and is the place to record the next one.

## BEETLE-PSX IS NEVER A BLOCKER. USER RULE.

> *"Beetle is never a blocker, you can just drop beetle altogether if it causes a problem"* — USER,
> 2026-08-12.

`vendor/beetle-psx` supplies the GTE/MDEC/SPU/CHD backends. Dropping it means porting those native —
which is already this project's stated long-term direction, so this is **permission to accelerate
that, not a new plan.** Never let a beetle constraint (its licence, its behaviour, its build) stop a
decision: state what dropping it would cost, then proceed.

## TDD — the framework change starts with a RED test

psxport currently has **no wired test suite at all**: `tests/` holds two files
(`test_coro.cpp`, `test_leaf.c`) that `CMakeLists.txt` never references. That is why a week of
shared-framework work broke three games without anything catching it. So:

1. **Write the failing test first**, in `psxport/tests/`, and wire it into `ctest`. Run it. It must
   FAIL, and you must paste that failure in your report — a test that was never seen red proves
   nothing about what it covers.
2. Then make it pass with the smallest change that names the actual cause.
3. Prefer a HERMETIC test (no disc image, no GPU): feed the unit its inputs directly. The FMV work
   already shows the shape — `tools/fmv_export/test_fmv_decode.cpp` runs 4/4 without a disc.
4. Where the bug genuinely needs the running game, still add the hermetic test for the unit, and
   cite the game run separately as the integration evidence.

**A negative result must carry its denominator.** "No divergence" means nothing without "compared N
frames, M fields each, and here is the case that WOULD have tripped it". If your test cannot fail,
you have not tested anything.

## THE PICTURE COMES FROM GAME STATE, NEVER FROM WHAT THE GTE PRODUCED. USER RULE.

> *"never do this please NEVER, just leaving the effect as is is better than this"* — USER, 2026-08-04
> *"interpreting GTE isn't good but you can find what submits to GTE and resolve from there instead"* — USER, 2026-08-06

This used to be called "the no-taps rule". That word is retired: it needed case-by-case adjudication
every time it came up, which is the signature of an underspecified rule. It is TWO rules, both
checkable.

### 1. The shipping picture path runs NO guest body

A native producer draws from the game's own state. It does not run a `gen_func_*` body to make
pixels. That is structural and you can check it by reading the call path — no judgement call.

**The mechanical gate that enforces it:** a producer that runs a gen body CANNOT interpolate — you
cannot re-run it under a lerped camera, because it would write guest RAM. So any effect that must
move smoothly at 60 fps has to be a real port by construction.

**READS ARE NOT THE PROBLEM.** A producer reads the node's own fields; that is porting. Diagnostics
read anything they like — `OtAttr`, `PSXPORT_PRIMAT`, `debug objid`/`otattr` exist to ANSWER
QUESTIONS and never produce the picture, and they are explicitly exempt. The line is *produce the
picture* vs *answer a question*, not *touch guest memory*.

### 2. Resolve from what SUBMITS to the GTE, never from what the GTE produced

This is the method, not just the prohibition. When you need a transform:

- **DO** find the code that submits to the GTE — the `SetRotMatrix`/`SetTransMatrix`/RTPS setup site —
  and take its INPUTS. Those are the game's own values, before the hardware touched them.
- **DO NOT** read `gte_read_ctrl()`, the OT, composed GP0 packets, or a guest pre-composed matrix and
  invert them to recover something the game never lost.

Intercepting the guest's own store as it writes its result (the framework's `gte_store_xy` store
hook) is observation at the submission boundary, not inversion. It is allowed.

**WHY, measured rather than asserted.** An effect was reported "vibrating". The cause was reading
`gte_read_ctrl(0..4)` and factoring the camera out of an already-s16-quantised matrix while the
display pass re-composed with the camera. `camᵀ`-then-`cam` is identity only in exact arithmetic, so
the residue was A FUNCTION OF THE CAMERA — 0.13 px with the camera still, 1.53 px with 12/12 sign
alternations while panning. **Nothing in the game makes it vibrate.** All of that motion was
manufactured by the port.

**The test:** could this artefact be traced to code in the GAME that produces it? If not, it is the
port's own mechanism, and the fix belongs at the mechanism — never in compensating arithmetic that
makes the number smaller. Resolve from the submitter and the artefact is not reduced, it is
**structurally impossible**.

### DUSKLIGHT DOES INTERPOLATE MATRICES. WE STILL MAY NOT — and the reason is PRECISION, not depth

`~/repo/dusklight` is the reference for structure and UI (see the workspace `CLAUDE.md`), and its
frame interpolation RECORDS FINAL MATRICES and lerps them. Do not copy that against our substrate,
and do not conclude the rule above is therefore optional. The difference is concrete:

- **Dusklight's matrices are FLOAT values the game itself computed**, in a decomp, before any hardware
  saw them. Lerping them loses nothing.
- **A GTE-side matrix is s16 fixed point.** Recovering a transform from it means inverting a quantised
  value, and the error is camera-dependent — that is the 1.53 px above, not a rounding nit.

**So the technique is fine and the SOURCE is what differs.** Resolve from the submitter — the game's
own pre-quantisation values — and we are in Dusklight's position, not the PSX's. Interpolation is
therefore not banned in principle; it is **gated on the PC owning the code that computes the
transform**, because then the matrix is our own variable and there is nothing to invert.

**A separate PSX fact, often confused with the above:** the PSX has no Z-buffer. RTPS yields screen XY
plus `OTZ`, an *averaged and shifted* Z used only to choose an ordering-table bucket — a bucket index,
not a distance. GameCube has real per-pixel depth. This is an argument for computing depth NATIVELY
from game state (which the framework's per-vertex depth path already does), and it has nothing to do
with the matrix rule. Do not cite "PSX depth is wrong" as a reason for or against interpolation.

### BREAK FIRST, THEN REBUILD

**An UNPORTED effect is better than a WRONGLY-SOURCED one.** Unported is honest — the gap is visible
and someone will fix it. A layer drawn from GTE output looks finished while the substrate is still
doing the projection, so it never gets fixed.

Delete the wrong-sourced producer, let the layer be honestly absent, then build the real one. Never
keep it alive alongside the replacement "to be safe" — its presence hides whether the rebuild works.

**AND BREAK FIRST BEFORE DIAGNOSING, NOT ONLY BEFORE REBUILDING.** USER, 2026-08-05: *"it's impossible
to identify bugs when things still render from tap."* A layer still drawing from the wrong source
makes every bug in it undiagnosable — you cannot tell a broken port from one papering over a bug, and
a plausible picture reads as evidence that the port works. When a render bug is reported in such an
area, delete the producer first and look at what is genuinely there.

## CLOSE A BUG WHEN YOU BELIEVE IT IS SOLVED — AND MAKE THE CLOSE AUDITABLE

> **AMENDED BY THE USER, 2026-08-05:** *"you can close bugs you think are solved, I will reopen if
> not solved"*. This SUPERSEDES the previous absolute rule ("a user-reported bug is closed by the
> user, never by your own measurement"). Reopening is now the correction mechanism, so the cost of a
> wrong close is one round trip — but only if the close SHOWS ITS WORK. Everything below still
> stands, because it is what makes a close cheap to overturn.

**What has NOT changed, and is the whole reason the old rule existed:**

- **Close on a FIX plus evidence, never on a diagnosis.** A root cause you have identified but not
  repaired is still an OPEN bug. Knowing why the palettes are wrong is not the same as the palettes
  being right.
- **State the negative control in the close.** *This same instrument, in this same mode, produced
  the FAILING answer before the change.* A close backed by a measurement that could never have shown
  the bug is worse than leaving it open.
- **Name what you did NOT verify.** The user's reopen is only cheap if they can see at a glance which
  part of their original report your evidence actually covers. "Measured X under Y; did not check Z"
  is a good close. "Fixed" is not.
- **Never close by making the bug invisible to yourself.** The original wording of this section is
  kept below because it names the failure precisely, and the failure is unchanged by the amendment:

> *"Anytime I give you a bug, you make the bug invisible for yourself then claim it is fixed or
> worse, you put something like a TAP on it"* — USER, 2026-08-05

This is the failure this whole document keeps circling, stated plainly. It is not carelessness. The
mechanism is that **we choose the measurement**, and we keep choosing ones that cannot produce the
failing answer — then a green number stands in for the user's observation. A tap is the same move in
code: emit a plausible picture so the layer looks ported.

Three hard rules.

**1. Reproduce the bug in the user's conditions BEFORE fixing anything.** Not a proxy, not a
headless approximation, not "the underlying unit". If they saw it in a window, reproduce it in a
window. If you cannot reproduce it, you do not understand it yet — say so instead of fixing the
thing you *can* see. Measured example: a black intro was "fixed" and verified at 99.95% non-black,
entirely under `PSXPORT_VK_HEADLESS=1` — a mode that SKIPS INTRO FMVS BY CONSTRUCTION
(`native_boot.cpp:612`). The numbers were true and answered a question nobody asked.

**2. State the NEGATIVE CONTROL or do not claim a fix.** Every "fixed" must be accompanied by: *this
same instrument, in this same mode, produced the FAILING answer before the change.* If it never
showed the bug, it cannot show the bug is gone. "0 problems found" from a check that never could
find one is the defect, not the evidence.

**3. Never write "fixed" for a user-reported symptom. Write what you measured, under what
conditions, and hand the verdict back.** They are looking at the running system; we are inferring
from artifacts we generated. Say "measured X under Y — does it look right to you?" The user closes
the bug.

And the tap corollary, since it is the same instinct: if the fix makes the symptom go away without
explaining why it occurred, it is a tap whatever it is made of. See the no-hacks and no-tap sections
below.

## HEADLESS AND WINDOWED ARE ONE CODE PATH. USER RULE, absolute.

> *"Headless and windowed should never be different code paths"* — USER, 2026-08-05

Headless is the same pipeline with a different FINAL SINK — a readback instead of a swapchain
present. It is not a parallel implementation, and nothing before that last step may branch on it.

**Why this is a correctness rule and not a style preference:** almost every measurement in this
project is taken headless (`PSXPORT_SHOT_AT`, frame histograms, gate runs, `preseq`). If the two
paths can differ, then every one of those numbers is a claim about a program the user never runs.
That has already happened: a port measured 99.95% non-black on intro frames headless while the USER,
watching the window, still saw a black screen. The headless evidence was not wrong about headless —
it was answering a different question than the one asked.

So:
- **A divergence between the two IS A BUG**, to be root-caused and removed, never characterised and
  worked around. Do not "fix" it by adding a windowed-only branch.
- **`cfg_on("PSXPORT_VK_HEADLESS")` reached anywhere except the final present/readback step is a
  defect.** `native_boot.cpp:612` already gates intro FMVs on it — that is exactly this bug, and it
  is why a headless run is structurally blind to an FMV defect.
- When a user reports something the headless numbers deny, **believe the user and suspect the
  split** before suspecting their observation.

## DEBUG THE CODE. Navigate by the CODEMAP, not by history. USER RULE.

> *"I don't like git bisecting generally because we should be focusing on debugging rather than
> shuffling through history, and the code map should be pristine so the agent knows where to debug
> from the code map"* — USER, 2026-08-05

**`git bisect` is not the first move, and usually not any move.** It tells you WHICH COMMIT changed
an output. It never tells you WHY, it costs a build per step, and on tapped code it is measuring the
tap rather than the port. Reach for the running system instead: instrument it, watch the state,
read the code that owns the behaviour.

**The codemap is the navigation instrument.** The question "where do I debug this from" is answered
by looking it up — guest address → native owner, subsystem → file, layer → producer — not by
searching history for when it last changed. Every repo has one:

    Tomba2Engine   tools/codemap.py -> docs/code-map.md   (--addr <hex>, --conflicts, --selftest;
                                                           the live native/address counts live in the
                                                           generated doc's header, never in a doc like
                                                           this one — a hand-copied count goes stale
                                                           and this one did: it read "~350" while the
                                                           map held 1009)
    spider1        docs/codemap.md
    spyro          docs/codemap.md

**Which puts an obligation on every agent: leave the codemap PRISTINE.** It is not documentation to
update when convenient — it is the instrument the next debugging session navigates by, so a stale
entry sends someone to the wrong file and a missing one sends them to history, which is the thing
this rule exists to avoid. Update it in the SAME change that moves, adds, deletes or re-owns
anything. Mark a subsystem done only when VERIFIED on real data. If you find an entry that is wrong,
FIX IT — an honestly "missing" or "unsound" entry is far better than a confident wrong one, and a
codemap nobody trusts is a codemap nobody reads.

This is the same discipline that today caught a claim reading `holds` a week after the code it
described was fixed. A registry is only worth consulting if consulting it is reliable.

Corollary for taps: **do not bisect a render regression in tapped code at all.** Delete the tap,
look at what is genuinely there, then port.

## NO HACKS IN THE CODE. None. USER RULE, absolute.

Not "few hacks", not "marked hacks", not "temporary hacks". **None.** A change that makes a symptom
disappear without explaining and fixing why it occurred is not work, it is debt with a commit
message.

**If your change is one of these, stop:** a magic constant or offset that makes output line up;
special-casing the failing input; a swallowed error, `|| true`, retry-until-pass, or sleep-to-fix-a-
race; commenting out or skipping the failing check; hardcoding an expected value; duplicating code
to avoid touching a shared path; anything labelled "for now" or "temporary".

**If the real fix is genuinely too big, SAY SO — do not silently patch.** Name the proper fix, say
what the stopgap risks, and let the operator decide. If a stopgap is approved it is marked
`// STOPGAP: <proper fix> because <why>` AND registered as debt in the project's tracker, with a
death condition. An approved stopgap is a decision; an unmarked one is a lie.

**Register debt where the tracker can see it, or the tracker becomes a liar.** This is not
hypothetical: `re_frontier.py hacks` reported "No hacks tracked. (Good — no-hacks rule holds.)"
while one-slot module pinning — proven unsound, and the reason a port reaches no screen at all —
was shipping. The frontier file described it; nothing carried `status: hack`, so the headline
answer was false. If you leave debt, `--status hack` it. If you remove debt, mark it resolved only
once it is actually gone.

**Deleting a hack means DELETING it**, not disabling it behind a flag and not leaving a comment
saying it used to be there. No tombstones — absence is the cleanest documentation.

## The deliverable is a READABLE PORT, not a fix. RE with Ghidra first.

USER directive, and the single most common failure of past agents on these repos: they optimised for
making a symptom go away and left behind code nobody can read. **A change that makes the symptom
vanish while the surrounding code still reads as guest-memory soup is not finished work.**

### RE FIRST — never black-box debug, never hand-walk disassembly

At any of these triggers — a magic offset, a `sub_XXXX`/`FUN_XXXX` you are about to call, a mystery
`obj[+0xNN]`, a value you cannot name — **STOP and decompile the surrounding function** before
changing anything. Ghidra headless is the default, not the last resort.

    psxport/tools/decomp.sh            Ghidra headless -> C, the entry point for all three games
    psxport/tools/ghidra_decomp.py     the underlying decompile driver
    psxport/tools/abi_extract.py       frame size, spill offsets, ra constants, callee-saved liveness
                                       straight from generated/ — run this BEFORE hand-deriving a frame
    psxport/tools/port_gen.py          byte-faithful first draft of a port
    psxport/tools/port_check.py        static equivalence gate (read its trap below)

    spider1/tools/    ghidra_export.py, ghidra_import.sh, ghidra_query.py, ghidra_seed.py
    Tomba2Engine/tools/  ghidra_overlay.py, ghidra_xrefs.py, disasm_overlay.py
    spyro/tools/      NO Ghidra tooling exists — use psxport's decomp.sh. If you need a Spyro-specific
                      wrapper, BUILD IT and say so; that gap is itself a workflow defect.

`disas.py` / `disasm.py` are single-instruction SPOT-CHECKS after Ghidra. Using them to understand
behaviour, or walking backwards through addresses by hand, is the banned method — it is how five
wrong attributions got made in spider1's RE-16 saga.

### WRAP THE GUEST-MEMORY SOUP

A body full of `c->mem_r32(0x800E7FD8)` and `mem_r8(node + 0xB)` is a transcript, not a port, and it
actively HIDES bugs — you cannot see a state fork that never fires when every read is an opaque hex
address. So when you touch a body:

- typed struct **lenses** over guest blocks (`dlg.state()`, not an offset)
- **named constants** for every literal address, saying what it IS
- **enums** or named constants for state-machine states
- **method names that say what the code DOES**, on real C++ classes — not `ov_*` free functions
- ABI plumbing through `runtime/recomp/guest_abi.h`, not open-coded `r[]` juggling

Byte-exact mechanics STAY byte-exact. This is about how the code READS, not what it does.

Exemplars to match rather than inventing a style: `Tomba2Engine/game/ui/panel_fill.cpp` (converted
from a `port_gen` transcription — named packet layout, decoded attribute bits, a table replacing the
guest's jump table) and `MusicCoord::voiceMixTick`.

### The `port_check` trap — do not let it push you back into a transcription

`port_check.py` compares STATIC STORE SEQUENCES, so a genuine rebuild can FAIL it by construction: a
table replacing an unrolled jump table has fewer store sites. When that happens, **prove equivalence
by RUNNING** — run the repro with the native installed and again with it disabled so the gen body
runs, diff the 2 MB RAM dumps, cite the result. Do NOT contort readable code back into a
transcription to satisfy the checker, and do NOT skip the proof either.

## Diagnostics: `lucent::`, ONE line per call site, NO `if` around it

Two hard rules, both violated by agents already. USER directive.

### 1. New and touched code calls `lucent::`, not `cfg_`

`cfg_*` is a printf-style shim that already forwards 1:1 to lucent; it is being retired. Do not add
new `cfg_logf` / `cfg_logi` / `cfg_dbg` call sites. When you touch a line that has one, convert it.

    cfg_logf("cd",  "sector %u -> %08X", n, dst);   →  lucent::debug("cd",  "sector {} -> {:08X}", n, dst);
    cfg_logi("boot","loaded %s (%d bytes)", p, n);  →  lucent::info ("boot","loaded {} ({} bytes)", p, n);
    cfg_logw / cfg_loge                             →  lucent::warn / lucent::error

Format strings become `std::format`: `%08X`→`{:08X}`, `%-12s`→`{:<12}`, `%zu`/`%llu`/`%u`→`{}`,
`%.2f`→`{:.2f}`, `%%`→`%`. **One trap:** `printf("%s", p)` on a null `const char*` prints `(null)`
under glibc; `std::format` on a null `const char*` is undefined behaviour. Check every `%s`.

### 2. NEVER wrap a log call in a condition

**The logger is configurable — that is the whole point of it.** `lucent::debug` (and `cfg_logf`
before it) is channel-gated internally AND does not evaluate its arguments when the channel is off.
So a guard around one is pure noise that re-creates the `if (dbg) fprintf(...)` idiom the logger
exists to abolish.

    // WRONG — every one of these is redundant
    if (cfg_dbg("cd")) cfg_logf("cd", "sector %u", n);
    if (lucent::channel_on("cd")) lucent::debug("cd", "sector {}", n);

    // RIGHT — one line, no condition
    lucent::debug("cd", "sector {}", n);

Pick by AUDIENCE, not by wrapping: something a normal run should print is `info`/`warn`/`error`
(always emitted); something shown only when asked for is `debug(channel, ...)`.

**The ONE legitimate guard** is around genuinely expensive NON-LOGGING work — building a hex dump,
walking a structure, setting a subsystem's verbose mode. It guards a BLOCK, never a single log call:

    static const lucent::Channel ch{"otattr"};   // interned: load, compare, branch
    if (ch) {                                    // guards the WORK, not the print
      auto snapshot = walk_the_whole_object_table();   // expensive
      lucent::debug(ch, "table: {}", snapshot);
    }

`lucent::Channel` is new (lucent f12c954) and exists for exactly this: on a hot path the gate is
0.49 ns vs 20.5 ns for the string_view form. Use it ONLY where a site is genuinely hot — a dozen
places, not everywhere. Everywhere else, plain `lucent::debug("chan", ...)` with no guard at all.

Rows built in a loop (hex dump, register grid) use `lucent::Line`: accumulate, flush once. Never a
log call per byte.

## Your game's boot gate

Your game must still boot at the end at least as far as it did at the start. Record the before
number, and re-run at the end:

    cd <yourgame> && cmake --build build --target <port> -j$(nproc)   # build EXPLICITLY, never run.sh
    python3 tools/gate.py boot --frames 400 --expect-stage <entry> --expect-sm48 <n>

**NEVER `./run.sh` — that is the USER's play launcher** (USER 2026-08-11), and its submodule re-sync
silently reverts in-progress framework work to the recorded pin under you. A game without a gate tool
grows one; `Tomba2Engine/tools/gate.py` is the reference shape. Assert the ADVANCE past the newgame
prologue and the END STATE, never an absolute end frame — `newgame` takes a variable number of frames to
reach the prologue, so an absolute number is a hardcoded expected value that fails for unrelated reasons.

`PSXPORT_NOPACE=1` because the gate measures boot PROGRESS in a wall-clock budget, and a headless run
is paced now (see the divergence table above). Without it the gate measures ~60 presents per second
instead of several hundred, and every recorded before/after number becomes incomparable.

Going backwards is a regression even if your own test is green — that is exactly how last week's
damage got in.
