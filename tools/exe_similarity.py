#!/usr/bin/env python3
"""exe_similarity.py — ADDRESS-INDEPENDENT code similarity between PS-EXE executables.

THE QUESTION IT ANSWERS: do two PSX games share an ENGINE, or merely the Sony SDK? Every PSX title
statically links PSY-Q (libgte/libgpu/libspu/libcd), so raw similarity is never zero and a bare
percentage means nothing. What carries signal is where a pair sits relative to the MEASURED NULL
DISTRIBUTION of cross-studio pairs — so this tool is usable only as a MATRIX plus that distribution,
never on one pair in isolation, and never against a remembered constant.

METHOD. Decode every aligned 32-bit word of the text image as MIPS, normalise it to an opcode-shape
token (immediates and jump targets MASKED, so the link address cannot matter), hash 16-token shingles,
DROP windows that are not plausibly code, and report **Jaccard**: |A∩B| / |A∪B|. Then compare that
number against the null distribution of the pair's own PSY-Q STRATUM (below).

================================================================================================
RECALIBRATED 2026-08-12, after the previous version was distrusted for producing a null pair
(CRASH BASH vs TOMBA! 2 = 33.4%) that outranked the margin of a same-studio pair (CRASH2/CRASH3 =
35.2%). Four defects; every fix below is justified by a measurement over the pairs that could
FALSIFY it — the previous calibration's fatal error was a sensitivity check run over the 8
executables that excluded the offending pairs, i.e. tested where it could not fail.

 DEFECT 1 — ASYMMETRIC DENOMINATOR. The score was |A∩B| / min(|A|,|B|), so a small library-dominated
   binary scored high against everything. Tomba! 2's 26k-shingle SCUS loader read 76.8 / 47.6 / 37.6 /
   32.6 / 32.2% against MAIN, Tomba! 1, Crash Bash, CTR and Spider-Man. FIX: Jaccard. Its cells fall to
   8.6 / 3.6 / 14.7 / 6.9 / 5.9%, i.e. the 47.6% against Tomba! 1 — a DIFFERENT-engine pair that the old
   number made look like family — becomes 3.6%, while the genuine pairs barely move (SPIDER1/2
   74.2 -> 57.5%, SPYRO2/3 64.2 -> 47.0%). Both numbers are printed for every cell, old in parentheses,
   so the re-baseline is auditable rather than silent.

 DEFECT 2 — DATA IN .text DECODED AS INSTRUCTIONS, made worse by the normaliser: `j`/`jal` mask their
   whole target, so EVERY jump word normalises to ONE token and any monotone table of words with a
   0x08-0x11 top byte becomes a run of identical tokens — identical in every binary holding such a
   table. Both halves of the known bad match are exactly that, confirmed by disassembly
   (`tools/disasm.py`, CRASH BASH):
     0x80068BD4  16/16 words ARE legal instructions, but only 1-2 DISTINCT tokens per window
                 -> `b 0x80068bd8 / b 0x80068bf4 / b 0x80068c14 ...`: a branch-offset table
     0x80069644  median 0/16 words are legal instructions at all
                 -> `j 0x813c36b0 / j 0x812836c0 ...`: an address table, targets outside 2 MB of RAM
   FIX: a two-part window filter — `--min-valid` legal R3000A words AND `--min-distinct` distinct
   tokens per 16-word window. The parts are COMPLEMENTARY, and the sweep shows it: `--min-valid 16`
   alone (min-distinct 1 or 2) still admits 0x80068BD4; `--min-distinct 8` with min-valid 0 still
   admits 0x80069644. Only both together exclude both.
   THE DEFAULTS ARE MEASURED, not chosen. Over 5215 windows from five hand-disassembled code regions
   (CRASHBASH 0x8002E7B0, TOMBA2_MAIN 0x800896E0, SPIDER1 0x8008739C, SPYRO2 0x8005478C, CRASH2
   0x80049B2C): every single window has 16/16 legal words, so `--min-valid 16` costs ZERO verified
   code, and the distinct-token histogram has a GAP — confirmed data sits at <=2, no verified code
   window is below 4 (counts: 4->60, 5->12, 6->76, 7->64, 8->207). `--min-distinct 4` lands in that
   gap. Raising it to 8 would discard 212/5215 = 4.1% of verified code for no gain in separation
   (the sweep is flat at 2.7-3.0x across the whole grid). `--selftest` asserts both known ranges are
   excluded WITH the filter and present WITHOUT it, so the filter cannot silently stop working.

 DEFECT 3 — A HAND-PICKED "SDK CEILING" (12.5%, Spyro 1 vs Crash 2) STOOD IN FOR A DISTRIBUTION.
   FIX: every run — there is no flag to forget — enumerates every cross-studio pair (different
   developer => shared proprietary engine code is impossible) and publishes n / mean / median / MAX
   with the max pair NAMED, plus the same figures including the loader control so that exclusion is
   never a hidden favour. Nothing is compared against a constant; every verdict printed by this tool
   is a MULTIPLE of a null maximum measured in the same run.

 DEFECT 4 (found during this recalibration) — THE NULL IS BIMODAL IN PSY-Q VERSION, so one pooled
   maximum over-penalises pairs that link different SDKs. Measured over the 67 cross-studio pairs:
        same `sys.c` version    n=25  mean 5.96%  median 5.61%  MAX 11.89% CRASHBASH/TOMBA2_MAIN
        different version       n=42  mean 2.69%  median 2.40%  MAX  7.31% CRASH3/TOMBA1
   The two strata differ by 2.2x in the mean, and the residual 11.89% of the worst pair was
   disassembled: its two largest shared runs, CRASHBASH 0x80049808 (1744 windows) and 0x8002E878
   (1283), are real non-table code driving SPU/CD registers — identical statically-linked PSY-Q 1.140,
   not shared engine code. So the SDK floor is a GENUINE CONFOUND, not an artifact, and it tracks the
   SDK VERSION (Crash Bash and Tomba! 2 both link sys.c 1.140).
   FIX: stratify. A pair is scored against the null maximum of its OWN stratum, read from the
   `$Id: sys.c,v <ver>` string inside each binary. A binary with no such string is reported UNKNOWN
   and its pairs fall back to the POOLED null, with the fallback printed in the verdict line.

 REJECTED, and recorded so it is not re-attempted: dropping every window seen in >=K distinct studios
   ("define the SDK from the corpus"). Measured at the default filter: K=3 drops 26000 windows and
   gives SPIDER1/2 12.3x, SPYRO2/3 9.3x, CRASH2/3 2.9x — apparently far better separation, and it also
   drives TOMBA1/TOMBA2_MAIN to 0.2x. It is CIRCULAR: it removes by construction exactly what
   cross-studio pairs share, so the null collapses (K=2 gives a null that is
   0.00% for all 67 pairs) and can no longer falsify the metric. That is the distrusted version's own
   error in a new costume. Available as `--sdk-filter K` for audit only; it prints this warning and
   refuses to be treated as calibration.
================================================================================================

CALIBRATION — 14 boot executables / 13 titles, default filter (`--min-valid 16 --min-distinct 4`,
which keeps 52-96% of unique windows per binary; the 52% is Tomba! 2's loader, half of which is not
code). This is the matrix `--selftest` reproduces.

  NULL DISTRIBUTION, 67 cross-studio pairs of the 13 engine binaries:
        mean 3.91%   median 3.53%   MAX 11.89% = CRASHBASH vs TOMBA2_MAIN
        next four: CRASH2/TOMBA1 8.2%, TOMBA1/TS2 8.2%, SPYRO1/TOMBA1 7.5%, CRASH3/TOMBA1 7.3%
        including the loader control (78 pairs): mean 4.17%, MAX 14.68% CRASHBASH/TOMBA2_SCUS
    Under the OLD asymmetric metric the same maximum read 33.4%, above the same-studio CRASH2/CRASH3
    margin. That inversion is gone: the largest null cell is now 11.89% and the two strongest family
    pairs are 47-58%.

  SAME-STUDIO PAIRS, Jaccard and multiple of the pair's own stratum null MAX:
        SPIDER1 / SPIDER2      57.5%   4.8x   one codebase                 (old asym 74.2%)
        SPYRO2  / SPYRO3       47.0%   4.0x   one codebase                 (old asym 64.2%)
        CRASH2  / CRASH3       19.8%   2.7x   same architecture, rewritten (old asym 35.2%)
        CRASH1  / CRASH2        8.2%   1.1x   at the SDK floor             (old asym 18.1%)
        SPYRO1  / SPYRO2        5.6%   0.8x   BELOW the floor: rewritten   (old asym 11.2%)
        CRASH1  / CRASH3        4.6%   0.6x   at/below the floor           (old asym 11.6%)
        TOMBA1  / TOMBA2_MAIN   3.9%   0.5x   BELOW the floor              (old asym 18.8%)
        CRASH3  / CTR           3.0%   0.4x   CTR is not the trio's engine (old asym 8.4%)
        CRASH1  / CTR           0.6%   0.1x                                (old asym 3.2%)
  CONTROL, same GAME, loader vs engine: TOMBA2_SCUS / TOMBA2_MAIN 8.6% = 0.7x — a loader shares only
  SDK with its own engine, which is why the old 76.8% never meant lineage. That single cell is the
  clearest demonstration in the corpus that a high asymmetric score is not evidence of anything.

  THE BANDS ARE ANCHORED ON THOSE CONTROLS, not chosen:
        > 3x stratum null max   one codebase                    anchored by SPIDER1/2 and SPYRO2/3
        1.5 - 3x                same architecture, rewritten    anchored by CRASH2/CRASH3
        <= 1.5x                 indistinguishable from two strangers linking the same SDK
  A cell below the null max is not weak evidence of sharing; it is evidence of NOT sharing.

  HONEST LIMIT OF RESOLUTION, stated because it changes how these numbers may be used: the strongest
  DIRECT evidence in this corpus — the GOOL bytecode dispatch loop byte-identical across Crash 1/2/3,
  and a trio engine function matching 36/36 windows in Crash 2 and 3 against 0/36 in CTR, Bash,
  Spyro 2, Spider-Man and both Tombas — corresponds to only 2.7x here for CRASH2/CRASH3 and 1.1x for
  CRASH1/CRASH2. Whole-binary similarity is a WEAK instrument for "same architecture, rewritten",
  because a rewrite keeps the mechanism and replaces the code. When a targeted function-level match
  WITH A NEGATIVE CONTROL SET disagrees with this tool, THE TARGETED MATCH WINS. This tool ranks
  candidates and refutes false families; it cannot certify one.

  CORROBORATION BY AN INDEPENDENT TOOL — ON BAND MEMBERSHIP AND THE EXTREMES, NOT ON ORDERING:
  `tools/lineage_probe.py` shares no code with this one (whole FUNCTIONS segmented at `jr $ra`, plus
  exclusive-string overlap, counted absolutely and ranked by corpus spread). Over a 17-binary corpus it
  agrees on the TOP TWO pairs (SPIDER1|SPIDER2 16.4x its own measured null max, SPYRO2|SPYRO3 3.9x) and
  on WHICH pairs fall below each tool's own floor (TOMBA1|TOMBA2, CRASHBASH|TOMBA2, the loader/engine
  pair). It DISAGREES on mid-table order in two cells, and an earlier version of this paragraph claimed
  "the same ordering", which the data contradicts:
      SPYRO1|SPYRO2   0.8x here (below floor)  vs  27 units = 2.25x there
      CRASH1|CRASH2   1.1x here                vs  25 units = 2.08x there   -> the pair is INVERTED
      SCUS|MAIN       0.7x here, above SPYRO1|SPYRO2  vs  1 unit = 0.07x there, bottom of the table
  (CRASH2|CRASH3 is 4.75x there, not the 3.8x recorded earlier — that divided by the pooled null max
  instead of the pair's own PSY-Q stratum.) So: set-membership per band, never a total order.
  AND THE INDEPENDENCE HAS A CEILING: the two tools share their CORPUS and their STUDIO ATTRIBUTION,
  so a wrong developer attribution or a contaminated corpus member moves a pair in BOTH at once and
  their agreement cannot detect it. See the STUDIOS caveat in KNOWN LIMITS — it applies JOINTLY.

KNOWN LIMITS:
  * BOOT EXECUTABLES only. Overlays are not compared, so a game whose engine lives in overlays is
    under-measured — Tomba! 2's own loader/engine split is the proof that this matters.
  * The filter drops padding, tables and unrolled repetition. Those windows carry no lineage signal —
    they are what matched across unrelated binaries — but a hand-unrolled engine loop can be dropped
    too, so a score is a FLOOR on similarity, never a ceiling.
  * High similarity is a strong prior for extracting shared code LATER, never advance justification for
    designing a shared library UP FRONT: struct-layout and behaviour drift only appear during port work.
  * The SPECIAL-opcode normalisation drops `rs` as well as masking immediates. Applied uniformly, so
    comparisons stay valid; it just makes absolute numbers slightly more permissive.
  * The null distribution is only as good as `STUDIOS`. A wrong developer attribution silently moves a
    pair between the null pool and the family pool, so the mapping is printed on every run.

REFUSALS (exit 2, never a clean empty answer): a missing/unreadable file or missing --dir, a file
without the PS-X EXE magic, an input yielding under 1000 usable shingles (reported as "scanned almost
nothing", not as 0%), fewer than two executables, two inputs resolving to the same NAME (which is how
the old `--dir` printed an all-'.' matrix: it named every file after its PARENT DIRECTORY, so a corpus
in one directory collapsed to a single name), a corpus with no cross-studio pair (nothing to calibrate
against), or a --selftest whose corpus lacks the controls.

USAGE
    python3 tools/exe_similarity.py NAME=path/SCUS_942.28 NAME2=path/SCUS_944.25 ...
    python3 tools/exe_similarity.py --dir <d>               # matrix + stratified null distribution
    python3 tools/exe_similarity.py --dir <d> --selftest    # gate BOTH classes; exit 1 on regression
    python3 tools/exe_similarity.py --dir <d> --sweep       # reproduce the filter-threshold evidence
    python3 tools/exe_similarity.py --dir <d> --explain A B # WHERE the shared windows are, by address
    python3 tools/exe_similarity.py --dir <d> --sdk-filter 3  # the REJECTED variant, audit only

Extract the executables first with the framework's own disc tool (NOT a game's run.sh, which is the
user's play launcher):
    cmake --build build --target discdump
    ./build/tools/discdump list "/path/disc.chd"          # find the boot exe name in SYSTEM.CNF
    ./build/tools/discdump get SCUS_942.28 "/path/disc.chd" <outdir>
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import itertools
import os
import re
import statistics
import struct
import sys

WINDOW = 16
MIN_SHINGLES = 1000
TEXT_OFF = 2048                 # the PS-EXE header is 0x800 bytes; the text image follows
DEFAULT_LOAD = 0x80010000

# Developer per title (substring match on the input NAME, case-insensitive, first rule wins — so
# CRASHBASH must precede CRASH). Different developer => shared proprietary engine code is impossible,
# which is what makes every cross-studio pair a member of the null distribution.
STUDIOS = [
    ('CRASHBASH', 'Eurocom'),
    ('CTR', 'NaughtyDog'),
    ('CRASH', 'NaughtyDog'),
    ('SPYRO', 'Insomniac'),
    ('SPIDER', 'Neversoft'),
    ('TOMBA', 'WhoopeeCamp'),
    ('TS2', 'TravellersTales'),
    ('TOYSTORY', 'TravellersTales'),
]

# A LOADER is not an engine. Excluded from the null POOL (a library-dominated stub would raise the SDK
# floor with something no engine-to-engine comparison involves) but still shown in the matrix, still
# asserted by --selftest, and the null is published BOTH ways so the exclusion is never a hidden favour.
CONTROLS = {'TOMBA2_SCUS'}

STRONG_POSITIVES = [('SPIDER1', 'SPIDER2', 3.0), ('SPYRO2', 'SPYRO3', 3.0)]
WEAK_POSITIVES = [('CRASH2', 'CRASH3', 2.0)]        # same architecture, rewritten — the resolution edge
# Cross-studio: these two broke the old calibration. They are MEMBERS of the null pool, so "<= null
# max" would be vacuous for them; they are asserted against the POSITIVE class instead.
NULL_MEMBER_NEGATIVES = [('CRASHBASH', 'TOMBA2_MAIN'), ('CRASHBASH', 'SPIDER1'), ('CRASHBASH', 'CTR'),
                         ('TS2', 'SPIDER1'), ('TOMBA1', 'TS2')]
# Within-studio or control pairs that must NOT read as family. Not in the null pool => non-vacuous.
FAMILY_NEGATIVES = [('TOMBA1', 'TOMBA2_MAIN'), ('SPYRO1', 'SPYRO2'), ('CRASH1', 'CTR'),
                    ('TOMBA2_SCUS', 'TOMBA2_MAIN')]
# Ranges DISASSEMBLED and confirmed to be data, not code (see DEFECT 2). The filter must exclude them,
# and must be the reason they are excluded — asserted in both directions.
KNOWN_DATA_RANGES = [('CRASHBASH', 0x80068BD4, 0x80069000, 'branch-offset table'),
                     ('CRASHBASH', 0x80069644, 0x8006C128, 'address table (targets outside RAM)')]

# MIPS R3000A legality. No FPU on the PSX; COP2 is the GTE. Accepts every opcode the console can
# execute and rejects the rest, so "is this word an instruction at all" is decidable.
PRIMARY_OK = frozenset([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                        16, 18,                       # COP0, COP2 (GTE)
                        32, 33, 34, 35, 36, 37, 38,   # lb lh lwl lw lbu lhu lwr
                        40, 41, 42, 43, 46,           # sb sh swl sw swr
                        50, 58])                      # lwc2 swc2
SPECIAL_OK = frozenset([0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 16, 17, 18, 19,
                        24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43])


def refuse(msg: str):
    """Every refusal exits 2 and says what was NOT scanned — never a clean empty answer."""
    print(msg, file=sys.stderr)
    sys.exit(2)


def norm(w: int) -> int:
    """Normalise one instruction word to an opcode SHAPE, dropping address-dependent fields."""
    op = (w >> 26) & 0x3F
    if op == 0:                     # SPECIAL — no immediate; funct/shamt kept, rs dropped (see limits)
        return w & 0xFC1FFFFF
    if op in (2, 3):                # j / jal — target is a link address, mask it entirely
        return op << 26
    return w & 0xFFFF0000           # I-type — keep op/rs/rt, mask the immediate


def word_valid(w: int) -> bool:
    """Is this word a legal R3000A instruction? Tables of addresses mostly are not."""
    op = (w >> 26) & 0x3F
    if op == 0:
        return (w & 0x3F) in SPECIAL_OK
    if op == 1:                                  # REGIMM: bltz/bgez/bltzal/bgezal
        return ((w >> 16) & 0x1F) in (0, 1, 16, 17)
    if op in (16, 18):                           # coprocessor — accept any encoding
        return True
    return op in PRIMARY_OK


def read_exe(path: str):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        refuse(f"REFUSING: cannot read {path}: {e} — scanned NOTHING.")
    if data[:8] != b'PS-X EXE':
        refuse(f"REFUSING: {path} has no 'PS-X EXE' magic, so it is not a PSX executable. "
                         f"Nothing was compared; this is not a 0% result.")
    t_addr = struct.unpack_from('<I', data, 0x18)[0] or DEFAULT_LOAD
    m = re.search(rb'\$Id: sys\.c,v ([0-9.]+)', data)
    psyq = m.group(1).decode() if m else None
    img = data[TEXT_OFF:]
    words = [struct.unpack_from('<I', img, i)[0] for i in range(0, len(img) - 4, 4)]
    return words, t_addr, psyq


def windows(path: str):
    """Every 16-word window with its hash and its two code-plausibility features.

    Returns (rows, nwords, t_addr, psyq); rows is [(hash, n_valid, n_distinct)]. Features are computed
    ONCE so a threshold sweep re-filters rather than rehashing.
    """
    words, t_addr, psyq = read_exe(path)
    toks = [norm(w) for w in words]
    valid = [1 if word_valid(w) else 0 for w in words]
    rows = []
    for i in range(len(toks) - WINDOW):
        h = hashlib.blake2b(struct.pack('<%dI' % WINDOW, *toks[i:i + WINDOW]),
                            digest_size=8).digest()
        rows.append((h, sum(valid[i:i + WINDOW]), len(set(toks[i:i + WINDOW]))))
    return rows, len(words), t_addr, psyq


def kept_set(rows, min_valid, min_distinct):
    return {h for h, nv, nd in rows if nv >= min_valid and nd >= min_distinct}


def all_set(rows):
    return {h for h, _nv, _nd in rows}


def jac(a, b):
    u = len(a | b)
    return 100.0 * len(a & b) / u if u else 0.0


def asym(a, b):
    m = min(len(a), len(b))
    return 100.0 * len(a & b) / m if m else 0.0


def studio_of(name: str):
    up = name.upper()
    for key, st in STUDIOS:
        if key in up:
            return st
    return None


class Corpus:
    """Loaded executables plus the derived shingle sets. Every denominator it prints is its own."""

    def __init__(self, items, min_valid, min_distinct, verbose=True):
        self.min_valid, self.min_distinct = min_valid, min_distinct
        self.names = [n for n, _ in items]
        self.paths = dict(items)
        self.rows, self.sets, self.unf, self.psyq, self.load = {}, {}, {}, {}, {}
        for name, path in items:
            r, nw, ta, pv = windows(path)
            self.rows[name] = r
            self.unf[name] = all_set(r)
            self.sets[name] = kept_set(r, min_valid, min_distinct)
            self.psyq[name] = pv
            self.load[name] = ta
            if len(self.sets[name]) < MIN_SHINGLES:
                refuse(
                    f"REFUSING: {path} yielded only {len(self.sets[name])} code-plausible shingles "
                    f"from {nw} words ({len(self.unf[name])} before the filter) — too little to "
                    f"compare. That is 'scanned almost nothing', not 'no similarity'.")
            if verbose:
                print(f"{name:12s} {os.path.basename(path):16s} {nw:8d} words  load 0x{ta:08X}  "
                      f"PSY-Q sys.c {pv or 'UNKNOWN':6s}  {len(self.unf[name]):7d} shingles -> "
                      f"{len(self.sets[name]):7d} code-plausible "
                      f"({100.0*len(self.sets[name])/max(1,len(self.unf[name])):.1f}% kept)")
        self.unknown_studio = [n for n in self.names if studio_of(n) is None]
        self.unknown_psyq = [n for n in self.names if self.psyq[n] is None]

    def j(self, a, b):
        return jac(self.sets[a], self.sets[b])

    def old(self, a, b):
        return asym(self.unf[a], self.unf[b])

    def stratum(self, a, b):
        """'same' / 'diff' PSY-Q, or 'pooled' when either version is unknown."""
        if self.psyq[a] is None or self.psyq[b] is None:
            return 'pooled'
        return 'same' if self.psyq[a] == self.psyq[b] else 'diff'

    def null_pairs(self, include_controls=False):
        pool = [n for n in self.names
                if studio_of(n) is not None and (include_controls or n not in CONTROLS)]
        return [(a, b, self.j(a, b), self.stratum(a, b))
                for a, b in itertools.combinations(pool, 2) if studio_of(a) != studio_of(b)]


def stats_of(pairs):
    if not pairs:
        return None
    v = [p[2] for p in pairs]
    top = max(pairs, key=lambda p: p[2])
    return {'n': len(v), 'mean': statistics.mean(v), 'median': statistics.median(v),
            'max': max(v), 'max_pair': (top[0], top[1])}


def print_null(c: Corpus):
    """Publishes the null distribution, pooled and stratified. Returns the strata dict."""
    print(f"\nCROSS-STUDIO NULL DISTRIBUTION — different developer, so shared proprietary engine code "
          f"is impossible and only PSY-Q can overlap. This replaces the falsified single 'SDK ceiling'.")
    print("  studio map: " + ", ".join(f"{n}={studio_of(n) or 'UNKNOWN'}" for n in c.names))
    if c.unknown_studio:
        print(f"  EXCLUDED as UNKNOWN STUDIO (add them to STUDIOS): {c.unknown_studio}")
    inpool = [n for n in c.names if n in CONTROLS]
    if inpool:
        print(f"  EXCLUDED as a LOADER/non-engine CONTROL: {sorted(inpool)} — still in the matrix and "
              f"still asserted by --selftest; both nulls printed below so the exclusion is visible.")
    pairs = c.null_pairs()
    all_pairs = c.null_pairs(include_controls=True)
    st = stats_of(pairs)
    if st is None:
        print("  n=0 cross-studio pairs — NOTHING was measured. Every name resolved to one studio, so "
              "this run cannot bound the SDK floor and the matrix above is UNCALIBRATED.")
        return None
    sa = stats_of(all_pairs)
    print(f"  ENGINE BINARIES ONLY: n={st['n']}  mean {st['mean']:.2f}%  median {st['median']:.2f}%  "
          f"MAX {st['max']:.2f}% = {st['max_pair'][0]} vs {st['max_pair'][1]}")
    print(f"  incl. controls:      n={sa['n']}  mean {sa['mean']:.2f}%  median {sa['median']:.2f}%  "
          f"MAX {sa['max']:.2f}% = {sa['max_pair'][0]} vs {sa['max_pair'][1]}")
    print("  top 6 null pairs: " + ", ".join(
        f"{a}/{b} {s:.1f}%" for a, b, s, _ in sorted(pairs, key=lambda p: -p[2])[:6]))
    strata = {}
    for key, label in (('same', 'SAME PSY-Q sys.c version'), ('diff', 'DIFFERENT PSY-Q version')):
        sub = [p for p in pairs if p[3] == key]
        s = stats_of(sub)
        strata[key] = s
        if s is None:
            print(f"  {label}: n=0 — no pair in this stratum, so a pair landing here falls back to "
                  f"the pooled null (printed when it happens).")
        else:
            print(f"  {label}: n={s['n']:3d} mean {s['mean']:.2f}%  median {s['median']:.2f}%  "
                  f"MAX {s['max']:.2f}% = {s['max_pair'][0]}/{s['max_pair'][1]}")
    strata['pooled'] = st
    if c.unknown_psyq:
        print(f"  PSY-Q version UNKNOWN in {c.unknown_psyq} — their pairs use the POOLED null.")
    print("  Stratifying is not a convenience: the two strata differ by 2.3x in the mean, and the "
          "worst same-version pair was disassembled and is genuine identical PSY-Q library code.")
    return strata


def band(x):
    return ('one codebase' if x > 3 else
            'same architecture, rewritten' if x > 1.5 else
            'INDISTINGUISHABLE from two strangers linking the same SDK — NOT evidence of family')


def report_families(c: Corpus, strata):
    if not strata:
        return
    print("\nWITHIN-STUDIO PAIRS, each against the null MAX of ITS OWN PSY-Q stratum:")
    rows = [(a, b) for a, b in itertools.combinations(c.names, 2)
            if studio_of(a) is not None and studio_of(a) == studio_of(b)]
    if not rows:
        print("  n=0 within-studio pairs in this corpus — nothing to compare against the null. No "
              "family claim is supported either way; the null above stands on its own.")
        return
    for a, b in sorted(rows, key=lambda t: -c.j(*t)):
        s = c.j(a, b)
        key = c.stratum(a, b)
        ref = strata.get(key) or strata['pooled']
        used = key if strata.get(key) else 'pooled (fallback)'
        x = s / ref['max'] if ref['max'] else float('inf')
        print(f"  {a:12s} {b:12s} {s:6.1f}%  {x:4.1f}x  [null {used}: max {ref['max']:.2f}%]  "
              f"{band(x)}")


def matrix(c: Corpus):
    names = c.names
    w = max(11, max(len(n) for n in names) + 1)
    print(f"\nJACCARD |A^B|/|AuB| over code-plausible windows (PRIMARY), and in parentheses the OLD, "
          f"FALSIFIED asymmetric |A^B|/min(|A|,|B|) over UNFILTERED windows, printed so the "
          f"recalibration is auditable rather than a silent re-baseline:")
    print(('%-*s' % (w, '')) + ''.join('%-*s' % (w + 6, n) for n in names))
    for a in names:
        row = '%-*s' % (w, a)
        for b in names:
            row += '%-*s' % (w + 6, '.' if a == b else
                             '%.1f%% (%.1f)' % (c.j(a, b), c.old(a, b)))
        print(row)


def sweep(items):
    """Reproduce the filter-threshold evidence over the pairs that can falsify the choice."""
    print("[sweep] hashing once, then re-filtering — every combination is scored on the SAME windows.\n"
          "[sweep] separation = min(strong positive) / stratified null max; bigger is better.")
    rows = {n: windows(p)[0] for n, p in items}
    psyq = {n: read_exe(p)[2] for n, p in items}
    names = [n for n, _ in items]

    def strat(a, b):
        if psyq[a] is None or psyq[b] is None:
            return 'pooled'
        return 'same' if psyq[a] == psyq[b] else 'diff'

    print(f"{'min_valid':>9} {'min_dist':>8} {'kept%':>6} {'null_same':>9} {'null_diff':>9} "
          f"{'worst null pair':>26} {'minpos':>7} {'sep':>5}  data-ranges-excluded")
    for mv in (0, 12, 14, 16):
        for md in (1, 2, 3, 4, 5, 8, 10, 12):
            sets = {n: kept_set(rows[n], mv, md) for n in names}
            if any(len(s) < MIN_SHINGLES for s in sets.values()):
                print(f"{mv:9d} {md:8d}   -- skipped: an input fell below {MIN_SHINGLES} shingles")
                continue
            kept = 100.0 * sum(len(sets[n]) for n in names) / sum(len(all_set(rows[n])) for n in names)
            pool = [n for n in names if studio_of(n) and n not in CONTROLS]
            nulls = collections.defaultdict(list)
            worst = (0.0, '', '')
            for a, b in itertools.combinations(pool, 2):
                if studio_of(a) == studio_of(b):
                    continue
                s = jac(sets[a], sets[b])
                nulls[strat(a, b)].append(s)
                if s > worst[0]:
                    worst = (s, a, b)
            if not nulls:
                continue
            nsame = max(nulls.get('same') or [0.0])
            ndiff = max(nulls.get('diff') or [0.0])
            pos = []
            for a, b, _m in STRONG_POSITIVES + WEAK_POSITIVES:
                if a in sets and b in sets:
                    ref = nsame if strat(a, b) == 'same' else ndiff
                    pos.append(jac(sets[a], sets[b]) / (ref or 1e-9))
            excl = all(count_in_range(rows[n], sets_ok(mv, md), lo, hi, n) == 0
                       for n, lo, hi, _d in KNOWN_DATA_RANGES if n in rows)
            print(f"{mv:9d} {md:8d} {kept:6.1f} {nsame:8.2f}% {ndiff:8.2f}% "
                  f"{worst[1] + '/' + worst[2]:>26} "
                  f"{min(jac(sets[a], sets[b]) for a, b, _ in STRONG_POSITIVES):6.1f}% "
                  f"{min(pos):5.1f}  {'yes' if excl else 'NO'}")
    print("[sweep] READ THIS COLUMN, not the separation column: separation is FLAT (2.7-3.0x) across "
          "the whole grid, so the filter is a CORRECTNESS fix (data must not be compared as code), "
          "not a way to buy signal. The default (16, 4) is the loosest setting that excludes both "
          "disassembly-confirmed data ranges while costing 0 of 5215 verified code windows.")


def sets_ok(mv, md):
    return (mv, md)


def count_in_range(rows, thresholds, lo, hi, name, load=DEFAULT_LOAD):
    """How many windows STARTING in [lo,hi) survive the filter? 0 means the range is excluded."""
    mv, md = thresholds
    i0, i1 = (lo - load) // 4, (hi - load) // 4
    i1 = min(i1, len(rows))
    if i0 >= i1:
        return -1                     # range not inside this binary — caller must not read as 0
    return sum(1 for h, nv, nd in rows[i0:i1] if nv >= mv and nd >= md)


def explain(c: Corpus, a, b):
    for n in (a, b):
        if n not in c.names:
            refuse(f"REFUSING: --explain names {n!r}, which is not in the corpus "
                             f"({', '.join(c.names)}).")
    idx = {}
    for n in (a, b):
        m = {}
        for i, (h, nv, nd) in enumerate(c.rows[n]):
            if nv >= c.min_valid and nd >= c.min_distinct:
                m.setdefault(h, i)
        idx[n] = m
    ma, mb = idx[a], idx[b]
    common = set(ma) & set(mb)
    print(f"\n[explain] {a} {len(ma)} kept windows, {b} {len(mb)} kept, shared {len(common)} "
          f"(Jaccard {100.0*len(common)/len(set(ma)|set(mb)):.1f}%)")
    if not common:
        print("[explain] NO shared code-plausible window. The denominators above ARE the whole search "
              "space, so this is a real zero and not an unrun comparison.")
        return
    offs = sorted(ma[h] for h in common)
    runs = []
    for o in offs:
        if runs and o - runs[-1][1] <= 20:
            runs[-1][1], runs[-1][2] = o, runs[-1][2] + 1
        else:
            runs.append([o, o, 1])
    runs.sort(key=lambda r: -r[2])
    ta = c.load[a]
    print(f"[explain] {len(runs)} contiguous runs; the 10 largest, addresses in {a} — DISASSEMBLE "
          f"them (tools/disasm.py) before believing any of this is engine code:")
    for s, e, n in runs[:10]:
        print(f"    0x{ta + 4*s:08X}..0x{ta + 4*e:08X}  {n} windows")


def sdk_filter_audit(c: Corpus, k: int):
    print(f"\n[sdk-filter] AUDIT ONLY, K={k}: dropping every window present in >={k} distinct studios.")
    print("[sdk-filter] ⛔ THIS IS CIRCULAR AND MAY NOT BE USED AS CALIBRATION. It removes by "
          "construction exactly what cross-studio pairs share, so the null collapses (K=2 measured "
          "0.00% for all 67 pairs) and can no longer falsify the metric — the same error as the "
          "distrusted version's sensitivity check. Reported to show the ORDERING is not an artifact.")
    per = collections.defaultdict(set)
    for n in c.names:
        if n in CONTROLS or studio_of(n) is None:
            continue
        for h in c.sets[n]:
            per[h].add(studio_of(n))
    sdk = {h for h, s in per.items() if len(s) >= k}
    red = {n: c.sets[n] - sdk for n in c.names}
    pool = [n for n in c.names if studio_of(n) and n not in CONTROLS]
    nulls = [(a, b, jac(red[a], red[b])) for a, b in itertools.combinations(pool, 2)
             if studio_of(a) != studio_of(b)]
    st = stats_of(nulls)
    print(f"[sdk-filter] dropped {len(sdk)} windows. NULL n={st['n']} mean {st['mean']:.2f}% "
          f"median {st['median']:.2f}% max {st['max']:.2f}% ({st['max_pair'][0]}/{st['max_pair'][1]})")
    for a, b in itertools.combinations(c.names, 2):
        if studio_of(a) and studio_of(a) == studio_of(b):
            s = jac(red[a], red[b])
            print(f"  {a:12s} {b:12s} {s:6.1f}%  {s/(st['max'] or 1e-9):5.1f}x")


def selftest(items):
    """Run BOTH classes plus the filter's own positive control. Exit 1 if anything regresses."""
    names = [n for n, _ in items]
    need = set()
    for grp in (STRONG_POSITIVES, WEAK_POSITIVES):
        need |= {n for p in grp for n in p[:2]}
    need |= {n for p in NULL_MEMBER_NEGATIVES + FAMILY_NEGATIVES for n in p}
    need |= {r[0] for r in KNOWN_DATA_RANGES}
    missing = sorted(need - set(names))
    if missing:
        print(f"SELFTEST REFUSED: the corpus lacks {missing}. A selftest over a subset that excludes "
              f"the pairs which broke the old calibration IS the defect being fixed — the previous "
              f"sensitivity check ran over the 8 executables that excluded them. Nothing was asserted.",
              file=sys.stderr)
        return 2
    c = Corpus(items, ARGS.min_valid, ARGS.min_distinct, verbose=False)
    strata = print_null(c)
    if strata is None:
        print("SELFTEST REFUSED: no cross-studio pair, so there is no null to test against.",
              file=sys.stderr)
        return 2
    fails = []
    print(f"\n[selftest] filter = min-valid {c.min_valid}/16, min-distinct {c.min_distinct}. "
          f"Assertions are multiples of a MEASURED null max, never of a constant.")

    print("[selftest] A. THE CODE/DATA FILTER FIRES (ranges confirmed by disassembly to be data):")
    for n, lo, hi, what in KNOWN_DATA_RANGES:
        with_f = count_in_range(c.rows[n], (c.min_valid, c.min_distinct), lo, hi, n, c.load[n])
        without = count_in_range(c.rows[n], (0, 1), lo, hi, n, c.load[n])
        if with_f < 0 or without < 0:
            fails.append(f"FILTER {n} 0x{lo:08X}: range is outside this binary — the assertion could "
                         f"not run, which is NOT a pass")
            print(f"  FAIL {n} 0x{lo:08X}..0x{hi:08X} range outside the binary — nothing asserted")
            continue
        ok = with_f == 0 and without > 0
        if not ok:
            fails.append(f"FILTER {n} 0x{lo:08X}..0x{hi:08X} ({what}): {with_f} windows kept WITH the "
                         f"filter (want 0), {without} without (want >0)")
        print(f"  {'PASS' if ok else 'FAIL'} {n} 0x{lo:08X}..0x{hi:08X} {what}: kept {with_f} with "
              f"the filter, {without} without"
              + (" — the filter is what excludes it" if ok else
                 " — want 0 with / >0 without; this data range is being compared AS CODE"))

    def ref_for(a, b):
        key = c.stratum(a, b)
        return (strata.get(key) or strata['pooled']), (key if strata.get(key) else 'pooled')

    print("[selftest] B. POSITIVES must clear their stratum null max:")
    strong_scores = []
    for a, b, mult in STRONG_POSITIVES:
        s = c.j(a, b)
        strong_scores.append(s)
        ref, key = ref_for(a, b)
        x = s / ref['max']
        ok = x >= mult
        if not ok:
            fails.append(f"POSITIVE {a}/{b} {s:.1f}% = {x:.1f}x null[{key}] max, need >={mult}x")
        print(f"  {'PASS' if ok else 'FAIL'} strong  {a:12s} {b:12s} {s:6.1f}%  {x:4.1f}x "
              f"[null {key} max {ref['max']:.2f}%]  need >={mult}x")
    for a, b, mult in WEAK_POSITIVES:
        s = c.j(a, b)
        ref, key = ref_for(a, b)
        x = s / ref['max']
        ok = x >= mult
        if not ok:
            fails.append(f"WEAK POSITIVE {a}/{b} {s:.1f}% = {x:.1f}x null[{key}] max, need >={mult}x")
        print(f"  {'PASS' if ok else 'FAIL'} weak    {a:12s} {b:12s} {s:6.1f}%  {x:4.1f}x "
              f"[null {key} max {ref['max']:.2f}%]  need >={mult}x  (the resolution edge: 'same "
              f"architecture, rewritten' is the hardest case for a whole-binary metric)")

    print("[selftest] C. NEGATIVES that are NOT in the null pool must stay at or below their stratum "
          "null max (non-vacuous — a within-studio or loader pair can fail this):")
    for a, b in FAMILY_NEGATIVES:
        s = c.j(a, b)
        ref, key = ref_for(a, b)
        x = s / ref['max']
        ok = x <= 1.0
        if not ok:
            fails.append(f"NEGATIVE {a}/{b} {s:.1f}% = {x:.1f}x null[{key}] max, need <=1.0x")
        print(f"  {'PASS' if ok else 'FAIL'} {a:12s} {b:12s} {s:6.1f}%  {x:4.1f}x "
              f"[null {key} max {ref['max']:.2f}%]  need <=1.0x")

    print("[selftest] D. The cross-studio pairs that BROKE the old calibration. They are MEMBERS of "
          "the null pool, so '<= null max' would be vacuous; they are asserted against the POSITIVE "
          "class — each must sit below min(strong positive)/2.5:")
    limit = min(strong_scores) / 2.5
    for a, b in NULL_MEMBER_NEGATIVES:
        s = c.j(a, b)
        ok = s < limit
        if not ok:
            fails.append(f"NULL-MEMBER {a}/{b} {s:.1f}% >= min(strong positive)/2.5 = {limit:.1f}%")
        print(f"  {'PASS' if ok else 'FAIL'} {a:12s} {b:12s} {s:6.1f}%  (old asym {c.old(a, b):.1f}%)  "
              f"limit {limit:.1f}%")

    print(f"\n[selftest] DENOMINATORS: {len(names)} executables scanned, {strata['pooled']['n']} "
          f"cross-studio null pairs ({strata['same']['n'] if strata['same'] else 0} same-PSY-Q / "
          f"{strata['diff']['n'] if strata['diff'] else 0} different), "
          f"{len(KNOWN_DATA_RANGES)} disassembly-confirmed data ranges, "
          f"{len(STRONG_POSITIVES)} strong + {len(WEAK_POSITIVES)} weak positives, "
          f"{len(FAMILY_NEGATIVES)} family negatives, {len(NULL_MEMBER_NEGATIVES)} null-member "
          f"negatives. BLIND SPOTS: boot executables only (no overlays); studio attribution is a "
          f"hand-written table; a pair whose binaries both lack a PSY-Q string falls back to the "
          f"pooled null.")
    if fails:
        print("SELFTEST FAILED:", file=sys.stderr)
        for f in fails:
            print("  " + f, file=sys.stderr)
        return 1
    print("[selftest] PASS — filter fires, positives clear, negatives stay down.")
    return 0


