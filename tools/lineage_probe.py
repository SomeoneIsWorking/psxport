#!/usr/bin/env python3
"""lineage_probe.py — WHOLE-FUNCTION + STRING lineage evidence between PS-EXE executables.

WHY THIS EXISTS SEPARATELY FROM `exe_similarity.py`. That tool answers "what fraction of 16-word
opcode-shape windows do two binaries share". It was distrusted on 2026-08-12 because (a) the denominator
was the SMALLER window set, letting a small library-dominated binary score 22-36% against everything, and
(b) DATA inside .text was decoded as instructions, so jump-offset tables counted as matches. It was
RECALIBRATED the same day — Jaccard, a code/data filter, and a null distribution stratified by PSY-Q
version. This tool is the deliberately INDEPENDENT second opinion: it shares no code with it, and its
failure modes differ by construction (whole functions vs shingles; branch offsets KEPT here, masked there).

  WHERE THAT INDEPENDENCE STOPS, and it is not a footnote: the two tools share their CORPUS and their
  STUDIO ATTRIBUTION. Both define "null" as "different developer", from a hand-written table. A wrong
  attribution, or a contaminated corpus member, moves a pair in BOTH tools at once, so their agreement
  cannot detect either. They are independent in FEATURES and CODE, not in CALIBRATION INPUT.

WHAT IT MEASURES, and why each choice removes one of the failure modes above:

  1. WHOLE FUNCTIONS, not sliding windows. .text is segmented at `jr $ra` + delay slot, each block is
     validated as code, normalised, and hashed. A shared unit is therefore "an entire function is
     present in both binaries", which is a direct statement about lineage. A 16-word coincidence
     inside two unrelated functions cannot produce a match.
  2. A CODE/DATA GATE with a stated rejection count. A block is only a function if it ends in
     `jr $ra`, contains no undecodable word, is 6..3000 instructions, is >=55% ordinary integer/branch
     opcodes, and is not dominated by one repeated word. Jump tables, pointer tables and padding fail
     this. The gate PRINTS how many blocks it rejected per binary — a silent filter is a lie. THE
     THRESHOLDS ARE ASSERTED, NOT DERIVED FROM DISASSEMBLED GROUND TRUTH: run `--sweep` to see how far
     the verdicts move across a defensible grid before quoting any single multiple.
  3. EXCLUSIVITY IS FIRST-CLASS. Every function hash is counted across the WHOLE corpus (its SPREAD).
     A unit in 10 of 17 binaries is the Sony SDK; a unit in 2 or 3 is lineage. The headline number for
     a pair is the count of shared functions whose spread is inside a band, and the band is SWEPT
     (<=2, <=3, <=4) with every level printed — because a strict "exactly 2" hides a family of three
     (Crash 2 + Crash 3 + the Warped demo drops CRASH2|CRASH3 from 57 to 41). A BAND IS ONLY MEANINGFUL
     WHEN THE CORPUS IS BIGGER THAN THE BAND: at N <= k+1, "spread <= k" means "shared at all" and the
     statically-linked PSY-Q library is counted as lineage, so that band is SUPPRESSED with its reason
     printed, and a corpus below MIN_CORPUS is REFUSED. This is why the old 2-binary usage is gone: it
     reported TOMBA1|TOMBA2_MAIN as 46 (vs 1 on the 17-binary corpus), i.e. pure SDK read as lineage.
  4. ABSOLUTE COUNTS, NOT A RATIO OF THE SMALLER SET. The primary output is "N shared functions,
     W instruction words" — so a small binary cannot score high by being small. `%min` is printed too,
     but as context, never as the verdict. Every row also prints its multiple of THIS RUN's measured
     cross-studio null max for its own PSY-Q stratum, so no verdict depends on a remembered constant.
  5. AN INDEPENDENT SECOND CHANNEL: exclusive STRING overlap (>=8-char printable runs, e.g. source
     filenames, PSY-Q version strings, format magic, printf formats). Same exclusivity rule. Strings
     have completely different failure modes from disassembly, so agreement between the two channels
     is real corroboration and disagreement is a finding.

TWO NORMALISATIONS, both reported, because they bracket the answer:
  * STRICT — mask only what MUST change when the same object is linked at another address: j/jal
    targets and `lui` immediates. Everything else (registers, branch offsets, struct offsets,
    constants) is compared bit-exact. STRICT matches mean "the same object code".
  * LOOSE  — additionally mask every non-branch 16-bit immediate. Catches the same source recompiled
    against differently-placed globals, at the cost of being easier to collide. Branch offsets stay
    exact in both, so control-flow shape is always compared.

THE BLIND SPOT, stated because a negative that hides its blind spot is a lie: matching is BYTE-EXACT
over a whole normalised block. A function that differs by ONE instruction after masking is INVISIBLE at
STRICT (LOOSE brackets it), and a block shorter than 6 or longer than 3000 instructions is never
compared at all. "They share 1 unit" therefore means "one function survived byte-exact whole-block
comparison", NOT "one function of shared code". Below-the-floor is EVIDENCE OF NOTHING DETECTED, never
evidence of separate engines: this tool places CRASH1|CRASH3 (12) at 1.0x its stratum null max while the
byte-identical GOOL dispatch loop proves they are one architecture. A rewrite keeps the mechanism and
replaces the code, and this method cannot see a mechanism.

REFUSALS (exit 2 — never a clean empty answer; exit 1 is reserved for a --selftest regression):
a `--dir` that does not exist / is a file / lists no PS-EXE (each named separately, with the number of
entries actually listed); a file in `--dir` that is not a PS-EXE (unless `--skip-non-exe`); a duplicate
or colliding NAME; two byte-identical corpus members, or two whose function keysets overlap >=0.90
(a re-extraction, a revision, or one file reachable by two paths — it would push its own family's units
out of every band and report the family as ZERO); fewer than MIN_CORPUS inputs; a missing/unreadable
file; a file without `PS-X EXE` magic; a binary yielding fewer than 50 validated functions (reported as
"segmented almost nothing", NOT as "no similarity"). A pair with zero shared functions is reported WITH
its denominators (functions per side, corpus size, blocks rejected, blocks dropped as too short).

SELF-TEST — `--selftest --dir <corpus>`, and it REFUSES without a corpus, because a green synthetic gate
standing in for an unchecked corpus is the whole defect this tool exists to avoid. Three layers:
  A. SYNTHETIC UNIT layer: a relocated copy of a function must match STRICT, a different constant must
     not, a pointer table must be REJECTED by the gate.
  B. SYNTHETIC PIPELINE layer, run through the real CLI as a subprocess: a 5-binary corpus with one
     planted "library" function present in ALL FIVE and one planted "engine" function in exactly TWO.
     It asserts the engine pair sees the engine function, the library function contributes ZERO to
     every pair at spread<=3, the library pair ranks strictly below the engine pair, and the string
     channel fires on a planted exclusive string while ignoring a planted SDK-shaped one. These are the
     assertions that kill the two mutants the band had no test for: `2 <= len(v)` (upper bound dropped)
     and `len(v) >= 5`. It also asserts the EXIT CODE of seven refusal paths.
  C. CORPUS layer, on the real `--dir`: named positives must clear their own stratum's measured null
     max, named family negatives must stay at or below it, and the separation min(positive)/null-max
     must hold. A gate-threshold change that collapses every figure fails here.

MEASURED 2026-08-12 over a 17-binary corpus (13 titles + Tomba!2's loader as a labelled control +
Digimon World, Vagrant Story and Starfighter Sanvein added purely as extra NULLS), all extracted with
`discdump`. Two cross-promo demos (Crash 3 on the Spyro 1 disc, Spyro 2 on the CTR disc) are excluded
from the matrix because a demo is a third owner and would suppress its own family's exclusivity — their
numbers are published below as SAME-CODE-DIFFERENT-BUILD scale anchors instead of being described.

  THE EXACT COMMAND every figure in this block comes from — one invocation, one corpus:
      python3 tools/lineage_probe.py --dir scratch/lineage2/exes \
              --exclude zDEMO_CRASH1_on_SPYRO1,zDEMO_SPYRO2_on_CTR
  (An earlier version of this docstring mixed channels: its function counts were from this 17-binary
  run and its string counts from the 19-binary run WITH the demos, which is the configuration it itself
  declares invalid for family evidence. Both channels below are from the command above.)

  THE MEASURED CROSS-STUDIO NULL, stratified by PSY-Q `sys.c` version because the sibling tool found the
  null is BIMODAL in it (n excludes within-studio pairs, the TOMBA2_SCUS loader control, and the demos):
      stratum            n    mean  median  zeros   MAX (named)
      pooled            109   1.14     0      62    15  SPYRO1|TOMBA1
      same sys.c         41   1.61     1      15    15  SPYRO1|TOMBA1
      different sys.c    68   0.85     0      47    12  CRASH3|TOMBA1
  (n is 109, not the 123 an earlier note published: 136 pairs - 13 within-studio - 14 involving the
  TOMBA2_SCUS loader, which is a labelled CONTROL and out of the POOL. The tool prints this table itself
  now, so the denominator can never again be derived by hand outside the artifact.)
  The max pair (SPYRO1|TOMBA1) is same-`sys.c` (both 1.129). The cohort census is 1.140 x10, 1.129 x4
  (CRASH2, SPYRO1, TOMBA1, TOYSTORY2), 1.135 x2 (CRASH3, DIGIMON), 1.120 x1 (CRASH1) — so a 1.129
  function has spread 4 and is already excluded at spread<=3 by construction. (A previous note claimed
  "only 2-3 corpus members link that version"; that is FALSE — four do, so that sentence could not have
  explained the 15 units it was offered to explain. The real residual is finer than `sys.c`: TOMBA1 is
  the ONLY `intr.c` 1.74 in the corpus, and POSITIONALLY all 15 units sit in the LAST QUARTER of
  SPYRO1's .text — the linked-library tail — against SPIDER1|SPIDER2's 246 at Q1->107, Q2->92, Q3->47,
  Q4->0. `--focus` prints that quarter distribution, which is the cheapest available separator between
  a library floor and real lineage.) MEDIAN OF THE POOLED NULL IS ZERO and 62 of 109 pairs are exactly
  zero, so "1 unit" is ABOVE ~57% of unrelated pairs — never describe it as "below two random titles".

  SAME-ENGINE pairs against their OWN stratum's null max (STRICT/spread<=3, absolute counts; the
  strings are the same run's spread<=3 STRING channel, whose cross-studio null max is 8):
      SPIDER1|SPIDER2   246   (16.40x, same-1.140 stratum) + 180 exclusive strings
      SPYRO2|SPYRO3      59   ( 3.93x, same-1.140)         +  21 exclusive strings
      CRASH2|CRASH3      57   ( 4.75x, diff: 1.129/1.135)  +  22 exclusive strings
      SPYRO1|SPYRO2/3    27   ( 2.25x, diff: 1.129/1.140)  +   1 exclusive string ("DEMO MODE")
      CRASH1|CRASH2      25   ( 2.08x, diff: 1.120/1.129)  +  23 exclusive strings; at spread<=2 (null
                                          max 5) its 8 strings include the `\\S%X\\S%07X.NSF/.NSD` magic
      CRASH1|CRASH3      12   ( 1.00x — EXACTLY AT its stratum floor, i.e. NOTHING DETECTED. They are
                                          one architecture on direct evidence regardless. This is the
                                          in-corpus proof that "below the floor" can NEVER be read as
                                          evidence of separate engines; see THE BLIND SPOT)
      TOMBA1|TOMBA2       1   (0.08x — nothing detected. A shared codebase reads in the HUNDREDS here,
                                          as SPIDER1|SPIDER2's 246 does; but 1 is the ~57th percentile
                                          of the null, and this tool cannot separate "rewritten, same
                                          architecture" from "unrelated" at that level — CRASH1|CRASH3
                                          is the counterexample one line up)
      CRASHBASH|TOMBA2    3   (0.20x — the pair exe_similarity.py once read as 33.4%)
      TOMBA2_MAIN|TOMBA2_SCUS  41 shared units, of which 1 at spread 3 and the other 40 at spread>=4;
                               98 shared strings, of which 2 at spread 7 and the rest at >=8. So
                               spread<=3 is 1 unit / 32 instruction words (0.07x), NOT the 0 an earlier
                               note claimed with the words "EVERY shared unit sits in >=4 and EVERY
                               string in >=8" — quote the histogram, not a universal. The loader and the
                               engine of ONE GAME share essentially only SDK; their 76.8% on the old
                               metric was generic, and "must come out as the same game" is the wrong
                               expectation — they are two different programs.

  THE SAME-CODE UPPER ANCHOR, measured (19-binary run, demos INCLUDED, STRICT — the numbers an earlier
  version of this docstring described as "measured separately" without publishing them):
      CRASH3|zDEMO_CRASH1_on_SPYRO1   176 at spread<=2, 216 at spread<=3   (the disc's demo IS Crash 3)
      SPYRO2|zDEMO_SPYRO2_on_CTR       14 at spread<=2,  40 at spread<=3
  Two literally-same-game pairs, 5x apart, STRADDLING the family pairs (246 / 59 / 57). That spread is
  the honest scale caveat: a build difference alone moves this metric by 5x, so read band membership
  and the ordering of the extremes, not a decimal place.

  HOW MUCH OF A MULTIPLE IS THE GATE CONSTANTS? `--sweep` over min_insns x min_ordinary, measured:
      min_insns  6/0.55 (default)  6/0.95   12/0.55   20/0.55   40/0.55   40/0.95
      nullmax           15            9        10         7         4         2
      SPIDER1|2        246          208       190       133        63        51
      SPYRO2|3          59           54        36        26         7         6
      CRASH2|3          57           57        27        15         7         7
      TOMBA1|2           1            1         1         0         0         0
      separation      3.80         6.00      2.70      2.14      1.75      3.00
  What is ROBUST across every cell: SPIDER1|SPIDER2 is the top pair by 3-8x, and TOMBA1|TOMBA2_MAIN is
  at 0-1. What is NOT: the separation moves 1.75x-6.00x, and at min_insns=40 CRASH2|CRASH3 OVERTAKES
  SPYRO2|SPYRO3 (7 vs 6) — inverting the mid-table order. So cite this tool for band membership and the
  ordering of the EXTREMES; a mid-table decimal multiple is not a measurement of the binaries, it is
  partly a measurement of these constants.

  WHERE IT DISAGREES WITH exe_similarity.py — recorded, not smoothed over. Both tools agree on the
  TOP TWO pairs and on which pairs fall below their own floor, but NOT on mid-table ORDER:
      SPYRO1|SPYRO2            here 27 (2.25x) vs  there 5.6% (0.8x, BELOW its floor)
      CRASH1|CRASH2            here 25 (2.08x) vs  there 8.2% (1.1x)   -> the two tools INVERT this pair
      TOMBA2_SCUS|TOMBA2_MAIN  here  1 (0.07x) vs  there 8.6% (0.7x, above SPYRO1|SPYRO2 there)
  Also: STRICT separates and LOOSE does not — at LOOSE/spread<=3 the pooled null max rises to 74
  (CRASHBASH|SANVEIN) while CRASH2|CRASH3 is 137, only 1.85x. Read STRICT first; LOOSE only ranks.

USAGE
    python3 tools/lineage_probe.py --dir scratch/lineage2/exes
    python3 tools/lineage_probe.py A=a.exe B=b.exe C=c.exe D=d.exe E=e.exe   # >= MIN_CORPUS inputs
    python3 tools/lineage_probe.py --dir <d> --focus TOMBA1,TOMBA2_MAIN   # one pair, with its
                                                                          # denominators + blind spots
    python3 tools/lineage_probe.py --dir <d> --sweep        # gate-threshold sensitivity (audit)
    python3 tools/lineage_probe.py --dir <d> --selftest     # gates BOTH classes; refuses without --dir

Extract inputs with the framework disc tool (never a game's run.sh):
    cmake --build build --target discdump
    ./build/tools/discdump list "/path/disc.chd"
    ./build/tools/discdump get SCUS_942.28 "/path/disc.chd" <outdir>
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import statistics
import struct
import subprocess
import sys
from collections import Counter, defaultdict

JR_RA = 0x03E00008
MIN_FUNCS = 50
MIN_STR = 8
# A band "spread <= k" only removes library code if the corpus is BIGGER than k+1 (see docstring #3).
# MIN_CORPUS is therefore tied to the headline band (<=3): 3 + 2 = 5.
HEADLINE_BAND = 3
MIN_CORPUS = HEADLINE_BAND + 2
NEAR_DUP_JACCARD = 0.90

# Gate thresholds. ASSERTED, not derived from disassembled ground truth — `--sweep` shows how far the
# verdicts move across a defensible grid, and that sensitivity is part of the result.
DEFAULT_GATE = dict(min_insns=6, max_insns=3000, min_ordinary=0.55, max_repeat=0.45)

# Developer per title. Different developer => shared proprietary engine code is impossible, which is
# what makes every cross-studio pair a member of the null distribution. EXACT names win over substring
# rules (a cross-promo demo's filename names BOTH discs, so substring matching gets it wrong).
# THIS TABLE IS SHARED CALIBRATION INPUT WITH exe_similarity.py's own STUDIOS — see the docstring: a
# wrong attribution here moves a pair in both tools at once and their agreement cannot detect it.
STUDIO_EXACT = {
    'zDEMO_CRASH1_on_SPYRO1': 'NaughtyDog',     # measured: it is a Crash 3 demo (216 units vs CRASH3)
    'zDEMO_SPYRO2_on_CTR': 'Insomniac',
}
STUDIO_SUBSTR = [
    ('CRASHBASH', 'Eurocom'),                   # must precede CRASH
    ('CTR', 'NaughtyDog'),
    ('CRASH', 'NaughtyDog'),
    ('SPYRO', 'Insomniac'),
    ('SPIDER', 'Neversoft'),
    ('TOMBA', 'WhoopeeCamp'),
    ('TOYSTORY', 'TravellersTales'),
    ('TS2', 'TravellersTales'),
    ('VAGRANT', 'Square'),
    # Sole members of their studio in this corpus. The ONLY load-bearing property of these labels is
    # "shares no proprietary engine with any other corpus member", which holds under every candidate
    # developer attribution — so they are labelled as distinct rather than guessed at.
    ('DIGIMON', 'SOLE_DIGIMON'),
    ('SANVEIN', 'SOLE_SANVEIN'),
]
# A LOADER is not an engine, and a DEMO is the same code as its parent title. Both are kept in the
# matrix and asserted by --selftest, but excluded from the NULL POOL: a library-dominated stub or a
# same-code sibling would corrupt the "two strangers" distribution.
CONTROLS = {'TOMBA2_SCUS'}                        # matched case-insensitively
DEMO_CONTROLS = {'ZDEMO_CRASH1_ON_SPYRO1', 'ZDEMO_SPYRO2_ON_CTR'}


def is_control(name: str) -> bool:
    return name.upper() in CONTROLS


def is_demo(name: str) -> bool:
    return name.upper() in DEMO_CONTROLS

# --selftest layer C, on the real corpus. (name_a, name_b, min multiple of that pair's stratum null max)
STRONG_POSITIVES = [('SPIDER1', 'SPIDER2', 3.0), ('SPYRO2', 'SPYRO3', 3.0)]
WEAK_POSITIVES = [('CRASH2', 'CRASH3', 2.0)]          # same architecture, rewritten — resolution edge
# Within-studio or control pairs that must NOT read as family. Not in the null pool => non-vacuous.
# NOTE what is NOT here: SPYRO1|SPYRO2 reads 2.2x its stratum null max on THIS tool while
# exe_similarity.py reads 0.8x. That disagreement is recorded, not asserted away; it is gated as an
# ORDERING assertion below (it must stay under SPYRO2|SPYRO3) instead of a false "<= null max".
FAMILY_NEGATIVES = [('TOMBA1', 'TOMBA2_MAIN'), ('TOMBA2_SCUS', 'TOMBA2_MAIN'), ('CRASH1', 'CTR')]
ORDERING = [('SPYRO1', 'SPYRO2', 'SPYRO2', 'SPYRO3')]  # (a,b) must rank strictly below (c,d)

# ---------------------------------------------------------------------------
# MIPS R3000A decode tables. Only what a PSX compiler/assembler actually emits
# is "valid"; anything else marks the block as data.
# ---------------------------------------------------------------------------
OP_VALID = {
    0x00: 'SPECIAL', 0x01: 'REGIMM', 0x02: 'j', 0x03: 'jal', 0x04: 'beq', 0x05: 'bne',
    0x06: 'blez', 0x07: 'bgtz', 0x08: 'addi', 0x09: 'addiu', 0x0A: 'slti', 0x0B: 'sltiu',
    0x0C: 'andi', 0x0D: 'ori', 0x0E: 'xori', 0x0F: 'lui',
    0x10: 'cop0', 0x11: 'cop1', 0x12: 'cop2', 0x13: 'cop3',
    0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr',
    0x28: 'sb', 0x29: 'sh', 0x2A: 'swl', 0x2B: 'sw', 0x2E: 'swr',
    0x32: 'lwc2', 0x3A: 'swc2',
}
SPECIAL_VALID = {
    0x00, 0x02, 0x03, 0x04, 0x06, 0x07,          # sll srl sra sllv srlv srav
    0x08, 0x09, 0x0C, 0x0D,                      # jr jalr syscall break
    0x10, 0x11, 0x12, 0x13,                      # mfhi mthi mflo mtlo
    0x18, 0x19, 0x1A, 0x1B,                      # mult multu div divu
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,  # add addu sub subu and or xor nor
    0x2A, 0x2B,                                  # slt sltu
}
REGIMM_VALID = {0x00, 0x01, 0x10, 0x11}          # bltz bgez bltzal bgezal
# "ordinary" = the opcodes a normal function body is mostly made of.
ORDINARY_OPS = {0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x03, 0x08, 0x09, 0x0A, 0x0B,
                0x0C, 0x0D, 0x0E, 0x0F, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B}
# instructions whose 16-bit immediate is a PC-relative branch offset (keep it: it is structure)
BRANCH_OPS = {0x04, 0x05, 0x06, 0x07, 0x01}
# control transfer: branches + j/jal (jr/jalr handled via SPECIAL funct in gate())
CONTROL_OPS = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}

PSYQ_RE = re.compile(rb'\$Id: (sys|intr)\.c,v ([0-9.]+)')


def refuse(msg: str):
    """Every refusal exits 2 and says what was NOT scanned — never a clean empty answer.
    Exit 1 is reserved for a --selftest regression, so a wrapper can tell them apart."""
    print(msg, file=sys.stderr)
    sys.exit(2)


def decodable(w: int) -> bool:
    op = (w >> 26) & 0x3F
    name = OP_VALID.get(op)
    if name is None:
        return False
    if op == 0x00:
        return (w & 0x3F) in SPECIAL_VALID
    if op == 0x01:
        return ((w >> 16) & 0x1F) in REGIMM_VALID
    return True


def norm_word(w: int, loose: bool) -> int:
    op = (w >> 26) & 0x3F
    if op in (0x02, 0x03):              # j/jal — absolute target, must be masked
        return op << 26
    if op == 0x0F:                      # lui — high half of an address, must be masked
        return w & 0xFFFF0000
    if loose and op not in BRANCH_OPS and op >= 0x08:
        return w & 0xFFFF0000           # mask struct offsets / constants too
    return w


# ---------------------------------------------------------------------------
# PS-EXE parsing + function segmentation
# ---------------------------------------------------------------------------
def is_psx_exe(path: str) -> bool:
    try:
        with open(path, 'rb') as f:
            return f.read(8) == b'PS-X EXE'
    except OSError:
        return False


def read_exe(path: str) -> tuple[int, bytes, bytes, str | None]:
    """-> (text load address, .text bytes, whole file, header-override note or None).

    An implausible `t_size` is REPORTED, not silently reinterpreted: the fallback scans the whole tail
    of the file, which can include the data segment, and that changes what was measured."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        refuse(f"REFUSING: cannot read {path}: {e} — segmented NOTHING, compared NOTHING.")
    if data[:8] != b'PS-X EXE':
        refuse(f"REFUSING: {path} lacks 'PS-X EXE' magic (first 8 bytes {data[:8]!r}). Nothing was "
               f"segmented; this is NOT a 0-shared-functions result.")
    t_addr, t_size = struct.unpack_from('<II', data, 0x18)
    tail = len(data) - 0x800
    if 0 < t_size <= tail:
        return t_addr, data[0x800:0x800 + t_size], data, None
    note = (f"t_size=0x{t_size:X} implausible for a {len(data)}-byte file "
            f"(tail is {tail} bytes) — scanned the whole {max(tail, 0)}-byte tail as .text, "
            f"which may include the DATA segment")
    return t_addr, data[0x800:], data, note


