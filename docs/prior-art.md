# Prior art — other people working on these binaries, and what we may take from it

Researched 2026-08-12. This file exists because none of it was recorded anywhere in the workspace and two
of the projects below bear directly on titles we are porting. **Consult it before designing anything, and
before assuming we are alone on a binary.**

The rule that governs all of it: **where a reference and a MEASUREMENT disagree, the measurement wins.** A
decomp is an excellent source of function boundaries and names; it is not evidence about our port.

## The license asymmetry decides what "take it" means

| project | license | what that permits |
|---|---|---|
| `theMagicalKarp/open-spyro` | **CC0-1.0** | take code AND ideas freely, cite as courtesy |
| `TwilitRealm/dusklight` | **CC0** | same (see `CLAUDE.md`) |
| `mstan/psxrecomp` | **PolyForm Noncommercial 1.0.0** | **READ AND LEARN ONLY.** Copying code drags the noncommercial term into this repo. Take the SHAPE, never the text |
| `hansbonini/psx_tomba` | check before use | not yet verified — do that before taking anything |

## decomp.dev — the matching-decompilation progress tracker

`https://decomp.dev` tracks ~150 **matching** decompilation projects: source that compiles to a
byte-identical binary. The percentage is produced by `objdiff` progress reports, generated in CI and
uploaded as a GitHub Actions artifact named `<VERSION>_report`, with a GitHub App on the repo feeding the
site and commenting on PRs.

15 of its projects are PSX. Two matter here:

- **Tomba! — `hansbonini/psx_tomba`, 18.17%.** A matching decomp of Tomba! 1, the title
  `docs/workspace/WORKSPACE.md` names as the candidate second title of the Whoopee Camp lineage repo.
- Vagrant Story 62.63%, SotN 65.53%, Parasite Eve II 83.91%, Xenogears 32.58% — context for what a mature
  PSX decomp looks like.

**None of Crash, Spyro, Spider-Man or Toy Story 2 is listed there.**

**psxport cannot report to decomp.dev, and the reason is structural, not administrative.** That metric is
matched bytes against the original objects. Our verification axis is SBS byte-exact *RAM parity* — a real
guarantee, arguably a harder one, but not object identity, so there is nothing for `objdiff` to measure. I
found no stated policy on whether the site accepts non-matching or recompilation projects; `psxrecomp` is
not listed either, which is suggestive and not proof. **Treat eligibility as UNKNOWN rather than settled.**

## `mstan/psxrecomp` — the same technique, a different destination

A PS1 static-recompiler ecosystem whose architecture is strikingly close to ours: recompiler + runtime
split, per-game repos pinning the framework as a submodule, generated C never hand-edited (fixes go in the
recompiler), a recompiled BIOS as the correctness oracle, and a MIPS interpreter fallback that is compiled
away as coverage grows. It already ships **Tomba! and Tomba! 2** ports (`mstan/tombarecomp`), plus Ape
Escape, Mega Man X4–X6 and a community Xenogears port.

**The goals diverge and the difference is worth keeping straight.** psxrecomp aims at a faithful
hardware-accurate runtime plus enhancements. psxport's stated goal is the other end — *rebuild as a
PC-native engine, do NOT simulate the PSX* — with the recomp as oracle and scaffolding to be replaced.
Read theirs the way we read Dusklight: for the shape of solved problems (overlay capture-and-compile, BIOS
as oracle, per-game repo layout), not for code.

**USER DECISION 2026-08-12: we are NOT engaging with their community (R.A.I.D.).** *"I won't contact
R.A.I.D, I don't like their workflow either."* Do not propose it again, and do not route contributions
through it.

## Per-title prior art

- **Spyro 1 — already vendored.** `theMagicalKarp/open-spyro` is a submodule at
  `spyro/external/open-spyro` and documented in `spyro/docs/references.md`; it was NOT a new discovery.
  Verified 2026-08-12: our extracted `SCUS_942.28` is SHA-1 `84e3728ab94720d0873e2514adf4aade4935e0c5`,
  **byte-identical to its target**, so its `config/symbol_addrs.txt` + `include/{types,funcs,globals}.h`
  name OUR addresses with no translation. Its matched figure is now **14.63%** (828 main-EXE functions —
  673 game, 155 PSY-Q/libc — plus 37 overlays); `spyro/docs/references.md` said ~5% and was stale.
  Also: `TheMobyCollective/spyro-1`, `celophi/spyro-decompilation`.
- **Tomba! 1** — `hansbonini/psx_tomba` (above). Engine code will NOT transfer — 3.9% = 0.5× the
  cross-studio null max against Tomba! 2's `MAIN.EXE`, and 1 shared whole function = 0.08× (the 18.8% an
  earlier version of this line quoted is the pre-recalibration asymmetric figure; do not reuse it). Both
  readings are BELOW their floors, i.e. neither tool detects anything, which is weaker than "different
  engines" — but a shared codebase reads in the hundreds, so nothing transfers at the code level. Format
  and tooling knowledge should.
- **Crash** — no matching decomp. Partial efforts exist: a Crash 2 mini-decomp, a decomp strand inside the
  CTR ModSDK (active July 2026), and a Crash Bash RE project. Nothing to vendor yet.
- **Toy Story 2** — no decomp. Traveller's Tales *format* tooling exists and is the useful half:
  `juanmv94/TravellersTalesPSXCollisionViewer` (collision + GFX models, TS2 among its targets) and
  `mouksx/Toy-Story-2-Modding`.
