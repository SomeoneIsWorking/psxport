# `~/repo/psx` — the PSX-port WORKSPACE

**This file is a MAP AND A POINTER, nothing else**, and it lives in the psxport repo
(`docs/workspace/WORKSPACE.md`) so it survives a machine switch. `~/repo/psx/CLAUDE.md` is a SYMLINK
to it — the workspace directory itself is NOT a git repo and holds nothing durable.

| read this | for |
|---|---|
| **`psxport/CLAUDE.md`** | **how a game consumes the framework** — build/run, the CVar ladder, the seam, `generated/` rules, RE-first, diagnostics, registries, Dusklight. THE authority |
| **`psxport/docs/workspace/PROTOCOL.md`** | the multi-agent protocol (area claims) + the standing USER rules (no windowed agent runs, PC owns execution, housekeeping, worktrees) |
| `psxport/docs/workspace/LAYOUT.md` | the target directory organization for every tree |
| `psxport/docs/plans/*.md` | designs not yet implemented, each stating what is measured vs assumed |
| `<game>/CLAUDE.md` | that game's own specifics — the authority for that repo |

Everything above is TRACKED, in the psxport repo — so it survives a machine switch, reaches every game
tree through that repo's `external/psxport` submodule, and reaches every subagent by `grep`.

## What is here

`~/repo/psx` is a plain **directory** holding six independent repos side by side. There is no
workspace repo and no superproject: a game must build from a bare clone of itself, a gitlink at this
level would churn on every game commit, and a recursive clone would pull four copies of psxport +
beetle-psx.

| path | what it is |
|---|---|
| `psxport/` | **the framework DEV CLONE — the one writable framework checkout** · `github.com/SomeoneIsWorking/psxport`. Also the home of every doc listed above |
| `spyro/` | Spyro the Dragon (`SCUS_942.28`) port · `github.com/SomeoneIsWorking/spyro` |
| `spider1/` | Spider-Man (`SLUS_008.75`, USA) port · `github.com/SomeoneIsWorking/spider1` |
| `Tomba2Engine/` | Tomba! 2 port — psxport's reference consumer · `github.com/SomeoneIsWorking/Tomba2Engine` |
| `vagrant/` | Vagrant Story (`SLUS_010.40`, USA) port · created 2026-08-12, **local only, no remote yet**. Vendors the CC0 `rood-reverse` decomp. Defining fact: the boot exe is ~15% of the code, 933,925 B lives in `.PRG` overlays |
| `megamanx4/` | Mega Man X4 (`SLUS_005.61`, USA) — the ONLY **enhancement-class** port here: already 60fps, so no native producers, no lerp, no native depth. Wants widescreen + load removal + drop-in co-op. Vendors the AGPL-3.0 `mmx4` decomp, which may NOT be lifted into `psxport` |
| `coord/` | **UNTRACKED, machine-local, EPHEMERAL ONLY**: `claims/` (the area locks — a lock coordinates the agents running on THIS machine, so it must not be a tracked file), plus historical `patches*/` and agent scratch. Nothing durable belongs here: a finding goes in the psxport repo or a game repo, never here |

`$PSX` in any doc means this workspace root. To reproduce the workspace on a fresh machine:

```sh
git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
~/repo/psx/psxport/scripts/bootstrap-workspace.sh   # clones the games, inits vendors, relinks CLAUDE.md
```

## The structure rule: ONE writable framework checkout

Each game vendors the framework at `external/psxport` (a submodule, which itself nests
`vendor/beetle-psx` and `vendor/lucent`). **Four checkouts of one framework exist; exactly one is
writable.**

1. **Framework edits happen ONLY in `psxport/`.** A game's `external/psxport` is a read-only pinned
   consumer — `git checkout <pin>` territory. Never edit, commit, push or merge in there.
2. **Build a game against in-progress framework work** without touching its submodule:
   `PSXPORT_DIR=$PSX/psxport ./run.sh` (or `-DPSXPORT_DIR=...` to cmake). It defaults to the
   submodule, so each game repo still builds standalone from a bare clone — that is what keeps "each
   game is its own project using psxport as the framework" true rather than aspirational. `run.sh`
   announces which checkout it built from, and whether that checkout was dirty.
