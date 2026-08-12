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
- **Tomba! 1** — `hansbonini/psx_tomba` (above). Engine code will NOT transfer (measured 18.8% against
  Tomba! 2's `MAIN.EXE`), but format and tooling knowledge should.
- **Crash** — no matching decomp. Partial efforts exist: a Crash 2 mini-decomp, a decomp strand inside the
  CTR ModSDK (active July 2026), and a Crash Bash RE project. Nothing to vendor yet.
- **Toy Story 2** — no decomp. Traveller's Tales *format* tooling exists and is the useful half:
  `juanmv94/TravellersTalesPSXCollisionViewer` (collision + GFX models, TS2 among its targets) and
  `mouksx/Toy-Story-2-Modding`.
- **Spider-Man** — nothing found. Scanned: decomp.dev's full PSX list plus targeted searches; a negative
  from a search is weaker than a negative from a measurement, so treat it as "not found", not "absent".

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
   on 2026-08-12 after the original single "12.5% SDK ceiling" was falsified by a null pair reading 33.4%;
   both now ship a `--selftest` that gates positives AND negatives. Nobody else appears to have either.
   Same for the registry/gate patterns and `gpuguard`.
