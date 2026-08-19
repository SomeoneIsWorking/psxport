# psxport change coordination — read before changing the framework

**Unlabeled content is machine convention, revisable by any session. USER lines are verbatim dated
quotes, and only those.** Each rule's measurement lives in `docs/findings/workspace-incidents.md` — this
file keeps the rule, that file keeps the evidence. How a game *consumes* the framework (build, CVars,
seam, `generated/`, RE tooling, diagnostics) is `psxport/CLAUDE.md`; this file is about several agents
changing one framework at once.

Seven game repos share ONE framework, and there are seven checkouts of it — one writable, six not:

    $PSX/psxport             THE DEV CLONE — the one WRITABLE framework checkout
    <game>/external/psxport  submodule — read-only pinned consumer, in every game repo

Two agents fixing the same framework bug in two checkouts is the expensive failure: invisible until merge
time, and whichever lands second is thrown away with its testing.

## The rule: ONE WRITABLE CHECKOUT, and it is `$PSX/psxport`

**Framework edits happen in the dev clone. Never in a game's `external/psxport`** — no edit, no commit,
no push, no merge, no stash. That directory is `git checkout <pin>` territory, and a game's `run.sh`
re-syncs it to the recorded gitlink on every run, so an edit made there is *liable to be silently
reverted mid-gate*. Build a game against the dev clone instead: `-DPSXPORT_DIR=$PSX/psxport`.

**Two agents needing the framework at once: `git worktree add` off the dev clone**, one worktree per claim
area, that agent's `PSXPORT_DIR` pointing at it — separate clones are unnecessary (2026-08-11). The claim
decides who owns an AREA; the worktree keeps their FILES apart. Both are needed.

**A worktree shares `.git`, so `refs/stash` and the vendors under `.git/modules` are COMMON GROUND.** A
worktree stash-pop has already grabbed another agent's work here. In a worktree: **do not `git stash`, and
do not move a vendor pin** (`vendor/beetle-psx`, `vendor/lucent`).

**Landing is the operator's** — only the operator sees the whole tree and the other agents in flight. An
agent's job ends at a verified change plus a report: no `git add`, no commit, no push.

## Before you touch a framework file: CLAIM THE AREA

`mkdir` is atomic on POSIX, so it is the lock. No tool, no daemon, no race:

    mkdir $PSX/coord/claims/<area>

- **Succeeded** → the area is yours. Immediately write `claim.md` inside it.
- **"File exists"** → someone else owns it. `cat` their `claim.md`: if their change covers your need, say
  so in your report and build on it; if you need something different in the same area, **do not edit it
  anyway** — record the conflict and route around it, or stop and report the dependency. Two divergent
  edits to one framework area is the outcome this protocol exists to prevent.

`<area>` is the framework concern, not a filename — one claim covers every file a single fix touches.
Vocabulary in use: `render-queue`, `fmv-decode`, `mdec-dma`, `recomp-emitter`, `gpu-vk-present`,
`cd-path`, `logging-lucent`, `tests-harness`. Invent a new one if none fits; lowercase-kebab.

```markdown
area: render-queue          # + agent, game, opened: <date>
files: runtime/recomp/render_queue.cpp, runtime/recomp/render_queue.h
why: resolveKeyOrder wedges the frame loop at ~f1819 (watchdog backtrace in scratch/logs/…)
shape: <one line on the intended change — enough that another agent can tell if it covers them>
status: ready               # when done, plus:
test: <the exact command that FAILS before the patch and PASSES after>
verified: <what you actually ran, on what data, and the numbers>
```

Keep `shape:` current — another agent reads it to decide whether to wait on you. An edit taken without a
claim gets one created retroactively. A change with no failing-test-first is not ready (TDD below).

Report by leaving the DEV CLONE dirty: `git -C $PSX/psxport diff --stat`, the full `diff`, and
`status --porcelain` — **NEW files do not appear in a diff, so paste their FULL CONTENTS.** No patch files.
A **lucent** change (`vendor/lucent`) is the same shape one level down: edit in place, leave dirty, report,
and let the operator land it and bump psxport's gitlink.

## OPERATOR: RECORD THE GITLINK **BEFORE** YOU BUILD, not after

