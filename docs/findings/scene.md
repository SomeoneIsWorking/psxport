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
