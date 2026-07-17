# Findings — scene/area transitions

## Door/area-transition freeze: FUN_80073328 case 3 stalls; native AND substrate both hang here on REAL gameplay input
- **symptom:** deterministic pad-replay repro (2026-07-02, HEAD 4ad1119) — no teleport: `PSXPORT_AUTO_SKIP=1` reaches free-roam at f216 (spawn ~(3940,-1032,3968)), then walk right 34f + hold up 300f puts Tomba at (4675,-1023,4608) with `sm[0x48]=2 4a=1 4c=3 4e=1, bf818=2` STUCK forever (verified stable through f1234 = 600 idle frames after the walk). Same freeze under PSXPORT_GATE=1 substrate. Deterministic replay: `PSXPORT_PAD_REPLAY=scratch/bin/door_freeze.pad` reproduces bit-perfect. Dumps: `scratch/bin/door_freeze_f{634,934}.bin(+.spad)`.
- **status:** ROOT CAUSE OPEN — the prior "repro artefact of teleport" claim (2026-07-02 morning) is FALSIFIED by this pad-replay: the freeze reproduces from PURE walking input (no `w` writes), so it IS a real gameplay bug the port ships with, hitting BOTH the PC-native path AND the PSXPORT_GATE=1 substrate. Something upstream (present in both) fails to advance the transition.
- **freeze-state facts (from door_freeze_f634.bin):**
  - `bf818 = 2` (u32), `bf870 = 0` (area id, unchanged — hut entry is a SUB-SCENE swap, not an area load; matches handoff_hut_fade.md), `scene-active 0x800BE258 = 2`.
  - `DAT_800e7ea9 = 1` (NON-zero → contradicts the prior teleport-repro dump which had it 0), `DAT_800e7ffb = 0` (zero — the SECOND-branch condition is 2/3 met; only `node[0x29] != 0` is missing).
  - 6 entities with handler `0x80138FC8` at 0x800EF92C..0x800EFD00, all `node[3]=0x80, node[6]=0x0E, node[0x28]=0, node[0x29]=0`. The state-3-walker gate `node[0x28] & 0x80` FAILS on all 6, so under the pad-replay repro FUN_80073328 case 3 is not being reached via `beh_typed_jumptable_pair` — meaning bf818=2 is being written by a DIFFERENT code path than the teleport-repro finding described. Re-RE bf818 writers under this repro before recomputing the case-3 waiter chain.
- **the earlier case-3 RE (still valid as background but the entry path is now suspect):**
  - **`FUN_80026ad0` case 3** (0x80026bbc) waits for `bf818 == 3`. In the freeze, bf818=2 forever.
  - **Only 2 addresses ever write bf818=3:** `FUN_80073300` (0x8007331c) — called only from `FUN_80073328` case 3 (0x800734f0, 0x80073548).
  - **`FUN_80073328` case 3** (decomp L52271) has two branches:
    - **FIRST branch** (`node[0x29]==0 || DAT_800e7ea9==0 || DAT_800e7ffb!=0`) → waits `bf818 == 6` — but **NO code anywhere writes bf818=6** (RAM scan). Permanent dead-end.
    - **SECOND branch** (`node[0x29]!=0 && DAT_800e7ea9!=0 && DAT_800e7ffb==0`) → immediately `FUN_80073300()`.
  - Under the pad-replay repro, DAT_800e7ea9 IS nonzero and DAT_800e7ffb IS zero — only `node[0x29]!=0` is unmet. Some upstream code path is supposed to poke a specific entity's `node[0x29]` to unblock the case-3 waiter, and that upstream is NOT running.