`run.sh` calls `scripts/sync-submodules.sh`, which syncs the submodule to the **recorded** gitlink — so a
concurrent agent run silently reverts an un-recorded checkout and everything measured afterwards describes
a different framework (measured 2026-08-06). **Order: `git -C external/psxport checkout <sha>` →
`git add external/psxport` → commit → THEN build and gate.** Verify with
`git ls-tree HEAD external/psxport` against the submodule's own HEAD before trusting any number.

**And write the landing sha back:** replace the claim's `status:` with `status: LANDED as psxport <sha>`.
Landings squash several claims into one commit, so nothing else records which claim a commit came from and
`grep -c 'status: ready' claims/*/claim.md` stops meaning "what is outstanding". N claims in one commit get
the SAME sha, each naming the others. A claim you land nothing for gets a status saying so, not silence.
Unmappable → `status: UNKNOWN — could not map to a commit`; honestly unmapped is fine, a wrong sha is not.

## AGENTS NEVER RUN WINDOWED, AND HEADLESS IS ONE CODE PATH. USER RULE, absolute.

> *"Ideally agents should never do windowed runs and windowed and headless should be equal anyway, it
> shouldn't change anything in the game. headless just means no window and no audio"* — USER, 2026-08-06

> *"Headless and windowed should never be different code paths"* — USER, 2026-08-05

**Headless means exactly two things: no window surface, no audio device. NOTHING else may differ** — not
pacing, not internal resolution, not the render path, not which frames the game plays. Same pipeline,
different FINAL SINK (readback instead of present); nothing before that step may branch on it. This is
correctness, not style: almost every measurement here is taken headless, so if the paths can differ, every
one of those numbers is a claim about a program the user never runs.

- **A divergence IS A BUG** with an address, to be root-caused — never worked around with a windowed-only
  branch. Two such bugs landed as `80e3d203`; any headless timing number from before it is void.
- **`cfg_on("PSXPORT_VK_HEADLESS")` reached anywhere but the final present/readback is a defect.**
  `native_boot.cpp:612` gates intro FMVs on it, which is how a black intro got "verified" 99.95% non-black.
- **When a user reports what the headless numbers deny, believe the user and suspect the split.**
- **Speed is orthogonal to windowing.** A headless run is PACED now; ask for unpaced with `PSXPORT_NOPACE=1`.

So the answer to "this measurement needs a window" is never *open a window* — it is **fix the divergence
that made you think so.**

## THE PC OWNS AS MUCH EXECUTION AS FEASIBLE. USER RULE.

> *"PC should own as much execution as feasible to unblock whatever problem there is"* — USER, 2026-08-06

The port drives; the guest is what it has not taken over YET. Not the reverse.

- **Ownership is the unblocking move.** A path that cannot be diagnosed because guest code owns it is
  solved by owning it, not by instrumenting around it forever. "The guest's `main()` never returns, so the
  hook is unreachable" is not a finding, it is the work.
- **Take the biggest slice you can PROVE.** The byte-exact gate is a quality bar on what you own, never a
  reason to own nothing: own the part you can prove and name the seam.
- **This does NOT license a fake.** Owning a path means REIMPLEMENTING it from the RE, readably — the
  opposite of stubbing a return value to get past a screen.
- **Report ownership as a fraction with its denominator**, and name what is next.

## HOUSEKEEPING IS DONE, NOT ASKED ABOUT. USER RULE.

> *"Do not ask for permissions for things like this, do what you need, make this stick"* — USER,
> 2026-08-06, after being asked whether to clear 9.6 GB of orphaned agent clones instead of clearing them.

Deleting orphaned scratch dirs, agent clones, stale worktrees and build debris is ordinary work. Do it and
say so in one line. If a clearly-debris path hits a permission prompt, ADD THE PATTERN to
`.claude/settings.json` rather than asking a human to click through it. **Two checks first:** is a process
using it (`ps -eo pid,etimes,args` — never `pkill` a shared binary name), and does it hold unique work
(`git status --porcelain`, `git log origin/main..HEAD`). Clean on both → remove. **Not licensed:** the
user's own files, tracked work, anything irreplaceable. The rule is about DEBRIS.