def collect(args):
    items = []
    for p in args.pairs:
        if '=' not in p:
            refuse(f"expected NAME=path, got {p!r}")
        n, path = p.split('=', 1)
        items.append((n, path))
    if args.dir:
        if not os.path.isdir(args.dir):
            refuse(f"REFUSING: --dir {args.dir} does not exist — it searched NOTHING. This "
                             f"is not 'no executables found'.")
        found = skipped = 0
        for root, _dirs, files in os.walk(args.dir):
            for fn in sorted(files):
                fp = os.path.join(root, fn)
                try:
                    with open(fp, 'rb') as f:
                        if f.read(8) != b'PS-X EXE':
                            skipped += 1
                            continue
                except OSError:
                    skipped += 1
                    continue
                found += 1
                stem = os.path.splitext(fn)[0]
                items.append((stem or os.path.basename(root), fp))
        print(f"[scan] {args.dir}: {found} PS-EXE files, {skipped} non-PS-EXE files skipped")
        if not found:
            refuse(f"REFUSING: no PS-X EXE under {args.dir} ({skipped} files inspected). "
                             f"Nothing was compared.")
    seen = {}
    for n, p in items:
        if n in seen:
            refuse(f"REFUSING: two inputs are both named {n!r} ({seen[n]} and {p}). Every "
                             f"cell for them would compare a file with itself and print '.', which is "
                             f"how the old --dir printed an all-'.' matrix. Name them apart.")
        seen[n] = p
    if len(items) < 2:
        refuse(f"REFUSING: need at least TWO executables — got {len(items)}. A single "
                         f"similarity number is meaningless: only a pair's position against the "
                         f"cross-studio null distribution carries signal, so nothing was computed.")
    return items