def gate(ws: list[int], p: dict) -> str | None:
    """Return None if ws is a plausible function, else the REASON it was rejected."""
    n = len(ws)
    if n < p['min_insns']:
        return 'too_short'
    if n > p['max_insns']:
        return 'too_long'
    if ws[-2] != JR_RA:
        return 'no_jr_ra'
    ordinary = 0
    for w in ws:
        if not decodable(w):
            return 'undecodable'
        if ((w >> 26) & 0x3F) in ORDINARY_OPS:
            ordinary += 1
    if ordinary / n < p['min_ordinary']:
        return 'not_ordinary'
    c = Counter(ws)
    top, cnt = c.most_common(1)[0]
    if top != 0 and cnt / n > p['max_repeat']:
        return 'repetitive'
    # Compiled MIPS has short basic blocks (mean ~5-7 insns), so any real function of
    # non-trivial length contains control flow. A pointer/offset table contains none —
    # this is the gate that rejects the `0x80068bd4` jump-offset-table class of match that
    # exe_similarity.py counted as code. Also: a table uses 1-2 distinct opcodes.
    if n >= 20:
        ctl = sum(1 for w in ws if ((w >> 26) & 0x3F) in CONTROL_OPS
                  or (((w >> 26) & 0x3F) == 0 and (w & 0x3F) in (0x08, 0x09)))
        if ctl < 1 + n // 40:
            return 'no_control_flow'
        if len({(w >> 26) & 0x3F for w in ws}) < 4:
            return 'too_few_opcodes'
    return None