- **why PSXPORT_GATE=1 also freezes:** same state trap on the substrate. It runs the same substrate transition code and hits the same upstream miss. This narrows the bug to something SHARED by native and substrate — likely an HLE stub returning fake completion, or a wrong platform value both consult, or a coop scheduler edge both share.
- **NEXT INVESTIGATION:** RE who writes `node[0x29]` to a live entity in the running port (dumpram-scan-diff pre- vs post-walk to find upstream writers under this repro), then trace who's supposed to invoke them at door-entry.
- **do NOT** patch bf818 or node[0x29] directly to skip the freeze — that's a bandaid and only hides the missing upstream path.
- **2026-07-02 later-291 (post class-ification of the swap handshake — commits 4a50fa1..ebd3bee):**
  - Reproduced with the native `class SceneTransition::stepSwapWaiter` (byte-gated: 550+ subswapverify matches, 0 mismatches vs `rec_super_call(0x80073328)` under the pad-replay). No native regression — the freeze is inherent to the shared state.
  - Refined RAM inspection at f934 (post-restructure): the 6 door entities live at 0x800EF910/0x800EF9D4/0x800EFA98/0x800EFB5C/0x800EFC20/0x800EFCE4 (68B each, 0xC4 stride), each with `node[3]∈[0..5]`, `node[0x28]=0x81`, `node[0xBF]=1`. Only **entity 0x800EF910 (node[3]=0)** is stuck at `node[6]=3`; the others are still at `node[6]=0..1` (haven't advanced through case 0 yet).
  - Scene block `S1=0x800E7E80`: `S+0x2a=0x02`, `S+5=0` — the JT1[0] `s5==0` gate matches only entity 0x800EF910 (its `node[0x2a]==0x02`). So THIS specific entity IS what beh_typed_jumptable_pair drives; that alignment is correct.
  - **PSXPORT_WWATCH=800ef939,800ef93a** (byte watch on entity 0x800EF910's `node[0x29]`) across the pad-replay: every store is VALUE=0 (writers pc≈0x80073328 = beh common-tail reset, pc≈0x80020868 = a second reset in an unrelated resident SM). **No non-zero writer to that byte fires anywhere in the pad-replay** — neither native nor substrate. So the "upstream code path fabricates node[0x29]!=0 right before the entity's frame" hypothesis is falsified for this repro: nothing ever writes it non-zero for this entity in this scene. The gate cannot open by that mechanism.
  - Consequence: the ORIGINAL RE that "node[0x29] must be written by an upstream just before the entity's frame" doesn't map to observable behaviour under this repro. Either (a) real gameplay reaches a DIFFERENT entity/state where node[0x29]!=0 fires naturally via JT1[0] s5==1's own write (i.e. the entity that must clear it is not 0x800EF910), (b) some overlay handler outside MAIN.EXE writes it, or (c) real gameplay reaches bf818=5 via some path we haven't found and case 0 FIRST branch (bf817==node[3]) completes the transition.
  - Actionable next step: hand-recorded `hut.pad` remains the most reliable disambiguator. Absent that, next-best: instrument stepSwapWaiter case 3 entries + log EVERY (frame, entity, node[6], node[5], node[0x29], bf818) transition, then re-run to see if a DIFFERENT entity reaches case 3 briefly with node[0x29]==1.
  - **★ mid-transition uses the SUBSTRATE walker (discovered later-291b, 2026-07-02):** `ov_field_frame_x` (game/scene/engine_stage.cpp:703, the sm[0x4a]==5 mid-transition frame variant) drives the transition via `d0(c, 0x8007b04cu)` — the state-3 object walker at guest 0x8007B04C. That walker is STILL SUBSTRATE (rec_dispatch), so during mid-transition it calls `(**(code **)(iVar1 + 0x1c))()` per node — which resolves to substrate `func_80138FC8` → substrate `func_80073328`, BYPASSING the native `SceneTransition::stepSwapWaiter` entirely (dispatch_native_behavior only routes through the NATIVE walker, not substrate `func_8007B04C`). Instrumentation confirms: my native stepSwapWaiter fires only ~34 times at transition-start (the pre-transition frames), then goes silent for the remaining ~900 pad-replay frames while the substrate walker takes over. So the SECOND-branch deadlock is happening in substrate MAIN.EXE code — my class ownership currently gives us the RE + verified port, but the mid-transition path itself needs porting for the freeze to move under native control.
  - Next code target: port FUN_8007B04C native (entity list walker, decomp scratch/decomp/ram_f1000_all.c L56987-L57017). That routes every mid-transition beh dispatch through `dispatch_native_behavior` → `beh_typed_jumptable_pair` (native) → `SceneTransition::stepSwapWaiter` (native), making the freeze fully observable + debuggable inside the class. The state-machine deadlock will STILL reproduce (it's a shared-state bug), but it will do so under native code so we can add targeted probes / a well-scoped, non-bandaid fix once the missing `node[0x29]` writer is identified.
- **refs:** scratch/bin/door_freeze.pad (deterministic repro), scratch/bin/door_freeze_f{634,934}.bin(+.spad) (freeze-state dumps), scratch/handoff_door_freeze.md (superseded — its "orphaned cooperative task" hypothesis is not proven either way; the finding needs re-RE under this new repro), scratch/handoff_hut_fade.md (matches — hut entry is a sub-scene swap, area id stays 0), scratch/decomp/ram_f1000_all.c L52186-L52316 (FUN_80073328/300/2C0 decomp).
- **the RE (drop this into future sessions instead of re-deriving):**
  - **Scene-transition dispatcher** = `FUN_80026368` (MAIN.EXE 0x80026368), sibling to the SOP dispatcher `FUN_80026c88`. It walks 8 slots × 76B at `0x80100400`, calls `*(u32*)(0x8009d314 + slot[2]*4)(slot)` for each active slot.
  - **Slot[3]** @ `0x801004e4` in the freeze state: `idx=04 → handler = 0x801167ac`, slot[3]=02, slot[4]=01, slot[5]=03. The handler at 0x801167ac dispatches on slot[4]: case 1 (@0x80116870) calls `FUN_80026ad0(slot)`. slot[5] is FUN_80026ad0's own substate — currently 3.
  - **`FUN_80026ad0` case 3** (0x80026bbc, decomp `scratch/decomp/ram_f1000_all.c` L6258) waits for `bf818 == 3`. In the freeze, bf818=2 forever.
  - **Only 2 addresses ever write bf818=3:** `FUN_80073300` (0x8007331c, `sb v0=3, bf818`) — and IT is called only from `FUN_80073328` case 3 (0x800734f0, 0x80073548).
  - **`FUN_80073328` is called only from 3 jal sites**, all inside `beh_typed_jumptable_pair` (overlay `FUN_80138FC8`) JT1 cases 0/1/2 (@0x801393B8, 0x801393F8, 0x80139414 — game/ai/beh_typed_jumptable_pair.cpp:225/247/254).
  - **In the freeze RAM, 6 live entities have handler 0x80138FC8** (list1 heads `0x800FB168` → chain reaches `0x800EF910..0x800EFCE4`), one per node[3]∈[0,5], flag=0x81 (state-3 walker `0x8007B04C` gate on `node+0x28 & 0x80` PASSES). Entity 0x800ef9d4 (node[3]=1) has already reached FUN_80073328 case 3 — its `node[6]=03`. So the driver is ALIVE and running each mid-transition frame; it just can't advance.
  - **FUN_80073328 case 3** (decomp L52271) has two branches:
    - **FIRST branch** (taken when `node[0x29]==0 || DAT_800e7ea9==0 || DAT_800e7ffb!=0`) → waits `bf818 == 6`, then `FUN_80073300()`.
    - **SECOND branch** (`node[0x29]!=0 && DAT_800e7ea9!=0 && DAT_800e7ffb==0`) → immediately `FUN_80073300()`.
  - **★ NO code anywhere writes bf818=6.** Full RAM scan of MAIN.EXE + the resident overlay for stores to 0x800bf818 finds writers for values 0/1/2/3/4/7/8/9 — never 6. So the FIRST branch of case 3 is a permanent dead end; the transition can ONLY complete via the SECOND branch.
  - **Entity 0x800ef9d4 has `node[0x29]=0`** in the freeze state → gate → FIRST branch → waits forever.
  - **`beh_typed_jumptable_pair` state-1 tail RESETS `node[0x29]=0` at the end of every dispatch** (game/ai/beh_typed_jumptable_pair.cpp: "then a common tail: … node[0x29]=0"), so `node[0x29]!=0` is a MOMENTARY condition an upstream code path must fabricate right before the entity's frame. Under a real walk-through-door, some scene-setup writes node[0x29]!=0 in the same frame the entity's case 3 runs; under the teleport repro that upstream path never fires (the destination-area id stays `bf870=0`, no new overlay/spawn, no scene setup to poke node[0x29]).
- **why `PSXPORT_GATE=1` also freezes:** it does — same state trap on the substrate. The recomp substrate's overlay body implements the exact same case-3 gate logic (bf818==6 dead end + node[0x29] tail-reset). Real hardware would follow the SAME rule, so real gameplay must reach the SECOND branch — which we cannot reach from a raw-`w` teleport.
- **REAL fix:** obtain a PROPER repro. Hand-drive a WINDOWED session that walks Tomba into a doorway (no teleport) with `PSXPORT_PAD_RECORD=scratch/bin/hut.pad` (recording is byte-perfect deterministic — verified today). Replay headless and dump the RAM at the swap frame. THAT will show what upstream write puts `node[0x29]!=0` (or `DAT_800e7ea9!=0 && DAT_800e7ffb==0`) into position to let FUN_80073328 case 3 SECOND branch fire. Only then can we say whether native regresses vs substrate on a real transition.
- **do NOT** patch bf818 or node[0x29] directly to skip the freeze — that's a bandaid and only hides the missing upstream path.
- **refs:** scratch/handoff_door_freeze.md (superseded — its "orphaned cooperative task" hypothesis is wrong), scratch/handoff_hut_fade.md, game/ai/beh_typed_jumptable_pair.cpp:225/247/254 (JT1 cases 0/1/2 dispatching FUN_80073328), game/scene/engine_stage.cpp:893 ov_field_transition + :699 ov_field_frame_x (state-3 mid-transition path, already native), game/world/pool.cpp:118 ov_800783DC (writes bf816=1 during area load — the initial kick), scratch/decomp/ram_f1000_all.c L6223/L52186-L52316 (FUN_80026ad0 + FUN_800732C0/300/328 decomp)

## Cutscene SCRIPT INTERPRETER — resident dispatch loop + entry format + handler table (2026-07-03)
- **symptom:** cutscene fade fn pointers (0x8013B29C, 0x8013B074, 0x8013B178, 0x80139728, 0x8013B274, 0x8013AEF0, 0x8013AFD8) live only as DATA WORDS in A06 script tables (e.g. 0x80149A20, 0x80149CDC), never as direct-jal targets. When investigating "who calls FUN_80139728" or trying to intercept the fade fnptr from a native override, greps return zero jal sites — you need to know it's dispatched through the script interpreter.
- **status:** RE'd (structure fully identified); NATIVE PORT NOT YET LANDED (opcode inventory of ~63 handlers is a bounded follow-up).
- **cause:** the fade fnptrs are OPCODE ARGS in a cutscene bytecode. A06 sets up an object with `FUN_80040CDC(obj, tableA, scriptPtr)` (script init) — passing e.g. `scriptPtr=0x80149A20` — then the scheduler ticks it via `FUN_80041098(obj)` (script step) each frame. `FUN_80041098` reads the current entry's opcode, indexes a HANDLER TABLE, and jalr's the handler. Op 0x03E's handler at `FUN_800412CC` is `lw v0, 0x74(a0); jalr v0` — it CALLS the fnptr stored in the entry's arg words.
- **fix (interpreter structure — port this):**
  - Entry format: **8 bytes** = `{ u16 opcodeWord, u16 argA, u16 argB, u16 argC }`. If flag bit 0x2000 of `opcodeWord` is set, the entry is **16 bytes** (extra `{argD..argG}` block at offset +8). Opcode ID is `opcodeWord & 0x07FF` (low 11 bits); the top 5 bits are FLAGS (0x0800 = branch/cond, 0x1000 = "valid entry" seen on every real entry, 0x2000 = has extra 8-byte block, 0x4000 = sets obj[+0x71] flag bit 2, 0x8000 = advance modifier).
  - **`FUN_80040CDC` — init(obj, tableA, scriptPtr):** obj[+0x7C]=tableA, obj[+0x46]=0xFF, obj[+0x10]=0, obj[+0x70]=0 (progress counter), obj[+0x71]=0 (flag byte), obj[+0x78]=0. Reads first entry; if opcode bit 0x1000 → obj[+0x71]=2; if bit 0x4000 → obj[+0x71] |= 4. Calls FUN_80040DE0 to load current entry data.
  - **`FUN_80040DE0` — loadCurrentEntry(obj, scriptPtr):** obj[+0x6C]=scriptPtr, obj[+0x72]=*(u16)(scriptPtr+2), obj[+0x74]=*(u16)(scriptPtr+4), obj[+0x76]=*(u16)(scriptPtr+6). If opcode bit 0x2000 set: also obj[+0x64..+0x6A] = *(u16*4)(scriptPtr+8..14). Because obj+0x74 and obj+0x76 are ADJACENT u16 halves loaded LE, a subsequent `lw` at obj+0x74 recovers a full 32-bit VALUE (used as a fn ptr for op 0x03E).
  - **`FUN_80040E54` — advanceEntry(obj, kindArg):** reads current entry, extracts top 3 bits (opcodeWord & 0xE000), branches on 0x2000/0x4000/0x6000/0x8000/0xA000/0xC000/0xE000. Advances scriptPtr by 8 (base), 16 (0x2000 flag: skips extra block), 24 (0x6000 combo), or follows a branch pointer from the entry (via lw @+0x0C for 0x4000 / lw @+0x14 for 0x6000 branch targets — DAT-relative branches inside the script). Then re-calls FUN_80040DE0 to reload obj fields from the new position.
  - **`FUN_80041098` — step(obj) — DISPATCH LOOP:**
    ```
    // s3 = 0x800A3B78 (handler table base — resident MAIN.EXE)
    while (obj[+0x70] > 0) {                            // signed loop guard
        u16 op = *(u16*)obj[+0x6C];
        u32 oid = op & 0x07FF;
        handler = *(u32*)(0x800A3B78 + oid*4);
        a0 = obj; ret = handler(a0);                    // JALR
        switch (ret) {
            case 0: obj[+0x71] |= 2; return;            // pause script
            case 1: continue;                           // re-run same entry (handler advanced internally)
            case 2: FUN_80040FA0(obj, 0); continue;     // advance
            case 3: FUN_80040FA0(obj, 1); continue;     // advance-branch (obj[+0x70]++ semantic)
            default: return;                            // exit
        }
    }
    ```
    (FUN_80040FA0 is a small wrapper around FUN_80040E54 that increments obj[+0x70] and dispatches per the kindArg.)
  - **Handler table at `0x800A3B78`, 63 entries (opcodes 0..62), each a 32-bit fn ptr into MAIN.EXE `0x8004xxxx`.** Known handlers so far:
    - `0x03E → 0x800412CC` — CALL fnptr: `jalr obj[+0x74]` (the mechanism the fade dispatch rides).
    - `0x000 → 0x80041C54`, `0x005 → 0x80042090`, `0x006 → 0x800420AC`, `0x00C → 0x80042448`, `0x013 → 0x800427F4`, `0x015 → 0x80042894`, `0x01E → 0x8004179C`, `0x01F → 0x80041468`, `0x02E → 0x80043A10`, `0x02F → 0x80043A40`, `0x030 → 0x80043BB0`, `0x031 → 0x80043BD4`, `0x034 → 0x80043EDC`, `0x037 → 0x80044144`. (Full inventory pending; each opcode's semantics is its own RE unit.)
- **native port strategy (when landing):**
  1. `class ScriptInterp` on Engine (`c->engine.script`) — instance subsystem per project OOP directive; not static because it will grow state (per-object script debug traces, etc.).
  2. Method `step(uint32_t obj)` mirrors the FUN_80041098 loop. Op 0x03E's native path calls `c->engine.behaviors.dispatchObj(obj, fnptr)` — so any fade fnptr registered in `BehaviorDispatch::kTable` runs native automatically; unowned fnptrs fall through to `rec_dispatch` (substrate).
  3. Wire the native step at guest 0x80041098 via `dispatch_native_behavior` (existing route). Do NOT reintroduce `rec_set_override` — the CLAUDE.md ban stands.
  4. Once the loop is native, wire each fade fnptr from the script tables (0x8013B29C etc.) as its OWN `beh_*` in kTable — each is a small state machine to RE case-by-case. Only then do the two already-native fade sub-machines (whiteFlashPhaseRamp @0x801178A4, whiteFadeHold @0x80117AAC in `beh_a06_multi_actor.cpp`) share a "same visual mechanism, different SMs" family with them — the script-driven fns are DISTINCT addresses, not the same fns.
- **misconception to avoid:** the handoff phrasing "porting this enables the two A06 fade sub-machines to be reached via native call chain" is imprecise — whiteFlashPhaseRamp/whiteFadeHold are ALREADY reached natively via `beh_a06_multi_actor` case 10 (they're static helpers there). The script interpreter dispatches a DIFFERENT family of fade fns (0x8013B*, 0x80139728). Both families likely coexist and drive different cutscene beats; landing the interpreter enables the SECOND family.
- **refs:** commit 8ccbfca (fade sub-machines RE'd inline in beh_a06_multi_actor.cpp), tools/disas.py 0x80040CDC/0x80040DE0/0x80040E54/0x80041098/0x800412CC (interpreter fns), A06.BIN script tables around 0x80149A20 (fade scripts) and 0x80149CDC (opening-cutscene script header 0x0000000D), scratch/ghidra/A06 (Ghidra 12.0.4 project, base 0x80108F9C, 484 fns).

## Un-owned entity-behavior cluster (0x801244E8/0x8012866C/0x8012E168/0x8013DD48) — 1 ported, 3 blocked
- **task framing:** all 4 addresses were flagged "un-owned, likely per-area/behavior game logic" (siblings
  of the HOT 0x8013DD48, 8 native callers) — the assumption was all 4 are ownable object-behavior fns.
- **RE result (Ghidra headless, `ram_derail2.bin` — the only captured dump where this 0x8012/0x8013
  overlay code is resident; `main_ram.bin`/most other scratch dumps have this range all-zero):**
  - **0x801244E8 — PORTED.** `game/ai/beh_jumptable_release_trigger.cpp` `release_position_801244e8`.
    Clean, self-contained function: own prologue (sw ra/s0/s1/s2/s3), own epilogue, every input (obj,
    mode=a1, ref=obj[0x10]) resolved from its own args/memory — no inherited register state. A
    release-trigger position/respawn sub-behavior: state 0 = one-shot placement (camera-relative offset
    or a lookup-table entry, `0x801498B0`, keyed by obj[0x60]); state 1 = per-frame "respawn adjacent
    item" (rand()&0x3f==0 → `Spawn::spawnAndInit(0x107,...)`) + reposition + re-arm to state 0. Verified
    the TWO real call sites (jt[1]/jt[4] in the same file) always pass mode∈{0,1} exactly as ported.
  - **0x8012866C — BLOCKED, not a real function.** Isolated Ghidra decompile flags `unaff_s0`/`in_v0`
    (register-continuity vars with no local def) — confirmed by raw disas: the code reached at this
    address has NO prologue of its own (no `addiu sp`/`sw ra`), it's a FALL-THROUGH continuation inside
    a LARGER enclosing function (confirmed: the bytes just before 0x8012866c end in a `slti`/branch, and
    the bytes after its `jal 0x80083e80` continue straight on to a REAL epilogue — `lw ra/s0; jr ra` —
    at 0x80128750, i.e. one shared function body, entered EITHER by fallthrough OR by an external `jal`
    from `FUN_80040558` (both call sites there pass mode=v0=obj[1]'s just-loaded byte, confirmed via
    disas at 0x80040d[80-a0]). The two external callers ARE fully resolved (a0=obj, mode=v0), but porting
    this as an isolated method would silently assume the enclosing function's OWN stack-frame convention
    (ra @ sp+0x14, s0 @ sp+0x10, 24-byte frame) is safe to skip past — that requires RE'ing the TRUE
    enclosing function first (not yet done; not one of the 4 assigned addresses). Left as rec_dispatch.
  - **0x8012E168 — BLOCKED on register provenance.** Same fall-through-continuation shape (raw disas:
    the instruction immediately before 0x8012e168 is `addiu v0,zero,-0xc8`, i.e. this address is ALSO
    reached by fallthrough with a DIFFERENT semantic v0 than the external-jal case). Additionally the
    function body reads `s1` (`lw a3,0xd0(s1)`, `lbu v0,1(s1)`) with NO local `lw s1,...` anywhere in it —
    s1 must be live-resident from several call levels up. Traced as far as: `FUN_80040558` (the only
    external caller, both call sites gated on `DAT_800bf870==8`, so `in_v0` IS resolved = constant 8) does
    NOT set s1 in its own prologue, so s1 is inherited from EITHER `entity_walk_7a904` (which sets
    NOTHING but a0 before dispatch) or `ObjectTable::dispatchFaithful`/`FUN_80026C88` (which explicitly
    OVERWRITES s1 as its 0..39 LOOP COUNTER — game/world/object_table.cpp:194-224 — meaning if THIS is
    the call path, `s1+0xD0` would read near-NULL, which can't be right). Which path actually feeds
    FUN_80040558 was not resolved this session. Porting with a GUESSED s1 meaning would be exactly the
    "magic constant" bandaid CLAUDE.md bans. Left as rec_dispatch pending that trace.
  - **0x8013DD48 — LEAVE PSX (GTE render leaf), see docs/findings/render.md.**
- **lesson:** an isolated-address Ghidra decompile that reports `unaff_sN`/`in_vN` pseudo-vars is a
  reliable SIGNAL (not just decompiler noise) that the address is a shared fall-through fragment, not a
  true function — always cross-check against the REAL caller's decompile (which usually shows a normal
  call) AND the raw bytes immediately before the target address (a real prologue vs. a trailing branch)
  before trusting an isolated RE.
- **refs:** game/ai/beh_jumptable_release_trigger.cpp (release_position_801244e8), game/world/entity.cpp
  (sm40558's calls to 0x8012866C/0x8012E168, unchanged), game/world/object_table.cpp (dispatchFaithful
  s1 loop-counter reuse), docs/findings/render.md (0x8013DD48).

## ScriptInterp opcode cluster — §9 re-verify + frontier-tier wiring (2026-07-10)

- **status:** RESOLVED (5 opcode handlers + advanceStep + PcScheduler::tickSleepCountdown promoted
  from wide-RE draft to VERIFIED, WIRED). SBS-full 0-diff held (6720+ frames, autonav). `PSXPORT_DEBUG=
  dispatch` confirms op05WaitFrames/op06TestSceneFlag actually FIRE during intro-area autonav (49-50
  hits each, matching A/B); op34ClaimGate/op36MoveTowardScriptTarget/op31TurnTowardTarget are
  installed via `ScriptInterp::registerOverrides()` but NOT exercised by this autonav path (0 hits) —
  correctness for those three rests on the §9 line-by-line re-verify below, not the gate. A future
  session with cutscene/movement-script coverage should re-gate to confirm they fire.
- **bug found (§9 re-verify catch):** `op34ClaimGate`'s `kClaimGateByte` constant was wrong —
  `0x800BF86Fu` in the original wide-RE draft vs the ground truth `0x800BF80Fu` (an 0x60 transcription
  slip, apparently copied from op06's unrelated `0x800BF870` table neighborhood). Recomputed directly
  from `generated/shard_2.c:4772`'s own constant arithmetic: `(32780<<16) + (-2040) + 7` in the
  claim/write path and independently `(32780<<16) + (-2033)` in the poll path — both give
  `0x800BF80F`, confirmed self-consistent within the function. Fixed in `game/scene/script_interp.cpp`
  (`kClaimGateByte`) and the header banner (`game/scene/script_interp.h`).
- **everything else verified clean:** op05WaitFrames (shard_7.c:5216), op06TestSceneFlag
  (shard_0.c:5231), advanceStep/FUN_80040FA0 (shard_2.c:4564), stepAngleToward/FUN_8004139C
  (shard_1.c:6657), stepEventPulse/FUN_80042EA4 (shard_3.c:11682), op36MoveTowardScriptTarget
  (shard_5.c:5667), op31TurnTowardTarget (shard_3.c:11362) all matched their `generated/` bodies
  instruction-for-instruction on independent re-derivation — including the two op31 polarity fixes the
  original draft's own commentary claimed to have self-caught (both confirmed correct: argA sign bit
  SET selects the scratchpad secondary-actor global / CLEAR selects self; the commit-tail SNAPs when
  `maskedDelta < threshold OR threshold <= 0`).
- **naming fix:** `ScriptInterp::advanceEntry()` was documented as owning guest FUN_80040E54
  (`kAdvanceAddr`) but its actual behavior has always been FUN_80040FA0's (the post-advance switch
  step() really calls) — `advanceEntry()` now calls the native `advanceStep()` body directly instead
  of `rec_dispatch(c, 0x80040FA0u)`; FUN_80040E54 (the raw entry-advance) stays substrate, called from
  inside `advanceStep()` (out of band for this pass).
- **wiring mechanism:** `step()`'s opcode dispatch already calls `rec_dispatch(c, handler)` for every
  non-0x3E opcode (handler = the resident table read from `0x800A3B78 + oid*4`), and `rec_dispatch`
  consults `c->game->engine_overrides` before falling to the `gen_func_*` body — already oracle-gated
  (core B / psx_fallback never consults it). So wiring these 5 opcodes was ONLY
  `ScriptInterp::registerOverrides()` registering `kOp05Addr/kOp06Addr/kOp34Addr/kOp36Addr/kOp31Addr`
  onto `EngineOverrides` (called from `register_engine_overrides()` in `runtime/recomp/boot.cpp`) — no
  change to `step()`'s dispatch loop itself.
- **PcScheduler::tickSleepCountdown** (FUN_800506D0, `game/core/pc_scheduler.cpp`) verified clean
  against `generated/shard_5.c:7522` (3-slot sweep, state==1 decrement+re-arm-to-2-on-underflow) and
  wired by direct-call swap at `runtime/recomp/native_boot.cpp:129` (was `rc0(c, 0x800506d0)`).
- **refs:** game/scene/script_interp.{h,cpp}, game/core/pc_scheduler.{h,cpp},
  runtime/recomp/{boot,native_boot}.cpp.

## SOP intro-cutscene cluster (0x8010AF60-0x8010BEAC) + Demo::s3SubMachine (0x80106AC4) — §9 promote pass (2026-07-10)
- **status:** SOP cluster now 6/6 promoted VERIFIED+WIRED (sopBeatAdvanceWalk 0x8010AF60,
  sopBeatAdvanceNarration 0x8010B078, sopOrbitPathStep 0x8010B11C, sopIntroEffectTick 0x8010B2D4,
  sopIntroEffectSpawn 0x8010B44C, beh_orbit_spark_effect 0x8010BEAC, **sopLiftedSubtick 0x8010B588 —
  RESOLVED 2026-07-10, frontier convergence pass, see the "sopLiftedSubtick / ScriptInterp::step"
  finding below** — `game/ai/sop_intro_events.cpp`). `Demo::s3SubMachine` (0x80106AC4,
  `game/scene/demo.cpp`) is still §9-verified byte-exact but DELIBERATELY left UNWIRED — a separate
  pre-existing r16 register-liveness gap OUTSIDE this cluster (unrelated to ScriptInterp, see below).
  **Correction to the earlier claim "intro-area autonav doesn't reach the SOP scene":** that was true
  only for `PSXPORT_AUTO_SKIP=1` (which deliberately SKIPS the intro narration by pulsing Start).
  `PSXPORT_SBS_AUTONAV=1` (the standard SBS gate's own autonav, `docs/fleet-workflow.md` §2) does NOT
  skip it — the SOP narration IS the opening intro cutscene (`docs/narration-port.md`) and fires
  `sopLiftedSubtick` from frame ~62 onward, hundreds of hits by f5000. SBS-full 0-diff (`PSXPORT_SBS_
  AUTONAV=1`, standard gate command) now holds through f5070+ WITH all 6 SOP addresses wired and firing
  — a genuine content-specific gate, not just "never reached."
  sopIntroEffectSpawn 0x8010B44C, beh_orbit_spark_effect 0x8010BEAC — `game/ai/sop_intro_events.cpp`).
  `sopLiftedSubtick` (0x8010B588) is §9-verified byte-exact but DELIBERATELY left UNWIRED — it exposed
  a pre-existing `ScriptInterp::step` bug outside this cluster when wired (see below). `Demo::
  s3SubMachine` (0x80106AC4, `game/scene/demo.cpp`) was ALSO initially left unwired for the same
  reason but its exposed bug is now RESOLVED and it IS wired (see the RESOLVED entry below) — SBS-full
  0-diff holds for 5700+ frames (~95s) with s3SubMachine wired and firing every frame from f13. SBS-
  full 0-diff held to f9120 with sopLiftedSubtick unwired (the other 5 SOP + demo wired).
  `PSXPORT_DEBUG=dispatch` over a 95s intro-area autonav run shows ZERO hits for all 6 SOP addresses —
  intro-area autonav doesn't reach the SOP scene (a later cutscene, not the opening intro). Correctness
  for the wired 5 rests on the §9 line-by-line re-verify, not the gate; a future session with SOP-area
  coverage should re-gate to confirm they actually fire.- **systemic bug found (§9 re-verify catch, all 7 functions incl. Demo's):** EVERY function in the
  original wide-RE draft was logically byte-exact (confirmed instruction-by-instruction against
  `generated/ov_sop_shard_*.c` / `generated/ov_demo_shard_0.c`) but NONE mirrored the guest-stack
  frame their `ov_*_gen_*` counterpart pushes (`addiu sp,-N` + s0/s1/ra spills) — CLAUDE.md's "MIRROR
  THE GUEST STACK" rule. `Demo::s3SubMachine`'s own draft comment ("no stack frame observed in the
  decompile") was flatly WRONG — `ov_demo_gen_80106AC4` pushes `addiu sp,-32` + spills ra/s1/s0. Fixed
  in all 7 (save incoming r16/r17/r31, spill to the RE'd sp offsets, restore before every return —
  `game/ai/sop_intro_events.cpp` + `game/scene/demo.cpp`). This mattered in practice: several of these
  leaves call still-substrate `rec_dispatch` leaves internally (Animation::attach, GraphicsBind::
  recordArrayInit, etc.) that DO clobber the shared r16/r17/r31 register file, so without the mirror
  the CALLER's registers come back corrupted, not just the stack bytes.
- **finding: sopLiftedSubtick / ScriptInterp::step divergence — RESOLVED (2026-07-10, frontier
  convergence pass).** Registering `sopLiftedSubtick` (states 1/6 call `ScriptInterp::step(node)`,
  already-native, `game/scene/script_interp.cpp`, NOT part of this cluster) produced an SBS diff at
  `node+0x71` (`OBJ_FLAGS_71`) starting ~frame 63: native (A) stayed `02`, oracle (B) advanced to `03`.
  `sopLiftedSubtick` itself was already byte-exact; the bug was in `ScriptInterp::step`'s RET_PAUSE
  handler (§9 re-verify against `generated/shard_3.c:11302` `gen_func_80041098`, line-by-line delay-
  slot trace of `L_800410C0..L_80041178`): the handler ORed `obj[+0x71] |= 0x02u`, but tracing the
  guest's own delay-slot chain (`r2` gets set to `1` — NOT `2` — by the time control reaches the OR at
  `L_80041138`, via three chained `{compare; delay-slot-assign; branch}` blocks at 0x800410FC-
  0x80041120) proves the ground-truth mask is `0x01u`. Native re-ORing the already-set 0x02 pause bit
  with the wrong mask was a no-op (stayed `02`); oracle correctly ORed in the new `0x01` bit on top of
  the existing `0x02` (`03`) — exactly matching the recorded symptom. Fixed in
  `game/scene/script_interp.cpp` (`RET_PAUSE` case, plus the stale header-comment table and the
  `OBJ_FLAGS_71` bit-layout comment, both of which had mistranscribed the same bit). `sopLiftedSubtick`
  is now registered in `EngineOverrides` (`ScriptInterp::registerOverrides`-adjacent
  `RegisterSopIntroEventOverrides`, `game/ai/sop_intro_events.cpp`); `beh_sop_intro_lifted.cpp`'s
  `overlay_subtick` call site is UNCHANGED (`rec_dispatch(c, 0x8010B588u)`) since `rec_dispatch`
  already routes through `EngineOverrides` before falling to substrate — no taxi needed. **Gated:**
  `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1` (standard gate) 0-diff through f5070+ with
  `sopLiftedSubtick` firing hundreds of times from f62 (the SOP narration IS the opening intro, see
  the status note above); `PSXPORT_MIRROR_VERIFY=0x8010B588 PSXPORT_MIRROR_VERIFY_CONTINUE=1` armed
  over the same run: 0 mismatches across ~4400 invocations through f4470+.
- **finding: Demo::s3SubMachine wiring exposes a pre-existing r16 register-liveness gap** — registering
  `Demo::s3SubMachine` produces an SBS diff at the guest stack (`0x801FE98C`, sp+16 slot of
  `demo_frame_s3()`'s call frame) starting frame 13: oracle (B) spills r16=`0x1F800000` (the live loop
  constant `Demo::stageBodyFaithful` sets up and documents as required "at every dispatch boundary");
  native (A) spills r16=`2` instead. `demo_frame_s3()` (`game/scene/demo.cpp`, PRE-EXISTING, not part
  of this pass) calls `rec_dispatch(c, 0x80106ac4u)` without re-establishing r16 itself, relying on it
  staying live from `stageBodyFaithful`'s setup through whichever earlier substate
  (`demo_frame_s1`/`demo_frame_s2`, also pre-existing) ran first in the same faithful-loop iteration —
  one of those doesn't preserve it. `Demo::registerOverrides()` exists (`game/scene/demo.cpp`) but is
  NOT called from `register_engine_overrides()` (`runtime/recomp/boot.cpp`) pending that fix.
  Also DISPROVED en route: `0x80106AC4` DOES have a direct intra-shard call site
  (`ov_demo_shard_0.c:126`, inside `FUN_801064E8` = the substrate's own s3 body) exactly like
  `game/ai/actor_melee_engage.cpp`'s shape — but dual-wiring it via `ov_demo_set_override` caused a
  genuine DOUBLE-FIRE per frame (the guest's own root coroutine at 0x80106388 keeps walking its
  substate dispatch in parallel with the native `demo_frame_s3()` path). Removing the dual-wire (kept
  ONLY `EngineOverrides::register_`, the route `demo_frame_s3()`'s `rec_dispatch` actually uses) fixed
  the double-fire but NOT the r16 divergence — two separate bugs, both pre-existing/out-of-scope.
- **finding: sopLiftedSubtick wiring exposes a pre-existing ScriptInterp::step divergence** —
  registering `sopLiftedSubtick` (states 1/6 call `ScriptInterp::step(node)`, already-native,
  `game/scene/script_interp.cpp`, NOT part of this cluster) produces an SBS diff at `node+0x71`
  (`OBJ_FLAGS_71`) starting ~frame 63 of intro-area autonav: native (A) stays `02`, oracle (B) advances
  to `03`. Isolated by disabling ONLY `sopLiftedSubtick`'s `EngineOverrides::register_`/
  `ov_sop_set_override` calls — the other 5 SOP functions + Demo all still 0-diff. `sopLiftedSubtick`
  itself re-verified byte-exact (including its stack frame) against `ov_sop_gen_8010B588`; the
  divergence traces to `ScriptInterp::step`'s handling of SOP's specific script opcode content (an
  opcode/ret-code path this leaf is apparently the first caller to exercise under SBS since it was
  wired). `game/ai/beh_sop_intro_lifted.cpp`'s `overlay_subtick` deliberately stays on
  `rec_dispatch(c, 0x8010B588u)` (runs the oracle-verified substrate body under BOTH pc_skip=true
  default play AND pc_skip=false/SBS) rather than calling the native `sopLiftedSubtick` directly, so
  normal gameplay isn't exposed to the latent ScriptInterp bug either. NEXT: root-cause
  `ScriptInterp::step`/`advanceEntry` against `generated/shard_3.c:11302` (`gen_func_80041098`) for
  the specific opcode SOP's script data selects — out of THIS cluster's scope.
- **finding: Demo::s3SubMachine wiring exposes a pre-existing r16 register-liveness gap — RESOLVED
  (2026-07-10, root cause was r17, not r16)** — registering `Demo::s3SubMachine` produced an SBS diff
  at guest address `0x801FE98C` starting frame 13: oracle (B) wrote `0x1F800000` there, native (A)
  wrote `2`. The original diagnosis (byte-labeled "sp+16 slot of `demo_frame_s3()`'s call frame ...
  r16 register-liveness gap in demo_frame_s1/s2") was WRONG on every count — root-caused with
  `PSXPORT_SBS_PREWATCH=0x801FE98C` (arms a write-watch from boot, dumps the writing call's host+guest
  backtrace on the first divergent store — see docs/fleet-workflow.md §3): 0x801FE98C is NOT r16's
  spill slot in ANY of Demo's own frames — it's the **r17** spill slot (sp+36) inside
  `ov_demo_gen_80106824`'s prologue (the "commit pair" leaf both s2's and s3's inner sub-machines
  call), reached via `Demo::s3SubMachine` -> `rec_dispatch(c, 0x80106824u)`. `ov_demo_gen_80106AC4`
  (the REAL substrate s3 sub-machine, `generated/ov_demo_shard_0.c:333`) does
  `r17 = 0x1F800000; jal 0x80106824` right before that call — NOT an argument (0x80106824 reads only
  a0/a1=r4/r5), but the caller reusing its own callee-saved s1 as a scratch base register it wants
  ready for a post-call re-read (`sm = *(r17+312)` right after the call returns, matching `sm =
  *0x1F800138`). Since 0x80106824 spills the INCOMING r17 to its own guest stack before restoring it
  verbatim, that spill byte is part of the byte-exact guest-stack comparison even though the VALUE has
  no other observable effect. `s3SubMachine`'s port never set r17 before this call, so the loop's
  persistent `r17=2` (`Demo::stageBodyFaithful`'s "s1=2" constant) leaked into the spill instead. r16
  itself was ALWAYS correct end-to-end (traced with temp `[r16trace]` prints at every substate
  boundary — stayed `0x1F800000` from `Demo::stageBodyFaithful`'s setup through every frame, s1, s2,
  s3, unconditionally). **Fix:** `Demo::s3SubMachine` now sets `c->r[17] = 0x1F800000u;` immediately
  before `rec_dispatch(c, 0x80106824u)`, mirroring `ov_demo_gen_80106AC4:333` exactly
  (`game/scene/demo.cpp`). `Demo::registerOverrides(game)` is now called from
  `register_engine_overrides()` (`runtime/recomp/boot.cpp`). Verified: `PSXPORT_DEBUG=dispatch` shows
  `Demo::s3SubMachine` firing every frame from f13 onward (menu-cursor substate reached by
  `PSXPORT_SBS_AUTONAV=1`); SBS-full 0-diff held for 5700+ frames (~95s,
  `PSXPORT_SBS_EXIT_FRAME=5700`).
  Also DISPROVED en route (unchanged from the original note): `0x80106AC4` DOES have a direct
  intra-shard call site (`ov_demo_shard_0.c:126`, inside `FUN_801064E8` = the substrate's own s3 body)
  exactly like `game/ai/actor_melee_engage.cpp`'s shape — but dual-wiring it via `ov_demo_set_override`
  caused a genuine DOUBLE-FIRE per frame. Kept ONLY `EngineOverrides::register_` (the route
  `demo_frame_s3()`'s `rec_dispatch` actually uses); the direct call site inside `FUN_801064E8` stays
  unhooked (falls through to plain substrate, harmless, matches pre-existing behavior).
  **(stale note, RESOLVED elsewhere):** this session also re-observed the ovhit "all NEVER HIT under
  SBS" binding bug — that was independently root-caused and FIXED on main (commit b63eac3, per-Game
  registry + g_tab merge; see docs/findings/animation.md). ovhit is now reliable under SBS.- **refs:** game/ai/sop_intro_events.{h,cpp}, game/ai/beh_sop_intro_lifted.cpp, game/scene/demo.{h,cpp},
  runtime/recomp/boot.cpp.

## pc_skip exec: prologue vortex backdrop missing + scene SM stalls before area load (2026-07-10, RESOLVED)

- **status: RESOLVED (2026-07-10).** Root cause: `ScriptInterp::op05WaitFrames` (game/scene/script_
  interp.cpp) returned -1 on wait-expiry, mis-RE'd as a MIPS sign-replicate 0/-1 idiom. The generated
  substrate (`generated/shard_7.c:5216`, `gen_func_80042090`) computes `c->r[2] = c->r[2] << 16;
  c->r[2] = c->r[2] >> 31;` on a `uint32_t` — a LOGICAL shift (srl, not sra) yielding 0 or 1, not
  0/-1. In `ScriptInterp::step()`'s ret-code switch, ret=1 is RET_ADVANCE_0_A (cursor advances);
  ret=-1 falls to `default` (never advances). Every script in any exec path consulting
  EngineOverrides (default/pc_skip) was permanently parked at its first `op05` wait — the GAME-
  prologue pilot actor's script at 0x8010CA28 opens with `op05 wait 0x3C`, so it never reached the
  downstream beat-advance chain (SCENE_BEAT 0x800BF9B4: 0→1→2→3→5) that spawns the vortex void prop
  (beat==5 gate in beh_sop_intro_narration). Covers BOTH symptom 1 (wrong backdrop) and symptom 2 (SM
  freeze at 50=2/52=2 — same stalled script never reaches its terminal ops). Fix: flip the expiry
  return to `1`, rewrite the stale banner comment. GATE=1 core (recomp_path) was never affected — it
  doesn't consult EngineOverrides, so this only masked default/pc_skip.
- **also falsified in the same pass**: `game/ai/sop_intro_events.cpp`'s "per-KEYFRAME ANIMATION-EVENT
  callback table at 0x8010CA60" hypothesis — those are op-0x3E (call-fnptr) SCRIPT entries of the same
  pilot script at 0x8010CA28, not an animation-event table; both sopBeatAdvanceWalk/Narration ARE
  registered in RegisterSopIntroEventOverrides (the "Left UNWIRED" note was stale).
- **verification**: see below (rebuild + f600 screenshot + SCENE_BEAT trace + SBS-full gate).

- **how found**: operator live oracle-compare session (screenshot matrix drive, scratch/screenshots/oc/).
  Identical REPL input scripts (newgame; run 600; walk right; tap x) in two configs:
  oracle (`PSXPORT_ORACLE=1`) vs default pc run (pc_skip=true + pc_render).
- **symptom 1 — wrong prologue backdrop**: at f600 BOTH configs show the identical prologue dialogue
  ("Was she kidnapped? / Is she safe?") — state in sync — but oracle draws the purple VORTEX cutscene
  backdrop with spinning Tomba, while the pc config draws the gameplay FIELD (grass/fence/tree/
  birdhouse) with a grey/pink object at left. ISOLATED: `PSXPORT_GATE=1` + pc_render draws the vortex
  CORRECTLY (scratch/screenshots/oc/gate_pcrender_f600.png ≈ oracle) → the bug is in the PC (pc_skip)
  EXECUTION path, NOT pc_render. The vortex scene's state is never built by the shortcut path.
- **symptom 2 — scene SM stall**: with the same scripted input, oracle's stage SM advances into the
  area load at ~f1135 (`sm[4c]` transitions; `@0x80109450` flips 3C021F80 → 801138A4, then "Loading...",
  then gameplay at f1500); the pc run's SM counters freeze at `50=2 52=2` (f1054) and its picture is
  byte-identical from f1100 to f1500. Input-timing sensitivity between configs hasn't been excluded for
  symptom 2 (the x-tap may land on different dialogue lines); symptom 1 needs no such caveat.
- **tooling gap (fix with the root-cause)**: `PSXPORT_SBS_MODE=skip`'s curated observable-state list
  did NOT flag the missing vortex scene state (54/54 of its divergences were AUDIO spu_regs). When this
  is root-caused, add the scene/backdrop identity state to the observable list so the harness catches
  this class without screenshots.
- **repro**: scratch/logs/oc_drive_{oracle,pc}.repl + logs; screenshots under scratch/screenshots/oc/.
- **refs**: docs/bug-hunt-workflow.md (PC SKIP ON cell); MODE=render verified 0-diff same session
  (pc_render read-only invariant holds).

## Prologue-vortex root cause #2: SOP op3E wrappers discarded v0 (2026-07-10, ultracode workflow, RESOLVED)

- **stacked on** the op05WaitFrames expiry-return fix (root cause #1, same workflow, commit
  "scene: fix ScriptInterp::op05WaitFrames expiry return"): with op05 fixed the pilot's cutscene
  script ran up to its first op-0x3E entry and derailed there.
- **cause**: `ov_sopBeatAdvanceWalk`/`ov_sopBeatAdvanceNarration` (game/ai/sop_intro_events.cpp) did
  `(void)fn(c)` and never wrote `c->r[2]`, but these are op-0x3E fnptr callees —
  `ScriptInterp::callFnptr` returns `(int)c->r[2]` and `step()` drives pause(0)/advance(1..3) from
  it. Stale r[2] (whatever the last rec_dispatch left) fed garbage advance codes: walk exited at
  call 31 (first internal rec_dispatch) instead of 101; narration churned 156 calls without ever
  reaching its state-1 expiry that stamps 0x800BF80F=1 — so the script never reached the
  reveal/terminal ops → no vortex scene, SM freeze (both prologue symptoms, one chain).
- **fix**: wrappers publish the return (`c->r[2] = fn(c)`), matching the gen tails (0 running /
  1 done). Bodies untouched (transcription-correct). Header's falsified "animation-event table"
  reachability claim corrected.
- **dead ends closed**: ScriptInterp::step()'s ret-switch and both beat-advance bodies re-verified
  correct — the defect was wrapper-layer only. The op3E contract is now documented at
  ScriptInterp::callFnptr and in sop_intro_events.h.
- **process note**: found by the Evidence(sonnet)→Diagnose(Fable)→Execute(sonnet)→Verify(Fable)
  ultracode pipeline — two rounds, verifier independently confirmed cause #1 and forced the
  revision that found cause #2. Wrapper audit rule: any EngineOverrides wrapper for a function the
  substrate calls for its RETURN VALUE must set c->r[2]; grep for `(void)` wrappers when wiring.
- **refs**: workflow wf_a7f630fd-dc6; game/ai/sop_intro_events.{h,cpp}, game/scene/script_interp.cpp
  (callFnptr banner), runtime/recomp/sbs.cpp (SCENE_BEAT observable).

## Prologue-vortex root cause #3: objMatrixCompose dispatched FUN_80051128 with stale a0 → matMul zeroed SOP script data (2026-07-10, RESOLVED)

- **stacked on** causes #1 (op05 expiry return) and #2 (op3E wrapper r[2]) — with both fixed, the
  default config crashed at ~f750: `rec_dispatch_miss 0x801464C0` (caller `gen_func_8001D4C8`,
  a0=0).
- **cause chain** (each step verified live, exact frames):
  1. `Engine::objMatrixCompose` (FUN_800518FC, game/core/engine.cpp) dispatched its finalize leaf
     `rec_dispatch(0x80051128)` WITHOUT re-arming a0 — `gen_func_800518FC` does `r4 = r17` (obj)
     first. r4 was still `obj+0x98` from the preceding 0x80084470 call.
  2. `FUN_80051128` (skeleton bone-matrix composer) walked `obj+0x98` as an object: fake bone count
     0x50, "bone pointer" slot i=7 lands at `obj+0x174` — INSIDE THE NEIGHBOR NODE, on the SOP intro
     effect child's script cursor (0x8010CAC0). matMul then wrote a zero matrix over SOP overlay
     script data at 0x8010CAD8/0x8010CADC (caught by PSXPORT_WWATCH_BT: `[wwatch-regs] s3=800FBB00
     s1=8010CAC0 s2=7`).
  3. The child script's entry at 0x8010CAD8 (op 0x101E on the oracle) read back as opword 0x0000 =
     table[0] = `FUN_80041C54` (spawn positional-SFX emitter) with argA=0 → emitter node with sfx
     id 0 / zone fields 0 → first tick ran the zone-SFX dispatcher `FUN_8001D41C` → jump table at
     0x80010080 indexed by area byte 0x800BF870 (=0 during the prologue) → trampoline
     `gen_func_8001D4C8` → hard call 0x801464C0 (area-0 overlay, not resident) → recomp-MISS.
- **trigger config**: only paths where objMatrixCompose runs native (default/pc_skip). The clobber
  fired from `beh_sop_intro_narration` state_running sub==1 (beat 5, ~f544+).
- **second defect found in the same diagnosis**: `spawn_narration_prop`
  (game/ai/beh_sop_intro_narration.cpp) staged the FUN_8003116C arg block PACKED (X/Y/Z at
  +0/+2/+4); `ov_sop_gen_8010B990` stages halfwords at sp+18/+22/+26 with a1=sp+16 — the callee
  reads X/Y/Z at a1+2/+6/+10. The narration prop spawned at garbage coordinates. Fixed to the gen
  layout.
- **fixes**: `c->r[4] = obj` before the finalize dispatch (engine.cpp); arg-block layout
  (beh_sop_intro_narration.cpp). Verified: default runs past f1500 with 0 misses, script bytes at
  0x8010CAD8 stay `1E 10 ...`, narration text sequence matches oracle at aligned frames, SBS-full
  AUTONAV=combat 0-diff (f9030).
- **measurement traps closed** (recorded so nobody re-walks them):
  - wwatch pc/ra go STALE under native execution (native mem_w* doesn't advance c->pc) — writer
    attribution by pc alone produced three wrong suspects (spawn, classifier, sequencer) before
    PSXPORT_WWATCH_BT (host backtrace + guest regs) pinned matMul. Use the BT.
  - The 30-frame `[native_boot] frame` brackets ALIAS event timing: "beat 3 fires 30 frames early"
    was false — exact-frame wwatch shows EVERY beat lands at a constant +12-frame offset
    (default enters GAME 12 frames before the oracle; cadence identical). Frame-aligned compares
    must use default fN ↔ oracle fN+12 for this drive script.
  - `pc_skip exec + RENDER_PSX=1` shows NO scene entities — that's ARCHITECTURAL (entity render is
    owned by the pc walker `ov_scene_native`, which doesn't feed the PSX OT), not an exec bug. Do
    not use that combination to judge exec state; compare pc_render pictures or RAM.
- **still open after this chain** (default config, prologue, aligned-frame compare vs oracle):
  (a) beat-5 vortex swirl + spinning-Tomba visual missing under pc_render — the swirl packet
  builder is substrate overlay code whose output pc_render must rebuild natively (packets 0x34/0x09
  into 0x800C0xxx on the oracle; nothing on default); (b) oversized black shadow blob bottom-right
  at ~f300 (pc_render); (c) a missing character sprite + lighting/fade differences ~f500;
  (d) pilot anim-field divergence at aligned frames (+0x0E/+0x38/+0x8A/+0x92) — suspect follow-on
  of the walk anim chain, needs its own diagnosis. These go on the default-config burndown.
- **refs**: game/core/engine.cpp (objMatrixCompose), game/ai/beh_sop_intro_narration.cpp,
  runtime/recomp/mem.cpp (WWATCH frame/BT/regs), runtime/recomp/hle.cpp (miss-regs/miss-node),
  game/scene/script_interp.cpp (`PSXPORT_DEBUG=script`), docs/config.md.

## Prologue audio + anim divergences: one-byte instrument slip, stale-a3 anim seed, missing guest-frame mirrors (2026-07-10, RESOLVED except font)

- **audio (user-confirmed fixed)**: `AreaSlots::updateTail` action arm passed a2 = slot byte +1 to
  the note-request leaf FUN_80092660; gen (`gen_func_80075A80`) passes slot byte +2 (`r6 =
  mem_r8(r16+1)`, r16=slot+1). Every sequencer-routed SFX in the prologue played INSTRUMENT 0x0F
  instead of 0x01 (wrong sample 0x13D0 vs 0x0202, wrong pitch). Found by diffing per-voice
  `[spudbg] Vnn start/pitch` streams (extended `debug spu` channel) — after the fix the default
  config's SPU stream is byte-identical to the oracle through f800.
- **anim (Charles vertex explosion + Tomba pose)**: `Engine::walkStart` dispatched the anim-attach
  leaves without a3 — `gen_func_80054D14` passes a3 = subMode, which `gen_func_80077CFC` consumes
  as the anim PHASE SEED (obj+0x0E = a3+0x1000) and the decoder frame-seek arg. Stale a3 seeked the
  stream decoder to a garbage frame.
- **guest-frame mirrors added** (SBS-full WATCH_CUT leg divergences, worked with `sbs diff`/`sbs
  watch` + abi_extract contracts): ScriptInterp::step (sp-40, live r17=obj/r18=1/r19=table, r31
  constants 0x800410FC/0x800412E4/0x80041168, v0 on exits), ScriptInterp::advanceStep (sp-24,
  r16=obj, r31=0x80040FB4), Engine::animTick (sp-24, r16=obj, r31=0x80041920; now routes to
  Animation::stepFramed), Engine::objMatrixCompose (sp-32 + live r16/r17/r18 + per-site r31),
  Engine::walkStart (sp-32 + live regs + per-site r31), Engine::animEnvInit (sp-32),
  Sfx::trigger (ra@+32 spill), sopIntroEffectTick (live r17=node, r16 per gen; r31=0x8010B348).
- **gate-coverage gap (workflow)**: the standard AUTONAV=combat SBS leg SKIPS the intro cutscene
  (Start-mashing), so none of these divergences could ever trip it. `PSXPORT_SBS_AUTONAV=1` +
  `PSXPORT_SBS_WATCH_CUT=1` (cutscene plays out) is the leg that catches them: 3102 divs before
  these fixes, 922 after — remaining cluster is ONE defect (below).
- **RESOLVED (glyph half)**: the f289 `[packet_pool_ptrs]` 0x60 pool-pointer skew was
  `Font::glyphEmit`'s epilogue restoring `sp = sp0 + 56` where sp0 was already the ENTRY sp — the
  guest stack leaked UP 0x38 per call (MIRROR_VERIFY=0x80078CA8 caught it at invocation #1: exit
  sp 801FE980 vs entry 801FE948). Fixed to `sp = sp0`; 2497 invocations now byte-exact.
- **RESOLVED (actor half, 2026-07-10 late-late)**: the f289 "different cmd-word branch" framing
  was FALLOUT — the real chain, walked with three new probes (per-store `[sbs-ww]` t-reg + GTE-CR
  dumps via `sbs watchp`/`PSXPORT_SBS_PREWATCH`+`PSXPORT_SBS_WW_FROMFRAME`, then
  `PSXPORT_MIRROR_VERIFY` with entry-args in the header):
  1. Pool-cursor (0x800BF544) store sequences: both cores make the SAME 340 stores at f289; first
     divergent VALUE at store #159 — one extra GT3 passes the screen-cull on A (Δ=0x28) for prim
     group 0x8018E5DC.
  2. GTE CR dump at that store: composed ROTATION CR0-4 differs, translation CR5-7 matches ⇒ the
     compose input `cmd+0x18` differed MID-FRAME (frame-end RAM matched — one-frame transient, the
     "settled" reporter never flags it).
  3. `PSXPORT_SBS_PREWATCH=0x800F2BDC` (Charles cmd[0]+0x18): written once/frame by
     Math::matMul ← gen_func_80051128 ← objMatrixCompose, same site both cores, DIFFERENT values
     only at f289 (pose kickoff frame). Angle source (cmd+0x38): core A wrote via native
     Animation chain, core B via gen — different code paths on identical RAM.
  4. `PSXPORT_MIRROR_VERIFY=0x80077C40` (Animation::attach): TWO native defects, both fixed:
     (a) **v0/v1 leak** — gen attach's return value is the last callee's v0 (0 from
     FUN_80076904) or 0xC000 on the tag==0x8000 exit; the native never set v0, and the SOP
     overlay call site 0x8010B828 (ra=0x8010B830) BRANCHES on it — flipping the lifted actor's
     pose-kickoff arm for exactly one frame. Native now mirrors v0/v1 per exit.
     (b) **loadFrame 9-byte header** — gen's flags&0x40 arm has its OWN inline pose-triple unpack
     that advances the stream by 9 (padded header; final read does `r5+=5`), while the native
     reused the shared +4 (shared-nibble) helper — Loop1 then decoded every limb 5 bytes early
     (native f8=0x0020 constants vs gen 0x0FF5/0x0222, MV invocation #5, entry a0=800FB960
     a1=8010D39C a2=2, entry 0x8010D428 rec=0xCF000778). Native now skips the 5 pad bytes.
  5. f328 next: same class one deeper — native sopLiftedSubtick dispatched its callees
     (attach/77CFC/sfx/installSceneRecord/animEnvInit/script.step) WITHOUT arming r31; the
     substrate callees spill the caller's ra into their guest frames (A spilled stale 0x8010B874
     where gen B spilled 0x8010B6C8 at 0x801FE90C). All ten gen call-site ra constants now armed
     in sopLiftedSubtickBody.
  LESSON (recurring): a native replacing a gen body must mirror (i) return-value v0/v1 dataflow
  per exit — real callers branch on it; (ii) r31 before EVERY call — callees spill it; (iii) the
  exact stream-cursor arithmetic of per-branch inline decoders, not a shared helper's contract.
- **OPEN — next frontier f735 (after 24c7727)**: watch-cut first-div now 0x801FE8C0..CC (stack):
  last-writer A pc=80054D14 (native walkStart) ra=8010A990 (from substrate ov_sop_gen_8010A900) vs
  B pc=80077C40 (attach) ra=8010B4D0 (from substrate ov_sop_gen_8010B498) — the cores run DIFFERENT
  SOP behavior handlers at the same sp, and the stack bytes name different actor nodes (A spills
  0x800FB858, B 0x800FBB70). Both handlers are pure substrate; the flip is upstream (behavior-SM
  state or walk order). walkStart's v0 is NOT consumed at 0x8010A990 (checked — r2 overwritten
  immediately). Playbook that cracked f289/f328: `PSXPORT_SBS_PREWATCH=0x801FE8C0
  PSXPORT_SBS_WW_FROMFRAME=734` for the write sequences, then `PSXPORT_MIRROR_VERIFY=<suspect>`
  (header now prints entry a0-a3). Repro: PSXPORT_SBS=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1
  PSXPORT_SBS_WATCH_CUT=1 (headless exits at first div with last-writer map).
  RESOLVED (same session, commit after ce522c4): three probes deep, the cause was the op3E
  TRAMPOLINE FRAME — gen does not special-case op 0x3E (the handler table maps it to
  gen_func_800412CC, reached with r31=0x800410FC like every opcode), and that trampoline is NOT
  frameless: `addiu sp,-24`, spills ra@+16, arms r31=0x800412E4, jalr fnptr. Native callFnptr
  skipped the frame, so EVERY op3E callee on core A ran 24 bytes high on the guest stack — all
  their spills landed at shifted addresses (f735: A's attach spill missed 0x801FE8C0). Fixed by
  mirroring the frame in ScriptInterp::callFnptr (script_interp.cpp) + arming 0x800410FC at the
  step call site. RESULT: watch-cut leg **0-diff through f57300** (entire cutscene + gameplay
  horizon; was 3102 divs at leg introduction). Leg promoted into the standard push gate
  (docs/fleet-workflow.md §2). Probe trail below kept for the method.
  NARROWED (earlier same session, PREWATCH capture at f733-735): the walkStart writes match on both cores
  (the a3=0x659-vs-0 in the reg dump is mid-body trash, store values equal); the REAL first diff is
  that core B makes a SECOND write A never makes — ov_sop_gen_8010B498 (a tiny standalone snap-pose
  +attach helper: node+46/50/54/86 = 16000/-3888/20149/0x800 then attach(node,0x8001B860,0),
  ra=0x8010B4D0) runs for the effect child 0x800FBB70 on B only. Nothing jals 0x8010B498 — it is
  reached as a SCRIPT-OPCODE FUNCTION POINTER (ScriptInterp op3E callFnptr), i.e. the child's
  script CURSOR is one op ahead on B at f735 while script state matched at f734 end — a
  gated-op / mid-frame-condition timing split (same class as the "pilot 1-anim-step lead").
  Next probe: PSXPORT_DEBUG=script on both cores at f734-735 (per-opcode dispatch log) to see
  which opcode/condition diverges, then MIRROR_VERIFY the native that produces the gate value.
- **OPEN (superseded framing, kept for the probe trail)**: at the f289 store (`sbs watch 800c3c54` + the new
  `[ww-regs]` line), core A and core B are in DIFFERENT PRIM-TYPE BRANCHES of gen_func_8007FDB0
  (`(cmdword>>24)&3`: A=1 at L_8007FF24, B=2 at L_8007FF78) — i.e. the mid-frame CMD-LIST ENTRY for
  Charles' prim (s2=0x800FB960 live at the store) has a DIFFERENT command word on core A. The
  producer is the object-walk side (native behaviors enqueueing render cmds); the cmd list is
  consumed within the frame so end-of-frame SBS never sees it directly — watch the cmd-list write
  next (find the list base via Render::cmdListDispatch, then `sbs watch` the entry). Chain:
  Render::renderWalk(native A) / gen_func_8003C048(B) → mode handler (A: perModeDispatch replaces
  gen_func_8003F698 — NB that gen also runs a PER-AREA pre-draw hook via table 0x80015268 gated on
  0x1F800234==0, verify the native replicates the gate) → gen_func_800803DC → gen_func_8007FDB0.
- **prior framing (kept for context)**: with stacks aligned the remaining
  first-div is an ACTOR VERTEX: `sbs watch 800c3c54` fires in the entity render walk (ra chain
  0x8003F790/0x8003D07C; Charles 0x800FB960 + effect child 0x800FBB70 on the stack) with A packing
  vertex (0x116,0x82) vs B (0x113,0x83) — a (3,1)px pose delta, i.e. an anim/orbit phase lead on
  core A. MIRROR_VERIFY=all through f320 (PSXPORT_MIRROR_VERIFY_CONTINUE=1) flags real non-reg
  divergences to triage: `Demo::s3SubMachine` 0x80106AC4 writes stack bytes 0x801FE9A8-AD
  native=F0,64 vs substrate=40/10,6B (real RAM diff, default path), and
  `Render::gpuDmaQueueEnqueue` 0x80082D04 leaves hi/lo=FFFFFFFF/FFFFFF00 vs 0/0000F000 (callee
  mult/div not reproduced natively — matters only if a caller reads hi/lo after). Plus ~7k
  caller-saved-register-only (v0/v1/t) mismatches across 0x80079528/0x80080F6C/0x80081458/
  sopLiftedSubtick/sopIntroEffectTick/op36/op34/sopBeatAdvanceWalk — likely harness-visible noise
  but any one whose caller stores the stale reg is a real source; triage next session. Also a
  f1126 stack residual (0x801FE888: A=0x800FB960 B=0x0000000E). Repro:
  `PSXPORT_SBS=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_WATCH_CUT=1`.
- **tooling added**: `PSXPORT_SPUBT=<hex SPU reg off>` host backtrace per SPU write; `debug spu`
  logs per-voice pitch/start; wwatch prints exact gpu frame (see config.md).

## Narration cutscene out of sync in MODE=skip: DEMO→GAME handoff 12 frames early on the skip pane (2026-07-14, RESOLVED)

- **symptom (USER):** MODE=skip WATCH_CUT renderdiff panes visibly out of sync through the whole
  narration cutscene — port pane's text pages / character walk-ins run ahead of the oracle
  (13-20% pixel diff band, growing per beat).
- **cause:** `[sbs-skiptick]` probe (docs/config.md) showed vsync/scratch tick counters EQUAL
  (no frame drift) but the task-0 stage word flipping DEMO(0x801062E4)→GAME(0x8010637C) at f26 on
  A vs f38 on B, and every SOP beat (0x800BF9B4) firing exactly 12 frames early on A thereafter.
  `demo_frame_s5` → `Engine::startStage(2)` loads the GAME overlay in ONE native call where the
  oracle spends ~12 frames of CD-paced sectors — an un-gated pc_skip fork (only `start_bin_load`
  had a rendezvous).
- **fix (two parts):** (1) `demo_frame_s5` (game/scene/demo.cpp) holds s5 via
  `skipRendezvousReached(task+0xc, 0x637C, "demo_start_game")` until the sibling's task entry
  flips — compare-mode-only, no-op in every other config; (2) MODE=skip now steps core B (oracle)
  FIRST in the lockstep loop (sbs.cpp) so rendezvous reads see B's same-frame state — with A-first
  every gated fork lagged 2 residual frames (measured), B-first makes it 0. Verified: skiptick
  shows stage + beat deltas never change post-boot; narration panes pixel-near-identical (~4%
  residual = real render diffs, text edges/dither), down from 13-20% skew.
- **NEXT (open):** the diff ramps to 40%+ at ~f588 — the narration→next-scene transition is
  another un-gated fork of the same class. Same recipe: SKIPTICK probe, name the transition,
  gate it with a rendezvous label. Repeat until the whole watch-cut leg is beat-aligned.
- **refs:** scratch/logs/skiptick*.log, scratch/logs/renderdiff_aligned.log,
  scratch/screenshots/renderdiff-* (pane dumps pre/post alignment), docs/config.md
  (SKIPTICK / RENDERDIFF_FROM / step-order note).

## Vortex void beat black on pc_skip — RESOLVED (2026-07-14, bug #43)

- **symptom:** default config (pc_skip + pc_render), `newgame; run 600`: black + narration text
  (scratch/screenshots/vortex_default_f600.png). MODE=skip pane A black from ~f588.
- **isolated:** GATE=1 + pc_render draws vortex + Tabby + coin correctly at the same state
  (vortex_gate_f600.png) → pc_render fine, **pc_skip EXEC fails to build the vortex scene state**.
  Same isolation shape as the 2026-07-10 finding above, but the old cause is EXCLUDED: op05WaitFrames
  returns 1 on expiry (verified in source), and SCENE_BEAT advances identically to the oracle
  (SKIPTICK, A==B through f900). NB the initial "beat==5 consumer not firing" suspicion was WRONG —
  beat-aligned full-RAM diff showed the entity lists byte-identical (incl. scene-2 node 0x800FBA68);
  what differed was packet pool (0x4D4 vs 0x2F70 bytes emitted) and OT bins (empty chains vs threaded).
- **cause:** the native `Sop::fieldUpdate` (game/scene/sop.cpp) had DELETED the substrate per-frame
  body's two UNCONDITIONAL render dispatches — scene-table render `0x80109FE0(a0=0x800F2418)` and
  object render-list walk `0x8003C048` (generated/ov_sop_shard_1.c, between the two beat-gated BG
  calls) — on the theory that ov_scene_native solely owned rendering. But sceneNative is gated OFF
  for the void beat (game_tomba2.cpp beat==5 skip, the old sea-in-void fix), so on pc_skip NOTHING
  emitted the narration prop's swirl quads, and the full-OT walk that IS the narration picture drew
  an empty OT. (The 2026-07-10 spawn-coordinates fix above was a different, real bug.)
- **fix:** restore both dispatches in Sop::fieldUpdate at the substrate's exact call positions. Also
  repairs a guest-state divergence — OT/packet-pool bytes are faithful state pc_skip must reproduce.
- **verify (5/5):** default beat-5 shots show vortex+Tabby+coin; pool cursor 0x800C2F70 == GATE band;
  OT bins threaded; beat-3 A/B no double-draw; SBS-full 0-diff hut-replay f70620 + autonav f117540.
- **lesson:** a "sole render owner" refactor must account for every beat-gate on the native side;
  the substrate's unconditional dispatches are faithful state even when a native pass draws the picture.
- **refs:** bug #43, scratch/screenshots/vortex_{default,gate}_f600.png, sop_overlay_shadow.cpp
  header (beat gate map), beh_sop_intro_narration.cpp.

## pc_skip FUN_80044BD4-collapse INCOMPLETENESS class (2026-07-15) — audit of the #53 bug family — FIXED
- ROOT CAUSE (workflow): FUN_80044BD4's tail was hand-re-derived at 3 pc_skip collapse sites instead of
  shared. The COMPLETE byte-exact port is PcScheduler::spawnAndWait (pc_scheduler.cpp:142-187) — all sites
  should route through a shared helper of its tail side-effects, not re-author.
- FUN_80044BD4 (gen_func_80044BD4, generated/shard_3.c:11775): spawns slot-1 task, then if a3!=1:
  draws RNG (FUN_8009A450) and UNCONDITIONALLY stores its RETURN VALUE (v0) as a HALFWORD at task+0x56
  (the earlier RE misread this as literal 1 — v0 is the RNG result, the `1` was clobbered by the RNG call);
  then if a3==2 the wait loop bumps u16 0x1F800198 + dispatches FUN_8007FD54 (icon/label placement).
- INCOMPLETE sites (ranked by reachability), all now fixed:
  1. Engine::submode1Case0Skip (engine.cpp:2277, a3=2) — HIGHEST, every field area load. Was missing the
     WHOLE tail (task+0x56 stamp + 0x1F800198 bump + FUN_8007FD54). Probe CONFIRMED: 0x801FE056 stale
     (73 3A) on default vs written (25 7E -> 23 10) on faithful (PSXPORT_PC_SKIP=0) across submode1
     case0->2 (~f112-116). FIXED: `c->game->pcSched.bd4Tail(c->mem_r32(0x1f800138u), 2)` inserted BEFORE
     `SV_CHECK(...transitionAreaLoad())`.
  2. Sop::fieldMode case 0 (sop.cpp:493, a3=3) — was missing the task+0x56 RNG stamp (the RNG draw
     already existed for cadence timing but its result was discarded). FIXED: `(void)c->rng.next()`
     replaced with `c->game->pcSched.bd4Tail(sm, 3)` — same single draw, now stored (no counter/FD54,
     a3!=2). Draw count unchanged (1).
  3. demo.cpp:938 (#53 fix, a3=2) — VALUE bug: wrote literal 1 to sm+0x56 instead of the RNG stamp.
     FIXED: `c->mem_w16(sm + 0x56u, 1)` + the separate counter-bump/FD54 lines replaced with
     `c->game->pcSched.bd4Tail(sm, 2)`.
- FIX: extracted `PcScheduler::bd4Tail(uint32_t taskBase, uint32_t flag)` (game/core/pc_scheduler.h/.cpp) —
  the ONE authoritative copy of the a3!=1 tail (RNG stamp store + flag==2 counter/FD54). All 3 collapse
  sites now call it instead of re-deriving. spawnAndWait itself was NOT routed through it — its flag==2
  counter/FD54 repeats per wait-loop iteration gated on done==0, a shape the single-shot helper doesn't
  model; spawnAndWait keeps its own inline tail (see pc_scheduler.cpp comment above bd4Tail).
- VERIFIED: build clean; SBS-full (PSXPORT_SBS_MODE=full AUTO_SKIP) 0-diff through 30390+ frames (helper
  only touches pc_skip branches, which core B doesn't run); MODE=skip AUTO_SKIP SKIP_CONTINUE shows zero
  task+0x56/field-load-related divergence (only pre-existing unrelated AUDIO spu_reg timing jitter, a
  separate known class); default free-roam boot + attract-DEMO boot both unaffected (AUTO_SKIP reaches
  free-roam at f216 as before). Per-site probe of 0x801FE056 (task+0x56): default now writes a real drawn
  stamp instead of staying stale/literal at every site (demo default f500: `23 10`, was literal `01 00`;
  engine.cpp default f200+: `82 5C`, was stale/missing). The exact byte value differs from the faithful
  run's own draw (`23 10` at the same probe) — expected: pc_skip and faithful accumulate different total
  frame counts before reaching this point (documented "SBS two compare modes" — MODE=skip does NOT require
  byte-exact RNG-stream alignment, only the fixed observable list + SBS-full on the faithful path, which
  stayed 0-diff).
- NOTE: PSXPORT_GATE=1 is NOT a pc_skip=false oracle (only changes exec substrate); use PSXPORT_PC_SKIP=0
  or SBS-full to force the faithful Engine::pc_skip branch (boot.cpp:141-144).
- REGRESSION FOLLOW-UP (2026-07-15) — bd4Tail DOUBLE-DRAW, fixed same day. The 11b3205 fix inserted
  `bd4Tail(...)` (which draws the RNG stamp as its FIRST action, pc_scheduler.cpp:150) right after
  pre-existing standalone `(void)c->rng.next()` "Slip #5" lines at demo.cpp:920 and engine.cpp:2279 —
  so those 2 sites drew the RNG TWICE per invocation where the guest draws it ONCE. Those stray lines
  were written under a FALSE belief (documented in their own comments) that `func_80051F14` (spawnPrim,
  the task-1 registration) itself draws RNG. It does NOT: `gen_func_80051F14` (generated/shard_2.c:6253)
  and its callees (func_80080930/890/860/8A0…) make ZERO func_8009A450 calls; the ONE draw in the whole
  FUN_80044BD4 body is at gen line 11809, AFTER the `if (r19==1) goto epilogue` check — i.e. only flag!=1.
  Also `native_area_load_bd4` (engine.cpp:1661, the flag=1 door/sub-scene load) drew 1 RNG where the
  guest draws 0 (flag=1 jumps to the epilogue before func_8009A450). FIX: deleted all three stray draws;
  bd4Tail is now the SOLE RNG draw for flag!=1 sites, and flag=1 draws none. This is a pc_skip=true-ONLY
  bug — invisible to SBS-full (which forces pc_skip=false), only visible via PSXPORT_RNG_CALLTRACE=1.
  VERIFIED: post-fix RNG_CALLTRACE (AUTO_SKIP headless) shows the stray `submode1Case0Skip+0x2c` draw
  GONE and only `bd4Tail+0x18` firing (matching gen's 1-draw-per-flag!=1-load); SBS-full 0-diff to f30720.
  LESSON: when extracting a shared helper that performs a side effect, AUDIT every call site for a
  pre-existing standalone copy of that same side effect — the helper insertion doesn't remove it.

## FUN_80040B48 dual-ownership (scene-event ARM) — DEDUPED (2026-07-15)
- SYMPTOM/DEFECT: two independent native bodies for ONE guest fn. game/scene/scene_events.cpp owned
  FUN_80040B48 as SceneEvents::armBody (called via the native `arm(eventId)` API by entity.cpp,
  beh_pickup_collect_trigger, etc.), while game/object/cube_text_ledger.cpp ALSO owned it as
  CubeTextLedger::activateSlot (registered in the EngineOverrides + g_override[] tables, so
  substrate func_80040B48 + ActorReward's rec_dispatch(0x80040B48) hit THAT copy). Native-API callers
  and address-reaching callers ran DIFFERENT native bodies for the same function.
- WHY IT SLIPPED: the two authors (scene_events arc-12, cube_text_ledger 2026-07-08 wide-RE) each
  RE'd the address independently under different names ("scene-event arm" vs "cube-text popup ledger
  activate") — the SAME mechanism (per-slot flag + counter + cost accumulate + ring log at 0x800BF870/
  0x800BF8A8/0x800BF874/0x800ED058). codemap couldn't warn because it never parsed live
  `ov.register_(0x…, "…", fn)` calls, so cube_text_ledger's ownership was invisible.
- GROUND TRUTH: gen_func_80044B48... (gen_func_80040B48, generated/shard_4.c:4944) — gate s16@0x800E7FEE
  ==0 → -1; SLOT_STATE (0x800BF870+r4+68, r4 UNMASKED) !=0 → 0; else set=1, ACTIVE_COUNT(0x800BF8A8)++,
  RUNNING_COST(0x800BF874) += classSize(r4, hi-nibble), ring-log (slot@0x800ED06E+idx, event=0@
  0x800ED074+idx, LOG_INDEX@0x800ED06D++), return 1. Both native copies matched this EXCEPT armBody
  masked the slot index to a byte (`r4 & 0xFF`) — a latent deviation, unreachable while event IDs < 256.
- FIX: SceneEvents is the sole owner. armBody's byte-mask dropped (now full-width r4, matches gen +
  the former activateSlot). Added SceneEvents::armOverride (guest-ABI thunk) + registerOverrides
  (EngineOverrides + psx_fallback-gated shard_set_override), moved the 0x80040B48 wiring off
  CubeTextLedger; deleted CubeTextLedger::activateSlot. cube_text_ledger.cpp now owns only
  FUN_80040C00 (deactivate) + FUN_80040AA4 (spawn). Same dedup shape FUN_80040A58→classSize got.
- TOOL FIX (the real root cause — see docs/findings/tooling.md): codemap.py now parses live
  EngineOverrides registrations (load_engine_overrides) and has a `--conflicts` mode + `--addr`
  ⚠ DUAL-OWNERSHIP warning that flags any guest address implemented across ≥2 files. This would have
  caught the duplicate at authoring time.
- VERIFIED: build clean; codemap --addr 0x80040B48 = single-file owner, gone from --conflicts;
  SBS-full (SBS_AUTONAV, dark-screen replay which exercises the arm path) 0-diff to f23100 with
  PSXPORT_MIRROR_VERIFY=0x80040B48 "OK (pass #1)". Logs: scratch/logs/sbs_b48_dedup.log.

## FUN_80040CDC dual-ownership (script init mis-named "animEnvInit") — DEDUPED (2026-07-15)
- SYMPTOM/DEFECT: second dual-ownership found by the improved `codemap.py --conflicts` (authoritative
  filter). game/scene/script_interp.cpp owned FUN_80040CDC as ScriptInterp::init (the cutscene-script
  bytecode init — documented, wired, gated, 8+ live callers via c->engine.script.init). game/core/
  engine.cpp ALSO carried Engine::animEnvInit "FUN_80040CDC" — a mis-named duplicate written in the
  engine.cpp anim-leaf-cluster refactor under the wrong belief that FUN_80040CDC is an animation-env
  init. It is not: gen_func_80040CDC writes obj[0x7C]=arg1, obj[0x46]=0xFF, obj[0x10/0x70/0x78]=0,
  calls func_80040DE0 (=loadCurrentEntry), derives obj[0x71] from op0's 0x1000/0x4000 bits — the
  SCRIPT machine's fields, byte-for-byte ScriptInterp::init.
- FIX: deleted Engine::animEnvInit + its engine.h decl + its now-orphaned ObjAnimField constants
  (kEnvPtr/kFlags10/kFlag70/kFlag78/kFlags71). Redirected its 3 callers (beh_sop_intro_pilot.cpp
  anim_env_setup, sop_intro_events.cpp:341/482) to c->engine.script.init(obj, tableA, scriptPtr) —
  a direct arg-order match (envArg→tableA, animData→scriptPtr). ScriptInterp::init is strictly the
  better body: fully native (native loadCurrentEntry, no guest-frame descent) vs the duplicate's
  framed + substrate-loadCurrentEntry path; both produce identical object writes, the only difference
  being dead guest-stack scratch neither the game nor SBS reads.
- VERIFIED: build clean; codemap --addr 0x80040CDC single-owner, gone from --conflicts; SBS-full
  AUTO_SKIP 0-diff to f14130. NOTE: these call sites are DIRECT native calls (not rec_dispatch), so
  both SBS cores run the identical native body — the redirect is equivalent by construction, not
  oracle-gated. The new-game SOP-intro path that exercises them live is not in the current SBS replay
  set (AUTO_SKIP skips the new-game intro) — disclosed, like other replay-coverage gaps.
- LESSON (same as FUN_80040B48): before RE'ing any FUN_xxxx, `codemap.py --addr <hex>` — a canonical
  owner may already exist under a different subsystem's name. The --conflicts detector now catches the
  slip after the fact; checking --addr up front prevents it.

## Remaining codemap --conflicts candidates (2026-07-15) — TO TRIAGE next iteration
After deduping FUN_80040B48 + FUN_80040CDC, `codemap.py --conflicts` (authoritative filter) leaves 3:
- **0x80044BD4** — Demo::s0PreYield / PcScheduler::bd4Tail / PcScheduler::spawnAndWait. INTENTIONAL,
  NOT a bug: the documented pc_skip FUN_80044BD4-collapse family (bd4Tail is the shared tail helper,
  spawnAndWait the full port, s0PreYield the demo collapse). Leave as-is.
- **0x800518FC** — Engine::objMatrixCompose (engine.cpp anim-cluster) vs NodeXform::buildWithOffset
  (node_xform.cpp). LIKELY a real dup (same cluster pattern as the animEnvInit slip), BUT unlike
  animEnvInit this is NOT a trivial field-write dup: objMatrixCompose does GTE-ish matrix work via
  SUBSTRATE leaves (setvec 0x80085480 + mul) while buildWithOffset uses NodeXform native math — need
  to RE both vs gen_func_800518FC and confirm byte-equivalence BEFORE consolidating. NodeXform is the
  semantic home; objMatrixCompose (LIVE via beh_sop_intro_pilot post_cull_update) would redirect there.
- **0x8002AB5C** — NativeScenePass::terrainRender (native_terrain.cpp) vs Render::terrain (submit.cpp,
  desc starts "RETIRED 2026-07-07 #32"). AMBIGUOUS: Render::terrain may be a stale-tagged retired
  method (remove the tag) or a real second owner. Investigate which.

## Cross-area interior code-overlay never loads — RE chain to the sole MODE-slot loader (2026-07-17, OPEN, RE-frontier)

- **symptom (USER):** interiors (fisherman's hut, sm[0x4c]==3) don't render as their own area; earlier finding
  "hut interior much different / still shows outside" + fps60 30fps. Cross-area warps recomp-MISS on the
  destination area's code (e.g. `warp 21` → miss `0x8010D030`; `warp 3` → miss `0x8010B37C`) with **A00
  still resident** in the MODE slot — the per-area CODE overlay `ov_a0<id>` never loads.
- **RE'd chain (Ghidra ram_sea, this session — supersedes the 2026-07-10/11 finding which mis-blamed
  "case 6 skips case 0" and mis-read the next-state table endianness):**
  - **The sole MODE-slot (0x80108f9c) code-overlay loader is `FUN_800452c0` line ~162:**
    `FUN_80045080(0x80108f9c, area+3)` → `FUN_8001dc40(0x80108f9c, tbl[area+3].lba, .size)` where
    `tbl = 0x800be118` (stride-8 LBA/size). `FUN_80045558` (the only OTHER `FUN_80045080` caller) targets
    `0x8020A000` (bonus/secondary data), NOT the MODE slot.
  - `FUN_800452c0` is spawned ONLY by `FUN_80044bd4(FUN_800452c0, area, 0, 2)` = **submode1 (`FUN_801088d8`,
    the sm[0x4a]==1 field area machine) case 0** — which FALLS THROUGH to case1 (`sm[0x4c] = *(u8*)(0x80108f60+area)`).
    Native mirror: `Engine::submode1Faithful`/`submode1` + `Sop::transitionAreaLoad` (which DOES reproduce the
    `FUN_80045080(0x80108f9c, area+3)` load at sop.cpp:164).
  - **next-state table `0x80108f60[0..0x17]` (LE-corrected):** `02 02 04 05 02 02 02 04 02×… 00(22) 00(23)`
    i.e. area0=2, area21=2, only areas 22/23 = 0. So the door path (`fieldRun`/`FUN_80106b98` case 6, trig==3
    → sm[0x4a]=1/4c=1/4e=0 → submode1 case1 → `sm[0x4c]=nexttab[dest]`) routes MOST dests (incl. 21) straight
    to a RUNNING state (2/4/5), **never entering submode1 case 0**, so the code overlay never loads.
- **the open contradiction (next RE step):** the guest ALSO routes case6→sm[0x4c]=1→case1 and skips submode1
  case 0 for nexttab≠0 areas, yet on real HW the overlay IS resident. So the load for those areas fires from
  a path not yet found — candidates: (a) one of `fieldRun` case 0's other 6 setup leaves (`FUN_8007b18c/
  800796dc/800263e8/80072a78/80075240/800783dc/80078610` — only `FUN_80074f24` checked = audio, not a loader);
  (b) another `FUN_800452c0` spawn site inside the **ov_game overlay** (not searched — the main-shard grep for
  `FUN_80044bd4(0x800452c0,…)` only sees the submode1 one); (c) the initial field-entry load path. RE those,
  find the real per-area code-overlay-load trigger, port it. Downstream: native interior WORLD render producer
  (renderHutInterior currently abortUnimplemented()s by design, break-first 2026-07-16).
- **refs:** engine.cpp `submode1Faithful`/`Sop::transitionAreaLoad`/`fieldRunFaithful` case 6; Ghidra dumps
  this session; `warp <id>` REPL + `PSXPORT_DEBUG=ovload,stage`.

### UPDATE (2026-07-17, same session) — load PATH proven functional; gap is routing + NO REPRO

- **The MODE-slot load path WORKS for nexttab==0 areas.** `warp 22`/`warp 23` (`0x80108f60[22]=[23]=0`)
  hit submode1 case 0 → `Sop::transitionAreaLoad` (bf870=dest, sm6d=2, full DMA path) → **zero recomp-miss**.
  So `FUN_800452c0`/`FUN_80045080(0x80108f9c,area+3)` correctly load the code overlay when case 0 runs.
- **Corrected table read (final):** `0x80108f60[0..0x17]` = `02 02 04 05 | 02 02 02 04 | 02×8 | 04 02 00 00`
  → nexttab[3]=5, [21]=2, [22]=[23]=0. Only 22/23 route through the load (case 0); 0..21 go straight to a
  running state (2/4/5) that dispatches the area's code — a MISS if that code is a non-resident A0X overlay
  (warp 3 → ov_a03 0x8010B37C; warp 21 → ov_a0l 0x8010D030 — both confirmed A0X, not A00 → true residency gap).
- **NO WORKING REPRO of a cross-overlay area entry.** `hut-entry-door-freeze.pad` runs to completion on BOTH
  pc_faithful AND the ORACLE (`PSXPORT_GATE=1`) without ever loading an A0X into the MODE slot — it never
  reaches the hut interior. Forced `warp <id>` (trig==3) for nexttab≠0 areas reproduces the miss but is an
  ARTIFICIAL transition (a real door to an overlay-swap area may set state that routes through case 0 / a
  different trig). **This is the current blocker: no way to observe/verify the real overlay-swap door path.**
- **NEXT STEP (unblock):** RE the actual hut/world door OBJECT that triggers an overlay swap (what trig /
  sm[0x6d] / bf89c it writes) to (a) build a real repro and (b) see whether it routes through submode1 case 0.
  Ruled out as loaders this session: FUN_80074f24 (audio), FUN_80106a24/sm[0x4a]==3 (FMV poll). Until a repro
  exists, further static tracing is unproductive — the interior is repro-blocked, not RE-blocked.

### UPDATE 2 (2026-07-17) — the warp is an INVALID repro; a0l is preloaded; needs a real driven entry

- **`warp <id>` to a cross-overlay area is INVALID — it crashes even the ORACLE.** `PSXPORT_GATE=1` +
  `warp 21` → EXIT 139 at `sm[0x4e]=0xb`: fieldRun case 0 SPECIAL-CASES `bf870==0x15` (area 21) →
  `sm[0x4e]=0xb` → `FUN_8010957c` (an a0l-overlay fn) — but a0l is NOT resident, so the substrate itself
  derails. Warping jumps to area 21 without its overlay preloaded; it is not a valid transition.
- **The hut door-fade sequence IS a0l code.** `game/render/screen_fade.cpp` (the case-5 writer of
  `bf839=3 / bf83a=0x1501`) calls `helperCC68` = `rec_dispatch(0x8010CC68)` = **`ov_a0l_func_8010CC68`** at
  every fade step. So a0l is ALREADY resident when the hut door arms — the door transitions WITHIN the a0l
  region; a0l is loaded EARLIER (when the hut's outer area is first entered from A00), not at door-cross.
- **CONCLUSION: the interior cross-overlay load cannot be warp-reproduced headless.** Reaching it requires
  DRIVING the real game from the seaside into the hut's a0l area (interactive, or a genuine pad-capture that
  actually reaches it — the existing `hut-entry-door-freeze.pad` does NOT, on either path). This is a
  reproduction-infrastructure blocker, outside headless static RE. **The RE understanding is complete; the
  work is blocked on a real driven repro** (user-captured hut-entry replay, or the A00→a0l region-entry path
  RE'd from the specific seaside door object that first loads a0l — a separate, larger effort).
- **Recommendation:** pause interior work pending a real repro; the ledger of established facts above is the
  handoff. Do NOT re-attempt warp-based interior repro (proven invalid).

### UPDATE 3 (2026-07-17) — dev-warp made cross-overlay-VALID; area 21 now blocked on a recompiler-seeding gap

- **The old door-record `warp` was INVALID for cross-overlay areas** (it never loaded the dest overlay).
  FIXED in `runtime/recomp/native_boot.cpp`: the warp now runs the FULL native area load directly —
  `sm[0x6e]=dest, sm[0x6d]=2 → Sop::transitionAreaLoad()` (code overlay `FUN_80045080(0x80108f9c,dest+3)` +
  area DATA + reloc tables + `bf870=dest`) then forces the running state (`sm[0x4a]=1, sm[0x4c]=nexttab[dest]`).
  **VERIFIED: `warp 3` (ov_a03) now loads clean, 0 miss (was a miss-crash on 0x8010B37C); `warp 0` clean.**
  So cross-overlay areas are now reachable via warp — the "be able to reach it" tooling gap is closed for
  areas whose overlay functions are all recompiled.
- **Area 21 (hut interior, ov_a0l) now gets MUCH further** — a0l loads, `bf870=21`, runs `sm[0x4c]=2` — but
  still crashes on a **recompiler-SEEDING gap**: dispatch to `0x80109200` (from sceneEventFifo's `FUN_800251F0`)
  misses because a0l's first recompiled fn is `0x80109208` — `0x80109200` (8 bytes earlier, reached via a
  runtime fn-ptr, not a static call) was never DISCOVERED by the recompiler, so `ov_a0l` has no entry for it.
- **NEXT STEP:** seed `0x80109200` (+ any sibling runtime-dispatched a0l fns surfaced by re-running
  `warp 21`) into the a0l overlay's recompiler function list and regenerate ov_a0l, then re-test `warp 21`.
  This is recompiler-seeding work (tools/ recompiler), distinct from the RE now complete. The dev-warp is
  the reachability vehicle.

## Resident-aware override dispatch — REJECTED (residency gate breaks render path) [2026-07-17]

- **Symptom:** batch-owning 291 "a00-unique" leaves via `ov_a00_set_override` → boot SIGABRT (both pc_skip + SBS), 0 leaves fired before abort. 190 MAIN-range batch is fine.
- **Root cause (2 diagnose agents, high conf):** `overrides::dispatch()` (overlay_router.cpp:182) is address-keyed and runs BEFORE the resident-overlay routing (L234-261). During START→SOP boot, SOP is resident in the shared MODE slot (0x80108F9C+); when SOP's body `rec_dispatch(c,X)`s a numeric address that is ALSO one of the 291 a00 override addrs, dispatch() hijacks it into the a00 native leaf → leaf reads foreign RAM / re-dispatches an a00 sub-addr SOP has no entry for → `rec_dispatch_miss` abort (hle.cpp:269). "a00-unique in the DISPATCH TABLE" ≠ cross-overlay ENTRY-unique; the dispatch path is entry-table-agnostic.
- **DEAD END — residency-gating dispatch() (both workflow designs, refuted):** gating `dispatch()` on `overlay_router_resident_name(c,addr)==owner` REGRESSES the a00 RENDER overrides. `override_registry.h:59-63` documents that dispatch interception exists PRECISELY to fire overrides *independent* of live-RAM residency (gen bodies always linked). pc_render's node walk dispatches a00 render leaves (OverlayGt3Gt4 0x801465EC/7BC, OverlayGroundGt3Gt4, TileGridLayer, WidescreenMarginQuad, ActorMeleeEngage) while SOP may still be signature-resident (the "later-275" window) or during a load-transition frame (name "none") → gate returns false → `res->disp(SOP)` at an a00 addr → abort. Do NOT add a residency gate to the crown-jewel dispatch path.
- **CORRECT FIX (batch selection, not dispatch change):** own an overlay leaf as an a00 override ONLY if its address is a valid code entry/dispatch-target in NO other overlay/module (true cross-overlay ENTRY uniqueness, verified against SOP/START/other overlays' entry tables), OR own the shared-code leaf as MAIN/shared (fired via `shard_set_override`, always resident). Shared-entry addresses stay substrate. Refs: workflow wf_340a9067-89b (journal has full designs+refutations).