3. **Parallel framework work: `git worktree` off `psxport/`, one per claim area**, and point that
   agent's `PSXPORT_DIR` at its worktree. Claims (`PROTOCOL.md`) decide who owns an area; the worktree
   keeps two agents' files apart. Clean them up — no dangling worktrees. One sharp edge: a worktree
   shares `.git`, so inside one, do not `git stash` and do not move a vendor pin (`PROTOCOL.md` has
   the incident).
4. **Landing:** commit + push in `psxport/`, then bump each game's gitlink — and record the gitlink
   BEFORE building or gating that tree (`PROTOCOL.md` says why: `sync-submodules.sh` syncs
   toward the RECORDED pin, so an un-recorded checkout gets silently reverted under you).

## The two things to know even if you read nothing else

- **Never commit** disc images (`*.chd`), extracted executables, `generated/`, or machine-specific
  absolute paths (`/home/<user>/…`). Every game repo ships `tools/go_public.py` to audit history.
- **Never write run artifacts to `/tmp`** — small RAM-backed tmpfs here. Use the repo's git-ignored
  `scratch/`, split by kind. Diagnose "disk quota exceeded" with `quota -s`, not `df`.

## Known workspace defects

- **`sync-submodules.sh` certifies pins it never checked, and NO FIXED COPY EXISTS.**
  `git submodule status --recursive` aborts on beetle-psx's URL-less nested `deps/lightning/gnulib`,
  so it never reaches `vendor/lucent`; the script's `|| true` swallows the non-zero exit and it prints
  "all at recorded gitlinks" over a partial enumeration. Write-up:
  `KNOWN-DEFECT-sync-submodules.md`, next to this file. Verify with
  `md5sum $PSX/coord/st.sh */external/psxport/scripts/sync-submodules.sh` before trusting any claim that
  this is fixed. **Re-confirmed 2026-08-11 by the same abort in a different tool:** creating the dev
  clone with `git clone --recurse-submodules` died on that exact path *after* cloning beetle and
  *before* checking out `vendor/lucent`, which left lucent's worktree empty with every file staged
  deleted. Recursive submodule operations on this tree do not fail loudly — they stop early.

## THE SIMILARITY METRIC IS RECALIBRATED (2026-08-12) — usable again, with a stated resolution limit

**`tools/exe_similarity.py` was DISTRUSTED earlier the same day and has now been fixed, remeasured over
all 14 executables, and given a `--selftest` that gates BOTH classes.** The old headline defect — a NULL
pair (`CRASHBASH ↔ TOMBA2` 33.4%) outranking the margin of the same-studio `CRASH2 ↔ CRASH3` (35.2%) — is
gone: that null pair now reads **11.9%** against family pairs at **47–58%**. Reproduce with
`python3 tools/exe_similarity.py --dir <exes> --selftest` (exit 0 = both classes pass; exit 1 = a
regression; exit 2 = it refused and says what it did NOT scan).

**What changed, each fix justified by a measurement over the pairs that could falsify it:**
1. **Jaccard** `|A∩B|/|A∪B|` replaces `|A∩B|/min(|A|,|B|)`, so no pair can be inflated by one binary being
   small. Tomba! 2's SCUS loader read 76.8 / 47.6 / 37.6 / 32.6 / 32.2% against MAIN / Tomba! 1 / Bash / CTR
   / Spider-Man; it now reads 8.6 / 3.6 / 14.7 / 6.9 / 5.9%. Every cell still prints the OLD number in
   parentheses, so the re-baseline is auditable rather than silent.
2. **A code/data filter**: a 16-word window is compared only if all 16 words are legal R3000A instructions
   AND it holds ≥4 distinct normalised tokens. Both parts are needed — validity alone still admits
   `CRASHBASH 0x80068BD4` (a branch-offset table: 16/16 legal words, 1–2 distinct tokens), diversity alone
   still admits `0x80069644` (an address table: median 0/16 legal words). The thresholds are measured, not
   chosen: over **5215 windows from five hand-disassembled code regions**, every window has 16/16 legal
   words and none has fewer than 4 distinct tokens, while the confirmed data sits at ≤2 — the default lands
   in that gap and costs **zero** verified code windows.