def segment(path: str, name: str, p: dict = DEFAULT_GATE) -> dict:
    t_addr, text, raw, hdr_note = read_exe(path)
    nw = len(text) // 4
    ws = list(struct.unpack_from('<%dI' % nw, text, 0))
    ends = [i for i, w in enumerate(ws) if w == JR_RA and i + 1 < nw]
    funcs, rejects = [], Counter()
    prev = 0
    for e in ends:
        s = prev
        while s <= e and ws[s] in (0, JR_RA):         # skip padding / a stray tail
            s += 1
        blk = ws[s:e + 2]
        prev = e + 2
        why = gate(blk, p)
        if why:
            rejects[why] += 1
            continue
        funcs.append((t_addr + s * 4, blk))
    strs = set(m.group(0) for m in re.finditer(rb'[\x20-\x7e]{%d,}' % MIN_STR, raw))
    psyq = {}
    for m in PSYQ_RE.finditer(raw):
        psyq.setdefault(m.group(1).decode(), m.group(2).decode())
    return dict(name=name, path=path, t_addr=t_addr, words=nw, jr_ra_sites=len(ends),
                funcs=funcs, rejects=rejects, strings=strs, hdr_note=hdr_note,
                sha=hashlib.sha256(raw).hexdigest(), size=len(raw), psyq=psyq,
                med_insns=(statistics.median(len(b) for _, b in funcs) if funcs else 0))


