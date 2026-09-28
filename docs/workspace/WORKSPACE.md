# `~/repo/psx` — the PSX-port WORKSPACE

**This file is a MAP AND A POINTER, nothing else.** It lives in the psxport repo so it survives a machine
switch and reaches every game tree through that repo's `external/psxport` submodule; the workspace
`AGENTS.md` and `CLAUDE.md` entries are symlinks to it. The workspace directory itself is not a git
repo and holds nothing durable.

All of these live in the psxport repo, so they reach every game tree and every subagent by `grep`:

| read this | for |
|---|---|
| **`psxport/AGENTS.md`** | **how a game consumes the framework** — the per-Core Lightrec/native seam, state/exits, image-scoped calls, invalidation, RE-first, diagnostics, and registries. THE authority |
| **`docs/workspace/PROTOCOL.md`** | the multi-agent protocol (area claims) and the standing rules |
| `docs/codemap.md` | current responsibility ownership and placement |
| `docs/findings/*.md` | measured findings, and the incidents the rules came from |
| `<game>/AGENTS.md` | that game's own specifics — the authority for that repo (its `CLAUDE.md` is a symlink) |

## What is here

Independent repos live side by side, all public under `github.com/SomeoneIsWorking`. No workspace repo
and no superproject: a game must build from a bare clone of itself, a gitlink at this level would churn on
every game commit, and a recursive clone would pull seven copies of psxport + beetle-psx.

### MEASURED 2026-09-27 — the horizontal projection parameter is GAMEPLAY STATE in at least one title