3. **A real NULL DISTRIBUTION replaces the hand-picked "12.5% SDK ceiling"**, and it is **stratified by
   PSY-Q version**, because the null turned out bimodal (the `$Id: sys.c,v` string is in each binary):

   | cross-studio null, 13 engine binaries | n | mean | median | MAX (named) |
   |---|---|---|---|---|
   | pooled | 67 | 3.91% | 3.53% | **11.89% CRASHBASH ↔ TOMBA2_MAIN** |
   | same PSY-Q `sys.c` | 25 | 5.96% | 5.61% | 11.89% CRASHBASH ↔ TOMBA2_MAIN |
   | different PSY-Q | 42 | 2.69% | 2.40% | 7.31% CRASH3 ↔ TOMBA1 |
   | incl. the TOMBA2 loader control | 78 | 4.17% | 3.74% | 14.68% CRASHBASH ↔ TOMBA2_SCUS |

   The residual 11.89% was **disassembled**: the two largest shared runs (`0x80049808`, 1744 windows;
   `0x8002E878`, 1283) are real non-table code driving SPU/CD registers — identical statically-linked
   PSY-Q 1.140. So the SDK floor is a GENUINE confound that tracks the SDK version, not an artifact.
4. **`--selftest` runs both classes and fails if either regresses**, including an assertion that the filter
   FIRES (both disassembly-confirmed data ranges kept 0 windows WITH the filter, 267 and 2745 WITHOUT).
   Shown failing on purpose: `--selftest --no-filter` exits 1 with 4 FAILs. The negatives are chosen to be
   non-vacuous — a cross-studio pair cannot fail "≤ the null max" it helps define, so those pairs are
   asserted against the POSITIVE class instead, while the within-studio and loader negatives are asserted
   against the null.
5. **REJECTED and recorded:** "define the SDK from the corpus" (drop windows seen in ≥K studios) looks much
   better (SPIDER1/2 12.3×, SPYRO2/3 9.3×) but is CIRCULAR — it removes by construction what cross-studio
   pairs share, so at K=2 the null is 0.00% for all 67 pairs and can no longer falsify anything. That is the
   distrusted version's own error in a new costume. Kept as `--sdk-filter K`, audit only, prints the warning.

**THE CORRECTED MATRIX (Jaccard; old asymmetric number in parentheses), and the verdict bands anchored on
the controls: >3× stratum null max = one codebase · 1.5–3× = same architecture rewritten · ≤1.5× =
indistinguishable from two strangers linking the same SDK.**

| pair | Jaccard | × stratum null max | verdict | (old) |
|---|---|---|---|---|
| SPIDER1 ↔ SPIDER2 | **57.5%** | 4.8× | one codebase | 74.2% |
| SPYRO2 ↔ SPYRO3 | **47.0%** | 4.0× | one codebase | 64.2% |
| CRASH2 ↔ CRASH3 | 19.8% | 2.7× | same architecture, rewritten | 35.2% |
| CRASH1 ↔ CRASH2 | 8.2% | 1.1× | at the SDK floor | 18.1% |
| TOMBA2_SCUS ↔ TOMBA2_MAIN (same GAME) | 8.6% | 0.7× | loader shares only SDK with its own engine | 76.8% |
| SPYRO1 ↔ SPYRO2 | 5.6% | 0.8× | below the floor — rewritten | 11.2% |
| CRASH1 ↔ CRASH3 | 4.6% | 0.6× | below the floor | 11.6% |
| TOMBA1 ↔ TOMBA2_MAIN | 3.9% | 0.5× | below the floor — different engines | 18.8% |
| CRASH3 ↔ CTR | 3.0% | 0.4× | CTR is not the trio's engine | 8.4% |

**THE RESOLUTION LIMIT, which is the one thing to carry forward.** The strongest DIRECT evidence in this
corpus — the GOOL dispatch loop byte-identical across Crash 1/2/3, and a trio engine function matching
**36/36** windows in Crash 2 and 3 against **0/36** in CTR, Bash, Spyro 2, Spider-Man, Tomba! 1 and 2 —
corresponds to only **2.7×** for `CRASH2 ↔ CRASH3` and **1.1×** for `CRASH1 ↔ CRASH2`. Whole-binary
similarity is a WEAK instrument for "same architecture, rewritten", because a rewrite keeps the mechanism
and replaces the code. **Where a targeted function-level match WITH A NEGATIVE CONTROL SET disagrees with
this metric, the targeted match wins.** The metric ranks candidates and REFUTES false families; it does not
certify one. Direct evidence still decides repo shape.