def fn_keys(b: dict, loose: bool) -> dict:
    """map normalised-function-key -> (count, total instruction words)"""
    out: dict[bytes, list[int]] = {}
    for addr, blk in b['funcs']:
        key = struct.pack('<%dI' % len(blk), *[norm_word(w, loose) for w in blk])
        e = out.setdefault(key, [0, len(blk)])
        e[0] += 1
    return out


def fn_addr_map(b: dict, loose: bool) -> dict:
    out: dict[bytes, int] = {}
    for addr, blk in b['funcs']:
        key = struct.pack('<%dI' % len(blk), *[norm_word(w, loose) for w in blk])
        out.setdefault(key, addr)
    return out


def studio_of(name: str) -> str | None:
    if name in STUDIO_EXACT:
        return STUDIO_EXACT[name]
    up = name.upper()
    for frag, st in STUDIO_SUBSTR:
        if frag in up:
            return st
    return None


# ---------------------------------------------------------------------------
# corpus assembly — every skipped input is an accounted-for failure, not a filter
# ---------------------------------------------------------------------------
def collect_items(a) -> list[tuple[str, str]]:
    items: list[tuple[str, str]] = []
    if a.dir:
        d = os.path.abspath(a.dir)
        if not os.path.exists(d):
            refuse(f"REFUSING: --dir {d} DOES NOT EXIST. Listed 0 entries, sniffed 0 files, "
                   f"segmented NOTHING. This is not an empty corpus — it is a wrong path.")
        if not os.path.isdir(d):
            refuse(f"REFUSING: --dir {d} is a FILE, not a directory ({os.path.getsize(d)} bytes). "
                   f"Pass it as NAME={a.dir} if you meant it as one corpus member. Listed NOTHING.")
        entries = sorted(os.listdir(d))
        files = [e for e in entries if os.path.isfile(os.path.join(d, e))]
        nondirs = len(entries) - len(files)
        good, bad = [], []
        for e in files:
            p = os.path.join(d, e)
            if is_psx_exe(p):
                # Strip only a real .exe extension. A discdump name like `SLUS_008.75` must NOT be
                # truncated to `SLUS_008` — that hides which revision is in the corpus.
                stem, ext = os.path.splitext(e)
                good.append((stem if ext.lower() == '.exe' else e, p))
            else:
                bad.append(e)
        print(f"DIR {d}: listed {len(entries)} entries ({nondirs} non-file), sniffed {len(files)} "
              f"files, {len(good)} accepted as PS-EXE, {len(bad)} rejected"
              + (f": {', '.join(bad)} (no 'PS-X EXE' magic)" if bad else "."))
        if bad and not a.skip_non_exe:
            refuse(f"REFUSING: {len(bad)} file(s) in {d} are not PS-EXE: {', '.join(bad)}. Every "
                   f"number this tool prints is a function of corpus COMPOSITION (exclusivity and "
                   f"spread), so a silently truncated corpus is a WRONG measurement, not a smaller "
                   f"one. Move them out, or pass --skip-non-exe to accept the {len(good)} that "
                   f"sniffed clean.")
        if not good:
            refuse(f"REFUSING: {d} contains {len(entries)} entries and NOT ONE PS-EXE. Segmented "
                   f"nothing, compared nothing.")
        items += good
    for spec in a.pairs:
        if '=' not in spec:
            refuse(f"REFUSING: '{spec}' is not NAME=path. Nothing was compared.")
        n, p = spec.split('=', 1)
        items.append((n, p))
    seen: dict[str, str] = {}
    for n, p in items:
        if n in seen:
            refuse(f"REFUSING: name {n!r} given twice ({seen[n]} and {p}). A duplicate name would "
                   f"COLLAPSE the two binaries into one keyset — the earlier one's functions would "
                   f"vanish and its pair would be reported as a clean ZERO. Rename one.")
        seen[n] = p
    if a.exclude:
        drop = set(a.exclude.split(','))
        have = {n for n, _ in items}
        if drop - have:
            refuse(f"REFUSING: --exclude names not in corpus: {sorted(drop - have)}")
        items = [(n, p) for n, p in items if n not in drop]
    if len(items) < MIN_CORPUS:
        refuse(f"REFUSING: {len(items)} input(s), MIN_CORPUS is {MIN_CORPUS}. Exclusivity needs a "
               f"CORPUS: at N <= k+1 the band 'spread <= k' just means 'shared at all', so the "
               f"statically-linked PSY-Q library counts as lineage. MEASURED: TOMBA1|TOMBA2_MAIN "
               f"reads 46 units at N=2, 28 at N=4 and 1 at N=17 — the first two are SDK. Nothing "
               f"was compared.")
    return items


def load_corpus(items, a, p=DEFAULT_GATE) -> list[dict]:
    bins = []
    for n, path in items:
        b = segment(path, n, p)
        if len(b['funcs']) < MIN_FUNCS:
            refuse(f"REFUSING: {path} segmented only {len(b['funcs'])} functions from "
                   f"{b['words']} words / {b['jr_ra_sites']} jr-ra sites — that is "
                   f"'segmented almost nothing', NOT 'no similarity'. rejects={dict(b['rejects'])}")
        bins.append(b)
    by_sha: dict[str, str] = {}
    for b in bins:
        if b['sha'] in by_sha:
            refuse(f"REFUSING: {b['name']} and {by_sha[b['sha']]} are BYTE-IDENTICAL "
                   f"(sha256 {b['sha'][:16]}, {b['size']} bytes). A duplicate is a third owner: it "
                   f"pushes its own family's shared functions out of every exclusivity band and "
                   f"reports the family as ZERO — indistinguishable from no lineage. Remove one.")
        by_sha[b['sha']] = b['name']
    ks = [set(fn_keys(b, False)) for b in bins]
    for i in range(len(bins)):
        for j in range(i + 1, len(bins)):
            u = len(ks[i] | ks[j])
            jac = len(ks[i] & ks[j]) / u if u else 0.0
            if jac >= NEAR_DUP_JACCARD:
                msg = (f"{bins[i]['name']} and {bins[j]['name']} share {jac:.3f} of their function "
                       f"keysets (Jaccard) — that is the SAME PROGRAM (a re-extraction, a regional "
                       f"revision, or one file reachable by two paths), not two corpus members.")
                if a.allow_near_duplicates:
                    print(f"WARNING (--allow-near-duplicates): {msg} Every band below is "
                          f"CONTAMINATED: this pair is a third owner for its own family.")
                else:
                    refuse(f"REFUSING: {msg} It would suppress its own family's exclusivity and "
                           f"report that family as ZERO. Remove one, or pass "
                           f"--allow-near-duplicates to measure anyway (loudly stamped).")
    return bins