- **Spider-Man** — nothing found. Scanned: decomp.dev's full PSX list plus targeted searches; a negative
  from a search is weaker than a negative from a measurement, so treat it as "not found", not "absent".
- **Tekken 3 — SCOUTED 2026-08-12, DECIDED AGAINST, and the verdict is now USER-CONFIRMED. Do not build
  a tree for it.** The measurements below are worth keeping so nobody re-scouts; the verdict is worth
  keeping so nobody re-argues it.
  - **PROVENANCE, because it changed.** The USER's own input was only *"tekken 3 too maybe"* (2026-08-12,
    hedged), so the decision-against was a SESSION's, not theirs — recorded that way at first, correctly.
    They then asked outright whether Tekken 3 had been started, were offered three paths (bootstrap it
    anyway · leave it and do framework work first · scout the netplay/input path deeper before deciding)
    and chose **leave it, framework first** (2026-08-12; a choice among options, not a quote). So the
    "framework before any new title" prerequisite in `docs/findings/lineage-metric.md` now has direct
    USER endorsement, and it applies to every unbuilt tree, not just this one.
  - **The reason is the VALUE PROPOSITION, not difficulty.** DuckStation already ships this title's
    entire cheap enhancement set — widescreen via the GTE hack, 4K, PGXP perspective-correct texturing
    and wobble removal, texture replacement, rewind, save states. A port would spend the workspace's
    hardest RE budget to reproduce checkbox features. That is the inverse of Tomba!2 and Spyro, where
    the port delivers what emulation structurally cannot. The only moat is rollback netplay and
    input-latency reduction, and both sit behind full engine RE. Everything cheap is already free;
    everything with a moat is expensive.
  - **Mega Man X4 is NOT the analogy.** X4 has an AGPL decomp whose build target is byte-identical to
    our extraction — free symbols at our addresses. Tekken 3 is X4's enhancement shape with Toy Story
    2's prior art *minus* TS2's format tooling: one MIT archive unpacker, its file map derived from the
    wrong region. Hardest tree in the workspace on the prior-art axis.
  - **It DOES stream code overlays** — `SLUS_004.02`, SHA-1 `562c82d5888f5cb19a883dbfbf1e61a5fa143cbe`,
    1,185,792 B. Overlay code lives in `TEKKEN3.BNS`: 7 word-aligned clusters, ≥220,564 B, +49% on the
    resident 448 KiB. The decisive signal is call-target locality — 3,104 of 3,164 `j`/`jal` targets in
    those clusters land inside `0x80010000..0x80131000`, against 2.7% for BNS as a whole.
  - **The "hand-written assembly arcade port" reputation is REFUTED**: it links stock PSY-Q and ships
    the debug strings verbatim (`sys.c` 1.135, `intr.c` 1.76, `bios.c` 1.86) — the same cohort as
    CRASH3 and DIGIMON. Largest boot exe in the corpus but only ~448 KiB is code; "big exe" here means
    big embedded data.
  - **`TEKKEN3.DA` is a worse `MOJIPAT.ARC`**: 24.3% code-plausible with **zero** `jr $ra`. Running
    `megamanx4/tools/code_scan.py --selftest` against Tekken's own files correctly FAILS (exit 1) — the
    instrument is sound, the file is precisely what the three-signal design exists for. `TEKKEN3.DMY`
    is pure padding (0 of 5887 pages non-zero).
  - FALSIFIER for the verdict: a moat feature emulation cannot reach (rollback netplay shipping in a
    PS1 emulator would strengthen the case against; a decomp appearing would weaken the cost argument).

## If we ever want to contribute outward

Ordered by value, with the blocker named first: **every repo here is private and its history contains
disc-derived material.** `tools/go_public.py` in each game repo audits the full history for exactly that
(disc images, extracted executables, `/home/<user>` paths). That audit is the gate; nothing is published
before it is clean and the USER has approved.

1. **`open-spyro`** — CC0 both directions, and the SHA match makes it mechanical: our RE'd addresses and
   names flow out, their symbol map flows in. Already vendored, so the plumbing exists.
2. **`psx_tomba`** — our Tomba! 2 engine RE is deep; format/tooling knowledge transfers even though engine
   code does not.
3. **Our own tooling, which has no equivalent elsewhere** — `tools/exe_similarity.py` (Jaccard over
   code-plausible windows, calibrated against the MEASURED cross-studio null distribution: n=67, mean 3.91%,
   max 11.89%, stratified by PSY-Q version; positives 57.5% and 47.0% at 4.8x and 4.0x that max) plus the
   independent `tools/lineage_probe.py` (whole-function + exclusive-string evidence). Both were recalibrated
   on 2026-08-12 after the original single "12.5% SDK ceiling" was falsified by a null pair reading 33.4%.
   Both now ship a `--selftest` that gates positives AND negatives ON THE REAL CORPUS, and both refuse
   (exit 2) rather than certify a corpus they did not scan — but note that `lineage_probe.py` only got its
   corpus layer on 2026-08-12 in a later pass: until then its selftest was SYNTHETIC-ONLY and passed with
   the corpus absent, truncated or swapped, which is exactly the gap that let the first tool's wrong SDK
   ceiling survive. Nobody else appears to have either.
   Same for the registry/gate patterns and `gpuguard`.