**AN INDEPENDENT SECOND TOOL AGREES ON BAND MEMBERSHIP AND ON THE EXTREMES — NOT ON MID-TABLE ORDER.**
`tools/lineage_probe.py` (built the same day, sharing no code with this one: whole FUNCTIONS segmented at
`jr $ra`, plus exclusive-string overlap, counted absolutely and ranked by corpus SPREAD) agrees on the
TOP TWO pairs (SPIDER1|SPIDER2 strongest at 246 units = 16.4× its own measured null max, SPYRO2|SPYRO3
next at 59 = 3.9×) and on the SET of pairs that fall below each tool's own floor (TOMBA1|TOMBA2,
CRASHBASH|TOMBA2, the TOMBA2 loader/engine pair). It **DISAGREES on mid-table rank in two cells, both
listed** — an earlier version of this paragraph claimed "the same ordering", which its own data
contradicts:

| pair | lineage_probe | exe_similarity | |
|---|---|---|---|
| SPYRO1 ↔ SPYRO2 | 27 units = **2.25×** | 5.6% = **0.8×** (below floor) | inverted vs CRASH1↔CRASH2 |
| CRASH1 ↔ CRASH2 | 25 units = **2.08×** | 8.2% = **1.1×** | inverted vs SPYRO1↔SPYRO2 |
| TOMBA2_SCUS ↔ TOMBA2_MAIN | 1 unit = **0.07×**, bottom | 8.6% = 0.7×, *above* SPYRO1↔SPYRO2 there | recorded nowhere before |

State the agreement as **set-membership per band, never as a total order.** CRASH2|CRASH3 is 4.75× (not
the 3.8× recorded earlier: that divided by the pooled null max instead of the pair's own PSY-Q stratum,
which is the stratification the sibling tool was forced into and this one had not been).

**The two tools are independent in FEATURES and CODE but NOT in CALIBRATION INPUT.** They share the same
19-file corpus and the same hand-written studio attribution defining "cross-studio null". A wrong
developer attribution, or a contaminated corpus member, moves a pair in BOTH tools at once and both
report a stronger family signal — **their agreement cannot detect an attribution or corpus error.** Both
now print their own null (n, mean, median, zero count, named max, stratified by PSY-Q `sys.c`) and
`lineage_probe.py` prints a per-file sha256 fingerprint, so a published count names the corpus it came
from.

**`lineage_probe.py`'s multiples are partly a measurement of its own gate constants**, whose thresholds
are asserted rather than derived from disassembled ground truth. Its `--sweep` (measured 2026-08-12)
moves separation from 1.75× to 6.00× across a defensible grid, and at `min_insns=40` CRASH2|CRASH3
OVERTAKES SPYRO2|SPYRO3 — the mid-table order is not robust. Robust across every cell: SPIDER1|SPIDER2
is the top pair by 3–8×, and TOMBA1|TOMBA2_MAIN sits at 0–1. **Cite it for band membership and the
ordering of the extremes; never for a mid-table decimal.** Its same-code upper anchors (a title vs its
own demo build: 216 and 40) straddle the family pairs, which is the same caveat from the other side.

**DOES EACH RECORDED DECISION SURVIVE THE CORRECTED METRIC? (2026-08-12)** — every one does; two get
stronger reasons, three are re-labelled from "measured" to "preference", and none is reversed.