# ---------------------------------------------------------------------------
# the null distribution, measured in THIS run and stratified by PSY-Q sys.c cohort
# ---------------------------------------------------------------------------
def stratum_of(bins_by_name: dict, x: str, y: str) -> str:
    vx = bins_by_name[x]['psyq'].get('sys')
    vy = bins_by_name[y]['psyq'].get('sys')
    if not vx or not vy:
        return 'unknown_psyq'
    return 'same_psyq' if vx == vy else 'diff_psyq'


def in_null_pool(bins_by_name: dict, x: str, y: str) -> bool:
    if is_control(x) or is_control(y) or is_demo(x) or is_demo(y):
        return False
    sx, sy = studio_of(x), studio_of(y)
    return bool(sx and sy and sx != sy)


def null_stats(bins_by_name, rows) -> dict:
    """rows: list of (count, insn, (a,b)). -> per-stratum stats over CROSS-STUDIO pairs only."""
    buckets: dict[str, list] = defaultdict(list)
    for c, w, pr in rows:
        if not in_null_pool(bins_by_name, *pr):
            continue
        buckets['pooled'].append((c, pr))
        buckets[stratum_of(bins_by_name, *pr)].append((c, pr))
    out = {}
    for k, v in buckets.items():
        vals = [c for c, _ in v]
        mx, mxpr = max(v, key=lambda t: t[0]) if v else (0, ('-', '-'))
        out[k] = dict(n=len(v), mean=(sum(vals) / len(vals) if vals else 0.0),
                      median=(statistics.median(vals) if vals else 0),
                      zeros=sum(1 for x in vals if x == 0), max=mx,
                      max_pair=f"{mxpr[0]}|{mxpr[1]}")
    return out


def print_null(ns: dict, label: str):
    print(f"  MEASURED CROSS-STUDIO NULL for {label} (different developer; the TOMBA2_SCUS loader and "
          f"the demos are excluded from the POOL, not from the matrix):")
    print(f"    {'stratum':<16}{'n':>5}{'mean':>8}{'median':>8}{'zeros':>7}{'MAX':>6}  max pair")
    for k in ('pooled', 'same_psyq', 'diff_psyq', 'unknown_psyq'):
        if k in ns:
            s = ns[k]
            print(f"    {k:<16}{s['n']:>5}{s['mean']:>8.2f}{s['median']:>8g}{s['zeros']:>7}"
                  f"{s['max']:>6}  {s['max_pair']}")
    if not ns:
        print("    (none — no cross-studio pair in this corpus; every multiple below is UNCOMPUTABLE)")
    elif ns['pooled']['n'] < 20:
        print(f"    NULL POOL UNDERPOWERED: n={ns['pooled']['n']} cross-studio pairs. Every `xnull` "
              f"multiple in this band divides by the MAX of that handful, so it cannot support a "
              f"verdict — the recorded figures come from a 109-pair pool. Add unrelated titles (and "
              f"attribute every name in STUDIO_SUBSTR: an UNKNOWN studio is out of the pool).")


def x_null(ns, bins_by_name, pr, c) -> tuple[float | None, str]:
    st = stratum_of(bins_by_name, *pr)
    s = ns.get(st) or ns.get('pooled')
    if not s or not s['max']:
        return None, st
    return c / s['max'], st


