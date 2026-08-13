# FINDING: engine-lineage measurement across the PSX corpus — what it can and cannot decide

**Status:** HOLDS as of 2026-08-12 (recalibrated the same day it was distrusted).
**Question it answers:** do two PSX titles share an engine CODEBASE, i.e. should they live in one repo
with a shared `game/`?
**Reproduce:** `python3 tools/exe_similarity.py --dir <exes> --selftest` — exit 0 = both classes pass,
exit 1 = a regression, exit 2 = it refused and says what it did NOT scan. Boot executables are
extracted with `discdump` into a gitignored `scratch/`; no disc image or executable is ever committed.
**Second, independent tool:** `tools/lineage_probe.py` (`--sweep` for its threshold sensitivity).
**FALSIFIER for any verdict below:** a targeted function-level match WITH a negative control set. This
file says outright that such a match BEATS the aggregate metric, so one is enough to overturn a row.
**FALSIFIER for the tools themselves:** a corrected studio attribution, or a contaminated corpus
member — see "what their agreement cannot detect".

The repo-shape decisions this evidence supports are listed as one-line verdicts in
`docs/workspace/WORKSPACE.md`; the reasoning for each is in the survival table at the end of this file.

## THE SIMILARITY METRIC IS RECALIBRATED (2026-08-12) — usable again, with a stated resolution limit

**`tools/exe_similarity.py` was DISTRUSTED earlier the same day and has now been fixed, remeasured over
all 14 executables, and given a `--selftest` that gates BOTH classes.** The old headline defect — a NULL
pair (`CRASHBASH ↔ TOMBA2` 33.4%) outranking the margin of the same-studio `CRASH2 ↔ CRASH3` (35.2%) — is
gone: that null pair now reads **11.9%** against family pairs at **47–58%**.

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
| TOMBA1 ↔ TOMBA2_MAIN | 3.9% | 0.5× | below the floor — no shared codebase | 18.8% |
| CRASH3 ↔ CTR | 3.0% | 0.4× | CTR is not the trio's engine | 8.4% |

The four-cell submatrix measured when the two newest titles were added, same run:

|  | TOMBA1 | TOMBA2_MAIN | SPIDER1 | SPIDER2 |
|---|---|---|---|---|
| **TOMBA1** | · | 3.9% (18.8) | 2.1% (7.3) | 1.8% (6.9) |
| **TOMBA2_MAIN** | 3.9% (18.8) | · | 5.2% (11.8) | 4.6% (10.5) |
| **SPIDER1** | 2.1% (7.3) | 5.2% (11.8) | · | **57.5% (74.2)** |
| **SPIDER2** | 1.8% (6.9) | 4.6% (10.5) | **57.5% (74.2)** | · |

The cross-lineage cells are NOT a hand-picked "negative control" any more: the negative class is the whole
cross-studio null distribution (n=67, published above with its max pair named), which is why these cells
are readable instead of being four cells drawn from a distribution nobody had measured.

**THE RESOLUTION LIMIT, which is the one thing to carry forward.** The strongest DIRECT evidence in this
corpus — the GOOL dispatch loop byte-identical across Crash 1/2/3 (`0x80020218` / `0x8003A06C` /
`0x80038E80`), and a trio engine function matching **36/36** windows in Crash 2 and 3 against **0/36** in
CTR, Bash, Spyro 2, Spider-Man, Tomba! 1 and 2 — corresponds to only **2.7×** for `CRASH2 ↔ CRASH3` and
**1.1×** for `CRASH1 ↔ CRASH2`. Whole-binary similarity is a WEAK instrument for "same architecture,
rewritten", because a rewrite keeps the mechanism and replaces the code. **Where a targeted function-level
match WITH A NEGATIVE CONTROL SET disagrees with this metric, the targeted match wins.** The metric ranks
candidates and REFUTES false families; it does not certify one.