| decision | verdict | why, under the corrected metric |
|---|---|---|
| Spider-Man 1 + 2 in one repo | **SURVIVES, strengthened** | 57.5% = 4.8× the null max, the strongest pair in the corpus; both binaries are engine-sized (186,879 / 196,095 words), so the small-denominator defect cannot be inflating it |
| Spyro 2 + 3 in one repo | **SURVIVES, strengthened** | 47.0% = 4.0× the null max |
| Spyro 1 in the Insomniac repo | **SURVIVES as PREFERENCE, not measurement** | 5.6% = 0.8× — BELOW the null max, so this is evidence of NOT sharing engine code. Asset pipeline and format knowledge only |
| Tomba! 1 is its own engine | **SURVIVES as NOT-ONE-CODEBASE — and *not* as "strengthened"** | 3.9% = 0.5× and 1 whole function = 0.08×: both tools DETECT NOTHING. That is not evidence of separate engines. **The in-corpus counterexample: CRASH1↔CRASH3 reads 4.6% (0.6×) and 12 units (1.00×), at or below both floors, while WORKSPACE.md itself holds the Crash trio to be ONE architecture on direct evidence.** So "below the floor" means "neither tool can see it", and neither tool can distinguish "rewritten, same architecture" from "unrelated". What the measurement *does* support: a SHARED CODEBASE would read in the hundreds (SPIDER1↔SPIDER2 = 246 units, 57.5%), and Tomba! 1↔2 does not — so no shared `game/`. Deleted from this row as false: "strengthened", and "below two random titles" (1 unit is above ~57% of the 109 cross-studio pairs, whose median is 0). Different PSY-Q (`sys.c` 1.129/`intr.c` 1.74 vs 1.140/1.75) still agrees. FALSIFIER: a targeted function-level match with a negative control set, which this file already says beats an aggregate metric |
| the Crash trio as ONE architecture | **SURVIVES on DIRECT evidence, which this metric cannot resolve** | 19.8% = 2.7× for 2↔3 lands in the "rewritten" band, but CRASH1↔CRASH2 is 1.1× and CRASH1↔CRASH3 0.6×. The byte-identical GOOL loop and the 36/36-vs-0/36 function match are what the decision rests on — believe them over the aggregate |
| CTR and Crash Bash separate | **SURVIVES** | CTR ↔ the trio is 0.6–3.0% (0.1–0.4×) and Bash ↔ the trio 1.3–6.4%, both at or below the floor, matching 0/36 on the trio function |
| Toy Story 2 standalone | **SURVIVES** | it is cross-studio with all 12, so every cell is a NULL member by construction; its largest is 8.2% with Tomba! 1 = 0.7× the same-PSY-Q null max, and those two link the SAME PSY-Q 1.129 — SDK, not engine. Next is CRASH2 at 6.2% |

## DECIDED: how the workspace grows to the Spyro/Crash titles (2026-08-11)

The USER delegated this call ("I'm not gonna decide, maybe Fable should decide" → then "it's yours"), so
it is decided here rather than left open. Evidence is the engine-lineage measurement in
`tools/exe_similarity.py` (recalibrated 2026-08-12 — the section above holds the corrected numbers and the
per-decision survival check) plus `docs/plans/game-seam-redesign.md`. Every percentage in this section is
the CORRECTED Jaccard figure with its multiple of the measured cross-studio null maximum; a bare percentage
means nothing on its own.

**One repo per ENGINE LINEAGE, multiple titles inside it. No third vendored layer. Nothing speculative.**

| tree | covers | when it is created |
|---|---|---|
| `psxport/` | the framework, and the ONLY framework | exists |
| `spyro/` | the Insomniac lineage — Spyro 1, 2, 3 as `titles/<t>/` with shared `game/` | exists; converts to multi-title WHEN Spyro 2 work actually starts |
| `spider1/` | the Neversoft lineage — Spider-Man (`SLUS_008.75`) + **Spider-Man 2: Enter Electro** (USER, 2026-08-12: "can be part of spider-man") | exists; converts to multi-title WHEN Enter Electro work actually starts |
| `Tomba2Engine/` | the Whoopee Camp lineage — Tomba! 2; **Tomba! 1** may live here by PREFERENCE only (0.5× / 0.08× — measured NOT to share a CODEBASE; "not the same engine" is the preference, since below-the-floor means neither tool detects anything) | exists; Tomba! 1 gets no shared `game/` — see below |
| `crash/` | Crash 1, 2, 3 (Naughty Dog, GOOL VM) | when Crash work starts, not before |
| `ctr/`, `crashbash/` | one title each — measured as their own engines | when that work starts, not before |
| `vagrant/` | Vagrant Story — one title, one engine. No lineage scaffolding: Square's Ivalice-era code appears in nothing else we port | exists (2026-08-12) |
| `megamanx4/` | Mega Man X4 — one title. X5/X6 are NOT in scope; the port is enhancements, not an engine rebuild, so a shared lineage layer would carry nothing | exists (2026-08-12) |