# ---------------------------------------------------------------------------
def analyse(bins: list[dict], a, quiet: bool = False) -> dict:
    N = len(bins)
    names = [b['name'] for b in bins]
    by_name = {b['name']: b for b in bins}
    results: dict = {}
    ev = lambda *args, **kw: None if quiet else print(*args, **kw)

    print(f"CORPUS: {N} executables. Every pair count below is out of {N*(N-1)//2} pairs.")
    print(f"CORPUS FINGERPRINT (a published count names the corpus it came from):")
    print(f"{'binary':<24}{'bytes':>9}  sha256[:12]   sys.c   intr.c   studio")
    for b in bins:
        tag = ' [CONTROL]' if is_control(b['name']) else (' [DEMO]' if is_demo(b['name']) else '')
        print(f"{b['name']:<24}{b['size']:>9}  {b['sha'][:12]}  {b['psyq'].get('sys','?'):>6}  "
              f"{b['psyq'].get('intr','?'):>6}   {studio_of(b['name']) or 'UNKNOWN(excluded from null)'}{tag}")
    cohort = Counter(b['psyq'].get('sys', '?') for b in bins)
    print(f"  PSY-Q sys.c cohort census: "
          + ', '.join(f"{v} x{cohort[v]}" for v in sorted(cohort)))
    thin = [v for v, n in cohort.items() if n < 4]
    if thin:
        print(f"  COHORT DENSITY WARNING: cohort(s) {sorted(thin)} have <4 members. Exclusivity can "
              f"only strip a library version the corpus samples densely, so a SAME-COHORT pair inside "
              f"a thin cohort can read library code as lineage. Rows below are stamped `thin` when "
              f"both members sit in such a cohort.")
    unknown = [b['name'] for b in bins if studio_of(b['name']) is None]
    if unknown:
        print(f"  EXCLUDED FROM THE NULL POOL as UNKNOWN STUDIO (add them to STUDIO_SUBSTR): {unknown}")
    print()

    ev(f"{'binary':<24}{'words':>8}{'jr_ra':>7}{'funcs':>7}{'uniq':>7}{'insn':>8}{'med':>5}  rejected(by reason)")
    for b in bins:
        k = fn_keys(b, False)
        ev(f"{b['name']:<24}{b['words']:>8}{b['jr_ra_sites']:>7}{len(b['funcs']):>7}"
           f"{len(k):>7}{sum(len(x[1]) for x in b['funcs']):>8}{int(b['med_insns']):>5}  {dict(b['rejects'])}")
        if b['hdr_note']:
            ev(f"{'':<24}  ^^ HEADER OVERRIDDEN: {b['hdr_note']}")
    hdr_over = [b['name'] for b in bins if b['hdr_note']]
    if hdr_over:
        ev(f"  HEADER-OVERRIDE COUNT: {len(hdr_over)} of {N} binaries had an unusable t_size "
           f"({hdr_over}) — what was scanned as .text is NOT what the header said.")
    short = sum(b['rejects']['too_short'] for b in bins)
    ev(f"  BLIND SPOTS, stated because a negative that hides them is a lie: STRICT compares whole "
       f"blocks BYTE-EXACT after masking j/jal targets and lui immediates, so a function differing by "
       f">=1 instruction is INVISIBLE (LOOSE brackets it). {short} blocks corpus-wide were dropped as "
       f"<{DEFAULT_GATE['min_insns']} instructions and are never compared; median gated function is "
       f"{int(statistics.median([b['med_insns'] for b in bins]))} instructions.")
    ev()

    for loose in (False, True):
        keysets = [fn_keys(b, loose) for b in bins]          # BY INDEX — a name can never overwrite one
        owners: dict[bytes, list[int]] = defaultdict(list)
        for i, kset in enumerate(keysets):
            for k in kset:
                owners[k].append(i)
        label = 'LOOSE' if loose else 'STRICT'
        hist = Counter(len(v) for v in owners.values())
        ev(f"=== {label}: function-hash SPREAD histogram (#binaries containing a unit -> #units)")
        ev('   ' + '  '.join(f"{k}:{hist[k]}" for k in sorted(hist)))
        ev(f"    total distinct units {len(owners)}\n")
        nf = [len(keysets[i]) for i in range(N)]
        # NOTE ON WHY A BAND AND NOT JUST "EXACTLY 2": a function shared by a FAMILY of three
        # (Crash 2, Crash 3 and the Warped demo) is not exclusive-to-2, so a strict ==2 filter
        # reports the real Crash pair as ~0. The band is swept and every level printed, so the
        # reader sees whether a pair's evidence is spread-sensitive instead of being handed one
        # cut-off. A band is only printed when the CORPUS IS BIGGER THAN THE BAND (see below).
        for maxown in (2, 3, 4):
            if N <= maxown + 1:
                ev(f"{label} / spread<={maxown}: SUPPRESSED — corpus is {N} binaries and the band is "
                   f"{maxown}. At N <= band+1, 'spread <= {maxown}' is equivalent to 'shared at all', "
                   f"so statically-linked PSY-Q library code would be counted as lineage. NOTHING was "
                   f"measured for this band; it is not a zero.\n")
                continue
            pair_fn: dict[tuple, list[int]] = defaultdict(lambda: [0, 0])
            for k, v in owners.items():
                if not 2 <= len(v) <= maxown:
                    continue
                w = keysets[v[0]][k][1]
                for i in range(len(v)):
                    for j in range(i + 1, len(v)):
                        pr = tuple(sorted((names[v[i]], names[v[j]])))
                        pair_fn[pr][0] += 1
                        pair_fn[pr][1] += w
            rows = []
            for i in range(N):
                for j in range(i + 1, N):
                    pr = tuple(sorted((names[i], names[j])))
                    c, w = pair_fn.get(pr, (0, 0))
                    rows.append((c, w, pr))
            ns = null_stats(by_name, rows)
            rows.sort(reverse=True)
            nz = [r for r in rows if r[0] > 0]
            ev(f"{label} / spread<={maxown}: pair ranking by shared functions whose owner set "
               f"is 2..{maxown} of {N}")
            ev(f"{'pair':<40}{'fn':>7}{'insn':>9}{'min_fn':>8}{'%min':>7}{'xnull':>8}  stratum")
            for c, w, pr in rows[:a.top]:
                m = min(nf[names.index(pr[0])], nf[names.index(pr[1])])
                mult, st = x_null(ns, by_name, pr, c)
                flag = ' NULLPOOL*' if in_null_pool(by_name, *pr) else ''
                cx, cy = (by_name[pr[0]]['psyq'].get('sys'), by_name[pr[1]]['psyq'].get('sys'))
                if cx and cx == cy and cohort[cx] < 4:
                    flag += ' thin'
                ev(f"{pr[0]+' | '+pr[1]:<40}{c:>7}{w:>9}{m:>8}"
                   f"{(100.0*c/m if m else 0.0):>6.1f}%"
                   f"{(f'{mult:.2f}x' if mult is not None else '   n/a'):>8}  {st}{flag}")
            ev(f"  ...{len(rows)} pairs; {len(nz)} with >0; {len(rows)-len(nz)} with exactly 0. "
               f"ALL-PAIRS DISTRIBUTION (families INCLUDED — NOT a null): median "
               f"{sorted(r[0] for r in rows)[len(rows)//2]}, 90th pct "
               f"{sorted(r[0] for r in rows)[int(len(rows)*0.9)]}")
            if not quiet or (not loose and maxown == HEADLINE_BAND):
                print_null(ns, f"{label}/spread<={maxown}")
            ev("  * NULLPOOL = this pair is a MEMBER of the null distribution above, so its multiple "
               "cannot falsify anything — it helps define the denominator.")
            ev()
            results[f'{label}<={maxown}'] = dict(
                pairs={f"{p[0]}|{p[1]}": dict(fn=c, insn=w) for c, w, p in rows}, null=ns)

    # ---- independent channel: exclusive strings
    sowners: dict[bytes, list[int]] = defaultdict(list)
    for i, b in enumerate(bins):
        for s in b['strings']:
            sowners[s].append(i)
    for maxown in (2, 3):
        if N <= maxown + 1:
            ev(f"=== STRING channel, spread<={maxown}: SUPPRESSED, corpus {N} <= band+1.\n")
            continue
        ps: dict[tuple, list] = defaultdict(list)
        for k, v in sowners.items():
            if not 2 <= len(v) <= maxown:
                continue
            for i in range(len(v)):
                for j in range(i + 1, len(v)):
                    ps[tuple(sorted((names[v[i]], names[v[j]])))].append(k)
        srows = sorted(((len(v), k) for k, v in ps.items()), reverse=True)
        nullmax = max([c for c, pr in srows if in_null_pool(by_name, *pr)], default=0)
        ev(f"=== STRING channel, spread<={maxown}: printable runs >={MIN_STR} chars present in "
           f"2..{maxown} of {N} binaries. Cross-studio null max: {nullmax}")
        ev(f"{'pair':<40}{'strings':>8}  examples")
        for c, pr in srows[:a.top]:
            ex = sorted(ps[pr], key=len, reverse=True)[:3]
            ev(f"{pr[0]+' | '+pr[1]:<40}{c:>8}  " +
               ' ; '.join(x.decode('latin1')[:40] for x in ex))
        ev(f"  ...{len(srows)} pairs have >0; {N*(N-1)//2-len(srows)} have exactly 0. "
           f"Corpus: {len(sowners)} distinct strings.\n")
        results[f'STRINGS<={maxown}'] = dict(pairs={f"{p[0]}|{p[1]}": c for c, p in srows},
                                             null_max=nullmax)

    for spec in a.focus:
        focus(bins, names, by_name, spec)
    return results