**`vs_main_projectionDistance` is not only a projection parameter in Vagrant Story.** It is `0x8005E248`
(verified against the decomp's own `symbol_addrs.txt:798`), and two BATTLE functions BRANCH on it:

    146C.c:4209   if (vs_main_projectionDistance < 272) {
    146C.c:4266   if (vs_main_projectionDistance > 272) {

**Retail's resting value is `0x100` = 256, BELOW that threshold.** So a widening implemented by raising the
horizontal projection past 272 would **flip a gameplay decision**, and the same word also scales GTE fog
(`SetFogNear(768, vs_main_projectionDistance)`). Vagrant's owner therefore widens the CANVAS and never `H`,
and `guestWidescreenProjection()` is deliberately not overridden there at all — the absence is the
enforcement, asserted by a test. See `vagrant/docs/issues/0037`.

**This is the opposite of Crash 1**, where the horizontal bound turned out to BE the GTE near plane
(`H < Z < 12000`). Two titles, two different hazards from the same "widen the projection" instruction, which
is why the hazard has to be measured per title rather than assumed.

**METHOD REQUIREMENT, and only ONE title has been checked.** Before a title's widening touches any
gameplay-visible scalar — the horizontal projection, the near plane, the clip, a distance word — that scalar
must be searched for GAMEPLAY READS (a branch, a compare, a threshold), not just for render reads. A
literal-immediate scan does not find these: Crash 1's bound is a main-RAM global reached by `lui` plus a
16-bit displacement, and Vagrant's is a named global read by ordinary C. **Do not read "no cull" as "safe to
widen".** Only Vagrant Story has been checked this way; the others have not, and a loose grep is not a
substitute — the one that was tried here matched a camera variable, not a projection scalar.

### MEASURED 2026-09-27 — three titles' 60 fps SCOPE IS UNKNOWN, and I asserted it anyway

**Correction first, because I got this wrong in conversation and nearly acted on it.** A survey of the
per-title docs produced `ctr 60 fps`, `spider1 60fps`, `vagrant 60fps`, and I was about to use that to
declare all three outside lerp scope. **It is false.** Every one of those strings is a GOAL, not a
measurement — `ctr/docs/project-goals.md:59` "## G003 — Interpolated presentation at 60 fps and above",
`spider1/docs/project-goals.md:54` "The 60fps outcome is true interpolation", `vagrant/docs/project-goals.md:35`
"the only 60fps difference is insertion of the additional lerped presentation". Reading a goal as a
measurement is the same error as arming the store observer on a data address, and it is the reason this
paragraph exists.

**What is actually recorded, by method — the VSync argument, which IS a field count:**

| title | `VSync(n)` sites found in its docs and sources | verdict |
|---|---|---|
| `vagrant` | `VSync(2)` x3, `VSync(3)` x2, plus 0 and -1 | **leads to 30 fps** (2 fields/frame) |
| `ctr` | `VSync(2)` x1, plus 0 and -1 | **leads to 30 fps**, one site only |
| `spider1` | `VSync(0)` x3, `VSync(-1)` x7 — **no waiting call at all** | **unknown**, and see below |

The method is established, not assumed: `crashbash` is now MEASURED at 2 fields per game frame
(`crashbash/docs/issues/0031`), and it was established by reading the argument reaching the display owner
and by the wait routine branching to NO-WAIT for `a0 == 1` and `a0 <= 0` and only waiting for `a0 >= 2`.
**A `VSync(0)` or `VSync(-1)` site therefore says nothing about rate** — 0 and -1 are the return-current and
query modes. `spyro` is a confirmed 30 fps title and has ONLY `VSync(0)` and `VSync(-1)`, which is exactly
why `spider1` having no waiting call proves nothing either way.

**So: `vagrant` and `ctr` each have ONE lead toward 30 fps, and `spider1` has none. None of the three has a
measured rate, and all three carry an interpolation goal whose scope depends on it.** `spider1` has a
provisioned image, so its rate is measurable and simply has not been measured. `ctr` and `vagrant` have no
disc image on this machine, so for those two the rate cannot be established from the retail binary at all
until media is provisioned — **which also means the two remaining widescreen gaps and this scope question
share one blocker.**

**Do not add an interpolation path to, or rule one out of, any of these three until the rate is measured.**

### MEASURED 2026-09-28 — all three are now measured, and NONE of them is a 60 fps title

The paragraph above is now **SUPERSEDED on its facts, and its blocker is gone**: the images it said were
absent are provisioned (see the provisioning table further down this file). Each answer is read from guest
bytes, each carries a registered instrument, and each instrument's selftest is shown red on its own subject.

| title | fields per game frame | fps | lerp scope | how established |
|---|---|---|---|---|
| `vagrant` | **2, or 4 at run time — never 1** | **30 or 15** | **IN** | `vs_gametime_tickspeed` `0x8005E24C` is the argument; 368,826 words scanned across the resident + 3 overlays, 10 writers, and the public setter `func_8007C36C` admits **only 2 and 4** — so 60 fps is structurally impossible, not merely unobserved. `vagrant/docs/issues/0039` |
| `ctr` | **2** | **30** | **IN** | a two-field countdown at `[gp+0x348]`, armed with a literal 2 and drained by 1 per field by the vblank callback; **exactly 4** accesses in 128,512 words, and the census refuses at 5. `ctr/docs/issues/0030` |
| `spider1` | **NOT ESTABLISHED** | **neither 30 nor 60** | **UNDECIDED — do not start and do not rule out** | it does not pace through VSync at all; the frame body's second wait site is inside a `DrawSync(1)` back-edge loop, so the per-frame field count is not a compile-time constant. `spider1/docs/issues/0030` |

**No title in this workspace that has been measured is already 60 fps**, so no title is currently excluded
from lerp scope on rate grounds. That is a real answer, not a dead end.

**TWO OF THE THREE EARLIER NUMBERS HAD THE WRONG REASON, AND ONE HAD A WRONG COUNT.**

- `vagrant`'s `VSync(2)` x3 / `VSync(3)` x2 was a census of **decompiled source text**, not of the image, and
  it mixed CD/sound wait loops into a rate question. The rate-bearing argument is exactly one variable with
  two legal values. `VSync(3)` does not pace anything in this image at all.
- `ctr`'s single `VSync(2)` is a **boot resource load** (`FUN_80031FDC`'s `param_5 == -1` branch), not the
  frame loop. The 30 fps number was right and the reasoning was wrong. The frame loop's only VSync is
  `VSync(0)`.
- `spider1`'s `VSync(-1)` x7 was a **`jal`-only** census. This image reaches its routines through
  `jalr $ra,$vN` function pointers, so a `jal`-only view reports **0 for every library routine** — including
  VSync. The zero was a property of the scan, not of the title.

**THE `n >= 2` RULE WAS RE-ESTABLISHED PER TITLE, NOT INHERITED.** Each of the three reads its own VSync's
argument branches out of its own image (`bgez a0` / `beq a0,1` / `blez a0` for the no-wait cases,
`addiu a1,a0,-1` for `a0 >= 2`, and a wait helper that returns at once on a count of 0). All three agree
with `crashbash`, which makes it a corroboration across five images rather than a rule transported once.

**WHAT WOULD SETTLE `spider1`**, named precisely because "cannot be determined" is only a real result if the
next step is concrete: a headless run reporting game-frames-per-second **and** the per-frame advance of
`[gp+0x0C74]` **together**. Frames/s alone cannot separate 30 from 60 without the field rate; the counter
delta alone says nothing about how many frames the CPU retires between waits. The image is provisioned and
the port runs headless, so this is **a live measurement away, not a blocked one**. If it measures ~30 fps
with 1 field per frame, `spider1` is a 30 fps title and joins the other two in scope.

### Target title scope

The target ports are Spyro 1/2/3; Crash 1/2/3; Crash Bash; Crash Team Racing; Vagrant Story; Mega Man
X4; Tomba! 1/2; Tekken 3; and Spider-Man 1/2.

Tekken 3 (`SLUS_004.02`), Tomba! 1 (`SCUS_942.36`), and Mega Man X4 (`SLUS_005.61`) are already 60 fps, so their rendering-enhancement scope is
widescreen only: no fps60 mode, interpolation/lerp, or temporal pipeline added solely to support
interpolation. This does not apply to Tomba! 2 (`SCUS_944.54`, then `MAIN.EXE`). X4 separately retains its later load-removal and
drop-in co-op goals. All planned lineage repositories now have public, reproducible trees. Most newly
added titles are honest harness-first scaffolds, not implementation coverage; no widescreen or
interpolation support is implied by repository existence.

### MEASURED 2026-09-28 — WHY each title without a widened picture does not have one

The table above says three owners are measured firing and widening a canvas, and none has been seen
widening a *scene*. This is the per-title cause, so nobody re-derives it. **Every entry is a
measurement, and each names what would unblock it.**

| title | canvas widens | picture | WHY NOT, measured |
|---|---|---|---|
| Spyro 1 | yes | **yes** | — 684x240, real content in all 684 columns |
| Tomba! 1/2 | yes | **yes** | — 428x240; `interp` split also measured (50.0% reconstructed, control leg 0) |
| Crash Bash | yes | **yes** | — 512x234 vs 684x234, margins 99.1%/77.2% non-black |
| **Tekken 3** | yes, 492 vs 368 | **partial** | the only content is an authored 4:3 card: the band is **exactly 133x16 in both legs**, shifted +62 = the centring margin, and both margins are 0.0% non-black. **A card re-centred into a wider frame is not widescreen.** Unblocked by reaching gameplay (S003 / issue 0011) |
| **Spider-Man 1** | yes, 428 vs 320 | **no — all black** | 0 of 25,920 pixels non-black at either aspect. **CORRECTED 2026-09-29 — the "unowned CD callback pointer" was a dead tap, and the routine it named is not an interrupt handler at all.** `0x8008C3E0` is **POLLED**: `tools/re_cd_stream.py` measures exactly four direct `jal` sites and **zero** `lui+addiu` materialisations, zero stored pointers and zero jumps through a materialised address across all 186,880 text words; the body has no `jalr` and no self-branch. Three of the four sites are gated by the halfword at `0x800B2886` — and **nothing in the image ever sets it**: a closed census of every store whose byte range covers that address returns exactly one instruction, a clearer that is itself the delay slot of a `jal` the CD driver makes while initialising. So those three loops are **dead code in retail**, and the fourth site is ungated and does run. Writing the gate would fabricate guest state retail never has and enable three loops retail never runs, so the owner does not write it. `spider1` `4b96e51` makes the one site retail executes readable in C++ with the byte evidence. The real frontier is unchanged and is the one the false explanation obscured: the BIOS's own CD-ROM interrupt handler is ROM code this port does not have, `psxport` issue 0123 |
| **Crash 1** | yes, 428 vs 320 | **yes** | **MEASURED 2026-09-29 — "no frame at all" was READ OFF A COUNTER NOTHING FEEDS, and it is the third time that has happened in this table.** The claim came from one line, `[producers] run-end: OtAttr spans recorded 0 (overflow 0)`, and `OtAttr` counts a guest store only inside the legacy `GameConfig` packet-pool window (`runtime/psx/ot_attr.cpp` → `pool_range_uncached`). Crash 1 is a typed `GameRuntime` and declares no packet pool, so `c->cfg` is null and the count is **0 by construction whatever the guest draws**. The framework's own "this means NOT measured" warning is **unreachable** for a null-config title: `poolRangeMiss` runs only under `if (c->cfg != mPoolCfg)` and `mPoolCfg` starts `nullptr`. Measured in ONE disc-backed process, 400 frames, exit 0: **239,549 GP0 primitives** (210,672 `0x7C` sprites, 28,877 polys; 1..925 per frame, mean 598.9) against 46,438,388 instructions in 3,461,249 blocks from 3,011 translated with 0 fallback, and **6 of 6 captured fences carry a picture — 512×240, 35.7% non-black** (a lit 3D scene: hut, sky, cloud band, grass). The object list was never empty and the producer walk was never unreached. `crash` `88965c0`, `crash/docs/issues/0022`. **The probe is built to FAIL rather than report a zero**: a missing primdump CSV is a failure, the PNG decoder raises on a non-PNG or interlaced file, and a media-less leg is refused (shown both ways: exit 0 with media, exit 1 on a bad disc, exit 2 with no `--disc`). Same run, a second real defect: the size-class block pool owner compared the **shifted** key against a cell's class, and the guest compares the whole request word — `0x8001599C beq $2,$4` and `0x800159B0 bne $2,$4` are against `$a0`, decoded from the image and re-derived by `tools/probe_crash1_block_pool.py`, so the correction cannot be undone by editing a comment. **The widening is still not shown** (`render_width == native_width` survives), so S006 remains `partial` on that half alone |
| **CTR** | yes, 684 vs 512 | **no — margins black, band SHRANK** | ~~**0 of 73,695 prims is 3D.**~~ **CORRECTED 2026-09-28 — that number was a DEAD TAP, not a measurement.** `is3d` means "every packet vertex resolved a per-vertex view-Z in psxport's `ProjPrim` cache", and the only writers of that cache (`gte_store_xy`, `gte_record_pz`, `gte_copy_pz`) have **no callers anywhere in the framework** — verified: every non-definition occurrence of all three in `psxport/runtime` and four ports' `game/` trees is a **comment**. They were fed by the static translator, which is deleted. **So `is3d` is 0 by construction for EVERY title on Lightrec, and no 3D census taken from it measured anything.** Counting the GP0 command byte instead: **73,547 of 73,695 (99.8%) of CTR's submitted prims are Gouraud-shaded polygons**, 46,213 textured, and the guest's own `H` has **18 writers** (16 raw `ctc2 $26` + 2 `jal SetGeomScreen`). The submission path is recovered and owned (`ctr` `448516f`, `ctr/docs/issues/0031`): 10 `jal`-reachable submitters with a common `ctc2 $24/25/26` tail, and a native owner that widens **911** perspective transforms in 16:9 against **0** in 4:3, moving the rightmost submitted `x1` **812 → 894**. **Widening is still NOT visibly working**: the 241-column right margin is 0/173,520 non-black and did not move, because the limit is now 2D content a projection change cannot reach. `ctr` S005 stays `missing` |. Unblocked by the guest submitting 3D |
| **Mega Man X4** | not reached | n/a | **SUPERSEDED 2026-09-28 — the fault is fixed and the port now stops on a DIFFERENT corruption.** The guest appended **nine** 12-byte records into the **eight**-entry array at `0x801659D0`, the writer being `0x80015FE0 sw $a1,0x1F68($at)`, one of exactly three instructions in 1,177,600 bytes that write the word. **The mechanism was wrong in two places and both are corrected from bytes:** the cursor advances **per LOOP ENTRY, not per call** — `0x80015FD4` is the **delay slot** of the `bgez $t0` back-edge at `0x80015FD0`, and that delay slot is `addiu $a1,$t2,12`. And `base+9*12` is `0x80165A3C`, not `base+10*12`. **Use `psxport/tools/disasm.py`, and give it a RAM dump built the PS-X EXE way (`file[0x800]` → `t_addr`) — the text is loaded from file offset `0x800`, so mapping the file from its start lands `0xF800` bytes high and puts an ASCII attribution string where the faulting call should be.** **CORRECTED 2026-09-29: this entry previously said "never `llvm-objdump --triple=mips`, which misdecodes `SLUS_005.61`". That was wrong, and it caused a real defect.** llvm-objdump does not misdecode the image, it **refuses** it — *"The file was not recognized as a valid object file"* — because it wants an object or a recognised container, not a PS-X EXE. Under the framework's Capstone tool the same regions decode cleanly (16/16, zero unknown). The consequence of the false claim was that a listing in `megamanx4/docs/issues/0036` was hand-decoded instead, and **5 of its 7 call targets were wrong** — every one of them by the same rule, `wrong == right + (delay_slot & 0x0FFFFFFF)`, because a J-type target was treated as PC-relative. **Each wrong value was a plausible guest address**, which is why it survived review. See `megamanx4/docs/issues/0007`. **The measured attribution: one call site of 17 (`0x80022058`), 184 invocations in one guest field, of which 9 stored — and it is ONE object nine times, not nine objects**, because the appender early-returns while the animation index is unchanged and the player's index alternated 53↔54 on nine consecutive passes. The writer has **no capacity test anywhere**. The sharper defect: the post-loop terminator `sw zero,8($5)` lands on `&entry[emitted]`, so **at the designed occupancy of eight it zeroes `item_objects[0].x_pos` with no ninth append needed**. **The mitigation does not depend on whether retail wanted nine in one field:** the guest's own uploader loops `while (entry < base+0x60)`, so an entry at index 8 was **never uploadable** — no possible benefit, corruption cost. `megamanx4` `970f346` replaces all three guest functions with readable C++ and a **membership test** on the cursor (not an upper bound: `kCursorGlobal` is a published global, so a cursor below the base is equally possible). **No field-count improvement is claimed**: the build now stops on an uncharacterised corruption where the guest **computed and dispatched on `0x0113D7D0`**. **CORRECTED 2026-09-29 — this row previously ended "the class-0 interrupt table entry at `0x8011CB98` holds `0x0113D7D0`", and that is refuted.** `tools/probe_class0_table.py` measured the value absent at **0 of 97** spot observations and **0 of 41,984** word-reads over presented frames 15→13,426, with `0x8011CB98` holding a stable, plausible `0x800DD7FC` at every observation including the fault, and **0 of 294,912** static image words containing it. **The dispatch is real; the table never held the value.** So the frontier is **what computes it** — and `0x0113` as a top half is the signature of a packed pair read as an address — not which slot carries it. See `megamanx4/docs/issues/0036` |
| **Vagrant Story** | deliberately does not | n/a | `H` is gameplay state: branches at `<272` and `>272` and a **768** clamp the decompilation never recorded. No dynarec adapter, so nothing runs |

**THE RECURRING SHAPE, and it is worth more than any single row.** Four titles widen a canvas and show
no widening, and **three of the four are blocked on the same class of thing — a frontier, not a
projection.** The projection owners are correct and measured in every case. What is missing is
something for a wider projection to reveal: 3D geometry (CTR), a scene (Tekken), or a running game
(MMX4). **"The owner fires" and "the player sees a wider game" are different claims, and
only one of them has been made for the titles above.**

**AND THE FOURTH MEMBER WAS NEVER A MEMBER — it was the same instrument defect wearing a different
hat.** Crash 1 is now measured presenting a picture, so the remaining question there is not "is there
a frame" but "why is `render_width == native_width`". That makes **four** of the rows above traceable
to one dead tap rather than to four frontiers, and the class is now: `is3d` (CTR, section below), the
`VSync(0)` census (Spyro), `OtAttr` (Crash 1), and the Spider-Man gate word. **A counter that reads
zero looks exactly like a clean measurement of absence, which is why every one of these survived
long enough to be published.** The rule that generalises: before quoting a counter, name its FEEDER
and show the feeder running.

**The tenth trap, and it is the mirror image of the other nine: a lead that MATCHES when nothing
should match.** Every dead tap above produced a convincing zero. This one produced a convincing
match, and it was wrong for the same reason — nobody compared it to the null first. `megamanx4`'s
corruption target `0x0113D7D0` shares its low 16 bits exactly with the valid guest address
`0x8001D7D0`, which sits in a tagged handler table at `0x800F2174` alongside ASCII (`"PQRS"`) and
small integers. "The pointer lost its top halfword" is a complete, specific, satisfying explanation
and it explains exactly the value observed. **Measured: halfword `0xD7D0` occurs 1 time in 294,400
text words, where chance predicts `294,400 x 2 / 65,536 = 8.98` — a ratio of 0.11, i.e. BELOW
chance.** Every byte-aligned 2- and 4-byte read within ±128 B of that entry yields the target 0 times,
so the misaligned-read mechanism cannot produce it either.

**So the rule is the null, in both directions, and it is the same null the lineage metric already
demands.** A bare similarity percentage means nothing without its multiple of the measured
cross-studio null (`docs/findings/lineage-metric.md`); the identical discipline applied to a single
halfword is what separates "the pointer lost its top half" from "two numbers share sixteen bits".
**Compute the expected count before naming a lead — whether the observation is a zero or a match.**
A refutation with a denominator belongs in the issue file, because the attractive wrong lead is
exactly the one that gets re-derived.

**A measurement trap that has now bitten twice: quote the LAST `[wide]` line, not the first.**
`picture_announce` prints on CHANGE. Spider-Man's 16:9 log carries `native_width=512 render_width=512`
at line 24 AND line 66, so quoting the first gives `512 == 512` and a correct "not widened" on a leg
that is. Both probes count occurrences and their selftests pin that log shape.

### MEASURED 2026-09-28 — `is3d` is a DEAD TAP, so EVERY title's "N of M prims is 3D" was a vacuous zero

**This one is a framework defect, not a CTR defect, and it invalidates a class of published number.**

`is3d` (`runtime/psx/gpu_primitive_dump.cpp`, consumed in `gpu_native.cpp`) does not ask whether the
guest submitted 3D work. It asks whether **every packet vertex resolved a per-vertex view-Z in
psxport's `ProjPrim` cache** — a *native-depth* classification of psxport's own bookkeeping.

The only writers of that cache are `gte_store_xy`, `gte_record_pz` and `gte_copy_pz`. **None of the
three has a caller anywhere in the framework.** Verified 2026-09-28: across `psxport/runtime` and the
`game/` trees of `ctr`, `spyro` and `Tomba2Engine`, every non-definition occurrence of all three
names is a **comment**. They were fed by the static translator, which was deliberately deleted.

**So `is3d` is 0 by construction for every title running on Lightrec.** A census reporting
`0 of 73,695` was reporting that nobody calls a function — not that the guest drew no 3D. The number
was published as a measurement in this map twice, in `ctr/docs/project-state.md`, and in
`ctr/docs/issues/0026`, and the *reasoning* built on it ("every prim is flat 2D from the guest's
ordering table, so no native producer can be justified") was built on the vacuous zero.

**THE GENERAL RULE, which is what makes this worth a section rather than a table edit:** a metric that
reads a tap nothing writes returns a confident answer about the wrong subject, and **the zero it
returns is the most believable possible output** — it looks like a clean measurement of absence. The
question "is the tap fed?" is a **different** question from "is any prim 3D?", and nothing in the
number distinguished them. Before trusting any counter here, **ask what feeds it and show the feeder
running**; a census that cannot name its feeder is not a measurement.

**What the guest actually does, measured instead by counting the GP0 command byte:** 73,547 of 73,695
(99.8%) of CTR's submitted prims are **Gouraud-shaded polygons**, 46,213 of them textured, and 55
`RTPS` sites carry the perspective transform. Gouraud is the only 3D primitive form the hardware has.
The guest's horizontal projection has **18 writers**, not the 0 this map's reasoning assumed.

### MEASURED 2026-09-27 — which titles actually HAVE a widescreen owner

The sentence above is a POLICY, stated where a reader would take it for a measurement. This is what the
trees contain. Method, so it can be falsified: for each title, the number of first-party source files under
`game/` and `titles/` matching `wide_engine|wideEngine|wide_project|presentationAspect|Wide16x9`, which is
what a title-owned widening owner actually contains.

| title | repo | widescreen owner files | note |
|---|---|---|---|
| Spyro 1 | `spyro` | 24 | verified live: `render_width=684` against `native_width=512` |
| Crash 1 | `crash` | 2 | retail `H=1000, OFX=0, OFY=0`. **CORRECTED 2026-09-27: the old note here — "no literal horizontal cull in 72,192 instructions", read as "widening cannot clip new geometry" — was right about the scan and wrong about the inference.** The bound is a main-RAM global `0x800578D0` (1 writer, 20 readers), so a literal-immediate scan cannot see it, and it is **not a screen-space cull: `FUN_8003A144` uses it as the GTE NEAR PLANE** (`H < Z < 12000`). Widening is safe only because the port widens the GPU projection and leaves that global at retail's value; a change that raised it would cull near geometry. See `crash/docs/issues/0016`. |
| Crash Bash | `crashbash` | 2 | widens in BOTH the model producer (OFX moved to the new left margin, draw area clamped so the authored briefing keeps its centred viewport) and the sprite-quad producer (authored canvas shift) |
| Mega Man X4 | `megamanx4` | 2 | widescreen-only profile, as its 60 fps status requires |
| Tekken 3 | `tekken3` | 2 | widescreen-only; the stage wedge is a direction, so widening is `atan(k·tan θ)` |
| Spider-Man 1 | `spider1` | 4 | the viewport window is a projection INPUT; `H` re-derived from the span |
| Tomba! 1 | `Tomba2Engine` | 14 (shared) | widescreen-only, `RenderCapabilities::widescreenOnly()` |
| Tomba! 2 | `Tomba2Engine` | 14 (shared) | in the lerp scope |
| **Crash Team Racing** | `ctr` | 3 | ~~owner measured and firing 176 of 176 publications, and the widening is INVISIBLE because `PSXPORT_PRIMDUMP` counts 0 of 73,695 prims as 3D~~ — **the 3D census was a dead tap; see the CTR row above for the correction and its evidence.** Owner fires (176/176); the submission path is now recovered and owned; the widening reaches 3D geometry (911 transforms, `x1` 812 → 894) but **the presented right margin is still 0/173,520 non-black and did not move**, so the widening is still not visibly working. The attract sequence presents at 99.43% non-black; the earlier "black" note was true of the first present only. `game/video/widescreen_owner.*` written. The image was recorded as absent and was not |
| **Vagrant Story** | `vagrant` | 1 | an owner, a derivation and five refusals (`game/render/battle_projection.*`) that **deliberately does not widen**: `H` is gameplay state there (branches at `<272`/`>272` against a resting 256) and the clip rectangle cannot be re-derived without bytes. **The absence is the enforcement** and a test asserts it. Verified against `scratch/bin/vagrant/SLUS_010.40` (SHA-1 matches the decomp) and `BATTLE.BIN`. Still no dynarec adapter, so nothing runs yet |

**CORRECTION 2026-09-27. THE BLOCKER WAS NEVER THE MEDIA, AND I RECORDED IT AS IF IT WERE.** This
paragraph said both remaining gaps are "blocked on a missing disc image on this machine". **The CHDs were
there the whole time**, in `/mnt/Boy/ROM/PSX CHD/` — the operator pointed at them directly. What was missing
was PROVISIONING: the extracted, identity-verified images inside each repository's gitignored `scratch/`.
I read "not provisioned into the repo" as "not on this machine", wrote that into the map as a blocker, and
then cited it as the reason two titles could not be advanced. **A title that was merely un-extracted was
reported as un-attemptable, and that is the same error as reading a goal string as a measurement.**

Both are now provisioned, from the media that was already here:

| title | provisioned | identity |
|---|---|---|
| `ctr` | `scratch/raw/ctr/SCUS_944.26` | SHA-256 `7b4aac0b…b838` via `tools/provision.py` |
| `vagrant` | `scratch/bin/vagrant/SLUS_010.40` | SHA-1 `fababcfd…e48c` **matching rood-reverse's target** |
| `vagrant` overlays | `BATTLE.BIN` 577,828 B, `INITBTL.BIN`, `TITLE.BIN` | 3 of 3 required modules verified |

**`vagrant`'s SHA-1 matching the decompilation's own target means its symbol addresses are now confirmed
against real bytes rather than trusted** — and `BATTLE.PRG` is the overlay that holds `func_800760CC`, the
projection owner, which is exactly the body that could not be read before.

`crashbash` shows the contrast: its images ARE present at `scratch/bin/crashbash/SCUS_945.70` with seven
overlays, which is why it has a widening owner and the other two do not. Note the main image has **no file
extension**, so `find -iname 'SCUS*.BIN'` misses it; the correct probe is `-size +100k`.

| path | what it is |
|---|---|
| `psxport/` | **the framework DEV CLONE — the one writable framework checkout.** Also the home of every doc listed above |
| `Tomba2Engine/` | Tomba! 2 (`SCUS_944.54` → `MAIN.EXE`) — psxport's reference consumer; also owns the separate, widescreen-only Tomba! 1 (`SCUS_942.36`) title project, with no shared `game/` |
| `spyro/` | Spyro 1/2/3, the Insomniac-lineage repository; Spyro 1 (`SCUS_942.28`) is the current implementation |
| `spider1/` | Spider-Man 1/2, the Neversoft-lineage repository; Spider-Man 1 (`SLUS_008.75`, USA) is the current implementation |
| `vagrant/` | Vagrant Story (`SLUS_010.40`, USA). Vendors the CC0 `rood-reverse` decomp. Defining fact: the boot exe is ~15% of the code, 933,925 B lives in `.PRG` overlays |
| `megamanx4/` | Mega Man X4 (`SLUS_005.61`, USA) — already 60 fps, so no fps60, native-producer, lerp, or native-depth pipeline. Wants widescreen + load removal + drop-in co-op. Vendors the AGPL-3.0 `mmx4` decomp, which may NOT be lifted into `psxport` |
| `crash/` | Crash Bandicoot 1/2/3 in one architecture repository; harness-first scaffold |
| `ctr/` | Crash Team Racing; standalone harness-first scaffold |
| `crashbash/` | Crash Bash; standalone harness-first scaffold |
| `tekken3/` | Tekken 3 (`SLUS_004.02`); standalone harness-first scaffold, already 60 fps and targeting widescreen only |
| `toystory2/` | Existing Toy Story 2 (`SLUS_008.93`, USA) checkout — not in the active title scope above |
| `coord/` | **UNTRACKED, machine-local, EPHEMERAL ONLY**: `claims/` (the area locks — a lock coordinates the agents on THIS machine, so it must not be tracked), plus agent scratch. Nothing durable belongs here |

`$PSX` in any doc means this workspace root. To reproduce the workspace on a fresh machine:

```text
git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
cd ~/repo/psx/psxport
uv run --frozen python scripts/bootstrap_workspace.py
```

All active target repositories are in that script's `REMOTE_BACKED` list. `toystory2` is not because
it is outside the active target scope.

## The structure rule: ONE framework checkout, and every port runs off it

**There is exactly ONE psxport working tree on a machine, and every game uses it.** `psxport/` is that
tree. Each game has `external/psxport`, which is **not tracked and not a submodule** — it is a SYMLINK to
`psxport/` when the workspace is present, or a private clone at that game's `psxport.pin` on a fresh
machine / CI / a stranger's clone of one repo. `tools/psxport_sync.py --auto` (run by `run.sh`)
establishes whichever applies. The PATH is unchanged, so every `external/psxport/...` reference in docs,
tools and code keeps working.

**So a framework edit is live in every port immediately, with no bump, no sync and no ceremony** — which
is the whole point. There is no longer a "read-only consumer" copy to drift from the writable one,
because there is no second copy.

1. **Framework edits happen in the one tree.** Reaching it through `psxport/` or through a game's
   `external/psxport` symlink is the same directory; both are the dev clone. Commit and push framework
   work in `psxport/`.
2. **`psxport.pin` records the framework commit a game was built and VERIFIED against.** It is
   provenance and the fresh-clone fallback, not what you build against day to day. `psxport_sync.py
   --bump` records it; `--check` (wired into each game's precommit gate) FAILS when the framework you
   built against is not the one the repo records, comparing against `build/psxport_resolved.txt`, which
   CMake writes at configure time.

   **MEASURED 2026-09-27 — and the coverage is worse than "uneven", because the pin was never
   CALLED.** Two claims in an earlier revision of this file were wrong, both the same way:

   - *"`crash` registers none — `ctest -N` finds zero tests matching `pin`."* It does register one, as
     **`crash_dependency_provenance`** (`crash/CMakeLists.txt:348`). The name simply lacks the substring
     `pin`. That is a grep for a string reported as a fact about a port — the same class of error as
     arming a store observer on a data address and reading the guaranteed `MATCHED NONE` as absence.
   - *"No pin was bumped; the bumps are outstanding work."* They were done that day, in the required
     `reconfigure → build → test → --bump` order, once the guard below was in place.

   **The real finding is one level down: NO PORT EVER RAN THE CHECK.** Every occurrence of `--check` in
   all ten `CMakeLists.txt` files was inside a CMake **comment**. The registered "pin tests" were
   **selftests of the check *function***, run against a temporary fixture with a fabricated receipt: they
   assert the function behaves and cannot fail when the repository's real pin is stale. That is why all
   ten ports reported green while four pins were demonstrably stale. A gate that cannot fail on the
   thing it gates is not a gate. The fix is a *live* test per port — registered in `Tomba2Engine` as
   `tomba_psxport_pin`:

   ```cmake
   add_test(NAME tomba_psxport_pin
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/psxport_sync.py"
                    --check --build "${CMAKE_BINARY_DIR}")
   ```

   `--build ${CMAKE_BINARY_DIR}` is the directory CTest is running in, configured moments earlier, so its
   receipt is current by construction and the staleness guard cannot fire spuriously. **Verified it can
   fail:** perturbing `psxport.pin` to a zero commit turns it red, restoring it turns it green.
   **The other nine ports still have no live check.**

   **AND THE BUMP ITSELF COULD RE-CREATE THE ORIGINAL INCIDENT.** `do_bump` recorded
   `head_of(external/psxport)` — the framework's *current* HEAD — and never read a build receipt. So the
   documented order was a convention nothing enforced, and `--bump` alone, with no build at all, recorded
   a commit the tree had never compiled against. **That is exactly how this workspace came to record
   `a1c53d7c` while building against `25dd7826`.** `do_bump` now reads the same receipt, applies the same
   staleness guard and selects the same build as `--check`, and refuses a receipt naming a framework
   other than the one `external/psxport` links. The regression test is
   `test_bump_refuses_when_the_framework_advanced_since_configure`.

   **The staleness guard is now in 10 of 10 copies**, verified per repo against each repo's own copy. Its
   absence was measurable: with `psxport_resolved.txt` naming a repo's own pin while the shared
   framework sat eight commits later, a guarded copy refuses and an unguarded one answers `check OK` —
   same input, opposite answers, and the wrong one is a pass. A second half of the same hole: an ABSENT
   receipt printed "Asserting nothing" and returned 0 in five copies; those now exit 2.

   **The cause is the duplication itself, and it is deliberate** — a port must build from a bare clone,
   so the tool travels with it. That makes "keep the copies in step" an obligation rather than an
   accident. **MEASURED 2026-09-27: the ten copies had TEN DISTINCT HASHES, 298–322 lines each, and 61–226
   changed lines against each other.** The check for that obligation was itself duplicated, which is why the
   guard could go missing from seven of them unnoticed.

   **DONE, and the consolidation found two more defects the duplicated check had been hiding.** There is now a
   canonical `psxport/tools/psxport_sync.py` and a canonical `psxport/tests/test_psxport_sync.py`, and
   `psxport/tools/check_port_pin_tools.py` gates every port against both (`pin_tools_in_step`,
   `pin_sync_behaviour`, `pin_tools_selftest` in psxport's CTest). All ten ports are in step and pinned. Four
   things came out of making the copies identical:

   - **The canonical was missing `import argparse`, and the byte gate reported all ten ports "in step" while
     every copy was unable to run.** The import is reached only from `main()`, so each port's test — which
     imports the module and never calls `main()` — raised nothing. The gate therefore asks **two** questions:
     is this copy the canonical text, *and* does it run (`--help`, exit 0). On the same ten files the
     byte-only gate answered `10 of 10 in step` and the two-question gate answers `0 of 10 in step AND
     runnable`. Byte-equality is not workingness, and the selftest pins the case: a script that imports
     cleanly and still fails `--help` is exactly the real defect, and a module-import test cannot see it.
   - **7 of the 10 ports shipped the pin tool with NO test gating it at all.** The six that had one were the
     six whose copies had drifted least — the ports nobody revisited are the ports nobody guarded, so the
     drift and the missing tests are one fact seen twice.
   - **Three call sites passed `--build-dir` and one `--resolved` to a tool that has neither flag**
     (`crashbash`, `spider1`, `crash/tools/verify.py`, `ctr`). They were calling a flag that did not exist,
     which is precisely what duplication produces.
   - **`ctr` had a second, older copy of the behaviour test under `tools/`** that the canonical is not
     installed over, and its registration pointed at it — a test reading as present while exercising a file the
     canonical no longer governed.

   **`tekken3`'s registrations are unguarded on purpose**: it calls `enable_testing()` directly and never
   defines `BUILD_TESTING`, so an `if(BUILD_TESTING) add_test(...)` there is silently inert. `ctest -N` went
   17 → 18 when that was corrected.

   **An operational cost of the guard, stated rather than left to be discovered later:** any commit to
   psxport — including a documentation-only one — moves HEAD, so every port's receipt goes stale and all ten
   need a `reconfigure → build → test → --bump` round. That is the guard working, not misbehaving: the pin
   records "the framework commit this tree was built and verified against", and after a framework commit that
   is genuinely no longer true. Whether a non-runtime commit should invalidate a *build* pin would need a
   build-relevance classifier — a larger design question, deliberately not attempted here.

   **MEASURED 2026-09-29 — and `ctr` was about to be a GREEN ZERO, which is what makes this
   paragraph a trap rather than history.** Every port's gate is `ctest --test-dir <dir>`, and the
   directory is **not the same in every repo**. `ctr/build/` contains no `CMakeCache.txt` at all — it
   is not a configured build tree — so `ctest --test-dir build` there discovers **zero tests and
   returns 0**, which reads exactly like a pass. It did: a CTR commit was written and pushed claiming
   "Gate: ctest green" on that zero, and the real tree, `ctr/build/agent-clang`, registers **23**
   tests, two of which were genuinely red (`ctr_framework_pin`, `ctr_psxport_pin_live`) until the pin
   was reconfigured, rebuilt and bumped. This is the ninth dead tap in this workspace, and unlike the
   other eight the one that produced it was running a gate rather than reading a metric.

   The gate directory, MEASURED by asking each configured tree for its test count. **A directory
   absent from this table has no `CMakeCache.txt` and is not a gate** — do not pass it to `ctest`:

   | repo | gate directory | tests | note |
   |---|---|---|---|
   | `psxport` | `build` | 183 | `build/ci` registers 181 — not the gate |
   | `crash` | `build` | 35 | `agent-clang` 34, `ci` 30 — smaller trees, not the gate |
   | `ctr` | `build/agent-clang` | 23 | **`build/` is NOT a build tree** |
   | `crashbash` | `build/player` | 29 | **`build/` is NOT a build tree** |
   | `megamanx4` | `build` | 35 | `build/player` is configured but registers 0 — a product build |
   | `spider1` | `build/agent-clang` | 34 | **`build/` is NOT a build tree** |
   | `spyro` | `build` | 102 | `build/player` registers 0 — a product build |
   | `tekken3` | `build` | 28 | `build/ci` registers 27 |
   | `Tomba2Engine` | `build` | 41 | `build/ci` also 41 |
   | `vagrant` | `build` | 22 | `build/player` registers 0 — a product build |

   **A configured tree registering 0 tests is a product build, not a broken gate** — `megamanx4`,
   `spyro` and `vagrant` each have one, and the test count in this table is what distinguishes the
   two. **An UNCONFIGURED tree registering 0 is a green zero** and means the gate did not run.

   **The reusable lesson, and it is the same one as the pin guard above:** a gate that cannot fail is
   not a gate, and `ctest` on an unconfigured directory cannot fail. Read the test COUNT in the same
   command that runs the tests, the way `psxport_sync.py` reads a receipt rather than trusting a
   path. Reporting "green" without a number is how this happened.

   **MEASURED 2026-09-27, every port rebuilt and gated against the framework as it stood during that
   session, after the store-observer and control-surface changes. NO REGRESSIONS:**

   | port | gate | the failures, and what each one is |
   |---|---|---|
   | `psxport` | 168/168 | — |
   | `crash` | 22/22 | — (registers `crash_dependency_provenance`, a selftest; see above) |
   | `ctr` | 13/14 | `ctr_framework_pin` — **correct**: framework moved, pin not bumped |
   | `crashbash` | 28/29 | `crashbash_psxport_pin` — **correct**, same reason |
   | `spider1` | 20/21 | `psxport_pin` — **correct**, same reason |
   | `megamanx4` | 27/27 | — |
   | `tekken3` | 18/18 | — |
   | `Tomba2Engine` | 33/33 | — (`tools/verify_ci.py`'s pin check fails separately, same reason) |
   | `vagrant` | 8/8 | — |
   | `toystory2` | 16/18 | both are **refusals for a missing provisioned corpus**: `scratch/flat` is empty, so `overlay_map_selftest` and `verify_fmv_boundary_selftest` each print `REFUSED: … provision the verified images` and fail. `toystory2` is outside the active title scope and was never provisioned here. |

   **SUPERSEDED LATER THE SAME DAY, and the supersession is the finding.** The four red pin rows above were
   correct while they stood, and every one of them was then made green by
   `reconfigure → build → test → --bump`: `spyro` 86/86, `ctr` 15/15, `crashbash` 29/29, `spider1` 21/21,
   `megamanx4` 27/27, `tekken3` 18/18, `Tomba2Engine` 34/34. `vagrant` and `toystory2` were not reconfigured
   that round. `toystory2`'s two failures remain the missing-corpus refusals, which are correct.

   The row that did NOT change is the one about the gate itself: those "pin" rows were selftests, so
   **making them green proved nothing about this port's real pin** — which is why adding the live check
   in `Tomba2Engine` immediately found a genuinely stale receipt in `build/ci` (configured against
   `2b07a8f6` while the tree sat at `9c962c08`), five build directories into the tree, none of which any
   test had been reading.
3. **Ports are deliberately NOT all on framework HEAD.** Measured 2026-08-16: six ports spanned 55
   commits of framework history. With one maintainer that is a feature — it is what lets one port be
   worked on daily while the others sit untouched, and it is why a Beetle GTE regression in every
   3D scene broke one tree rather than six. Bump a port when you are ready
   to re-verify it.
4. **Parallel framework work** is still one `git worktree` off `psxport/` per claim area, with that
   agent's `PSXPORT_DIR` pointing at it (PROTOCOL's).

### Why the submodule was dropped (2026-08-16)

Two incidents in one day, both caused by the mechanism rather than by anyone's mistake:

- Tomba2Engine was **built against psxport `25dd7826` while recording `a1c53d7c`**, so a bare clone did
  not compile — the game's hook table named a `GameHooks` field the pinned framework did not have.
  Nothing noticed, because a submodule working tree and its recorded gitlink drift silently.
- "Fixing" that drift by syncing to the recorded pin is what pulled a **broken Beetle GTE commit** into
  the working build; it had already broken concurrent 3D execution for two days (8 of 9
  replays segfaulting). That commit was made on a **detached HEAD inside the submodule** — the default
  state of a submodule checkout, and the reason it was never reviewed.

Recursive updates also entered Beetle's nested `deps/lightning/gnulib` dependency. Its mapping was
absent in an earlier checkout, causing Git to abort; a mapped checkout instead cloned a large unrelated
repository during launcher setup. `psxport` still manages its own declared top-level vendor submodules
without recursing into their dependencies. With a single psxport tree there is no duplicate framework
checkout to drift.

## The two things to know even if you read nothing else

- **Never commit** disc images (`*.chd`), extracted executables, or machine-specific
  absolute paths (`/home/<user>/…`). Every game repo ships `tools/go_public.py` to audit history.
- **Never write run artifacts to `/tmp`** — small RAM-backed tmpfs here. Use the repo's git-ignored
  `scratch/`, split by kind. Diagnose "disk quota exceeded" with `quota -s`, not `df`.
- **Some `scratch/` subdirectories hold PROVISIONED INPUTS, not artifacts, and THE DIRECTORY IS NOT
  THE SAME IN EVERY REPO.** The runtime images extracted from the user's disc live there, so a
  `scratch_gc.py --days 0` over a game repo deletes them and the next run refuses. **Check the repo
  before sweeping it**, because this line used to name only `bin/` and that is Tomba! 2's layout:

  | repo | provisioned inputs | re-provision with |
  |---|---|---|
  | `Tomba2Engine` | `scratch/bin/` | `tools/tomba2_provision.py --discdump external/psxport/build/tools/discdump "$DISC"` |
  | `spyro` | `scratch/assets/<title>/` | `tools/provision_title.py --title spyro1 --discdump external/psxport/build/tools/discdump "$DISC"` |

  Measured 2026-09-20: a sweep carrying `--keep 'bin/*'`, taken straight from this line, removed
  `scratch/assets/spyro1/SCUS_942.28` and the next drive died with `[boot:error] cannot read
  .../scratch/assets/spyro1/SCUS_942.28: No such file or directory`. Nothing is lost but the minute
  re-provisioning takes — it re-authenticates every image — but the refusal reads like a broken
  port rather than a missing input, which is the part that costs time. Pass every provisioned path
  this table names for the repo you are sweeping.

## Repo shape: one repo per ENGINE LINEAGE, multiple titles inside it. No third vendored layer

Evidence for every verdict below — matrices, null distributions, the per-decision survival check:
`docs/findings/lineage-metric.md`. A bare similarity percentage means nothing without its multiple of the
measured cross-studio null.

The accepted repository grouping is listed below. `docs/findings/lineage-metric.md` holds its measured
evidence; revise the grouping only when new evidence changes the ownership boundary.

- Spider-Man 1 + 2 share a repo · Spyro 1 + 2 + 3 share a repo (`titles/<t>/` over a shared `game/`), each
  converting to multi-title WHEN that title's work starts, not before.
- Tomba! 1, Vagrant Story, Mega Man X4, CTR, Crash Bash, and Tekken 3: **no shared `game/`.**
- The Crash trio (1/2/3) is ONE architecture, on direct evidence rather than the aggregate metric; `crash/`
  is created when Crash work starts. `ctr/` and `crashbash/` likewise, one title each.
- Rejected: one repo for Spyro AND Crash · an engine-family library vendored between psxport and a game ·
  8 sibling per-title repos.

## Submodule sync: FIXED, and what it now guarantees

`scripts/sync_submodules.py` manages only this repository's declared top-level gitlinks. It enumerates
them directly from `.gitmodules` and `ls-files -s`, updates only named top-level paths, and never uses
recursive Git updates. Thus first-run setup does not clone Beetle's nested
`deps/lightning/gnulib`, even when that nested dependency has a valid URL. The verdict gives a
denominator and names nested gitlinks it saw but deliberately excluded:

    [submodules] checked 2 of 2 submodule(s), all at this repo's recorded gitlinks — nested
    gitlink(s) outside this sync: vendor/beetle-psx/deps/lightning/gnulib

The focused test covers cold initialization, warm pin correction, missing declared paths, dirty and
deliberately advanced checkouts, and both mapped and unmapped nested gitlinks. A clean verdict applies
to the declared top-level set only.