**Why not the three things that were on the table.**

- **Not one repo for Spyro AND Crash**, which is what was first asked for. Different developer, different
  data formats (`WAD.WAD`/`SOURCE.TRD` vs `NSF`/`NSD`), and a Lisp-VM architecture needing its own
  tooling. They already share what they should share — psxport. The cross-shipped rival demos on every
  disc are why they *look* like one family; they are cross-promotion, not shared code.
- **Not an engine-family LIBRARY vendored between psxport and a game.** A middle layer whose whole nature
  is "holds facts for N games" is a factory for the residence defect `pc_scheduler.cpp` already is —
  invisible to `psxport_smoke`, outside `test_no_game_address_literals.cpp`'s scope, and a second pin to
  sync through a `sync-submodules.sh` that already certifies pins it never checked. Shared lineage code
  lives INSIDE the lineage repo, where inheritance is the seam plan's own answer and where the repo-local
  rule "shared `game/` may not hold title literals" can be linted the same way.
- **Not 8 sibling per-title repos.** Spyro 2↔3 measure **47.0% = 4.0× the cross-studio null maximum** —
  one codebase — so per-title repos would duplicate exactly the code most worth sharing.

**MEASURED 2026-08-12, then REMEASURED the same day with the corrected metric — and the two new titles
came out on OPPOSITE sides of the line.** Run before shaping either repo's shared `game/`, because assuming
was exactly what the Spyro measurement punished. `tools/exe_similarity.py --dir <exes>`, boot executables
extracted with `discdump` into a gitignored `scratch/`; Jaccard over code-plausible windows, with the OLD
asymmetric number in parentheses to show what the fix moved. Cross-studio null max: 11.89% (same PSY-Q) /
7.31% (different PSY-Q).

|  | TOMBA1 | TOMBA2_MAIN | SPIDER1 | SPIDER2 |
|---|---|---|---|---|
| **TOMBA1** | · | 3.9% (18.8) | 2.1% (7.3) | 1.8% (6.9) |
| **TOMBA2_MAIN** | 3.9% (18.8) | · | 5.2% (11.8) | 4.6% (10.5) |
| **SPIDER1** | 2.1% (7.3) | 5.2% (11.8) | · | **57.5% (74.2)** |
| **SPIDER2** | 1.8% (6.9) | 4.6% (10.5) | **57.5% (74.2)** | · |