def focus(bins, names, by_name, spec):
    N = len(bins)
    try:
        x, y = spec.split(',')
    except ValueError:
        refuse(f"REFUSING: --focus '{spec}' is not A,B. Nothing was reported.")
    if x not in names or y not in names:
        refuse(f"REFUSING: --focus {spec}: not in corpus {names}.")
    if x == y:
        refuse(f"REFUSING: --focus {spec} is a pair with itself. Nothing was reported.")
    bx, byy = by_name[x], by_name[y]
    print(f"\n=== FOCUS {x} | {y}   ({bx['psyq'].get('sys','?')} vs {byy['psyq'].get('sys','?')} sys.c, "
          f"stratum {stratum_of(by_name, x, y)})")
    for loose in (False, True):
        ks = [fn_keys(b, loose) for b in bins]
        own: dict[bytes, list[int]] = defaultdict(list)
        for i, kk in enumerate(ks):
            for k in kk:
                own[k].append(i)
        ix, iy = names.index(x), names.index(y)
        shared = [k for k in ks[ix] if k in ks[iy]]
        by = Counter(len(own[k]) for k in shared)
        lab = 'LOOSE' if loose else 'STRICT'
        print(f"  {lab}: {x} has {len(ks[ix])} distinct fns, {y} has {len(ks[iy])}; "
              f"they share {len(shared)} units.")
        print(f"    those {len(shared)} units by corpus spread (#of {N} binaries containing it): "
              + (', '.join(f'{k}->{by[k]}' for k in sorted(by)) or '(none)'))
        for band in (2, 3, 4):
            if N <= band + 1:
                print(f"    spread<={band}: SUPPRESSED (corpus {N} <= band+1; nothing measured)")
                continue
            lo = [k for k in shared if len(own[k]) <= band]
            note = ''
            if band == HEADLINE_BAND and lo:
                amap = fn_addr_map(bx, loose)
                tw = max(bx['words'], 1)
                qs = Counter()
                for k in lo:
                    if k in amap:
                        off = (amap[k] - bx['t_addr']) // 4
                        qs[min(3, off * 4 // max(tw, 1))] += 1
                note = ("  quarter-of-.text of " + x + ": "
                        + ', '.join(f'Q{i+1}->{qs[i]}' for i in sorted(qs)))
                med = statistics.median([ks[ix][k][1] for k in lo])
                note += f"; median matched unit {int(med)} insns vs corpus median {int(bx['med_insns'])}"
            print(f"    spread<={band}: {len(lo)} units, {sum(ks[ix][k][1] for k in lo)} "
                  f"instruction words{note}")
        if not shared:
            print(f"    NEGATIVE, and here is what was actually examined: "
                  f"{len(bx['funcs'])} vs {len(byy['funcs'])} gated functions, corpus {N}, "
                  f"{bx['rejects']['too_short']}+{byy['rejects']['too_short']} blocks dropped as "
                  f"too short to compare. This is 'no whole function matched BYTE-EXACT', NOT "
                  f"'nothing was scanned' — a function differing by one instruction is invisible here.")
    sx2 = set(bx['strings']) & set(byy['strings'])
    sown: dict[bytes, int] = Counter()
    for b in bins:
        for st in b['strings']:
            sown[st] += 1
    bys = Counter(sown[st] for st in sx2)
    print(f"  STRINGS: share {len(sx2)}; by spread: "
          + (', '.join(f'{k}->{bys[k]}' for k in sorted(bys)) or '(none)'))
    ex = sorted((st for st in sx2 if sown[st] <= 3), key=len, reverse=True)[:8]
    print("    spread<=3 examples: " + (' ; '.join(e.decode('latin1')[:52] for e in ex) or '(none)'))


# ---------------------------------------------------------------------------
# gate-threshold sensitivity: the verdict multiples are a FUNCTION of these constants
# ---------------------------------------------------------------------------
def sweep(items, a):
    print("=== GATE SENSITIVITY SWEEP (audit). The thresholds are ASSERTED, not derived from "
          "disassembled ground truth, so this is how far a defensible change moves the verdicts.")
    print(f"{'min_insns':>10}{'min_ordinary':>14}{'nullmax':>9}{'SPIDER':>8}{'SPY2|3':>8}"
          f"{'CR2|3':>8}{'TOM1|2':>8}{'separation':>12}")
    for mi in (6, 12, 20, 40):
        for mo in (0.55, 0.75, 0.95):
            p = dict(DEFAULT_GATE, min_insns=mi, min_ordinary=mo)
            try:
                bins = load_corpus(items, a, p)
            except SystemExit:
                print(f"{mi:>10}{mo:>14.2f}   REFUSED at this cell (see message above) — a "
                      f"threshold this aggressive segments almost nothing")
                continue
            names = [b['name'] for b in bins]
            by_name = {b['name']: b for b in bins}
            keysets = [fn_keys(b, False) for b in bins]
            owners: dict[bytes, list[int]] = defaultdict(list)
            for i, kset in enumerate(keysets):
                for k in kset:
                    owners[k].append(i)
            pair: Counter = Counter()
            for k, v in owners.items():
                if not 2 <= len(v) <= HEADLINE_BAND:
                    continue
                for i in range(len(v)):
                    for j in range(i + 1, len(v)):
                        pair[tuple(sorted((names[v[i]], names[v[j]])))] += 1
            rows = [(pair.get(tuple(sorted((names[i], names[j]))), 0), 0,
                     tuple(sorted((names[i], names[j]))))
                    for i in range(len(names)) for j in range(i + 1, len(names))]
            ns = null_stats(by_name, rows)
            nm = ns.get('pooled', {}).get('max', 0)
            g = lambda x, y: pair.get(tuple(sorted((x, y))), 0)
            pos = [g('SPIDER1', 'SPIDER2'), g('SPYRO2', 'SPYRO3'), g('CRASH2', 'CRASH3')]
            sep = (min(pos) / nm) if nm else float('inf')
            print(f"{mi:>10}{mo:>14.2f}{nm:>9}{pos[0]:>8}{pos[1]:>8}{pos[2]:>8}"
                  f"{g('TOMBA1','TOMBA2_MAIN'):>8}{sep:>12.2f}")
    print("  separation = min(the three family positives) / pooled cross-studio null max. A verdict "
          "that only holds at one cell of this grid is not a verdict.")


# ---------------------------------------------------------------------------
# self-test — three layers; see the docstring. Exit 1 = regression, 2 = refused to measure.
# ---------------------------------------------------------------------------
def _exe(words: list[int], t_addr: int) -> bytes:
    h = bytearray(0x800)
    h[0:8] = b'PS-X EXE'
    struct.pack_into('<II', h, 0x18, t_addr, len(words) * 4)
    return bytes(h) + struct.pack('<%dI' % len(words), *words)


def _fn(seed: int, hi: int) -> list[int]:
    # addiu sp,sp,-24 / sw ra / lui a0,hi / jal / nop / li v0,seed / addu v1 / lw ra / jr ra / addiu sp
    return [0x27BDFFE8, 0xAFBF0014, 0x3C040000 | hi, 0x0C000000 | (hi << 4),
            0x00000000, 0x24020000 | (seed & 0xFFFF), 0x00621821, 0x8FBF0014,
            JR_RA, 0x27BD0018] * 3


def _run(argv: list[str]) -> tuple[int, str]:
    r = subprocess.run([sys.executable, os.path.abspath(__file__)] + argv,
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def selftest_synthetic(chk) -> None:
    import tempfile
    A = _fn(0x1234, 0x8005)
    B = _fn(0x1234, 0x8009)          # same function, relocated -> MUST match STRICT
    C = _fn(0x9999, 0x8005)          # different constant -> STRICT differs, LOOSE matches
    table = [0x80068bd4 + 4 * i for i in range(200)] + [JR_RA, 0]
    here = os.path.dirname(os.path.abspath(__file__))
    print("[selftest] A. UNIT layer: the matcher fires, and the data gate rejects data.")
    with tempfile.TemporaryDirectory(dir=here) as d:
        pa, pb, pc = (os.path.join(d, n) for n in ('a.exe', 'b.exe', 'c.exe'))
        open(pa, 'wb').write(_exe(A * 20, 0x80010000))
        open(pb, 'wb').write(_exe(B * 20, 0x80200000))
        open(pc, 'wb').write(_exe(C * 20 + table, 0x80010000))
        sa, sb, sc = (segment(p, n) for p, n in ((pa, 'A'), (pb, 'B'), (pc, 'C')))
        ka, kb, kc = (set(fn_keys(s, False)) for s in (sa, sb, sc))
        la, lc = set(fn_keys(sa, True)), set(fn_keys(sc, True))
        chk('A segmented >0 functions', len(sa['funcs']) > 0)
        chk('relocated copy matches STRICT', bool(ka & kb))
        chk('different constant differs STRICT', not (ka & kc))
        chk('different constant matches LOOSE', bool(la & lc))
        chk('pointer table REJECTED by gate', sc['rejects']['no_control_flow'] +
            sc['rejects']['too_few_opcodes'] + sc['rejects']['not_ordinary'] +
            sc['rejects']['undecodable'] + sc['rejects']['repetitive'] > 0)

    # ---- B. PIPELINE layer, through the real CLI: the EXCLUSIVITY BAND has to discriminate.
    print("[selftest] B. PIPELINE layer: the spread band must separate a planted 2-owner ENGINE "
          "function from a planted 5-owner LIBRARY function, through main().")
    with tempfile.TemporaryDirectory(dir=here) as d:
        lib = _fn(0x0777, 0x8001)                      # planted in ALL FIVE -> spread 5, NOT lineage
        eng = _fn(0x0abc, 0x8002)                      # planted in E1+E2 only -> spread 2, lineage
        SDK_S = b'$Id: selftest_sdk.c,v 1.140 fake sony library string'
        EXC_S = b'ENGINE-ONLY-SELFTEST-STRING-not-in-any-other-member'
        paths = {}
        for i, nm in enumerate(['E1', 'E2', 'L1', 'L2', 'L3']):
            body = list(lib)
            for j in range(60):                        # unique filler so MIN_FUNCS is cleared
                body += _fn(0x1000 + i * 100 + j, 0x8003)
            if nm in ('E1', 'E2'):
                body += eng
            blob = bytearray(_exe(body, 0x80010000))
            blob += SDK_S + b'\0'
            if nm in ('E1', 'E2'):
                blob += EXC_S + b'\0'
            p = os.path.join(d, nm + '.exe')
            open(p, 'wb').write(bytes(blob))
            paths[nm] = p
        rc, out = _run(['--dir', d, '--top', '40', '--json', os.path.join(d, 'r.json')])
        chk('synthetic corpus runs clean', rc == 0)
        try:
            res = json.load(open(os.path.join(d, 'r.json')))['results']
        except Exception as e:
            chk(f'synthetic json readable ({e})', False)
            res = {}
        b3 = res.get('STRICT<=3', {}).get('pairs', {})
        b2 = res.get('STRICT<=2', {}).get('pairs', {})
        get = lambda t, k: (t.get(k) or t.get('|'.join(reversed(k.split('|')))) or {'fn': -1})['fn']
        chk('ENGINE pair sees the planted 2-owner function at spread<=2',
            get(b2, 'E1|E2') >= 1)
        chk('LIBRARY-only pair is ZERO at spread<=3 (5-owner unit excluded by the band)',
            get(b3, 'L1|L2') == 0 and get(b3, 'L1|L3') == 0)
        chk('LIBRARY pair ranks strictly BELOW the engine pair at spread<=3',
            get(b3, 'L1|L2') < get(b3, 'E1|E2'))
        s2 = res.get('STRINGS<=2', {}).get('pairs', {})
        chk('STRING channel fires on the planted exclusive string',
            (s2.get('E1|E2') or s2.get('E2|E1') or 0) >= 1)
        chk('STRING channel does NOT attribute the 5-owner SDK string to a pair',
            all(v == 0 for k, v in s2.items() if k not in ('E1|E2', 'E2|E1')))

        # ---- refusal paths: each must exit 2, and say what was NOT scanned.
        print("[selftest] B2. REFUSALS must exit 2 (1 is reserved for a regression).")
        missing = os.path.join(d, 'nope')
        trunc = os.path.join(d, 'trunc.exe')
        open(trunc, 'wb').write(_exe(_fn(1, 0x8001), 0x80010000))
        notexe = os.path.join(d, 'zz.txt')
        open(notexe, 'wb').write(b'not an exe at all')
        one = [f'A={paths["E1"]}']
        cases = [
            ('missing --dir', ['--dir', missing], 'DOES NOT EXIST'),
            ('--dir is a file', ['--dir', paths['E1']], 'is a FILE'),
            ('non-PS-EXE in --dir', ['--dir', d], 'are not PS-EXE'),
            ('one input', one, 'MIN_CORPUS'),
            ('below MIN_CORPUS (3)', [f'A={paths["E1"]}', f'B={paths["E2"]}',
                                      f'C={paths["L1"]}'], 'MIN_CORPUS'),
            ('duplicate NAME', [f'X={paths["E1"]}', f'X={paths["E2"]}', f'C={paths["L1"]}',
                                f'D={paths["L2"]}', f'E={paths["L3"]}'], 'given twice'),
            ('sub-MIN_FUNCS binary', [f'A={paths["E1"]}', f'B={paths["E2"]}', f'C={paths["L1"]}',
                                      f'D={paths["L2"]}', f'T={trunc}'], 'segmented only'),
            ('bad magic', [f'A={paths["E1"]}', f'B={paths["E2"]}', f'C={paths["L1"]}',
                           f'D={paths["L2"]}', f'Z={notexe}'], "lacks 'PS-X EXE'"),
            ('byte-identical member', [f'A={paths["E1"]}', f'A2={paths["E1"]}', f'C={paths["L1"]}',
                                       f'D={paths["L2"]}', f'E={paths["L3"]}'], 'BYTE-IDENTICAL'),
        ]
        for label, argv, needle in cases:
            rc, out = _run(argv)
            chk(f'refusal exits 2 + names it: {label}', rc == 2 and needle in out)
        # a suppressed band is not a zero
        rc, out = _run([f'A={paths["E1"]}', f'B={paths["E2"]}', f'C={paths["L1"]}',
                        f'D={paths["L2"]}', f'E={paths["L3"]}'])
        chk('vacuous band spread<=4 is SUPPRESSED at N=5, not printed as a number',
            rc == 0 and 'spread<=4: SUPPRESSED' in out)


def selftest_corpus(bins, a, chk) -> None:
    res = analyse(bins, a, quiet=True)
    band = f'STRICT<={HEADLINE_BAND}'
    pairs = res[band]['pairs']
    ns = res[band]['null']
    by_name = {b['name']: b for b in bins}
    names = set(by_name)
    get = lambda x, y: (pairs.get(f'{x}|{y}') or pairs.get(f'{y}|{x}') or {}).get('fn')
    need = {n for t in STRONG_POSITIVES + WEAK_POSITIVES for n in t[:2]}
    need |= {n for t in FAMILY_NEGATIVES for n in t}
    need |= {n for t in ORDERING for n in t}
    missing = sorted(need - names)
    if missing:
        refuse(f"SELFTEST REFUSED: the corpus lacks {missing}. A selftest over a subset that omits "
               f"the named positives/negatives cannot fail for a real regression, so it would "
               f"certify an UNCHECKED corpus. Point --dir at the full corpus.")
    print(f"[selftest] C. CORPUS layer on {len(bins)} binaries, band {band}. Null maxima: "
          + ', '.join(f"{k}={v['max']}({v['max_pair']})" for k, v in ns.items()))
    for x, y, mult in STRONG_POSITIVES + WEAK_POSITIVES:
        c = get(x, y)
        m, st = x_null(ns, by_name, (x, y), c)
        chk(f'POSITIVE {x}|{y} = {c} clears {mult}x its {st} null max '
            f'(measured {m if m is None else round(m,2)}x)', m is not None and m >= mult)
    for x, y in FAMILY_NEGATIVES:
        c = get(x, y)
        s = ns.get(stratum_of(by_name, x, y)) or ns['pooled']
        chk(f'FAMILY-NEGATIVE {x}|{y} = {c} stays <= its stratum null max {s["max"]}',
            c is not None and c <= s['max'])
    nullmax = ns['pooled']['max']
    minpos = min(get(x, y) for x, y, _ in STRONG_POSITIVES)
    chk(f'SEPARATION min(strong positive)={minpos} > 3x pooled null max {nullmax}',
        nullmax > 0 and minpos > 3 * nullmax)
    for xa, ya, xb, yb in ORDERING:
        chk(f'ORDERING {xa}|{ya} ({get(xa,ya)}) ranks below {xb}|{yb} ({get(xb,yb)})',
            get(xa, ya) < get(xb, yb))
    smax = res[f'STRINGS<={HEADLINE_BAND}']['null_max']
    sp = res[f'STRINGS<={HEADLINE_BAND}']['pairs']
    sv = sp.get('SPIDER1|SPIDER2') or sp.get('SPIDER2|SPIDER1') or 0
    chk(f'STRING channel: SPIDER1|SPIDER2 {sv} exceeds cross-studio string null max {smax}',
        sv > smax)


def selftest(a) -> int:
    fails: list[str] = []

    def chk(label, good):
        print(f"  [{'PASS' if good else 'FAIL'}] {label}")
        if not good:
            fails.append(label)

    selftest_synthetic(chk)
    if not a.dir and not a.pairs:
        if fails:
            return selftest_summary(fails)     # a regression outranks the refusal: exit 1, not 2
        print("SELFTEST: synthetic layers A+B PASS — and that is NOT a pass.")
        refuse("SELFTEST REFUSED: the synthetic layers ran, but NO CORPUS was given, so NOTHING "
               "recorded in the docstring was checked. A green synthetic gate standing in for an "
               "unchecked corpus is the defect this tool exists to avoid. Re-run with "
               "--selftest --dir <corpus>.")
    bins = load_corpus(collect_items(a), a)
    selftest_corpus(bins, a, chk)
    return selftest_summary(fails)


def selftest_summary(fails) -> int:
    print('SELFTEST', 'PASS' if not fails else f'FAIL ({len(fails)}): ' + '; '.join(fails))
    return 0 if not fails else 1


# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pairs', nargs='*', help='NAME=path entries')
    ap.add_argument('--dir', help='every PS-EXE in this dir, named by its filename stem')
    ap.add_argument('--json', help='write the full result table here')
    ap.add_argument('--top', type=int, default=25, help='rows of the pair ranking to print')
    ap.add_argument('--selftest', action='store_true', help='gate both classes; needs --dir')
    ap.add_argument('--sweep', action='store_true',
                    help='gate-threshold sensitivity of the verdicts (audit)')
    ap.add_argument('--focus', action='append', default=[],
                    help='A,B — dump the spread breakdown of every unit this pair shares. '
                         'Repeatable. Prints the negative WITH its denominators.')
    ap.add_argument('--exclude', default='', help='comma-separated names to drop from the corpus')
    ap.add_argument('--skip-non-exe', action='store_true',
                    help='accept a --dir holding non-PS-EXE files (they are still listed)')
    ap.add_argument('--allow-near-duplicates', action='store_true',
                    help='measure anyway when two members are the same program (loudly stamped)')
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest(a)

    items = collect_items(a)
    if a.sweep:
        sweep(items, a)
        return 0
    bins = load_corpus(items, a)
    results = analyse(bins, a)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(dict(corpus=[dict(name=b['name'], funcs=len(b['funcs']), words=b['words'],
                                        size=b['size'], sha256=b['sha'], psyq=b['psyq'],
                                        studio=studio_of(b['name']), hdr_note=b['hdr_note'],
                                        rejects=dict(b['rejects'])) for b in bins],
                           results=results), f, indent=1)
        print(f"\nwrote {a.json}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