**A "below the floor" reading means NEITHER TOOL CAN SEE ANYTHING — it is not positive evidence of
separate engines.** The in-corpus counterexample is CRASH1↔CRASH3: 4.6% (0.6×) and 12 units (1.00×), at or
below both floors, while the Crash trio IS one architecture on direct evidence. What a below-floor reading
DOES support is the narrower, useful claim: a shared codebase reads in the hundreds
(SPIDER1↔SPIDER2 = 246 units, 57.5%), so a below-floor pair gets **no shared `game/`**.

## AN INDEPENDENT SECOND TOOL AGREES ON BAND MEMBERSHIP AND THE EXTREMES — NOT ON MID-TABLE ORDER

`tools/lineage_probe.py` (built the same day, sharing no code with `exe_similarity.py`: whole FUNCTIONS
segmented at `jr $ra`, plus exclusive-string overlap, counted absolutely and ranked by corpus SPREAD)
agrees on the TOP TWO pairs (SPIDER1|SPIDER2 strongest at 246 units = 16.4× its own measured null max,
SPYRO2|SPYRO3 next at 59 = 3.9×) and on the SET of pairs falling below each tool's own floor
(TOMBA1|TOMBA2, CRASHBASH|TOMBA2, the TOMBA2 loader/engine pair). It **DISAGREES on mid-table rank in two
cells, both listed** — an earlier write-up claimed "the same ordering", which its own data contradicts:

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
`lineage_probe.py` prints a per-file sha256 fingerprint, so a published count names the corpus it came from.

**`lineage_probe.py`'s multiples are partly a measurement of its own gate constants**, whose thresholds
are asserted rather than derived from disassembled ground truth. Its `--sweep` (measured 2026-08-12)
moves separation from 1.75× to 6.00× across a defensible grid, and at `min_insns=40` CRASH2|CRASH3
OVERTAKES SPYRO2|SPYRO3 — the mid-table order is not robust. Robust across every cell: SPIDER1|SPIDER2
is the top pair by 3–8×, and TOMBA1|TOMBA2_MAIN sits at 0–1. **Cite it for band membership and the
ordering of the extremes; never for a mid-table decimal.** Its same-code upper anchors (a title vs its
own demo build: 216 and 40) straddle the family pairs, which is the same caveat from the other side.

**The loader control.** Tomba! 2's engine is `MAIN.EXE`; its `SCUS_944.54` is a small loader that under
the old asymmetric metric read 47.6% against Tomba! 1 and 76.8% against its OWN engine. Under Jaccard
those are 3.6% and 8.6% (0.5× and 0.7× the null max) — it now reads as what it is. It stays in the corpus
as a LABELLED control, is excluded from the null pool as a non-engine binary (the null is published both
ways), and `--selftest` asserts it stays below the null max. Engine-to-engine is the number that decides
repo shape.

## DOES EACH RECORDED REPO-SHAPE DECISION SURVIVE THE CORRECTED METRIC? (2026-08-12)

Every one does; two get stronger reasons, three are re-labelled from "measured" to "preference", none is
reversed.