- **Spider-Man 1 ↔ 2: 57.5% = 4.8× the null maximum — ONE CODEBASE, and the strongest pair in the
  workspace** (above Spyro 2↔3's 4.0×). The USER's call to fold Enter Electro into `spider1/` is confirmed
  by measurement, not merely permitted: there is more to share here than anywhere else, so the multi-title
  split matters most in this repo. Both binaries are engine-sized (186,879 and 196,095 text words), so the
  old small-denominator defect cannot be what produced the number.
- **Tomba! 1 ↔ Tomba! 2: 3.9% = 0.5× the null maximum — NOT one codebase, and BELOW the floor, which is
  positive evidence of NOT sharing.** The old 18.8% was inflation from data windows and the asymmetric
  denominator. Remeasured with the corrected filter: of the 7709 windows the two share, only **729 (9.5%)
  are shared by these two and NO other executable in the corpus — 0.37% of their union**, against 49.8% for
  Spider-Man 1↔2 and 33.0% for Spyro 2↔3. (An earlier note claimed that pair-exclusive code was "87–93%
  confined to the text tail"; that does NOT reproduce — it is 21% in the last quarter of `.text`, spread
  from 0x800173A8 to 0x80095720. The conclusion is unchanged, the reason was wrong.) The two also link
  DIFFERENT PSY-Q versions (`sys.c` 1.129/1996-12-25
  + `intr.c` 1.74 vs `sys.c` 1.140/1998-01-12 + `intr.c` 1.75, version strings in the binaries, not a
  metric). **Tomba! 1 is its own engine.** Keeping it in `Tomba2Engine/` is a PREFERENCE call exactly like
  Spyro 1: expect FORMAT and tooling knowledge to transfer, expect native engine classes NOT to, and give
  it no shared `game/`.
- **The cross-lineage cells are no longer a hand-picked "negative control".** The negative class is now the
  whole cross-studio null distribution (n=67; the section above publishes n / mean / median / max with the
  max pair named), which is why these four cells are readable instead of being four cells chosen from a
  distribution nobody had measured.

**The caveat that used to be needed here is now enforced by the tool.** Tomba! 2's engine is `MAIN.EXE`;
its `SCUS_944.54` is a small loader, and under the old asymmetric metric the loader read 47.6% against
Tomba! 1 and 76.8% against its OWN engine. Under Jaccard those are 3.6% and 8.6% (0.5× and 0.7× the null
max) — the loader now reads as what it is. It is kept in the corpus as a LABELLED control, excluded from the
null pool as a non-engine binary (with the null published both ways), and `--selftest` asserts it stays
below the null max. The engine-to-engine number is the one that decides repo shape.

**Spyro 1 is in the Insomniac repo by PREFERENCE, not by measurement.** It shares the asset pipeline and
tooling with 2/3 and **~no code**: 5.6% against Spyro 2 is **0.8× the null maximum**, i.e. below what two
unrelated studios score, so Insomniac rewrote the engine between 1 and 2. Do not expect its native classes
to serve Spyro 2; expect its FORMAT knowledge to.

**The Crash trio rests on DIRECT evidence, not on this metric.** The GOOL bytecode dispatch loop is
byte-identical across Crash 1/2/3 (`0x80020218` / `0x8003A06C` / `0x80038E80`) and a trio engine function
matches **36/36** windows in Crash 2 and 3 and **0/36** in CTR, Bash, Spyro 2, Spider-Man, Tomba! 2 and
Tomba! 1. The aggregate metric only reaches 2.7× for 2↔3 and 1.1× for 1↔2 — a rewrite keeps the mechanism
and replaces the code, so whole-binary similarity under-reads exactly this case. **Toy Story 2** shares
nothing with any of the other 12 (largest cell 8.2% = 0.7× against Tomba! 1, and those two link the same
PSY-Q 1.129): its own repo, no shared `game/`.

**Order, if it matters:** Spyro 2 first (strongest family prior, adjacent to a working port, and the
cheapest real test of the multi-title bet), then Spyro 3, then the Crash trio, then CTR/Bash.

**Prerequisite that is not negotiable:** do not bring up a second Insomniac title while the framework
still contains Tomba!2's frame loop and scheduler. `spyro/game/render/frame_loop.cpp` already documents
"THIS PORT CANNOT USE THE FRAMEWORK'S FRAME LOOP"; a third consumer would fork it again. Land
`docs/plans/game-seam-redesign.md`'s early steps first.

## DECIDED: the tooling hoist happens ADDITIVELY, starting now (2026-08-11)

The generic tool ENGINES (`info.py`, `catalog.py`, `re_frontier.py`, `codemap.py`, `whatis.py`,
`go_public.py`, …) move into `psxport/tools/port/`; the DATA (`docs/info/`, `docs/issues/`, codemaps,
roadmaps) stays per-game. Every game already vendors psxport, so the engines reach them all.

**The evidence is not theoretical.** `re_frontier.py`'s green-over-nothing bug has now been fixed FOUR
times across THREE diverged copies (890 / 443 / skill lines), and two of those copies were still reporting
"OK" over a zero-entry parse on 2026-08-11 — one of them the copy a CLAUDE.md tells you to run. Divergence
also produced the `RE_FRONTIER_ROADMAP` trap and the `codemap.md` vs `code-map.md` split.

**Additively, because Tomba2Engine is mid-Job-#1 and a flag day is unacceptable:** psxport gains the
engine as a new file; each game's existing tool keeps working untouched; a game switches to a 3-line shim
one tool at a time, when someone is already in that repo. No window exists in which a repo has no working
tool, and each step is independently revertible. Unify `codemap.md`/`code-map.md` naming in the same pass
as whichever repo switches that tool.