## WORKTREES ARE ALLOWED — AND YOU CLEAN UP AFTER YOURSELF. USER RULE.

> *"they can work on worktrees ... just make sure to put rules that state worktrees must be cleaned
> up, I don't want dangling worktrees"* — USER, 2026-08-06

1. **Under your repo's own git-ignored `scratch/`**, never at the workspace root — a directory beside
   `spyro/` and `spider1/` reads as another project.
2. **Named after the WORK, not a hash** (`scratch/wt/shockwave-ab`, not `agent-aa1ae37`): a name nobody
   can attribute is a name nobody dares delete.
3. **REMOVED IN THE SAME TASK THAT CREATED IT** — `git worktree remove <path>` (`--force` if dirty and you
   have already reported the diff) THEN `git worktree prune`. Removing the directory alone leaves the
   registration behind, which is the state 9.6 GB of debris was found in.
4. **Reported either way**: the path, and that you removed it. A deliberate leftover is fine if you say so
   and why; a silent one is not. Operator: `git worktree list` per repo is end-of-session cleanup.

## LICENSING IS NOT A CONSTRAINT — BUT COPYLEFT STAYS OUT OF `psxport`. USER RULE.

> *"Licensing isn't an issue, you can use whatever license needed"* — USER, 2026-08-12, when asked
> whether the AGPL-3.0 decomp `sozud/mmx4` could be used for Mega Man X4.

So do not deliberate about whether a reference may be used. Vendor it, cite it, take from it. **The one
engineering rule that survives that permission: copyleft-derived code lives INSIDE the game repo that
needs it, NEVER in `psxport`** — the framework is shared, so an AGPL file landing there is a decision about
seven titles made by an edit to one.

| tree | vendored reference | license | what that permits |
|---|---|---|---|
| `spyro/external/open-spyro`, `spyro/external/spyro-1`, `vagrant/external/rood-reverse` | Spyro 1 / Vagrant Story decomps | CC0-1.0 | code AND ideas, freely, both directions |
| `psxport/external/psycross` | Psy-Q SDK reimplementation (libgte/gpu/spu/cd + GTE/PGXP-Z) | **MIT** | usable and liftable; reference-only here, never built |
| `megamanx4/external/mmx4` | Mega Man X4 decomp | **AGPL-3.0** | usable, but **repo-local**. Never lift into `psxport` |
| `psxport/vendor/beetle-psx` | GTE/MDEC/SPU/CHD backends | GPL-2-**or-later** | verified 2026-08-12: 121 "any later version" headers, so upgradable to AGPL-3. GPL-2-ONLY would have made the combination undistributable |

A reference whose license is UNVERIFIED is read-only until someone checks it. `mstan/psxrecomp` is
PolyForm Noncommercial — **read and learn only**, take the SHAPE never the text. `docs/prior-art.md` holds
the full table and is where the next one goes.

## BEETLE-PSX IS NEVER A BLOCKER. USER RULE.

> *"Beetle is never a blocker, you can just drop beetle altogether if it causes a problem"* — USER,
> 2026-08-12.

Dropping `vendor/beetle-psx` means porting the GTE/MDEC/SPU/CHD backends native — already this project's
stated direction, so this is **permission to accelerate that, not a new plan.** Never let a beetle
constraint (licence, behaviour, build) stop a decision: state what dropping it would cost, then proceed.

## THE SHIPPED VALUE MUST BE COMPARED TO THE MEASURED ONE — BY CODE, NOT BY A HUMAN'S EYES

An RE step measures values with a tool; the tool keeps its own copy and asserts the binary matches it; the
port keeps a SECOND copy (`constexpr kFoo = 0x…` in `game_config.cpp`); **nothing compares the two.** The
tool verifies itself, the `static_assert`s verify internal consistency, both gates pass, and the value that
actually ships can be anything. Found INDEPENDENTLY in four of five projects, each proven by breaking the
gate and requiring RED (2026-08-12; table in findings).