| decision | verdict | why, under the corrected metric |
|---|---|---|
| Spider-Man 1 + 2 in one repo | **SURVIVES, strengthened** | 57.5% = 4.8× the null max, the strongest pair in the corpus; both binaries are engine-sized (186,879 / 196,095 text words), so the small-denominator defect cannot be inflating it. The USER's call to fold Enter Electro into `spider1/` ("can be part of spider-man", 2026-08-12) is confirmed by measurement, not merely permitted — there is more to share here than anywhere else |
| Spyro 2 + 3 in one repo | **SURVIVES, strengthened** | 47.0% = 4.0× the null max — one codebase, which is why per-title repos would duplicate exactly the code most worth sharing |
| Spyro 1 in the Insomniac repo | **SURVIVES as PREFERENCE, not measurement** | 5.6% = 0.8× — BELOW the null max, so this is evidence of NOT sharing engine code; Insomniac rewrote the engine between 1 and 2. Expect its FORMAT and asset-pipeline knowledge to transfer, expect its native classes NOT to. No shared `game/` |
| Tomba! 1 in `Tomba2Engine/` | **SURVIVES as NOT-ONE-CODEBASE, and *not* as "strengthened"** | 3.9% = 0.5× and 1 whole function = 0.08×: both tools DETECT NOTHING, which is not evidence of separate engines (see the CRASH1↔CRASH3 counterexample above). What it does support: no shared `game/`. Deleted as false from the earlier write-up: "strengthened", and "below two random titles" (1 unit is above ~57% of the 109 cross-studio pairs, whose median is 0). Also: an earlier note claimed pair-exclusive code was "87–93% confined to the text tail" — that does NOT reproduce, it is 21% in the last quarter of `.text`, spread 0x800173A8–0x80095720; conclusion unchanged, reason wrong. Of the 7709 windows the two share, 729 (9.5%) are exclusive to the pair = 0.37% of their union, against 49.8% for Spider-Man 1↔2 and 33.0% for Spyro 2↔3. Different PSY-Q (`sys.c` 1.129/1996-12-25 + `intr.c` 1.74 vs `sys.c` 1.140/1998-01-12 + `intr.c` 1.75) agrees |
| the Crash trio as ONE architecture | **SURVIVES on DIRECT evidence, which this metric cannot resolve** | 19.8% = 2.7× for 2↔3 lands in the "rewritten" band, but 1↔2 is 1.1× and 1↔3 is 0.6×. The byte-identical GOOL loop and the 36/36-vs-0/36 function match are what the decision rests on — believe them over the aggregate |
| CTR and Crash Bash separate | **SURVIVES** | CTR ↔ the trio is 0.6–3.0% (0.1–0.4×) and Bash ↔ the trio 1.3–6.4%, both at or below the floor, matching 0/36 on the trio function |
| Toy Story 2 standalone | **SURVIVES** | cross-studio with all 12, so every cell is a NULL member by construction; largest is 8.2% with Tomba! 1 = 0.7× the same-PSY-Q null max, and those two link the SAME PSY-Q 1.129 — SDK, not engine. Next is CRASH2 at 6.2%. No shared `game/` |
| Vagrant Story standalone | **not measurable against this corpus, and not needed** | Square's Ivalice-era code appears in nothing else we port, so there is no candidate sibling to test |
| Mega Man X4 standalone | **not measurable against this corpus, and not needed** | X5/X6 are out of scope, and the port is enhancements rather than an engine rebuild, so a shared lineage layer would carry nothing |

## Two things the shape decision REJECTED, and why (2026-08-11)

- **Not one repo for Spyro AND Crash** (which is what was first asked for). Different developer,
  different data formats (`WAD.WAD`/`SOURCE.TRD` vs `NSF`/`NSD`), and a Lisp-VM architecture needing its
  own tooling. They already share what they should share — psxport. The cross-shipped rival demos on every
  disc are why they *look* like one family; that is cross-promotion, not shared code.
- **Not an engine-family LIBRARY vendored between psxport and a game.** A middle layer whose whole nature
  is "holds facts for N games" is a factory for the residence defect `pc_scheduler.cpp` already is —
  invisible to `psxport_smoke`, outside `test_no_game_address_literals.cpp`'s scope, and a second pin to
  sync through a `sync-submodules.sh` whose enumeration defect cost three builds in a day before it was fixed. Shared lineage code
  lives INSIDE the lineage repo, where inheritance is the seam plan's own answer
  (`docs/plans/game-seam-redesign.md`) and where "shared `game/` may not hold title literals" can be
  linted the same way.

## Order and prerequisite for growing to a second Insomniac title

Order, if it matters: Spyro 2 first (strongest family prior, adjacent to a working port, and the cheapest
real test of the multi-title bet), then Spyro 3, then the Crash trio, then CTR/Bash.

**Prerequisite, not negotiable:** do not bring up a second Insomniac title while the framework still
contains Tomba!2's frame loop and scheduler. `spyro/game/render/frame_loop.cpp` already documents "THIS
PORT CANNOT USE THE FRAMEWORK'S FRAME LOOP"; a third consumer would fork it again. Land
`docs/plans/game-seam-redesign.md`'s early steps first.