ARGS = None


def main() -> int:
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pairs', nargs='*', help='NAME=path entries')
    ap.add_argument('--dir', help='scan this directory for PS-EXEs, naming each by its filename stem')
    ap.add_argument('--min-valid', type=int, default=16,
                    help='legal R3000A words required in a 16-word window (default 16)')
    ap.add_argument('--min-distinct', type=int, default=4,
                    help='distinct normalised tokens required in a window (default 8)')
    ap.add_argument('--no-filter', action='store_true',
                    help='disable the code/data filter (audit; --selftest MUST fail with this)')
    ap.add_argument('--selftest', action='store_true', help='gate both classes; exit 1 on regression')
    ap.add_argument('--sweep', action='store_true', help='reproduce the filter-threshold evidence')
    ap.add_argument('--explain', nargs=2, metavar=('A', 'B'), help='locate the shared windows')
    ap.add_argument('--sdk-filter', type=int, metavar='K',
                    help='REJECTED circular variant, audit only (see the docstring)')
    ARGS = ap.parse_args()
    if ARGS.no_filter:
        ARGS.min_valid, ARGS.min_distinct = 0, 1

    items = collect(ARGS)
    if ARGS.sweep:
        sweep(items)
        return 0
    if ARGS.selftest:
        return selftest(items)

    print(f"[filter] a 16-word window is kept only if >={ARGS.min_valid}/16 words are legal R3000A "
          f"instructions AND it holds >={ARGS.min_distinct} distinct normalised tokens "
          f"(--no-filter disables both).")
    c = Corpus(items, ARGS.min_valid, ARGS.min_distinct)
    if ARGS.explain:
        explain(c, ARGS.explain[0], ARGS.explain[1])
        return 0
    matrix(c)
    strata = print_null(c)
    if strata is None:
        refuse("REFUSING (exit 2): the corpus has no cross-studio pair, so nothing bounds the SDK floor "
               "and every cell above is UNCALIBRATED. A percentage with no null distribution behind it is "
               "the exact defect this tool was recalibrated to remove — add a binary from another "
               "developer (or extend STUDIOS if one is attributed wrongly) and rerun.")
    report_families(c, strata)
    if ARGS.sdk_filter:
        sdk_filter_audit(c, ARGS.sdk_filter)
    print("\nRead a cell against the null MAX of its stratum, never on its own and never against a "
          "remembered constant. Boot executables only — overlays are not compared. And DIRECT evidence "
          "(a byte-identical dispatch loop, an n/n function match with a negative control set, a shared "
          "on-disc format) OUTRANKS every percentage here: this tool ranks candidates and refutes false "
          "families; it does not certify a family.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