**THE RULE. A measured constant that ships in code must be checked, by something that runs, against the
measurement it came from.** Either the tool PARSES the shipping file and diffs it against what it measures
from the binary (`verify_crt0.py --check` should read `game_config.cpp`'s constants and diff them against
what it reads out of the binary), or the shipping file is GENERATED from the measurement with the
generator in the gate — then the two copies cannot drift because there is only one. A
`static_assert` over the constants' internal RELATIONS is worth having and is NOT this: `hi - lo ==
0x46B20` holds just as well when both are wrong. **And the selftest must exercise the SHIPPING path, not a
pure helper beside it** — twice in one session the tested code and the shipping code were different code.

## WRITE THE TEST FOR THE CASE THAT WOULD FAIL — do not break the tree to prove it could

> USER, 2026-08-12: *"this sabotage thing is too excessive, I'd rather verify that things work rather
> than break them and see they are broken"*

**So: DO NOT edit the live tree to make a gate go red.** That ritual is retired. It was tried heavily on
2026-08-12 and its own cost showed up the same day, twice, in the way that matters most — **broken code
left behind in the tree**. An agent died mid-break and left `tools/crt0_extract.cpp`'s PS-X EXE magic
check as `if (false)`, which would have shipped a tool that accepts a headerless blob and prints a boot
group of garbage in its normal confident format, into six repos' `game_config.cpp`. Hours later a
break-and-restore loop hit a command timeout before its restore ran and left `producer_census.h`
sabotaged. Neither was caught by a gate; both were caught by hand, one of them only because a checklist
had been written down.

**What survives is the QUESTION, not the ritual.** The thing worth keeping is:

- **Nothing a check prints is evidence until it COULD have said otherwise.** "0 problems found" from a
  check that could not have found one is the defect, not the evidence.
- **So enumerate the case that WOULD fail, and write it as a POSITIVE test.** Not "break the code and
  watch"— *add the input that must produce a failure, and assert the failure.* A test whose fixture is a
  foreign build id, an underflowed heap size, a headerless image, a claim earned by a different build, is
  permanent, runs on every build, needs no restore, and cannot leave debris. Every hole that breaking
  things found this session was fixed by adding exactly such a test; the break was only how the missing
  case got noticed, and noticing is cheaper by reading the check and asking what input it cannot see.
- **If you genuinely must confirm a gate can fail at all, do it on a COPY** — a scratch clone, never the
  live tree — and say so in your report.
- **The per-FIX half still holds and is not a ritual:** before claiming a fix, your instrument must have
  actually produced the failing answer FOR THIS BUG. Not a commissioning record from last month — those
  answer a different question. The incident: a fix closed at "99.95% non-black" measured entirely under a
  mode that SKIPS INTRO FMVS BY CONSTRUCTION (findings). The check was sound and still could not have come
  back negative for that bug. Reproducing the bug with your own instrument IS verifying that things work.

**And the selftest must exercise the SHIPPING path, not a pure helper beside it** — twice in one session
the tested code and the shipping code were different code.

## TDD — the framework change starts with a RED test

1. **Write the failing test first** in `psxport/tests/` (globbed — no shared file to edit), wire it into
   `ctest`, run it. It must FAIL and you must paste that failure.
2. Then make it pass with the smallest change that names the actual cause.
3. Prefer HERMETIC (no disc, no window): feed the unit its inputs.
   `tools/fmv_export/test_fmv_decode.cpp` runs 4/4 without a disc.
4. If the bug genuinely needs the running game, still add the hermetic unit test and cite the game run
   separately as integration evidence.

**A negative result must carry its denominator.** "No divergence" means nothing without "compared N frames,
M fields each, and here is the case that WOULD have tripped it".

## THE PICTURE COMES FROM GAME STATE, NEVER FROM WHAT THE GTE PRODUCED. USER RULE.

> *"never do this please NEVER, just leaving the effect as is is better than this"* — USER, 2026-08-04
> *"interpreting GTE isn't good but you can find what submits to GTE and resolve from there instead"* — USER, 2026-08-06

"The no-taps rule" is retired as a term — it needed case-by-case adjudication every time, the signature of
an underspecified rule. It is TWO checkable rules.

**1. The shipping picture path runs NO guest body.** A native producer draws from the game's own state; it
does not run a `gen_func_*` body to make pixels. Check it by reading the call path. The mechanical gate: a
producer running a gen body CANNOT interpolate (re-running it under a lerped camera would write guest RAM),
so anything that must move smoothly at 60 fps is a real port by construction. **READS ARE NOT THE
PROBLEM** — a producer reads the node's own fields, and diagnostics (`OtAttr`, `PSXPORT_PRIMAT`,
`debug objid`/`otattr`) read anything they like. The line is *produce the picture* vs *answer a question*.

**2. Resolve from what SUBMITS to the GTE, never from what it produced.** DO find the
`SetRotMatrix`/`SetTransMatrix`/RTPS setup site and take its INPUTS — the game's own values, before the
hardware touched them. DO NOT read `gte_read_ctrl()`, the OT, composed GP0 packets, or a guest pre-composed
matrix and invert them to recover something the game never lost. Intercepting the guest's own store as it
writes its result (`gte_store_xy`) is observation at the submission boundary, not inversion, and is allowed.

**The test:** could this artefact be traced to code in the GAME that produces it? If not it is the port's
own mechanism, and the fix belongs at the mechanism — never in compensating arithmetic that makes the
number smaller. A reported "vibration" was measured 100% manufactured by the port and camera-dependent at
0.13–1.53 px (findings, which also holds why Dusklight may lerp matrices and we may not: its are FLOATs the
game computed, ours are s16, so interpolation is **gated on the PC owning the code that COMPUTES the
transform**, not banned in principle).

**BREAK FIRST, THEN REBUILD.** Delete the wrong-sourced producer, let the layer be honestly absent, then
build the real one; never keep a WRONGLY-SOURCED producer alive alongside the replacement "to be safe". A
layer drawn from GTE output looks finished while the substrate still does the projection, so it never gets
fixed. And break first BEFORE DIAGNOSING too — USER, 2026-08-05: *"it's impossible to identify bugs when
things still render from tap."* USER, 2026-08-19, on why this rule is not softening: agents are **too
conservative** about tearing something down and rebuilding it, so the default stays tear-it-down.

**A GUEST-GEOMETRY FALLBACK WAS TRIED ON 2026-08-19 AND IS BANNED. Do not rebuild it.** The USER
briefly lifted the rule to allow one — "draw whatever has no native producer from the guest's OT" — on the
reasonable grounds that fixing missing graphics one object at a time does not scale. It was built,
measured, and reverted the same session. The reason it cannot work is structural, not a bug that could be
fixed with more care:

> **OT/GP0 content is POST-PROJECTION 2D at the guest's own 320x240.** The guest has already thrown away
> world position and depth by the time a packet exists. So a re-emitted prim cannot be re-projected for a
> wide frame, cannot join the depth buffer, and cannot be interpolated at 60fps.

What that looked like on screen, measured: with the engine wide (draw clip 0..693), the native pass drew
the world at 694 px and the re-emitted guest prims drew the SAME world again at 4:3 coordinates — the
scene rendered TWICE, side by side, in the user's window. An earlier revision with a weaker
already-drawn test re-emitted 832 of 1034 OT nodes and buried the player behind duplicate terrain.
Replaying GP0 words also drags the CPU rasterizer along with it (110 fps -> 25 fps), and USER,
2026-08-19: *"no CPU raster ever"*.

USER, 2026-08-19, on seeing it: *"This is probably why I banned GTE before"*. It is.

**What replaces it — the same ambition, from the other end of the pipe:** a GENERIC producer driven by the
inputs the guest itself starts from (an object's model/geomblk plus its own position and rotation), not by
what the guest's GTE produced. That is game state, so widescreen, the depth buffer and interpolation all
still apply, and it is global in the way the fallback was trying to be: one producer covering every object
that has no specific one, rather than one producer per object.

Unchanged, and it is the rule the whole episode illustrates: a native producer draws from GAME STATE, never from GTE output.

## SAY WHAT YOU ARE DOING, IN SHORT SENTENCES, WHILE YOU DO IT

> USER, 2026-08-19: *"you need to inform me of what you are doing occasionally in short sentences"* —
> said after a long stretch of tool work during which the USER could not tell what was being worked on.

A long silent run of tool calls is a defect in itself: the USER is the operator, they are often looking at
the same running game, and they cannot redirect work they cannot see. So surface a one- or two-line update
whenever the work changes shape — a new lead, a measurement that came back, a leg that started, a plan that
changed. Short. Not a report, not a plan, not a summary of what is about to happen — what is happening.

There is no rule anywhere in this workspace against narrating work, and none may be added.

## SAY WHAT YOU ARE DOING, IN SHORT SENTENCES, WHILE YOU DO IT

> USER, 2026-08-19: *"you need to inform me of what you are doing occasionally in short sentences"* —
> after a long stretch of tool work during which the USER could not tell what was being worked on.

A long silent run of tool calls is a defect in itself: the USER is the operator, they are often watching the
same running game, and they cannot redirect work they cannot see. Surface a line whenever the work changes
shape — a lead found, a measurement back, a background leg started. Short, present tense, what IS happening;
not a plan and not a summary of what is about to happen.

There is no rule in this workspace against narrating work, and none may be added.

## CLOSE A BUG WHEN YOU BELIEVE IT IS SOLVED — AND MAKE THE CLOSE AUDITABLE

> *"you can close bugs you think are solved, I will reopen if not solved"* — USER, 2026-08-05

Reopening is the correction mechanism, so a wrong close costs one round trip — but only if it SHOWS ITS WORK.

> *"Anytime I give you a bug, you make the bug invisible for yourself then claim it is fixed or
> worse, you put something like a TAP on it"* — USER, 2026-08-05

That is the failure this whole document circles, and it is not carelessness: **we choose the measurement**,
and we keep choosing ones that cannot produce the failing answer — then a green number stands in for the
user's observation. A tap is the same move in code.

1. **Reproduce in the user's conditions BEFORE fixing anything** — not a proxy, not a headless
   approximation, not "the underlying unit". If you cannot reproduce it you do not understand it yet.
2. **The instrument must have produced the FAILING answer FOR THIS BUG, BEFORE THIS CHANGE, or you have
   no fix claim** — "WRITE THE TEST FOR THE CASE THAT WOULD FAIL" above. Reproducing the bug with your own
   instrument is the requirement; a gate's history answers a different question.
3. **Close on a FIX plus evidence, never on a diagnosis**, and **name what you did NOT verify** — the
   reopen is only cheap if the user can see which part of their report your evidence covers. "Measured X
   under Y; did not check Z" is a good close. "Fixed" is not.

If a fix makes the symptom go away without explaining why it occurred, it is a tap whatever it is made of.

## DEBUG THE CODE. Navigate by the CODEMAP, not by history. USER RULE.

> *"I don't like git bisecting generally because we should be focusing on debugging rather than
> shuffling through history, and the code map should be pristine so the agent knows where to debug
> from the code map"* — USER, 2026-08-05

**`git bisect` is not the first move, and usually not any move.** It says WHICH COMMIT changed an output,
never WHY, it costs a build per step, and on tapped code it measures the tap. Instrument the running system
instead. Do not bisect a render regression in tapped code at all — delete the tap, look at what is genuinely
there, then port.

**The codemap is the navigation instrument** (`Tomba2Engine tools/codemap.py` → `docs/code-map.md`;
`spider1`/`spyro` → `docs/codemap.md`): guest address → native owner, subsystem → file, layer → producer.
**Leave it PRISTINE:** update it in the SAME change that moves, adds, deletes or re-owns anything; mark a
subsystem done only when VERIFIED on real data; if an entry is wrong, FIX IT. Live counts belong in the
generated doc's own header, never hand-copied into prose. A stale entry sends someone to the wrong file; a
missing one sends them to history.

## NO HACKS IN THE CODE. None. Absolute.

Not "few hacks", not "marked hacks", not "temporary hacks". **None.** A change that makes a symptom
disappear without explaining and fixing why it occurred is debt with a commit message.

**If your change is one of these, stop:** a magic constant or offset that makes output line up;
special-casing the failing input; a swallowed error, `|| true`, retry-until-pass, sleep-to-fix-a-race;
commenting out or skipping the failing check; hardcoding an expected value; duplicating code to avoid
touching a shared path; anything labelled "for now" or "temporary".

**If the real fix is genuinely too big, SAY SO — do not silently patch.** Name the proper fix, say what the
stopgap risks, let the operator decide. An approved stopgap is marked `// STOPGAP: <proper fix> because
<why>` AND registered as debt with a death condition; an unmarked one is a lie. **Register debt where the
tracker can see it** (`--status hack`) or the tracker becomes a liar — `re_frontier.py hacks` has already
reported "No hacks tracked" while a hack proven unsound was shipping. **Deleting a hack means DELETING
it**, not disabling it behind a flag and not leaving a comment saying it used to be there. No tombstones.

## RE FIRST, and the deliverable is a READABLE PORT

**A change that makes the symptom vanish while the surrounding code still reads as guest-memory soup is
not finished work.** At any of these triggers — a magic offset, a `sub_XXXX`/`FUN_XXXX` you are about to
call, a mystery `obj[+0xNN]`, a value you cannot name — **STOP and decompile the surrounding function.**
The shared tool inventory (`decomp.sh` — over `ghidra_decomp.py`, the underlying decompile driver —
`abi_extract.py`, `port_gen.py`, `port_check.py`) and the `port_check` static-store trap are in
`psxport/CLAUDE.md`. What is only here:

- **Per-repo tooling:** `spider1/tools/` (`ghidra_export.py`, `ghidra_import.sh`, `ghidra_query.py`,
  `ghidra_seed.py`), `Tomba2Engine/tools/` (`ghidra_overlay.py`, `ghidra_xrefs.py`, `disasm_overlay.py`),
  and `spyro/tools/` has NONE — use psxport's `decomp.sh`, and if you need a Spyro-specific wrapper BUILD
  IT and say so, because that gap is itself a workflow defect.
- **`disas.py` / `disasm.py` are single-instruction SPOT-CHECKS after Ghidra.** Using them to understand
  behaviour, or walking addresses backwards by hand, is the banned method — it produced five wrong
  attributions in spider1's RE-16 saga.
- **WRAP THE GUEST-MEMORY SOUP.** A body full of `c->mem_r32(0x800E7FD8)` and `mem_r8(node + 0xB)` is a
  transcript, not a port, and it HIDES bugs — you cannot see a state fork that never fires when every read
  is an opaque hex address. When you touch a body: typed struct **lenses** over guest blocks
  (`dlg.state()`, not an offset); **named constants** for every literal address, saying what it IS; enums
  for state-machine states; **method names that say what the code DOES** on real C++ classes, not `ov_*`
  free functions; ABI plumbing through `runtime/recomp/guest_abi.h`, not open-coded `r[]` juggling.
  Byte-exact mechanics STAY byte-exact — this is about how the code READS. Exemplars to match rather than
  inventing a style: `Tomba2Engine/game/ui/panel_fill.cpp` and `MusicCoord::voiceMixTick`.

## Diagnostics and the boot gate — two pointers and the two things agents get wrong

Diagnostics rules are in `psxport/CLAUDE.md`. The two agents keep violating: new and touched code calls
`lucent::` not `cfg_*`, and **never wrap a log call in a condition** — it is channel-gated internally and
does not evaluate its arguments when off, so a guard re-creates the `if (dbg) fprintf(...)` idiom the
logger exists to abolish. The one legitimate guard is around expensive NON-LOGGING work, guarding a BLOCK.

Your game must still boot at least as far as it did at the start: record the before number and re-run at
the end. Going backwards is a regression even if your own test is green.

    cd <yourgame> && cmake --build build --target <port> -j$(nproc)   # build EXPLICITLY, never run.sh
    PSXPORT_NOPACE=1 python3 tools/gate.py boot --frames 400 --expect-stage <entry> --expect-sm48 <n>

**NEVER `./run.sh` — that is the USER's play launcher** (USER 2026-08-11, quoted in `psxport/CLAUDE.md`),
and its submodule re-sync silently reverts in-progress framework work to the recorded pin under you.
`PSXPORT_NOPACE=1` because a headless run is paced now, and without it the gate measures ~60 presents per
second instead of several hundred, making every before/after number incomparable. Assert the ADVANCE past
the newgame prologue and the END STATE, never an absolute end frame.
