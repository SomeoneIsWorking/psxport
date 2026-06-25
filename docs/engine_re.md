# Tomba! 2 engine RE — the game's own engine (for the native reimplementation)

Living doc for the native-engine port (plan: reimplement Tomba2's engine in PC-native C). **Runtime is
INTERPRETER-ONLY (later-103):** un-owned code runs the real PSX binary on the flat interpreter; the static
recompiler is an offline analysis aid only. The **verification oracle is the Beetle emulator**
(`runtime/wide60rt`). Source for all line refs: `scratch/decomp/ram_f1000_all.c` (Ghidra decomp of
MAIN.EXE). Addresses are PSX RAM virtual addresses. **Verify against the oracle (or the original interpreted
path) before relying on any field — decomp is point-in-time.**

## Top-down PC-native engine port — FROM THE ENTRY POINT (later-159, the active spine)
Per the CLAUDE.md boundary, we REIMPLEMENT the engine PC-native starting at the game's entry point and
going down. The spine:
`crt0 FUN_800896E0` (BSS-zero, SP/gp/heap) `→ main FUN_80050b08` (override `ov_game_main`, native_boot.cpp)
`→ [init prefix]` `→ register task 0 = the stage sequencer FUN_800499e8 (START.BIN, stage-0 overlay load)`
`→ native frame loop`.

**Tool — `tools/disas.py <addr> [--mem]`:** MIPS-I disassembler for MAIN.EXE that resolves `lui+addiu/ori`
address builds and annotates every load/store with its absolute target + WIDTH (sb/sh/sw). USE THIS before
reimplementing any engine fn — Ghidra `DAT_*` hides widths and a wrong width silently corrupts the interface
state the PSX content reads (later-158). `--mem` = just the memory effects. For **overlay** code (>0x8010xxxx,
beyond MAIN.EXE — the field renderer/submitters live here) dump live RAM (`dumpram`) and disassemble the flat
image with capstone (`scratch/dual/mdis.py <addr> [n]`).

## ★ GAME-STAGE OBJECT PIPELINE — end-to-end (later-241, the map the user asked for)
How the field STAGE loads → places → stores → models → renders its objects, tying together the per-function
RE below. Driven/observed headless via `PSXPORT_AUTO_SKIP=1` (reaches real free-roam) + REPL `ents` (dumps
every live object node: addr, type@+0xc, render-intrinsic@+0xb, model id@+0xe, handler@+0x1c, world pos,
cmd count, geomblk of cmd[0]) + `node <addr>` (full node decode).

**1. STAGE.** GAME stage entry `0x8010637C`; a 4-level state machine in task0 (`sm[0x48]` area-mode /
`sm[0x4a]` sub-mode: 0=intro-cutscene, **1=running field** / `sm[0x4c]` area-machine / `sm[0x4e]` running
sub-state). See "GAME stage state machine". In REAL free-roam (reach it with the FIXED `PSXPORT_AUTO_SKIP=1`)
the native **`ov_render_frame` runs every frame** (`debug rfprobe`: ~once/frame) — it IS the render driver
(later-238 was right; my earlier "dormant" reading was an artifact of the BROKEN old AUTO_SKIP, which left
the game in the PAUSED auto-menu where `ov_render_frame` is gated off by `*(0x1F800136) < 2`). BUT
`ov_render_frame`'s passes mostly `rec_dispatch` the INTERPRETED PSX render, so the actual field geometry is
built by PSX overlay code (next paragraph) → flat quads.

**2. AREA / ASSET LOAD.** The area-load task `0x800452c0` → `FUN_8004514c` commits the area id to
`0x800BF870`, pulls the AREA OVERLAY (disc LBA/size from the area table `0x800BE118`, stride 8, indexed by
id+3) to **`0x80182000`**, and walks the per-area ASSET table at `area_base+0x51000`. The overlay carries the
area's render code (the field entity loop + GT3/GT4 submitters — see step 6) AND its model/texture assets.

**3. PLACEMENT.** `FUN_80072A78` (`ov_place_objects`, OWNED) selects the area PLACEMENT TABLE (by area id
`0x800BF870` + sub-state `0x800BF871`; seaside/area0 = `0x800A4C28[area]`), walks fixed **0x14-byte records**
(table ends at a record whose `byte[0]==0xff`), and spawns one object per record. Record format + per-field
node stamp are in "field OBJECT-PLACEMENT DRIVER" — notably `+0x00` TYPE, `+0x02/04/06` world XYZ,
`+0x0a/0c` facing, `+0x10` per-object behavior handler → node+0x1c.

**4. SPAWN + STORAGE.** `FUN_80079C3C` (`ov_entity_spawn`, OWNED) pops a node from the free-list and links it
into one of **3 doubly-linked active lists** (heads `0x800FB168` / `0x800F2624` / `0x800F2738`; next @node+0x24).
Nodes are **0xD0 (208) bytes**. Identity fields: `+0xb` render-intrinsic (**0x0F=3D mesh, 0x10..0x14=2D
billboard/sprite at a 3D world pos, 0x20=off-world/HUD anchor**), `+0xc` entity type, `+0xe` model id,
`+0x1c` behavior handler (per-type update — Tomba/enemy/prop/trigger differ here; the strongest identity
signal), `+0x2e/32/36` world XYZ, `+0x38` model-data descriptor ptr, `+0xC0` RENDER-COMMAND ptr array (count
@node+8). The entity walk `FUN_8007a904` runs each node's `+0x1c` handler every frame.

**5. MODEL ATTACH.** `FUN_80077b38(node, table, idx)` looks up `model = table[idx]`, stores the model-data
descriptor ptr at **node+0x38** and the model id (`*(model+2) & 0x3fff`) at node+0xe. The resident model
table is **`0x80017334`** (MAIN.EXE .data; entries point to descriptors at `0x8001792c+` — small per-model
anim/variant lists, NOT the raw geometry). The model-attach call sites (`0x80024e50+` etc.) dispatch by
entity subtype. NB some objects (e.g. **Tomba**) have node+0x38 == 0 — the player/some actors carry their
render commands directly (built by their handler) rather than via this descriptor table.

**6. THE REAL 3D MODEL = the GEOMBLK.** The actual 3D geometry an object draws is the **geomblk**, reached as
`geomblk = mem_r32(cmd + 0x40)` for each render command `cmd` in the node's `+0xC0` array. **geomblk format**:
word@+0 packs counts (lo16 = GT3 triangle count, hi16 = GT4 quad count); then GT3 records (36 B) then GT4
records (44 B), each holding MODEL-SPACE int16 verts + UV + per-vertex RGB + texpage/clut (full layout in
"Geometry SUBMIT"). Example — Tomba: node `…E8E8`, render cmds `{0x800F7844,0x800F7888}`, geomblk
`0x8015C094` = **6 triangles + 13 quads** (a real 3D mesh, not a sprite). Geomblks live in LOADED regions:
`0x8015xxxx` (resident models — Tomba, items), `0x801Cxxxx–0x801Exxxx` (area models — trees, NPCs, props),
pulled from disc by the asset pipeline (step 2). They are MODEL-SPACE; the per-object world transform is
`cmd+0x18` (matrix) + `cmd+0x2c` (position).

**7. RENDER + THE DEPTH REGRESSION (the user's "objects behind terrain/sea" bug).** The per-object render
(`gen_func_8003CCA4` → `submit_perobj_flush`) walks node+0xC0 → geomblk → the GT3/GT4 submitters, which
project each model vertex and emit a packet. There are TWO copies of the submit library: the resident
`0x8007FDB0`/`0x8008007C` (native-owned `ov_submit_poly_gt3/4`) and a per-area **OVERLAY** copy
(`0x801465EC`/`0x8013FE58`/`0x801401b8` entity loop). **Historically (later-166) the overlay copy was owned
native with REAL per-vertex depth via SCAN-ON-LOAD** (`engine_scan_overlay` → `rec_set_interp_override_auto`
registered the native impl for the freshly-loaded overlay submitters) → field world geometry went to the
render queue as `RQ_WORLD` with D32 depth. **That mechanism was part of the OVERRIDE SYSTEM removed
2026-06-22** ([[override-system-removed-top-down-pc-driven]]). With it gone, the overlay submitters run PURE
PSX again: they emit screen-space GP0 packets with **no view-Z**, so the native gp0 classifier marks them
**is3d=0 (flat)** and draws them in the 2D-foreground band — painter order, ON TOP of / behind the real-depth
world. That is exactly the regression: the field's objects + terrain are flattened to depthless quads and
composite by PSX OT order, so objects land behind terrain/sea.

**The LIVE render path, traced with `PSXPORT_PCTRAP=0xADDR` (+`_SKIP=N`, dumps the guest call chain when the
interpreter reaches ADDR — later-242):** `ov_render_frame` (native, every frame) → its passes `rec_dispatch`
the PSX bodies → the per-object geometry render goes `…→0x8003F698→0x80146478` (the OVERLAY GT3/GT4 renderer)
and the ground/scene entities go `0x8003D0BC→0x801401B8 (entity loop)→0x8013FE58/0x8013FB88 (overlay GT4/GT3
submitters)`, all INTERPRETED → is3d=0 flat. The native reimplementations DO exist — `submit_perobj_render`
(0x8003CCA4; every render-case 0x80014EC8 runs base flush 0x8003CDD8 = native `submit_perobj_flush`, then a
secondary-effect pass 0x8003D584/F344/F3F4/F594) and `ov_field_entity_render` (0x800F2418) — but they are
**NOT on the live path**: they only fire when the NATIVE render-walk (`ov_render_walk`/rwalks in
`ov_render_frame`) calls them, and those walk-lists are EMPTY in this field (`RLIST_HEAD==0`), so the
interpreted PSX render runs instead. Confirmed: `groundnative` (route 0x8003D0BC→`ov_field_entity_render`) and
routing all `submit_perobj_render` cases through the native flush BOTH leave the field 90.5% flat — they're
off the live path. **The frontier: make `ov_render_frame`'s rec_dispatched passes call the NATIVE render
reimpls (per-object dispatch + entity loop + submitters) instead of the PSX bodies — own the render-pass call
tree TOP-DOWN from `ov_render_frame`, NOT by reintroducing the removed scan-on-load override flip.**

**Init prefix of `ov_game_main` — classified (PLATFORM = native platform owns it / keep dispatched;
ENGINE = reimplement PC-native):**
| fn | what | class |
|----|------|-------|
| 80089788 | one-shot guard (DAT_800abef0) | platform (no-op) |
| 80085b20 | intr.c lib callback | platform |
| 800898a0 | **CdInit** | platform (native CD) |
| 80080bf0(3) | **ResetGraph** | platform (native GPU) |
| 80080d64/80080ed4 | SetGraphDebug / **SetDispMask** | platform |
| 800865f0 | lib state setter (DAT_800abe20) | platform |
| **80050a0c** | **frame-state init** (vblank ctr, buffer parity DAT_1f800135, frame divisor DAT_1f800235, swap-mode DAT_1f80019c, …, DAT_80105ee8=0x45) | **ENGINE — DONE (engine_init.cpp `eng_init_framestate`)** |
| **800509b4** | **display + GTE projection**: InitGeom (80083ff8: ZSF3=0x155 ZSF4=0x100 H=1000 DQA/DQB), SetGeomOffset(160,120), H=350→DAT_801003f8, SetGeomScreen | **ENGINE — DONE (`eng_init_display`)**; FUN_80050738 (PSX draw/disp env structs) still dispatched (native single-env = next display step) |
| **80050a80** | **camera init**: identity matrix → scratch 0x1F8000F8 (the camera-rot the renderer reads) + 0x1F800118; cam fields (_1f8000ec=0x1000, _ee=H*-5, _d8=H*-0x50000) | **ENGINE — DONE (`eng_init_camera`)** |
| 80096a70/80099310/800991b0/800993a0 | SPU/sound + heap lib | platform (verify) |
| 80089bac(0xe,…) | CD/_bu command | platform |
| 80085900(3/1) | **VSync** | platform |
| 80075130 | **font/text system init** (FUN_800963a0/80091d70/…) | ENGINE (UI — later) |
| 8009c620(0) | subsystem init | TBD |
| 8001cc00 | DMA channel + pad init (FUN_80080830 0xf4000001…) | platform (DMA/pad hw) |
| 800520e0 | engine subsystem init (FUN_8007b328, DAT_800ecf4x, FUN_80088b00/80086620/80087a60) | ENGINE (later) |
| 80051e00 | **scheduler task-table init** (DAT_801fe000, "A0F.BIN", stride 0x70) | ENGINE (later) |
| 80051f14(0,FUN_800499e8) | **register task 0 = stage sequencer** (the level loader's entry) | ENGINE (the big next system) |
| 80085bb0(LAB_800506b4) | register VSyncCallback | platform |

**Order of attack / status:** init display+camera DONE (eng_init_*). **Level LOADER core DONE** —
FUN_800450bc overlay loader is PC-native (engine/engine_level.cpp `eng_load_stage`, later-162); its
scheduler-coupled orchestration (FUN_80052078 task-restart ending in ChangeTh/ov_switch longjmp,
FUN_800499e8 file-resolve) stays dispatched and calls the native loader — native-izing them needs the
cooperative-scheduler longjmp handshake first. Remaining named systems: **object/entity placement &
spawning**, the **main menu** (DEMO stage state machine @0x801062E4), font/text (80075130), engine
subsystem init (800520e0), and a real PC-native single display env (replace FUN_80050738). Each: `disas.py`
the fn, understand the data, reimplement PC-native in `engine/`, keep the PSX-content interface state exact.

### FUN_80075130 font / text init (`ov_font_init`, engine/engine_font.cpp)
Init-prefix slot (called from ov_game_main). No args, no return. Frame: `addiu sp,-48; sw ra,40(sp)`;
epilogue `lw ra,40(sp); addiu sp,48; jr ra`. The body sets a handful of engine-state fields directly and
orchestrates **14 callees in order**. SCOPE: own the orchestration + direct writes + the 3 ENGINE-STATE
callees; KEEP the 8 libgpu/libgs/sound callees as `rec_dispatch` IN-CONTEXT (they do indirect draw-env /
FntLoad/FntOpen setup — later-182b nested-dispatch risk). Two of the kept callees (`0x80098330`,
`0x80098d30`) read a struct that FUN_80075130 builds on **its own stack frame** at sp+16, so the native
orchestrator MUST allocate the same sp-48 frame and populate sp+16..sp+26 before dispatching them, and pass
`a0 = sp+16`. Callees in execution order (✦ = keep dispatched / libgpu-sound, ★ = own native):

| # | callee | a0 / args | role | class |
|---|--------|-----------|------|-------|
| 1 | 0x8008e040 | — | sound/libgs/lib init (calls 80085b20, 80096a70 SPU, 80098de0(7), 8008dfa0) | ✦ dispatch |
| 2 | **0x800963a0** | a0=24 | font-bank selector: clamp & store byte | ★ own |
| 3 | **0x80096370** | a0=0 | font-bank2 store byte | ★ own |
| 4 | 0x80098f90 | a0=0, a1=0xffffff | libgs colour/clut setup | ✦ dispatch |
| 5 | 0x80091d70 | a0=1 | libgs/draw-env (calls 80086604) | ✦ dispatch |
| 6 | 0x80091b50 | a0=0x800be3d8, a1=14, a2=1 | libgs init over a 14-elem table | ✦ dispatch |
| 7 | 0x80090700 | a0=127, a1=127 (a1=a0 in delay slot) | libgs alloc/register | ✦ dispatch |
| 8 | 0x80090980 | — | libgs flush/finish | ✦ dispatch |
| 9 | **0x800752b4** | a0=2 | font glyph-class table fill (24 entries @0x800be238+8) | ★ own |
| 10 | 0x80098ce0 | a0=1 | libgs FntFlush-ish (returns into sp+26) | ✦ dispatch |
| 11 | 0x80098330 | a0=sp+16 (struct) | libgs FntOpen (reads sp+16 struct → 0x800ac5a0/a8) | ✦ dispatch |
| 12 | 0x80098150 | a0=1 | libgs FntLoad-ish | ✦ dispatch |
| 13 | 0x80098d30 | a0=sp+16 (struct) | libgs (reads *(sp+16)) | ✦ dispatch |
| 14 | 0x80098db0 | a0=1, a1=0xffffff | libgs colour 204/205 | ✦ dispatch |

**Direct writes in FUN_80075130 itself (own these natively, in this exact order relative to callees):**
- after #8: `*0x800bed78 = 0` (sw, 4b); then call #9; in #9's delay slot `*0x800bed80 = -1` (sh, 2b)
  — i.e. write 0x800bed80=0xffff(sh) right after the #9 call returns.
- `*0x800be358 = 0` (sw, 4b) [once], then a 14-iteration loop `sh 0` at addrs
  0x800be3d6,3ce,3c6,3be,3b6,3ae,3a6,39e,396,38e,386,37e,376,36e (v1 starts 0x800be3d0, store v1+6,
  v1-=8, counter 13→-1 = 14 stores).
- stack struct (consumed by dispatched #11/#13): `*(sp+16)=7`(sw), `*(sp+20)=258`(sw), `*(sp+24)=16384`(sh),
  `*(sp+26)=16384`(sh) — note sp+26 stores the SAME v0=16384 (it is the value set at 800751e8, NOT a return).
- after #14: `*0x800be22a = 0` (sb), `*0x800be22b = 0` (sb).  (base 0x800be1f8 + 50/51.)

**Callee #2 FUN_800963a0** — a0=font index. `v0=(a0-1)&0xff; if(v0 < 24){ *0x80105cec(sb)=a0; return
(a0<<24)>>24 (sign-extend low byte) }` else `return -1`. At call (a0=24): (24-1)&0xff=23 < 24 → store
0x80105cec=24, return 24. No sub-calls.

**Callee #3 FUN_80096370** — `*0x80105d28(sb)=a0; jr ra`. (a0=0 here → store 0, no return value set —
leaf, v0 untouched.) No sub-calls.

**Callee #9 FUN_800752b4** — a0=2 (the glyph "class" being assigned). Iterates `i = 0..23` (24 entries),
base a1=0x800be238, per-entry stride 12 (`(i*3)<<2`), writes byte at entry+8 (sb). Thresholds derived from
a0: t1=24-a0=22, t0=16-a0=14, a3=12-a0=10, a4=8-a0=6. The branch test is `slt(v1<thr)` with `bne` that
branches AWAY when the test is TRUE, so the FALL-THROUGH (test false, i.e. v1>=thr) sets the value:
```
if      (i >= 22) entry.b8 = 4;    // not(i<22)
else if (i >= 14) entry.b8 = 1;    // not(i<14)
else if (i >= 10) entry.b8 = 3;    // not(i<10)
else if (i >=  6) entry.b8 = 2;    // not(i<6)
else              entry.b8 = 0;    // i<6
```
So for a0=2: i 0..5 →0, 6..9 →2, 10..13 →3, 14..21 →1, 22..23 →4. The cascade is exclusive (first matching
branch `j`s to the loop tail 0x80075388). Returns count in v0 but the caller IGNORES it (the sh v0=-1 to
0x800bed80 happened in the call's DELAY SLOT, before the function ran). Only writes 0x800be238+i*12+8.

## BAV cel loader — `FUN_80096590` (`ov_bav_load`, engine/engine_bav.cpp, later-207) — OWNED native
The per-area **effect/animation CEL loader**: parses a BAV descriptor, allocates a cel SLOT (one of 16),
lays out the cel/UV tables, calls a VRAM allocator/upload callback, then patches per-frame tpage/clut
halfwords into the cel records and latches the slot's cel-system globals. Fires on area entry (the
seaside field loads 3 cels at boot — descriptors 0x801846b4 / 0x801858d4 / 0x801886f4). Same
0x80105Cxx/0x80105Dxx region as the font init above. Single caller wrapper `FUN_80096480` (prefills the
callback fn-ptr `0x800964b4` in a2); `FUN_80096480` is itself called by area-loader `FUN_800753D4`.

## Area CEL-GROUP load-and-wait — `FUN_800753D4` (`ov_cel_load_wait`, engine/engine_level.cpp) — OWNED native
The per-area asset bring-up primitive: pull ONE effect/animation cel group into VRAM and BLOCK until its
upload finishes. ABI `FUN_800753D4(a0=u16* out_slot, a1=BAV desc, a2=cb_arg4)` (ret v0 ignored):
1. `slot = FUN_80096480(desc, -1, cb_arg4)` — load via the native BAV loader (auto-slot; prefills upload
   callback `0x800964b4`, a3=0). `*(u16*)out_slot = slot`.
2. `FUN_80096980(cb_arg4, slot)` — kick the slot's upload state machine (leaves the slot's state byte
   `0x80105D18[slot] = 1`).
3. **poll-wait loop** @0x80075410: `while (sext16(FUN_80096a40(0)) == 0) FUN_80051f80(1);` — `FUN_80096a40`
   (→`FUN_800993a0`→ BIOS event syscall `FUN_80080840`) reports the GPU-DMA upload-done event; `FUN_80051f80(1)`
   is a one-frame coroutine YIELD (ChangeThread) between polls. The upload is **NOT** synchronous in the port —
   it settles over the following frame(s), so the yield is real and must be preserved (an early "drop the yield"
   attempt left the slot stuck at state 1 / cel #3 failing slot=-1).

**Callers:** `FUN_800451d0` (area init) → `FUN_800754f4` (per-area asset-table walk; reads an offset table at
`area_base+0x51000`, fires `FUN_800753D4` twice + 10× `FUN_80075448`) — area_base = `0x80182000` overlay region.
The 3 seaside boot cels load to slots 0/1/2 (descriptors 0x801846b4 / 0x801858d4 / 0x801886f4); steady state of
`0x80105D18` after load = `01 01 01`.

**Ownership model (later, this session):** the PROLOGUE (load + `*out=slot` + state-machine kick) is native
(the two callees complete without yielding — verified state byte = 1 after them, matching the recomp); the
cross-frame DMA-wait loop is handed back IN-CONTEXT via the coro-redirect handshake (`rec_coro_redirect` to
0x80075410, later-169) with the MIPS frame (sp-=0x20; s0/s1/ra saved) laid out byte-faithfully so the recomp
loop+epilogue resume correctly. Cel-system callees (`FUN_80096480`/`80096980`/`80096a40`) stay dispatched.
**VERIFY:** full main-RAM (2 MB) + scratchpad (1 KB) **0-diff** override-ON vs pure-recomp at field frame 120
(newgame→skip 650→run 120, dumpram + .spad); cel-state table and all 3 slot stores byte-identical; 0 bad
opcodes; reaches GAME. Diagnostic channel `celloadverify` (REPL `debug celloadverify`) logs each cel HIT.

**ABI** `v0 = FUN_80096590(a0 desc, a1 slot, a2 cb, a3 arg4)`:
- a0 = BAV descriptor ptr. a1 = requested slot ((int16); −1 = auto-allocate first free, else explicit [0,16)).
- a2 = allocator/upload **callback** fn-ptr, called `cb(a0=size_rounded64, a1=arg4, a2=slot) → vram_word_addr | -1`.
- a3 = arg4 forwarded to the callback (upload context / VRAM hint).
- v0 = the allocated **slot** on success; -1 / a transcribed leftover on error paths.

**BAV descriptor struct** (a0):
- +0  (u32) header. magic = `(word>>8)==0x00564142` ("BAV"); low byte = TYPE.
- +4  (u32) kind/bpp selector: `<5` → UV/frame sizes ×4 (`<<2`, 4bpp); `≥5` → ×8 (`<<3`, 8bpp). Also gates 64-vs-128 clamp (only when TYPE==112).
- +18 (u16) "hu" = cel-record count bound; must be ≤ the 64/128 clamp; also drives `a3 += hu<<9` to reach the UV table.
- +22 (u8)  "bu" = (UV/frame entry count − 1); the entry loops run `i ∈ [0, bu]`.
- +32 ..    DATA: a table of `clamp` (64/128) **16-byte CEL RECORDS**, then the per-frame UV table.

**Cel record** (16-byte stride, base = desc+32): +0 (u8) presence/U byte (0 ⇒ skipped from the packed index);
+8 (u32) packed index among non-zero records (written by loop 1); +12 (u16) tpage/clut word for EVEN frames;
+14 (u16) for ODD frames — both = `(vram_base + cumulative_byte_offset) >> 3` (written by loop 3).

**Cel-system globals written** (font+cel region):
- 0x80105C10 [slot*4] = data ptr (desc+32).  index = `(slot<<16)>>14` = slot*4 (a sll-in-delay-slot idiom; **NOT** field18).
- 0x80105C50 [slot*4] = the descriptor ptr for this slot.
- 0x80105C98 [slot*4] = UV-table base (after the cel records).
- 0x80105CDA (u16)     = the 64/128 cel-record-count clamp.
- 0x80105CF0 (u32)     = 0 (cleared at entry).
- 0x80105D18 [slot]    (u8) = slot STATE: 0 free / 1 allocating / 2 loaded.
- 0x80105D30 [slot*4]  = total packed VRAM size.
- 0x80105D70 (u16)     = active-slot REFCOUNT (++ on allocate, −− on error cleanup).
- 0x80105D78 [slot*4]  = allocated VRAM base word-address.
- 0x800AC638 (u32)     = re-entrancy LOCK: **1 = FREE, 0 = BUSY**. Guard `FUN_80099478` returns `(lock^1)!=0`
  → bails when BUSY(0). `FUN_80099450(1)` ACQUIRES — its `sw zero` is in the a0==1 branch's **delay slot**,
  so it writes 0/busy; `FUN_80099450(0)` RELEASES (writes 1/free). Success leaves the lock ACQUIRED(0); only
  the error cleanup tail releases it. (Getting this inverted was the first A/B mismatch.)

**Control flow** (per the disasm): lock guard → allocate/validate slot → record descriptor + clear 0x80105CF0
→ magic check (else free slot + cleanup) → pick clamp (64/128) → bounds-check hu ≤ clamp (else cleanup) →
record data ptr → **loop 1** (pack non-zero record index into rec+8) → **loop 2** (record UV base, sum
per-entry sizes into a sp+16 scratch array, kind-shift `<<2/<<3`) → round size up to 64 → **call the
callback** (jalr a2) → fail/overflow → cleanup; success → latch VRAM base → **loop 3** (per entry i∈[0,bu]:
`off += size[i]`, write `(vram_base+off)>>3` into cel-record (i/2)'s +12 (even i) / +14 (odd i)) → store total
size + mark slot LOADED(2). Return slot.

**Callees**: `FUN_80099478`/`FUN_80099450` (the lock test/set — trivial single-word ops on 0x800AC638,
owned inline). The **callback** (a2 = `FUN_800964b4` → the VRAM allocator `FUN_800977c0`) is the genuine
leaf, KEPT dispatched (`rec_dispatch`) in the recomp's exact order — its free-list lives in tracked RAM
(0x800AC5xx/6xx) so it returns the same VRAM base in an A/B re-run.

**Verify**: `bavload` full RAM+scratchpad+v0 A/B gate vs `rec_super_call` (engine_bav.cpp) — native run →
snapshot+rollback → recomp body → diff, excluding only the top-of-RAM stack window [sp-0x800, sp). **0-diff
over all 3 area-entry cel loads** (the natural exercise count for the reachable seaside field; this is a
spawn-time loader, not per-frame). GOTCHAs caught by the gate: (1) lock semantics inverted (delay-slot `sw
zero`); (2) the kind-shift inverted (`kind<5` → `<<2`, not `<<3`); (3) the C10/C98 index is **slot*4** (the
`sll s2,16 >> 14` idiom), which I first misread as `field18>>14`.

## DEMO / front-end MENU stage state machine @0x801062E4 — RE map (later-181)
The title/attract/menu front-end. Lives in the **DEMO overlay** (loaded at base 0x80106228, like GAME.BIN
— they ALIAS the same 0x80106xxx addresses, so the same jump-table address holds DIFFERENT entries per
overlay; do not confuse this table with the GAME-stage `sm[0x4c]` table below). Runs as task-0 coroutine.
Disassemble from a live menu RAM dump: `python3 tools/disas.py <addr> [n] --ram scratch/bin/tomba2/ram_menu.bin`
(and `--mem --ram …` for resolved load/store addrs). task struct `sm = *0x1F800138`.

**Root dispatcher 0x801062E4:** prologue runs ONCE (jal 0x800810F0 UI/ctx init with a 16-byte stack desc
whose +4 = 320=screen-width; clear globals `*0x800BE0E4`=0, edges `*0x800E7E68`=0, `*0x800ECF54/56`=0,
buffer-mode `*0x1F80019A`=0, `*0x1F80019D`=0; set `sm[0x48]=0`; jal 0x8005082C input/pad-table reset; clear
`sm[0x6E]`). Then the per-frame LOOP @0x80106388: reload sm, read **`sm[0x48]` (u16) = SUBSTATE**, bounds
`<8` (else→TAIL), dispatch via word table **@0x8010622C** → `jr`. **ONE substate per frame** (each body ends
by branching to the TAIL; only s0 falls THROUGH into s1, so the first frame runs s0+s1). TAIL @0x80106670:
`*0x1F800198 (u16 frame ctr)++`, `jal 0x80051F80` (the single yield), loop.

**Substate table @0x8010622C:** s0=0x801063C0 s1=0x8010641C s2=0x80106464 s3=0x801064E8 s4=0x80106580
s5=0x801065DC s6=0x801065EC s7=0x80106668.

**sm fields:** `[0x48]`u16 SUBSTATE · `[0x4a]`u16 sub-machine init/phase (also s7 phase) · `[0x4c]`u16 page
selector (written by 0x8007B45C) · `[0x50]`u16 page phase/counter · `[0x5a]`u16 intro/hold timer (inits 450) ·
`[0x68]`u8 menu cursor/phase · `[0x6b]`u8 page selector · `[0x6e]`u8 selected-option index (s7). Button
**pressed-edges** = `*0x800E7E68` (u16; Down=0x20, Up=0x80, confirm 0x4008, Circle/back 0x1000). Common exit
tails: 0x80106650 (jal 0x8001CF2C engine per-frame update → TAIL) · 0x80106658 (jal 0x80075A80 attract render
→ TAIL) · 0x80106674 (TAIL directly).

**Substate handlers (→ = SUBSTATE transition written to sm[0x48]):**
- **s0 0x801063C0** — run-once INIT: `sm[0x68]=0`, `sm[0x48]=1`, `sm[0x4a]=0`; load menu page-2 resources/
  text/font/display (jal 0x80045080(_,2,0), 0x80044BD4, 0x8007982C, 0x80075240, 0x8001CF00(1)); **falls
  through into s1** same frame.
- **s1 0x8010641C** — wait/advance: `v0=jal 0x80106F80(0)` (inner menu input machine, 8-way table @0x801062C4
  on `sm[0x4a]`; reads `*0x1F80019D`); if `v0!=0` → `sm[0x4a]=0`, **sm[0x48]→2**; else if any pad edge
  (`*0x800E7E68`) → `*0x1F80019D=1` (skip-request). yields.
- **s2 0x80106464** — sub-machine `v0=jal 0x8010696C` (NOT a table — beq/bne cascade on `sm[0x4a]`: 0=init set
  timer `sm[0x5a]=450`, 1=run jal 0x80106690; on timeout reads edges, Down/Up move cursor `sm[0x68]`+SFX
  0x80074590(21), confirm 0x4008→SFX(17) ret 2). Outcome 1→**sm[0x48]=7**; 2→phase trick on `sm[0x68]`:
  first pass **→3** (set sm[0x68]=2), second pass **→4** (clear sm[0x68], sm[0x50]=0, jal 0x8001CF2C, clear
  `*0x800BF84A`).
- **s3 0x801064E8** — sub-machine `v0=jal 0x80106AC4` (mirror of 0x8010696C). 1→jumps into s2's `sm[0x48]=7`
  tail; 2→phase trick **→5** (clear `*0x1F800134`) or **→6** (sm[0x6b]=0, sm[0x50]=0, jal 0x800750D8, clear
  `*0x800BF808`); 3 (back/cancel)→**sm[0x48]=2**.
- **s4 0x80106580** — closer `jal 0x8007BF20(0,0)`; branch on `sm[0x6b]` (CORRECTED later-185, disasm @
  0x8010658C+): ==1→**sm[0x48]=2**,sm[0x68]=1; ==2→**sm[0x48]=2**,sm[0x68]=0; ==7→**sm[0x48]=5** +
  `*0x1F800134=1` (all →TAIL_CF2C); else stay →TAIL_REND. (prior map said sm[0x48]=1 — wrong.)
- **s5 0x801065DC** — LEAVE DEMO: `jal 0x80052078(2)` (stage-transition/task-restart to stage selector 2),
  yields. (0x80052078 rewrites the task's stage-handler word + drives the scheduler natives.)
- **s6 0x801065EC** — page sub-machine `jal 0x8007B45C` (reads `*0x800E7E68 & 0x1000` Circle/back → SFX(17),
  writes page selector `sm[0x4c]=2`); if `sm[0x50]==3` fire commit pair 0x80106824(1,1)+0x80106690(1); on
  `sm[0x6b]`∈{1,2} → **sm[0x48]=3** (+sm[0x68]=3); else stay.
- **s7 0x80106668** — trampoline `jal 0x80106C24` then falls into TAIL. Body 0x80106C24 = 3-phase machine on
  `sm[0x4a]`: **phase0** (0=launch) loads the SELECTED option (cursor `sm[0x6e]` indexes byte table
  @0x8010770C → `*0x800BF870`; `*0x800BF89C=4`; the cross-frame yielding LOADER jal 0x80044BD4 + scene reinit
  fns 0x8007B18C/0x800796DC/0x800263E8/0x80072A78/0x80075240/0x800783DC/0x80078610 + option setup
  0x80079464(sm[0x6e]); `*0x1F80019A=1`; sm[0x4a]=1; cursor wraps <3); **phase1** (1=wait/animate) optional
  draw 0x80079374 gated by `*0x1F80017C & 0x10`, per-state update 0x80106EE4/0x80106E28 (by `*0x800BF870==3`),
  `sm[0x5a]--`, poll jal 0x800524B4(0); on timeout/clear sm[0x4a]→2; **phase2** (2=teardown) jal 0x80074BC4,
  `*0x1F80019A=0`, **sm[0x48]=0** (restart front-end). The actual DEMO→GAME stage switch happens inside the
  phase0 loader / phase2 teardown / 0x800524B4 poll, not via an in-s7 stage-request write.

**To OWN native:** replace task-0's longjmp coroutine for the DEMO stage with a native dispatcher (like
engine/engine_stage.cpp for GAME) — own the sm[0x48] switch + the per-frame loop/yield/frame-ctr, calling the
substate bodies C→C; the inner menu machines (0x80106F80 / 0x8010696C / 0x80106AC4 / 0x8007B45C) and the
loader/SFX/render callees stay dispatched until ported. Gate on the interface state the retained content reads
(sm fields above + the scratchpad flags). Frontier item 2 in docs/port-progress.md.

**OWNED (later-182, engine/engine_demo.cpp, scan-registered `demo_scan_overlay` when the DEMO overlay loads):**
substates **s1/s2/s3/s6** — the ones whose only sub-call is SYNCHRONOUS (verified yield-free with the new
`tools/yield_reach.py`). The override is NOT placed on the root function; it sits on each substate body's
address, fires when the guest loop's `jr v0` table-dispatch reaches it, runs the transition LOGIC native, and
coro-redirects to the guest TAIL (0x80106650 / 0x80106658 / 0x80106670) — the ov_game_s4c shape. **NOTE the
prologue register values the body comparisons use: s2=1, s1=2, s3=3** (0x8010633c/40/44).
**ALSO OWNED (later-185): s0 0x801063C0** — the run-once INIT substate. It HAS genuine pre-yield engine
state (sm[0x68]=0, **sm[0x48]++** [increment, NOT `=1`], sm[0x4a]=0) which it owns native, then sets the
loader-0 args (a0=0x80108F9C,a1=2,a2=task) and coro-redirects to the first loader jal 0x801063E4 (which
YIELDS; the guest runs the rest of the loaders + falls into s1 in-context). A/B (run 150 steady menu,
override-on vs -off): main-RAM + scratchpad **0-diff, no saved-ra artifact** (the guest jal sets its own ra).
The remaining DEEP-YIELDING substates: **MECHANISM CONSTRAINT (verified later-185, interp.cpp):** the
override table is consulted ONLY on `jal`/`j`/`jalr`/computed-`jr` CALL/JUMP targets (interp_flat 453-483);
a `jr ra` RETURN does NOT consult it. So a "post-yield override" at the instruction after a deep yielder
returns is IMPOSSIBLE (reached by `jr ra`, never fires). Therefore:
- **s4 0x80106580 — STAYS GUEST (final).** Its only engine logic is the sm[0x6b] branch at 0x8010658C
  (==1→sm[0x48]=2,sm[0x68]=1; ==2→sm[0x48]=2,sm[0x68]=0; ==7→sm[0x48]=5,*0x1f800134=1), reached by `jr ra`
  from the deep yielder 0x8007bf20 → unreachable by an override. (An earlier "own at 0x8010658C" note was
  WRONG, retracted.)
- **s5 0x801065DC — STAYS GUEST.** Whole body is `jal 0x80052078(2)` + tail yield; nothing to own.
- **s7 0x80106668 — OWNED (later-208, ov_demo_s7_phase, registered).** Its `jal 0x80106C24` IS an override-
  checked jal target; phase machine 0x80106C24's phase-selection prologue (sm[0x4a]) is PRE-yield and phase2
  teardown (0x80106dfc: jal 0x80074bc4 SYNC ; *0x1f80019a=0 ; sm[0x48]=0) is all-SYNC → own selection +
  phase2, redirect the yielding phase0/phase1 into the guest body (replicating the prologue's sp-40 frame so
  the body's `jr ra` epilogue returns to the trampoline). **REACH RECIPE (the prior "needs New-Game confirm"
  assumption was WRONG): s7 is the ATTRACT-demo auto-launch, reached by letting the title intro timer expire**
  — `tap 4008` once (title s2->s3) then `run ~455` so s3's sm[0x5a] intro timer (450) expires and the
  front-end AUTO-advances sm[0x48]->7 (no further input). Phases: phase0 (sm[0x4a]=0, ONE frame, overlay
  LOADER — @0x80109450 changes) -> phase1 (sm[0x4a]=1, the whole attract-play loop, sm[0x5a] counts down from
  ~26216 so it sits here ~7 min of frames) -> phase2 (sm[0x4a]=2, teardown -> sm[0x48]=0 restart). VERIFIED:
  full-RAM+scratchpad dump at a steady phase1 frame (override-ON) vs the guest baseline = **scratchpad 0-diff,
  main-RAM 2-byte diff at 0x1FE05A** (a single saved-context halfword in the top-of-RAM coroutine stack page —
  the documented coro-redirect saved-ra/sentinel artifact, never game data). phase0 fires once + phase1 fires
  2000+ frames clean; phase2 (reached by poking sm[0x4a]=2) drives sm[0x48]->0 restart with no crash. The
  newgame->GAME path is unaffected (s7 only fires on the attract path).
A plain rec_dispatch of a deep yielder kills task 0 (later-169).
A/B gate (override-on vs -off, REPL `run 150`): main-RAM + scratchpad 0-diff except task-0's saved-ra stack
slot (CORO_SENTINEL vs guest return-PC, the coro-redirect artifact). `dumpram` now also dumps a `.spad`.

## GAME stage state machine (the per-area scene/update driver) — RE map (later-168)
Overlay `\BIN\GAME.BIN` (LBA 1882, 11636 B), loaded RAW to base **0x80106228**; task-0 entry **0x8010637C**.
Runs as a COOPERATIVE TASK: an infinite loop that yields once per frame via `FUN_80051f80(1)`. The current
task object ptr is `*0x1f800138` (== task-0 obj `0x801fe000`). State fields in that object:
`sm[0x48]` top state · `sm[0x4a]` running sub-mode · `sm[0x4c]` area machine · `sm[0x5c]` intro timer (0x14a).
Three-level nested machine (disasm a GAME-stage RAM dump; `tools/disasm_overlay.py scratch/raw/GAME.BIN
<addr> <end>` — extract with `scratch/bin/fmv_compare dumplba 1882 11636 scratch/raw/GAME.BIN <chd>`):
- **Entry 0x8010637C** — init (zero display flags 0x1f800206/234/236, set buffer-mode 0x1f80019a=2, load
  `sm[0x48]=*0x1f800134`, zero `sm[0x4a..0x50]` + frame ctr 0x1f800198) then the per-frame loop: dispatch
  `sm[0x48]` → handler, `0x1f800198`++, `FUN_80051f80(1)` yield, repeat.
- **`sm[0x48]` handlers:** 0 → `0x801086e0` (area INIT: sm[0x48]=2, reset sub-states, call setup
  `FUN_8007a8e0`+`FUN_8007b38c`) · 1 → `0x80108720` (RESUME-INIT: sm[0x4a]=1, +`FUN_8007b3f4`, scratch
  0x1f8001ff=0xff/0x278=0) · 2 → `0x80108784` (RUNNING).
- **`sm[0x4a]` (in 0x80108784, 6-way jump table @0x8010631c):** 0→`0x8010882c` 1→`0x801088d8`
  **2→`0x80106478`** (the area machine) 3→`0x80106a24` 4→`0x801089c4` 5→`0x80108a60`.
- **`sm[0x4c]` (in 0x80106478, 9-way jump table @0x8010622c):** `{0x801064c4, 0x80106510, 0x80106580,
  0x801065b8, 0x801066b8, 0x80106830, 0x80106930, 0x8010694c, 0x801069b4}` = area load/intro/play states
  (timer countdown `sm[0x5c]` from 0x14a, pad-input skip via `*0x800e7e68`, calls resident system fns).

**OWNED native (engine/engine_stage.cpp, scan-registered when GAME.BIN loads):** ALL THREE `sm[0x48]`
handlers. The area-INIT pair (`==0`/`==1`) are clean `jr ra` functions whose callees are SYNCHRONOUS →
faithful as override+dispatch. The RUNNING dispatcher (`==2`, `0x80108784`) owns the 6-way `sm[0x4a]`
selection via the cooperative-yield handshake (below). Verified RAM 0-diff @ field f650/f1000 vs the
pre-change (guest-interpreted) build.

**SOLVED (later-169) — the cooperative-yield handshake = `rec_coro_redirect`.** The IMPORTANT structural
fact: GAME.BIN contains exactly ONE yield call (`jal FUN_80051f80` @0x80106468), at the TOP-LEVEL loop
(0x8010637C), AFTER the `sm[0x48]` dispatch returns — the sub-handlers do NOT yield in the overlay. But the
sub-handlers call RESIDENT MAIN.EXE fns (`0x8007xxxx`) that DO yield deep (waiting on asset loads across
frames). So the running dispatcher's callee can yield deep, which is why a `rec_dispatch` override killed
task 0 (nested `rec_interp`+`CORO_SENTINEL`; the deep yield's longjmp destroys that C frame → resume
mis-reads the return as task-end, st=2→0 @f53). **The fix:** the override does its native work, sets
`c->coro_redirect_pc` (via `rec_coro_redirect(c, target)`), and returns; the flat interp then runs `target`
IN-CONTEXT (in the SAME task run / `interp_flat`), so a deep yield longjmps to the scheduler and resumes
correctly — no nested sentinel. `ov_game_s48_2` uses it: native prologue + 6-way `sm[0x4a]` select, set
`ra = 0x801087CC + s4a*0x10` (the guest `j 0x8010881c` trampoline, byte-identical saved-ra), redirect to the
handler. The handler returns through the guest epilogue → the task loop. This is the GENERAL mechanism that
also unlocks owning `FUN_80052078`/`FUN_800499e8` (see below).

## Top-level control flow
- **Main loop** `FUN_80050b08` (`:31269`, override `ov_game_main`). After GPU/double-buffer + lib init,
  loops forever. Per frame, in order:
  1. clear OT / set buffer (`FUN_80081458`, buffer ptr `PTR_DAT_800ed8c8`, parity `DAT_1f800135`)
  2. `FUN_800788ac()` — per-frame fence (input + game sub-tick), override `ov_frame_update`
  3. `FUN_80051e60()` — **task scheduler** (runs the cooperative tasks; gameplay logic lives here)
  4. `FUN_80080f6c(0)` — flush/draw the OT to the GPU
  5. spin until vblank counter `DAT_800e809c` reaches `DAT_1f800235` (frame-rate gate)
  6. `FUN_800506d0()`, then swap buffers (`DAT_1f800135 = 1 - DAT_1f800135`)
- **Per-frame fence** `FUN_800788ac` (`:55345`, override `ov_frame_update`): reads pad, computes button
  **edges** — pressed = `DAT_800e7e68`, released = `DAT_800f23a4` (cur `DAT_800ecf54` & ~prev) — then
  calls `FUN_8005229c` (a CD/load sub-state-machine; NOT the object walk).

## Task scheduler (the engine is task-based, PSX-TCB style)
`FUN_80051e60` (`:?`) walks a **task table at `0x801fe000`**, stride **0x38 bytes**, until `0x801fe14f`
(~6 task slots). Per slot, field `+0` = state: `2` = ready→switch to it (`FUN_80080880`=change-thread),
`3` = needs-spawn (`FUN_80080860`=open-thread with args at +0x10/+0x18/+0x20, stores tid at +8). Thread
funcs are already native (`ov_open_thread`/`ov_change_thread`/`ov_switch`, `runtime/recomp/threads.c`).
**Gameplay (entity update + render submission) runs inside the main gameplay task** — its body is the
next RE target (see Open items).

## Area WARP / destination mechanism — RE map (this session) + the `warp` REPL dev command
**What selects the area to load.** The current area id is a single byte global **`0x800bf870`** (read ~304×
across the engine for per-area behavior; the seaside field is area id 0). The area DATA overlay always lands
at the fixed base **`0x80182000`** (its per-area asset table is at `area_base+0x51000`; see `FUN_800754f4`).
Each area's disc location is in the **AREA TABLE at `0x800be118`** — stride-8 `(LBA, byte_size)` records,
indexed by **`area_id + 3`** (the `+3` comes from `a1 = task[0x6e]+3` at `0x800453cc` → `FUN_80045080`).
Live dump (24 entries, ids 0..23):

| id | LBA | size | id | LBA | size | id | LBA | size |
|----|-----|------|----|-----|------|----|-----|------|
| 0 | 0x760 | 0x351C | 8 | 0x36A | 0x392B4 | 16 | 0x61A | 0x1D328 |
| 1 | 0x74A | 0x61E4 | 9 | 0x3DD | 0x456BC | 17 | 0x655 | 0x1E6F0 |
| 2 | 0x767 | 0x44FC | 10 | 0x468 | 0x38158 | 18 | 0x692 | 0x2114C |
| 3 | 0x176 | 0x459A8 | 11 | 0x4D9 | 0x40F8C | 19 | 0x6D5 | 0x463C |
| 4 | 0x202 | 0x316DC | 12 | 0x55B | 0x566D | 20 | 0x6DE | 0x36C0 |
| 5 | 0x265 | 0x3112C | 13 | 0x566 | 0x1EE38 | 21 | 0x6E5 | 0x3B30 |
| 6 | 0x2C8 | 0x1326C | 14 | 0x5A4 | 0x1C368 | 22 | 0x6ED | 0x4A7C |
| 7 | 0x2EF | 0x3D3A4 | 15 | 0x5DD | 0x1E1C0 | 23 | 0x6F7 | 0x16CC4 |

(The big ones — 3/4/5/7/8/9/10/11 ≈ 0x3xxxx–0x45xxx — are full field/level areas; the small 19..23 ≈ 0x3xxx–0x4xxx
are likely sub-rooms/boss arenas. id 13..18 ≈ 0x1Cxxx–0x21xxx mid-size. NB the table is RAM-resident, filled at
boot — it reads as all-zero in a fresh MAIN.EXE.) A parallel `(count, flags)` table sits at `0x800be368`
(stride 8, ~20 entries: e.g. id0=`(1, 0x000E)`). The translated overlay's per-area disc params land at
`0x800ef480/ef484` (LBA/end) + `0x800ef488` (record count) — written by the area-load orchestrator
`FUN_8004514c`.

**How a transition consumes it.** The area-load is **TASK SLOT 1** (`0x801fe070`, stride `0x70`), entry
`FUN_800452c0`. A door/exit triggers it via **`FUN_80044bd4(a0=0x800452c0, a1=dest_id, a2=mode, a3=phase)`**:
it kills sub-task slot 2 (`FUN_80052010(2)`), writes the dest id into **`task1[0x6e]` (`0x801fe0de`)** + mode
into `task1[0x6d]` (`0x801fe0dd`), then `FUN_80051f14(1, 0x800452c0)` restarts the load task. The load task
→ `FUN_8004514c` commits **`0x800bf870 = translate(task[0x6e])`** (`FUN_80045080`, table `0x800c5e18`/the id
table), pulls the area overlay (table `0x800be118[id+3]`) to `0x80182000`, walks the asset table at
`+0x51000` (cels via `FUN_800753D4`/`ov_cel_load_wait`), then GAME re-inits the scene.

**The trigger lives in the GAME-stage steady handler `0x801088d8` (sm[0x4a]==1), case `sm[0x4c]==0`**
(NOT the `sm[0x4c]` area machine `0x80106478` — that's never entered on the field; confirmed). case0:
`FUN_8005245c()` (CD-lib cleanup) → `FUN_80044bd4(0x800452c0, a1=lbu@0x800bf870, 0, 2)` (loads the area in
`0x800bf870`) → `sm[0x4c] = lbu@(0x80108f60 + lbu@0x800bf870)` (next area-machine state from a per-area byte
table). So a SCRIPTED door first writes the destination into `0x800bf870` (+ the area-machine deeper path
`FUN_80106b98`/`sm[0x4e]` 12-way table at `0x8010626c` does per-id special-casing on `0x800bf870`), then
the steady handler reloads. **`sm[0x4a]==4` (`0x801089c4`, `sm[0x4c]==1`) is the TITLE/DEMO teardown** — it
zeroes `task[0x69..0x6b]` + `sm[0x4a/4c/4e]` and calls `FUN_80052078(1)` (full stage reload → bounces to
the DEMO stage `0x801062E4`); it is NOT an area-to-area warp.

**`warp <area_id>` REPL dev command (runtime/recomp/native_boot.cpp).** From the field, seeds
`0x800bf870 = dest` and drives `sm[0x4a]=1, sm[0x4c]=0` so the GAME stage runs case0 itself next frame and
loads the dest area IN-CONTEXT (FUN_80044bd4's cooperative `FUN_80051f80` yields work because task0 yields →
task1 loads → task0 resumes). Two approaches that DON'T work (both verified): (a) `rec_dispatch(FUN_80044bd4)`
from the frame-loop top **deadlocks** (yields outside a task run); (b) restarting the load task ourselves under
a live area **corrupts task0** (overlay swap mid object-walk → bad-opcode flood).
- **VERIFIED CLEAN:** same-area reload `warp 0` from the seaside field — **0 bad opcodes**, stays in GAME
  stage, area machine runs (`sm[0x4c]` 0→2). The trigger mechanism is sound.
- **CROSS-area is prerequisite-state-dependent:** `warp 6`/`warp 20` ran with **0 bad opcodes** (got
  furthest), `warp 3` 7, `warp 1` / `warp 19` crash hard (1000s of bad opcodes). Root cause: case0 reloads
  the overlay while the OLD area's spawned object tasks/handlers are still registered and run against the
  swapped `0x80182000` memory. A clean cross-area warp needs the **door-transition preamble** that quiesces/
  tears down the per-area object tasks BEFORE the reload (the game runs this on a real door; reproducing it is
  the scoped follow-up = the "prerequisite-state" work for a boss/level selector). The destination mechanism
  itself (above) is fully mapped.

## Object / entity model — the entity LIST + node (RESOLVED via RAM-dump search)
The active entities are a **doubly-linked list of pool nodes, stride 0xD0 (208 bytes)** (found by
searching gameplay RAM dumps `scratch/bin/{level_ram,ours_ram_gf}.bin` for the handler address bytes;
verified the prev/next chain). **Two lists** (objects spawn into one or the other; init `:55858/55860`,
insert `:55976–56167`):
- head **`DAT_800fb168`** (0x800fb168) and head **`DAT_800f2624`** (0x800f2624).

Node fields (offsets from the node = the handler's `param_1`):
| off | type   | meaning |
|-----|--------|---------|
| +1  | u8     | per-frame render flag (cleared by the walk + by cull `FUN_8007712c`) |
| +4  | u8     | per-type state/substate (e.g. `FUN_80040558` switches on `param_1[4]`) |
| +0xc| u8     | **entity type** (cull switch) |
| +0xe| u16    | model id (`& 0x3fff`), set by `FUN_80077b38` |
| +0x1c| ptr   | **handler fn pointer** (per-type update/render; called with node in a0 — gameplay code, stays PSX) |
| +0x20| ptr   | **prev** node |
| +0x24| ptr   | **next** node (list walk follows this) |
| +0x28| u16×2 | low = state (0x0002 = active?), high = per-object id |
| +0x2e| u16   | **position X** |
| +0x32| u16   | **position Y** |
| +0x36| u16   | **position Z** |
| +0x38| ptr   | model-data pointer (set by `FUN_80077b38` from a table) |

Correction: there are **THREE** active lists, not two (a third pool/list `head 0x800f2738 / tail 0x800f23a0`
exists alongside the documented two). The pool init `FUN_800798f8` builds three free-lists with different
node strides (88, 196, **208=0xD0**) — the 208-byte one is the main entity list. Free-list head =
`0x800e8098`, free count (u8) = `0x800e7e7c`, free chain via node[+0x24]. The three active-list (head,tail)
pairs: `(0x800fb168,0x800f23a8)`, `(0x800f2624,0x800f239c)`, `(0x800f2738,0x800f23a0)`.

### Entity SPAWN / placement — `FUN_80079C3C` ✅ OWNED `ov_entity_spawn` (engine/entity_spawn.cpp, later-208)
The engine's core SPAWN PRIMITIVE — pop a node from the free-list, link it into an active list at a
requested position, stamp its identity. ABI: `node* spawn(a0=ref, a1=type, a2=mode, a3=list)`. Pure
pool/list memory; NO GTE, NO render packets. Reached by the per-type spawn dispatchers `FUN_8007A980`
(table `0x80016E4C`, 5 type-classes) / `FUN_8007AA38` (table `0x80016E64`, replace-variant), which
tail-jump (`jr v0`) to thin per-type handlers that call this. **`FUN_8007A980` ✅ OWNED
`ov_spawn_dispatch` (entity_spawn.cpp): routes class→variant `{0x80079c3c, 0x80079ddc, 0x80079f90,
0x8007a12c, 0x8007a2c8}` and calls `variant(ref=0, type, mode=3, list)`; the 5 spawn variants stay
dispatched (content). `spawndispverify` gate = full RAM+scratchpad+v0 A/B vs `rec_super_call(0x8007A980)`:
360+ live field spawns, 0 mismatches, clean. NEXT in the subsystem: the 5 variants + the replace-dispatcher
`FUN_8007AA38`.** Body:
- `cnt=u8[0x800e7e7c]; if (cnt<3) return 0;` (pool-low guard, keeps ≥2 spare).
- pop: `node=u32[0x800e8098]; u8[0x800e7e7c]=cnt-1; u32[0x800e8098]=node[+36];`
- list-select by **a3** → (head,tail): a3==1→list1, a3==2→list2, else→list0 (the three pairs above).
- insert by **mode a2**: 0=before ref (→head if ref->prev==0), 1=head, 2=after ref (→tail if ref->next==0),
  3=tail, other=no-link. prev=node[+0x20], next=node[+0x24]; head ptr is the prev-end, tail ptr the next-end.
- stamp (all paths): `u8[node+0x0a]=a3` (list id), `u8[node+0]=2` (active), `u8[node+0x0c]=a1` (entity type).
The per-type dispatch tables + handlers stay PSX (content-side type routing); only the alloc+link+init
primitive is owned. `spawnverify` gate = full main-RAM(0x200000)+scratchpad(0x400)+v0 A/B vs
`rec_super_call(0x80079C3C)`: **0 mismatches over 100+ live field spawns** (seaside, newgame→skip 650→run),
clean boot, no bad opcode. Registered in game_tomba2.cpp via `entity_spawn_register()`.

### The field OBJECT-PLACEMENT DRIVER — `FUN_80072A78` ✅ OWNED `ov_place_objects` (engine/entity_spawn.cpp, later-210)
**This is "the object spawn handler" — the TOP-DOWN entry that populates a field with its objects.** When a
field/area becomes active, the GAME-stage area machine calls it (4 sites in GAME.BIN: `0x80106bf4` /
`0x801072a8` / `0x801077f0` / `0x80108e14`; on the seaside field it fires twice via `0x80106bfc`). It selects
the area's PLACEMENT TABLE from (area id `0x800BF870`, sub-state `0x800BF871`), then walks the table's fixed
**0x14-byte records**, spawning one object per record via the owned per-type dispatch `FUN_8007A980` and
stamping the new node's identity/position/facing/behavior-handler from the record. Found TOP-DOWN by tracing
the spawn callers at field-load (`debug spawntrace` logs each spawn-entry's `ra`): the two dominant callers
were inside `FUN_80072A78` (table loop) and `FUN_80072DDC` (single-object spawn-with-parent helper). Resident
MAIN.EXE, no yield → plain override.

**`FUN_80072DDC` ✅ OWNED `ov_spawn_with_parent` (same file).** The 2nd dominant field-spawn caller — a
single-object helper: `node = FUN_8007A980(type&0x7f, (class==3)?3:class, (class==3)?1:0)`; if non-null
`node[0x28]=type` (full byte), `node[0x10]=parent` (a0), `node[2]=flag` (a3). `spawnparentverify` full-RAM+
scratchpad+v0 A/B = **100+ live field calls 0-diff**, 0 bad opcode.

**The per-object behavior HANDLERS the placement records install (node+0x1c).** The seaside table installs
22 distinct handlers; 2 are resident/generic (the rest are scene-overlay code). First owned (later-211):
- **`FUN_800739AC` ✅ OWNED `ov_beh_739ac` (engine/objbeh_739ac.cpp).** A resident per-object behavior SM
  (state byte node[4]: 0 init / 1 active / 2 idle / 3 despawn; the active state runs a 6-way node[5] sub-machine
  via jump table `0x80016B50 = {b20,b60,bbc,c1c,c90,b14}`) — a scene/UI TRIGGER (on confirm pushes node[3]
  into 0x800BF871 + calls area-transition FUN_800782F0; plays SFX FUN_80074590; case3 seeds camera/save
  globals 0x800BF890.. + FUN_8005082C). Control flow + node/global writes owned native; sub-calls
  rec_dispatched. `obj739acverify` full-RAM+scratchpad A/B = **1050+ live field calls 0-diff**, 0 bad opcode
  (idle path fully exercised; input-driven node[5] 1..5 transitions faithfully transcribed, verify when driven).
- **`FUN_80073CD8` ✅ OWNED `ov_beh_73cd8` (engine/objbeh_73cd8.cpp).** The resident generic sibling — same
  state-byte shape, but bigger: STATE 0 (init) seeds cull-record (FUN_80051B70 a1=0xc, a2=`(s16)DAT_800a4c94[area]`)
  + box/size fields, then a per-`node[3]` sub-switch (JT `0x80016B68`, node[3]-2 in [0,30]: cases 2/5-7/8/0xc-0xe/
  0x11/0x14/0x15-0x18/0x1d-0x1e/0x20) writing node+0x56/0x80..0x86/8/0xb (case 0x11 bumps node+0x32 by 100 if
  `DAT_800bfe56 & 0x10`). STATE 1 calls cull FUN_8007778C **but ignores its result** (unlike 739ac), then a
  node[5] sub-machine (JT `0x80016BE8`, [0,6]): case1/5 picks a scene id (`DAT_800a4ca8[node[3]]`, special-cased
  for node[3]==2 via DAT_800bf907/8c3) → FUN_8007E110 → node+0x14; case2 pad-edge (`DAT_800e7e68 & DAT_1f800174`);
  case3 releases node+0x14 → idle; case6 FUN_80042728; case4 re-arms→case0; case0 on `node[0x2b]==3` advances +
  per-type FUN_80040B48(0x4e/0x4f/0x50). Tail: special-area (2/7/0x14) release of node+0x14 when `DAT_800e7e85!=0x1f`,
  then node[0x2b]=0 + render FUN_800517F8. Control flow + node/global writes owned native; sub-calls rec_dispatched.
  `obj73cd8verify` full-RAM+scratchpad A/B = **1400+ live field calls 0-diff**, 0 bad opcode.
- **`FUN_800741DC` ✅ OWNED `ov_beh_741dc` (engine/objbeh_741dc.cpp).** The third resident handler that fires
  in the seaside field (a counting probe over {741dc,52078,499e8,4c930} showed only 741dc runs in seaside).
  Item/pickup scene trigger, same state-byte shape (state-1 dispatch is a plain if-chain, not a JT): state-0
  cull-init (FUN_80051B70 a1=1, a2=0x18) + box/size + node+0x56 = `DAT_800a4cec[node[3]]`; state-1 node[5]
  sub-machine — case0 registers a scene (FUN_8007E110 keyed `DAT_800a4cf8[node[3]]`) + SFX FUN_80040b48(0x39)
  / +2 on DAT_800bf8ed, sets DAT_800bf809=1; case1 FUN_80042728; case2 pad-edge `DAT_800e7e68 & DAT_1f800174`;
  case3 spawns a child FUN_8007413C bounded by `DAT_800a4d04[node[3]]` vs counter DAT_800bf874 (else node[5]=99
  re-arm); **case4** (driven) builds a 3-field struct on the guest stack (node+0x2e, node+0x32-(s16)node+0x84/2,
  node+0x36) → 2× FUN_80027144 + SFX FUN_80074590(0xc), then sets the per-type collected bit `1<<node[3]` in
  DAT_800bfa23 and toggles FUN_80040b48/c00(0x39/0x3a) incl. the all-collected (`==0x1f`) reward. Like 73cd8 it
  calls cull FUN_8007778C and IGNORES the result. To make case4 byte-faithful, `ov_beh_741dc` mirrors the recomp
  body's `sp -= 0x30` prologue (wrapper) so the stack buffer at sp+0x10 sits above the sub-call frames exactly
  where the recomp places it. Control flow + node/global writes owned native; sub-calls rec_dispatched.
  `obj741dcverify` full-RAM+scratchpad A/B = **500+ live field calls 0-diff**, 0 bad opcode (idle path; the
  pad/scene-driven sub-states incl. case4 faithfully transcribed, verify when driven).
  The remaining placement-installed handlers are scene-overlay code (0x8012/0x8013xxxx) that run only in OTHER
  scenes (not headless-verifiable in seaside; cross-area warp floods bad opcodes) — the seaside-resident set
  (739ac/73cd8/741dc) is now exhausted.

**Placement record (0x14 bytes; table terminated by a record whose `byte[0]==0xff`):**
| off | type | → node | meaning |
|-----|------|--------|---------|
| +0x00 | u8 | +0x28 (full byte) | TYPE; `a0` to spawn = `type & 0x7f` |
| +0x01 | u8 | — | CLASS; `a1` to spawn; if `class==3` → list/`a2`=1 else 0 |
| +0x02 | u16 | +0x2e | X |
| +0x04 | u16 | +0x32 | Y |
| +0x06 | u16 | +0x36 | Z |
| +0x08 | u8 | +0x02 | (flags) |
| +0x09 | u8 | +0x03 | (sub-id) |
| +0x0a | s16 | +0x56 | facing A (DEGREES → PSX 0..0xfff via signed ÷360 reciprocal `0xb60b60b7`) |
| +0x0c | s16 | +0x58 | facing B (same conversion) |
| +0x0e | s16 | — | COND gate: `1` = skip if collected-bit `0x800BFE56`(u16) bit[area] set; `2` = skip if global enable `0x800BF873`!=0 |
| +0x10 | u32 | +0x1c | per-object HANDLER fn ptr (content) |

(node+0x54 is zeroed per spawn.) **Table select:** area5/sub1..3→`0x8013C1A4`; area1/sub≥15→`0x80134918`;
area6: sub<6 `0x801437AC`/<9 `0x80143ACC`/else `0x80143AE0`; area8: sub<9 `0x8014304C`/<16 `0x801432B8`/<21
`0x80143470`/else `0x80143614`; area0x15: sub0..4 `0x80115004/18/F4/180/1F8`/else `0x80115310`; **default:**
if `u16@0x800BF870==0x704` none, else `0x800A4C28[area]` (0 → none). The seaside field (area 0) takes the
default PTR-table path. `placeverify` gate = full main-RAM+scratchpad A/B vs `rec_super_call(0x80072A78)`:
**seaside field 0-diff** (both per-load calls), 0 bad opcode. Cross-area exercise of the special tables is
blocked by the documented prerequisite-state warp limitation (the record-decode loop is shared & verified;
table-select is a 1:1 disasm transcription). Registered in `entity_spawn_register()`.

### The entity-list walk — `FUN_8007a904` (the engine's per-frame object driver)
```c
for (n = DAT_800fb168; n; n = *(n+0x24)) { *(n+1) = 0; (*(handler@n+0x1c))(n); }  // list 1
for (n = DAT_800f2624; n; n = *(n+0x24)) { *(n+1) = 0; (*(handler@n+0x1c))(n); }  // list 2
```
Clears the render flag, then calls each node's handler (the PSX gameplay/render routine). A second
walk of `DAT_800f2624` exists at `:18660` (likely a separate pass). **OWNED native (Phase 1) —
`ov_entity_walk_7a904` (engine/entity.cpp), registered in game_tomba2.cpp.** The list traversal is
reimplemented in C (capture `next` first, clear `+1`, dispatch each handler via `rec_dispatch`); the
per-type handlers stay PSX / honor their own owned overrides. `walkverify` gate = full main-RAM +
scratchpad A/B vs `rec_super_call(0x8007a904)` (same family as disp26c88/sm40558). This puts the engine
in charge of iterating the world's objects — the foundation for PC-owned per-object render
classification (the proper #4 fix: know each object's render TYPE so depth/ordering falls out). NEXT
(Phase 2): capture per-object render type + world transform during the walk and classify billboards /
foreground-decor at the source, instead of the late provenance heuristic at the OT walk.

## Per-object cull / LOD + submit
- **Cull/LOD** `FUN_8007712c` (`:54440`, override `ov_object_cull`): args = (obj*, dx, dy, dz) where
  (dx,dy,dz) = object pos − camera pos, sign-extended from the u16s. Computes `dist² = dx²+dy²+dz²`
  (`FUN_80077fb0` ≈ isqrt), and **depth along camera forward** = `dx*camFwd.x + dy*camFwd.y + dz*camFwd.z`
  (`_DAT_1f8000e8/ea/ec` = camera forward vector). Branches on entity type `+0xc` and global mode
  `_DAT_1f800084` to decide cull (return 0) vs LOD tier. Special: if `DAT_800bf870 == 4`, force mode 2.
- **Submit wrappers** (each sets render mode then calls the cull dispatcher), called from the per-type
  entity handlers:
  | fn | `_DAT_1f800080` | `_DAT_1f800084` | extra pos offset |
  |----|----|----|----|
  | `FUN_8007778c` `:54685` | 0 | 0 | none |
  | `FUN_800777fc` `:54702` | 0 | 2 | none |
  | `FUN_80077870` `:54719` | 0 | 1 | none |
  | `FUN_800778e4` `:54736` | 0 | 0 | +Y(param2) |
  | `FUN_80077958` `:54754` | 0 | 0 | +X,+Y |
  | `FUN_800779d0` `:54772` | 0 | 0 | +X,+Y,+Z |
  | `FUN_80077a4c` `:54791` | 1 | 0 | +X,+Y,+Z |
  | `FUN_80077acc` `:54810` | 1 | 4 | absolute pos args |
- Per-type handlers (call the wrappers with their obj*): e.g. `:21274 :27367 :28376` → `FUN_8007778c`;
  `:45944 :45991 :46120` → `FUN_80077a4c`. These are the entity update/render routines (Phase 2 targets).

## Per-object 2D BOX / hitbox-corner builder — `FUN_8003B220` (OWNED native, engine/hitbox.cpp)
Pure resident LEAF (64 insns, ZERO jal, ZERO GTE, ZERO render packets, no scratchpad). ~1.64% of the
seaside field's sampled interpreter time — the hottest still-recomp resident CONTENT leaf that is NOT a
render-boundary fn. Signature `void FUN_8003B220(a0=dst struct, a1=base value, a2=params)`. It builds a
small 2D box / corner set in the a0 struct from byte params in a2; every value is the one LIVE in memory at
that point (the recomp re-loads each halfword), so the load/store ORDER is load-bearing. Semantics (all dst
fields are u16 halfwords, all a2 reads bytes):
- `M32[a0+0]=a1`; then `a0[0] += (s8)a2[14]` (X origin += signed dx), `a0[2] += (s8)a2[15]` (Y origin),
  `a0[10]=a0[2]`, `a0[16]=a0[0]` (X snapshot), `a0[8]=a0[0]+(u8)a2[10]` (X far corner).
- zero `a0[4]/[12]/[20]/[28]`.
- `a0[18]=a0[2]+(u8)a2[11]` (Y far corner), `a0[24]=a0[8]` (X-far snapshot), `a0[26]=a0[18]` (Y-far snapshot).
- then scale ×5 (`sll x,2; addu x` = x*4+x): `a0[0]/[16]/[8]/[24]/[2]/[10]/[18]/[26] *= 5` in that exact
  reload order. v0 (ignored by callers) = the last computed value = `(s16)a0[26]_pre * 5`.
The "×5" is the cell→pixel scale (the collision/tile grid uses 5-unit cells). OWNED native; gate `boxverify`
= full main-RAM + scratchpad + v0 A/B vs rec_super_call(0x8003B220): **0 mismatches over 5000+ live field
calls** (press right 400 + press left 400). RAM/scratchpad were byte-identical from the first build — only
the return reg needed mirroring (the gen's delay-slot `sh v0,26(a0)` leaves v0 in r2). a0 is typically a
STACK-local struct (a0~0x801fe8c8). Registered in game_tomba2.cpp. (later: hitbox.cpp.)

## Deferred render pipeline — the render-command QUEUE + flush (later-130, the missing architecture)
Geometry submission is NOT inline in the entity handlers. It is a **deferred two-phase** system (proved by
`PSXPORT_DEBUG=geomblk`: all owned submits run with `g_current_object==0`, AFTER every per-object cull):
- **Phase 1 (entity walk, `FUN_8007a904`):** each handler runs gameplay AND, if its object is visible,
  ENQUEUES a **render command** into a per-frame render-command list (it does NOT project here).
- **Phase 2 (flush):** a loop drains the command list, loading each command's GTE transform and dispatching
  it to a per-mode renderer that does the actual projection + packet build (GT3/GT4 etc.).

**Render-command list + flush loop** (`gen_func_8003F174`, and a sibling at `gen_func_8003F0xx`; callers
0x8003d074/0x8003f138/0x8003f228): the list header has the command COUNT at `+8` (a second count/flag at
`+9`); the commands are reached via a **pointer array at `list+0xc0`** (`lw 0xc0(cursor)`, cursor += 4 per
iter). For each command struct `cmd`:
- `cmd+0x18 .. +0x2c` = the **per-object GTE transform** (rotation matrix + translation), loaded straight
  into the GTE control regs at flush via the `lwc2/ctc2` block — THIS is where the "96/54 ctc2 sites" live
  for queued objects (the transform is captured into the command at enqueue, replayed here).
- `cmd+0x40` = the **geomblk pointer** (the model's primitive-record list) → passed as `a0` to the dispatcher.
- flush calls dispatcher `gen_func_8003F698(a0=geomblk, a1=*0x800ED8C8 OTbase, a2=flag)`.

**Mode dispatcher `gen_func_8003F698`:** reads a render-mode byte `*0x800BF870` (`DAT_800bf870`, 0..0x15;
the engine_re cull note "if DAT_800bf870==4 force mode 2" is THIS global), indexes a **22-entry jump table at
0x80015268**, tail-calls the per-mode renderer. Early-outs to the generic GT3/GT4 path (mode→0x800803DC) when
`*0x1F800234 != 0`, `a2&1`, or mode≥0x16. Table (mode → renderer):
`0:0x80146478  1:0x80132dc0  2:0x8012555c  3:GT3/GT4(0x800803DC)  4:0x8013dafc  5:0x801362cc  6:0x8013d568
7:0x8012e1a0  8:0x8012a9dc  9..0x13:GT3/GT4  0x14:0x80116b14  0x15:0x8010b1b8`. The `0x8013xxxx` entries are
the per-scene OVERLAY submitter variants the margin handlers feed (NOT the natively-owned GT3/GT4 — that is
only mode 3 / ≥9). `*0x800BF870` is a GLOBAL set before/within a flush pass, so one pass renders in one mode.
- **Command struct fields (confirmed via `PSXPORT_DEBUG=rcmd`, later-131):** `+0x18` GTE transform (CR0-7),
  `+0x40` geomblk ptr. `a2`/`flag` passed to the dispatcher: bit0 forces the generic GT3/GT4 path. At the
  field, base/world commands carry flag=1, the widescreen-margin commands carry flag=0. The margin commands'
  geomblks are REAL (0x801exxxx model prim-lists) even though the node `+0x38` mdata is 0 — geometry is
  resolved at enqueue, not from `+0x38` (corrects later-129's "no model data").
- **Command struct ENQUEUE — `gen_func_80051B70`** (and a multi/instanced loop variant ~0x80051A60, and a
  layer-builder ~0x8003AE28): args `a0`=object node, `a1`=model group idx, `a2`=model sub idx. Allocates the
  command (`gen_func_8007AAE8`), stores the cmd ptr at **node+0xc0** (the object→command link), clears the cmd
  header (`+0/2/4/8/0xa/0xc`=0, `+6`=-1), sets scale `cmd+0x38/3a/3c`=0x1000, and resolves the geomblk via:
- **Geomblk resolution (data-driven, leaf `gen_func_80051B04`):** `geomblk = T + *(T + sub*4 + 4)`, where
  `T = *(0x800ECF58 + group*4)`. A **two-level model table at 0x800ECF58**: outer index = group, inner =
  sub-model. Fully deterministic from the handler's `(group,sub)` selectors → the native render-half computes
  the geomblk the same way (no guessing). Stored to `cmd+0x40`.
- **Transform build — `gen_func_80051C8C`:** builds the object's matrix into `node+0x98` (rotation from
  `node+0x54/56/58` via `gen_func_80084D10/80084EB0/80085050`) and translation `node+0xac/b0/b4` from position
  `node+0x2e/32/36`. This matrix is what the flush loads (cmd+0x18) into the GTE before projecting.
- **Commands are PERSISTENT, not per-frame (later-132, via `PSXPORT_WWATCH`):** `cmd+0x40` was written exactly
  ONCE across 2905 frames — the render command (geomblk + base transform) is built at object spawn / scene
  setup and kept at `node+0xc0`. Per frame only the transform translation/rotation updates + the flush runs.
  So the `+1` re-include's effect = the (already-built) cmd of a re-included object being drawn; the NATIVE
  margin plan reads `node+0xc0` (its persistent cmd) for a culled margin object and adds it to the flush list,
  WITHOUT poking `+1` (which also ticks gameplay). Probes: `PSXPORT_DEBUG=cmdenq`/`flush`/`enq`,
  `PSXPORT_WWATCH=lo,hi` (word-store PC tap; NB byte stores like `list+8` count are not caught).
- **NATIVE-OWNED (later-135) — `gen_func_8003CDD8` (the per-object flush), `gen_func_8003F698`
  (dispatcher generic path) and `gen_func_800803DC` are now reimplemented in C** (`engine/engine_submit.c`
  `submit_perobj_flush`/`native_dispatch`/`native_gt3gt4`, registered on `0x8003CDD8`). The world render
  submission runs with NO guest render code; **VRAM byte-identical** vs the recomp body (headless field
  f328, A/B `PSXPORT_PEROBJ_RECOMP=1`). Only the per-scene overlay submitter variants (mode-table
  `0x8013xxxx`) + the resident byte-packed `0x80027768` are still recomp (next RE). See journal later-135.
- **CORRECTION (later-133/134) — THE OBJECT NODE *IS* its render-command list, and rendering is per-object.**
  `node+0xc0` is the BASE of a cmd-pointer ARRAY (count at `node+8`), not a single ptr. Each visible object
  is rendered by **`gen_func_8003CCA4(node)`** (per-object render dispatch by `node+0xd` via jump table
  @0x80014ec8) → **`gen_func_8003CDD8(node, flag)`** (the MAJOR world flush; loop @0x8003ce40 reads
  `cmd = node[0xc0+i*4]`, `geomblk = cmd+0x40`, COMPOSES camera(`0x1f8000f8`)×object-matrix(`cmd+0x18`) into
  the GTE, translation from `cmd+0x2c/0x30/0x34`) → dispatcher `gen_func_8003F698`. The minor flush
  `0x8003F174` only drains the one static-decor list `0x800fb218` (8015ca04×24). The per-object transform
  (`cmd+0x18`, `node+0x98`) is built each frame by **`gen_func_80051C8C(node)`** in the handler's VISIBLE
  branch — a culled object's is stale/zero, so the native margin must call it before the render. Native margin
  (engine/margin_render.cpp): collect re-included type-`0x03` nodes in cull (no +1), then per node
  `gen_func_80051C8C` + `gen_func_8003CCA4` after the walk → +24 margin renders, base 100 byte-identical,
  gameplay 0-diff to render-cache. See journal later-133/later-134. RE REPL: `dbgclient.py ents/node/call/geomblk`.

## Geometry SUBMIT — `0x8007FDB0` (POLY_GT3 tri) + `0x8008007C` (POLY_GT4 quad) — NATIVE-OWNED (engine_submit.c)
These are the resident routines that turn a model's pre-built primitive-record list into GPU packets in
the OT. Both are now reimplemented natively in `engine/engine_submit.c` (`ov_submit_poly_gt3/4`),
**0-diff vs the recomp body on the field** (A/B: `PSXPORT_SUBMIT_RECOMP=1` keeps the recomp bodies).
This is the deletion of the reason the value-keyed "attach" depth-recovery hack existed — the engine now
computes the projection and can carry the real per-vertex view-Z straight to the renderer (Phase 2).
- **Caller** `gen_func_800803DC` (`0x800803DC`): `r16 = mem_r32(geomblk+0)` packs counts (low16 = tri
  count, high16 = quad count); `a0 = geomblk+16` (record array). Calls tri-submit `(a0, OTbase, triN)`,
  then quad-submit `(ret, OTbase, quadN)` — **tri-submit RETURNS a0 advanced past its records** (= quad
  array base), so the return value matters. Records are tri[] then quad[], contiguous.
- **Global** packet-pool write pointer `0x800BF544` (advanced past each committed packet; written back).
- **Triangle** (`0x8007FDB0`): 36-byte record `{+0 rgb0|code, +4 rgb1(rgb2=rgb1<<4), +8 uv0|clut,
  +12 uv1|tpage, +16 VXY0, +20 VZ0|VZ1, +24 VXY1, +28 VXY2, +32 VZ2|uv2}` → load V0..V2, **RTPT**, FLAG
  test, **NCLIP** backface (MAC0>0), frustum cull (drop iff all SX≥320 or all SY≥240 — right/bottom
  only), OT-z, → 40-byte **POLY_GT3** `{tag,rgb0,SXY0,uv0,rgb1,SXY1,uv1,rgb2,SXY2,uv2}`, OT tag len 9.
- **Quad** (`0x8008007C`): 44-byte record, project the lone 4th vertex (V3) via **RTPS** first, then the
  front tri (V0,V1,V2) via **RTPT**; 4-vertex frustum cull; → 52-byte **POLY_GT4**, OT tag len 12.
- **OT-bucket depth** (the `>>2` order-stat): record code byte `(code>>24)&3` selects **type 1 = farthest
  (max SZ)**, **type 2 = nearest (min SZ)**, **else = hardware AVSZ3/AVSZ4 average** (SZ FIFO: tri DR17-19,
  quad DR16-19). Then a log-compress `idx=(otz>>(sh&31))+(sh<<9), sh=otz>>10` and a **drawable clamp
  `idx∈[4,2047]`** (else the prim is dropped). Verified the min/max by exhaustively tracing both bodies.
- **Note:** the recomp bodies are slightly **nondeterministic** run-to-run (≈300px at one field frame —
  uninitialised packet padding the GPU re-reads); the native versions are deterministic and match a
  freshly-captured recomp run exactly.
- **Phase-2 depth — DONE for the owned prims.** Each owned submit records the vertex SZ (view-Z) keyed by
  the packet word address (`projprim_set_pz`); the renderer reads it (`projprim_lookup_pz` →
  `proj_pz_to_ord`) for true D32 occlusion under `PSXPORT_NATIVE_DEPTH`/`PSXPORT_SBS`. The value-keyed
  "attach" ring (gte_op capture + mem.c store hook) is **deleted**. Deterministic; fixed the water
  punch-through/flicker.
- **Overlay ownership via SCAN-ON-LOAD (later, this is the general mechanism).** The SAME GT3/GT4 submit
  library is also present in **runtime-loaded overlays** at per-scene-reused addresses (it ran interpreted
  → no depth). Owned generically: `rec_overlay_loaded(base,size)` (called by the CD loaders ov_cd_loadfile/
  ov_cd_async_read) clears prior scan-overrides and calls `engine_scan_overlay` (engine_submit.c), which
  scans the freshly-loaded bytes for the submit signature (packet-pool load `lui 0x800C`+`lw 0xF544`,
  RTPT, OT tag-len `lui 0x0900`=GT3 / `lui 0x0C00`=GT4) and registers the native impl via
  `rec_set_interp_override_auto` at each entry. Scan-on-LOAD (not per-call: classify-on-every-call was far
  too slow + thrashed). The flat interp now honours interp-overrides (coro_native_call → interp_override_for).
  VERIFIED 0-diff vs fully-interpreted (`PSXPORT_SUBMIT_RECOMP=1`) at f470/f560/f600. `PSXPORT_DEBUG=submit`
  logs each owned overlay submitter; `PSXPORT_NO_OVERLAY_OWN=1` is the A/B.
- **RESOLVED — field WORLD geometry is 100% engine-owned with real depth (later-166, re-measured).** The
  earlier "~70% of world polys fall to the 2D band / `0x80027768` is the next ownership target" claims are
  STALE — superseded by the render queue (M1, later-164) + the scan-on-load overlay ownership. Re-measured
  at the green field (present f650, `PSXPORT_AUTO_GAMEPLAY`):
  - `0x80027768` is **already owned** — `engine/engine_submit.cpp` `ov_submit_poly_gt4_bp`, registered in
    `game_tomba2.cpp:424`. (The byte-packed GT4 decode is committed; it is NOT open work.)
  - The dominant field submitter, dispatcher **mode 0** → overlay renderer `0x80146478`, is a *CALLER*
    wrapper owned by the scan-on-load (`PSXPORT_DEBUG=submit`: "own overlay CALLER @ 0x80146478"); the
    GT3/GT4 submitters it calls (`0x801465EC`/`0x801467BC`) are scan-owned native → they push straight to
    the render queue as `RQ_WORLD` with real per-vertex depth. (`PSXPORT_DEBUG=pdisp` counts the dispatch
    *wrapper* as "fallback" — a RED HERRING; the inner submit is native.)
  - The only prims still flowing through the guest OT walk at the field are **op 0x2D/0x2F flat-textured
    quads** (~34/frame, `PSXPORT_DEBUG=ndepth` op-histogram). Per `PSXPORT_POLYDUMP` these are small (8–12px)
    axis-aligned screen-space rects at the font/UI texpage `tp=(960,256)` with per-glyph colors — i.e. **HUD
    text/UI**, genuinely 2D, correctly in the 2D band. The gouraud world ops (0x3C GT4 / 0x34 GT3) the old
    note listed as leaking are **gone** (now owned). So M4 ("100% world geometry carries real depth") is
    effectively achieved for this scene; no more world submit VARIANTS need owning here.
  - The retired metrics: `PSXPORT_DEBUG=ndepth` (real-depth/2D-band counters) and the value-keyed
    `projprim` depth ring measure the OLD attach path, which the render queue superseded — under the queue
    they read ~0 even when world depth is correct. Don't gate on them; measure the queue instead.
- **M3 (in progress) — own the 2D BACKGROUND layer by PROVENANCE (later-167, DONE for the field).** The
  leftover 2D (HUD/text FT4 quads + the screen-space backdrop tiles) reaches the renderer via
  `gpu_dma2_linked_list` walking the guest OT. The BACKDROP is the field's scrolling-tilemap drawer
  **FUN_80115598** (+ a 2nd parallax layer 0x8010C26C): cols/rows@desc+16/+17, scroll@+40/+42, U-base@+6,
  clut@+4, mapdata ptr@+20; emits a grid of 352 16×16 textured-rect packets. Each tile is below the old
  `bg_2d` coverage threshold so the whole backdrop mis-classified as HUD. FIXED by provenance: the
  scan-on-load (`engine_scan_overlay`) anchors on the unique tile command-word build `lui a1,0x7d80 ; ori
  a1,a1,0x8080` and registers a BRACKET override (`ov_bg_tilemap`) that records the drawer's packet-pool
  span as RQ_BACKGROUND (`gpu_bg_range_add`/`node_is_bg`). The 2D classifier is now `node_is_bg(node) ||
  bg_2d(...)` — provenance wins (any tile size), coverage is the fallback for un-owned scenes. Proven the
  tiles ARE the backdrop two ways (writer-trace + full-OT draw order: tiles drawn before world). RAM 0-diff
  @ f650. **REMAINING:** own HUD/text at source too, then retire the OT-walk frame driver + a native
  background renderer (push tiles straight to RQ_BACKGROUND, no guest packet) + delete `bg_2d` (M3/M4).
- OPEN: 3D-projected overlay banners (hint signs) now sit behind nearer world geo (true depth vs the
  artist's OT-on-top) — overlay-vs-world depth semantics to resolve.

## Camera
- Position (u16): `_DAT_1f8000d2` (X), `_DAT_1f8000d6` (Y), `_DAT_1f8000da` (Z).
- Forward vector (s16): `_DAT_1f8000e8/ea/ec` (used in the cull depth dot product).
- **Per-frame camera UPDATE = a CHAIN of small per-axis SMOOTHER functions (the NEXT engine-ownership
  target, later-173).** (CORRECTION of an earlier note that wrongly called this one ~960-instr function:
  `tools/disas.py` is function-scoped — it stops at the first `jr ra` — so a single dump misled me. The real
  structure is several SHORT functions.) Init is already native (`eng_init_camera`, FUN_80050a80); these run
  per play-frame. Found via `PSXPORT_WWATCH=9f8000d0,9f8000de` during the walk scene — every store to the
  camera state comes from PCs 0x8006d9c4/d9d4, 0x8006e3c4, 0x8006e89c/e8bc:
  - **`FUN_8006d960`** (0x8006d960→`jr ra`@0x8006da4c, ~60 instrs): X-axis smoother. `s1=a1`=camera-TARGET obj
    (Tomba), `s0=0x1f8000d0`=cam-state base. `delta = target[+2] − cam[+0xe](0x1f8000de)`; if `|delta|<=10`
    snap, else step the 32-bit fixed accumulator `cam[+0xc](0x1f8000dc) += clamp(delta,±6144)<<13`.
  - **`FUN_8006da54`** (0x8006da54→`jr ra`, ~60 instrs): SAME shape for the next axis (`target[+6]` vs
    `cam[+0x12]=0x1f8000e2`). More per-axis siblings follow; the matrix-build writers are the 0x8006e3c4/
    e89c/e8bc cluster (a later fn using libgte `0x80083e80`/`f50`/`84080`/`85690`, `slti …,401` clamp).
  - **Shared rate-limiter `FUN_8006ce74`(delta, maxstep)** = `clamp(delta, −maxstep, +maxstep)` (pure; sign-
    extended s16). Used by every axis smoother.
  The OUTPUT (cam matrix `0x1f8000f8` + pos `0x1f8000d2..da` + forward `0x1f8000e8`) is the CONTENT/RENDER
  interface — `proj_native_vertex` (render) AND the cull `FUN_8007712c` read it — so the ownership gate is
  **RAM-match of those camera fields** (A/B: override-on vs off build, `PSXPORT_RAMDUMP` + `cmp -l`),
  verifiable on the free-roam MOTION scene (`AUTO_SKIP=500 AUTO_WALK`) that the static idle field (A==B) can't
  exercise. Watch the camera live with `PSXPORT_DEBUG=cam` (per-frame pos).
  - **OWNED native (later-174, verified RAM 0-diff):** `FUN_8006d960` X/Z (`ov_cam_track_xz`, maxstep 6144) +
    `FUN_8006da54` Y (`ov_cam_track_y`, maxstep 5632) → camera POSITION X/Y/Z is native (engine_camera.cpp).
  - **The rest of the camera-update system (NEXT), mapped by walking functions (disas.py is function-scoped):**
    - **Rotation / look-at builder `FUN_8006e464` — OWNED native (`ov_cam_rotbuild`, engine_camera.cpp,
      later-175).** Entry is **0x8006e464** (prologue: 48-byte frame, saves s0–s5/ra; epilogue `jr ra`@0x8006e8f0);
      0x8006e6a8 is just one CASE of the **0x800169cc** jump table. Structure: a mode dispatch with TWO jump
      tables — table1 @**0x80016994** (13 entries, on `G[+0x164]`, G=0x800e7e80) and table2 @**0x800169cc**
      (8 entries, on `((G[+0x61]&0xff)>>4)-1`) — plus a `c114(cam+0x72)&0x40` "mode-A" path and a
      `c114&2` path, each producing a heading ANGLE; then the COMMON look-at tail @0x8006e7c8:
      `look = center(0x1f800160/164) ± rcos/rsin(angle)·radius>>12`, `yaw = ratan2(-dz, dx)`,
      `dist = isqrt(dx²+dz²)`, then `0x1f8000d0 += rcos(yaw)·dist>>1`, `0x1f8000d8 -= rsin(yaw)·dist>>1`
      (cam[+0x66]|=1 when dist<401). cam pos read from `0x1f8000d2/da`. **RADIUS = sext16(-mem_r16(0x1f8000ee))**
      — set as s2 in the DELAY SLOT @e4d8 (UNCONDITIONAL, before dispatch), sext16'd at each use; the e518 path
      overrides to -ee-600. (Mis-reading that delay slot as an ancestor-supplied s2 register, and forgetting the
      sext16, were the two bugs caught by the gate.) **PORT APPROACH (done):** own control flow + arithmetic
      native, CALL libgte trig via `rec_dispatch` (0x80083e80=rsin, 0x80083f50=rcos, 0x80085690=ratan2,
      0x80084080=isqrt) — do NOT reproduce their LUTs. **GATE:** output is SCRATCHPAD → main-RAM A/B is BLIND;
      use the per-call comparator `PSXPORT_DEBUG=camverify` (snapshot scratchpad, run native, restore, run recomp
      oracle, compare) — 0-diff with d0 accumulating on the free-roam MOTION scene (continuous movement; a
      stopped scene is degenerate). Default field path = table1 case-0; special-camera modes ported but latent.
    - **Per-MODE orchestrators** (each calls the position smoothers + a matrix builder; different camera
      behaviours): `FUN_8006e0f0`, `FUN_8006e228`, `FUN_8006e3f4` (call 0x8006d960/da54), and matrix-side
      `FUN_8006dad8`/`FUN_8006def0` (call the libgte helpers). A camera-mode selector calls one per frame.
    - So the camera is a multi-mode system; own the matrix builder + the active mode's orchestrator next,
      A/B-gated on the motion scene the same way.
    - **DISTANCE / ZOOM solver `FUN_8006d2ac` — OWNED native (`ov_cam_dist_solve`, engine_camera.cpp, later-176).**
      First sub-fn of orchestrator FUN_8006e0f0; cam=a0 (main-RAM camera object), G=0x800e7e80. Steps:
      (1) sets a settle timer cam[+0x22] (240 unless blocked by G[+0x61]&0x80 / G[+0x17a] / 0x800bf816 / cam[+0x72]&4);
      (2) picks a TARGET planar point (tX,tZ in 16.16) — a **13-entry jump table @0x800168d4 on G[+0x164]** selects the
      source (world center G[+0x2c]/[+0x34]; a sub-object G[+0x10]→+0x2c/+0x34; scratchpad 0x1f800200/204; G[+0x14c]/[+0x150])
      plus a `cam[+0x72]&2` fast path; a "mode" byte (g147=G[+0x147], or 1−g147 when G[+0x158]≠0) decides whether the
      look heading adds 2048 (180°);
      (3) look point = (tX,tZ) ± rcos/rsin(baseAng)·cam[+0x22]·16; current cam pos along heading G[+0x140] at distance
      cam[+0x58]; planar distance `s0d = isqrt(Δx²+Δz²)<<8`, angular error `angd = (ratan2(−Δz,Δx) − G[+0x140] − 1024)&0xfff`;
      (4) smooth cam[+0x14]: FAR (s0d>0x140000) → step ±65536/frame toward ±0x280000 by sign of (angd<2048); NEAR → snap to
      ±s0d (sign = angd<2048). Then cam[+0x58] += cam[+0x14]>>8, and cam[+0x08]=tX+rcos·cam58>>4 / cam[+0x10]=tZ−rsin·cam58>>4.
      Own arithmetic native; CALL libgte trig via rec_dispatch. GATE: per-call comparator `PSXPORT_DEBUG=camverify`
      (also fires `ov_cam_dist_solve_verify`), 0-diff over 1800+ calls. **TRAP CAUGHT:** the far/near branch (`beq …,8006d5b8`)
      has a DELAY SLOT `slti v0,angd,2048` that executes on the TAKEN branch, so the NEAR-path store test `beq v0,zero,8006d5c4`
      is really `angd<2048` → NEGATE s0d. Missing that flipped the sign of the snapped distance; found by a trig-call spy that
      proved s0d was bit-identical between native and oracle, isolating the bug to the final branch (not the math).
- Full basis (right/up): per-object rotation matrix is loaded to GTE CR0-4 + translation CR5-7 right
  before each RTPS/RTPT (96 / 54 static ctc2 sites) — the per-object transform, the Phase-3 native target.

## Projection setup — `gen_func_800509B4` (0x800509B4) = the NATIVE WIDESCREEN lever (RESOLVED later-98)
Found by histogramming `gte_write_ctrl(reg,…)` in `generated/`: OFX/OFY/H (CR24/25/26) are written at
exactly 2 sites — libgte `InitGeom` defaults + this one real config. The engine's projection config:
```
gen_func_800509B4 (0x800509B4):
  InitGeom (gen_func_80083FF8 / 0x80083FF8): ZSF3(cr29)=341, ZSF4(cr30)=256, H(cr26)=1000,
      DQA(cr27)=-4194, DQB(cr28)=320<<16, OFX/OFY=0  (libgte reset; H+depth-cue overwritten next)
  SetGeomOffset(160,120)  (gen_func_800846D0 / 0x800846D0): OFX(cr24)=160<<16, OFY(cr25)=120<<16
  SetGeomScreen(350)      (gen_func_800846F0 / 0x800846F0): H(cr26)=350; also caches H=350 @0x801003F8
```
So the GTE projection is: **screen center (OFX,OFY)=(160,120), focal length H=350**, screen 320×240.
Screen X = OFX + IR1·H/Sz, screen Y = OFY + IR2·H/Sz.
- **NATIVE widescreen (no squish, no renderer trick):** override `gen_func_800509B4` to set **OFX=214**
  (=428/2 for 16:9) and **widen the draw-environment + clip rect to 428** (keep OFY=120, H=350 → identical
  vertical FOV and per-unit scale). The GTE then projects vertices across the wider screen → genuinely
  wider horizontal FOV, computed by the engine's own projection. 2D HUD (drawn in screen space, bypasses
  GTE) is repositioned separately. **TODO:** find the draw-environment / clip-rect (screen width) setup —
  near the double-buffer/OT setup `FUN_80081458` / disp-env; needed to widen the clip to match OFX.
- **Higher res (native, not supersampling) — IMPLEMENTED (`PSXPORT_IRES=N`, default 1 = faithful):**
  the VK backend rasterizes the engine's submitted geometry into the scratch FB at N×(320|428)×240 by
  scaling the rasterization viewport (`gpu_gpu.c` `s_ires`/`use_fb`/`push_wide`, shader already maps
  framebuffer-local×scale). Same engine output, sampled denser — crisp 3D edges; textures still sampled
  from PSX-res VRAM. Caps within the 1024-wide VRAM image: 4:3 → 3×, 16:9 → 2×. A dedicated >1024 render
  target would lift the cap (next). Distinct from the rejected supersample-and-downscale FB-cram trick.

## Water + sky — drawn via a NON-GTE (screen-space 2D) path; native-depth handling (later-98, later-104)
Observed during the lighting work (normal-viz): terrain/ground polys get a reconstructed per-face normal
(they go through RTPS/RTPT), but the **water surface does NOT** — it renders as a flat unlit layer. So the
water (and the sky backdrop) is **not GTE-projected 3D**; it's a separate screen-space / fixed-projection
layer (a scrolling textured surface and/or a framebuffer-reflection effect).

**Consequence for native depth (later-104, FIXED):** because the water/sky have no projection records,
the native-depth tee classifies them `is3d=0`. With the old two-band D32 model (3D world + a NEAR 2D
overlay band for HUD) the backdrops fell into the NEAR band and **occluded the entire 3D world** (water
over terrain). Fix = a **3-band depth model**: a FAR 2D-background band [0, 0.0625) for backdrops, the
3D world band [0.0625, 0.9375], and the NEAR 2D-overlay band (0.9375, 1] for HUD. Background vs HUD is
split by OT submission order (drawn before any 3D prim this frame = backdrop → far band; after = HUD →
near band; per-frame `s_seen3d` in gpu_native.c). The backdrops now sit behind the world correctly.

The water's *appearance* (the prior "green smeared streaks vs blue reflective water" report) is a
separate question about the reflection/MoveImage copy; to RE it, point `PSXPORT_PROVAT="x,y"` at a water
pixel for the prim op/texpage/CLUT + owning node, then read that handler in the decomp (compare to the
oracle at the SAME game-state, not by frame number).

## Lighting / shading model (RESOLVED — later-96, via GTE op histogram + control-reg snapshot)
Tooling: `PSXPORT_GTEPROBE=<frame>` (gte_beetle.c) dumps the GTE ops that ACTUALLY execute + a
lighting/fog control-register snapshot. Static-callsite histogram across `generated/shard_*.c` agrees.

**There is NO dynamic GTE lighting.** The hardware per-vertex lighting ops `NCDS/NCDT/NCCS/NCCT/NCS/
NCT/CC/CDP` execute **zero** times (and have zero call-sites). So no normal·light-matrix shading, no
light sources. What the GTE actually runs (f1500/f3000 counts): `RTPS/RTPT` (projection), `MVMVA`
(transforms), `NCLIP` (backface sign), `AVSZ3/4` (OT depth), `GPF` (interpolate IR·IR0), and
`DPCS/DPCT` (depth-cue). Implications for the shading model:
- **Vertex colors are BAKED** into the model data (artist-painted per-vertex RGB), not computed.
- **`GPF` (highest count after RTP) scales the baked color by a scalar IR0** — the per-object/global
  brightness + fade-in/out factor. (GPF: MAC = IR0·IR → color FIFO.)
- **Atmosphere = GTE depth-cue FOG (`DPCS`/`DPCT`)**: final vertex color is interpolated toward the
  **FarColor (CR21-23)** by a depth factor `IR0 = DQB + DQA·(H/Sz)` (`DQA=cr27`, `DQB=cr28`). The
  FarColor is **scene-tinted**: f1500 (water/dusk) = (0,0,0) fade-to-black; f3000 (lava) = (1280,0,0)
  red glow. DQA=6, DQB=0 in both. This depth-cue IS "the game's lighting/atmosphere".

**Where final color enters the GPU (the interception point):** the computed per-vertex RGB lands in the
**GP0 gouraud-polygon packets** → captured in `gpu_native.c` gp0_exec polygon tee as `rs/gs/bs`
(op 0x20-0x3F). That is the universal sink regardless of how the game derived the color.

**What a native lighting engine needs that the PSX stream doesn't directly give: per-vertex normals /
3D position.** Now available: PGXP (later-95) caches per-vertex screen (x,y) + **precise_z** (view-space
Z). Unproject (screen + z via H/OFX/OFY, CR24-26) → view-space position per vertex → per-FACE normal by
cross product of triangle edges. That unlocks PC-native directional/point lighting, normal-based
shading, SSAO, and a replacement per-pixel fog (read the scene FarColor from CR21-23 for the tint),
all replacing/augmenting the baked color + GTE depth-cue. (Camera basis: see Camera section / CR24-31.)

## Graphics pipeline — the REAL draw path (libgpu), the ownership target (later-99)
Today the recompiled game runs **Sony libgpu** → writes GP0/GP1 → our GPU emulator (gpu_native/gpu_gpu)
just rasterizes the resulting byte stream. "Owning the graphics" = reimplementing the libgpu layer in
native C (game calls into OUR DrawOTag/PutDrawEnv/primitive code), so every draw is understood, not
black-boxed. The layer is small and standard (libgpu), dispatched via a jump-table at **0x800A5998**
(the libgpu "GPU sys" struct; entries are fn-ptrs at +0x08 DMA-send, +0x14 DrawOTagEnv, +0x2c ClearOTagR,
+0x3c DrawOTag/DrawSync).

**Per-frame loop** `FUN_80050b08` (0x80050b08) — function names CORRECTED from each libgpu fn's debug
string (earlier draft mislabeled DrawSync/DrawOTag/PutDrawEnv):
```
parity = DAT_1f800135 (0/1);  drawbuf base = 0x800bfe68 + parity*0x14000 (DAT_800bf544)
ctx = &DAT_800e80a8 + parity*0x81c              // per-buffer GPU context (DRAWENV+DISPENV+OT head)
FUN_80081458(ctx, 0x800)   = ClearOTagR(OT, 2048 entries)   // reset ordering table
FUN_800788ac()             = input + sub-tick
FUN_80051e60()             = task scheduler -> BUILDS the OT (entity handlers AddPrim into it)
FUN_80080f6c(0)            = DrawSync(0)         // WAIT for previous frame's draw to finish (not the draw!)
wait DAT_800e809c >= DAT_1f800235               // vblank gate: 1 = 60fps, 2 = 30fps  (<-- 60fps lever)
FUN_800506d0()
swap: FUN_8008179c(ctx+0x2000)=PutDispEnv (display area, GP1),
      FUN_800815d0(ctx+0x2014)=PutDrawEnv (draw-area CLIP+offset),
      FUN_80081560(ctx+0x1ffc)=DrawOTag(OT)     // <-- THE actual draw: kick the OT DMA to the GPU
      flip parity
```
libgpu (all via the 0x800A5998 table; names from debug strings): `FUN_80080f6c`=**DrawSync** (table+0x3c),
`FUN_80081560`=**DrawOTag** (table+0x18, the OT draw kick → our gpu_dma2_linked_list), `FUN_800815d0`=
**PutDrawEnv** (builds the 0x40-byte env packet at struct+0x1c via FUN_80081fb0, sends via table+8),
`FUN_8008179c`=**PutDispEnv** (GP1 display cmds via table+0x10), `FUN_80081690`=DrawOTagEnv,
`FUN_80081458`=ClearOTagR (table+0x2c), `FUN_800810f0`=ClearImage, `FUN_80081180`=ClearImage2 (fill bit
0x80000000), `FUN_80081218`=LoadImage (CPU→VRAM), `FUN_80081278`=StoreImage, `FUN_800812d8`=MoveImage.
- **60fps lever (native, real):** `DAT_1f800235` is the vblank-count target the loop waits for (1 vs 2).
  Our native loop owns this wait → drive at 60 by gating on 1 and interpolating object transforms (the
  entity-list snapshot, Phase-1). Not a renderer trick.
- **Widescreen lever (native, real):** PutDrawEnv (FUN_800815d0, ctx+0x2014 — draw-area clip+offset) +
  PutDispEnv (FUN_8008179c, ctx+0x2000 — display area) + the GTE OFX (now native, ov_set_geom_offset).
  Widen all three → genuine wider FOV. (Supersedes the rejected FB re-center.)
- **MoveImage (FUN_800812d8) = VRAM→VRAM copy** — the likely water-reflection / fade-buffer mechanism;
  prime suspect for the broken water (reflection copy) AND a place our GP0 emulator can drift. RE next.

### Render-buffer memory map — the packet pool + OT extent (later-125; the overflow threshold)
RE'd to find the fixed-buffer overflow threshold behind the widescreen corruption (later-124 mechanism #2).
All double-buffered by `parity = DAT_1f800135` (0/1), swapped each frame in native_boot.c. Addresses are
guest-RAM (`0x800…`); each value confirmed empirically by the `PSXPORT_DEBUG=pool` probe (OT roots = the
DrawOTag madr) at the field scene:
```
packet pool parity0   [0x800BFE68, 0x800D3E68)   0x14000 B   base = 0x800BFE68 (DAT_800bf544 reset)
packet pool parity1   [0x800D3E68, 0x800E7E68)   0x14000 B   stride 0x14000   <-- pool buffers END 0x800E7E68
gap / globals         [0x800E7E68, 0x800E80A8)   0x240 B     (incl. DAT_800e809c dwell counter @0x800E809C)
ctx parity0           [0x800E80A8, 0x800EA118)   0x2070 B    = &DAT_800e80a8 + 0*0x2070
  -> OT array         [0x800E80A8, 0x800EA0A8)   0x2000 B    2048 entries; DrawOTag root (head) @0x800EA0A4
  -> DISP/DRAW env    [0x800EA0A8, 0x800EA118)               +0x2000 DISPENV, +0x2014 DRAWENV, setup-OT @+0x2030
ctx parity1           [0x800EA118, 0x800EC188)   0x2070 B    OT head @0x800EC114, setup-OT node @0x800EC148
```
- The packet pool is a per-frame **bump allocator** (write ptr `DAT_800bf544`), shared by the engine's
  geometry submit AND the inline `AddPrim` 2D path. It holds the primitive packets (40 B GT3 / 52 B GT4)
  + the 1-word OT link tags. The OT array (2048 words) holds only bucket heads and does NOT grow per-prim.
- **Overflow threshold:** parity1's pool overflowing past `0x800E7E68` lands (after the 0x240 gap) in
  **ctx parity0's OT at 0x800E80A8** — i.e. a pool overflow corrupts the *alternate* frame's ordering
  table (classic double-buffer cross-corruption → garbage / flicker), not just its own packets.
- **Measured pressure (field scene):** 4:3 used ~31.5 KB/buffer (hi `0x000DB9B0`), 16:9 ~44 KB (the wide
  frustum's `ov_object_cull` re-include = +260 prims / +12.7 KB). 46% headroom at this scene, but a denser
  scene closes it. **later-125 fix:** native-DL owned prims now consume only their 1-word link tag (4 B,
  not 40/52 B) → 4:3 pool drops to ~9.8 KB (`0x000D649C`, −69%), removing the overflow pressure entirely
  while staying byte-identical. See native_dl.h.

### Ported to native C so far (faithful-first, oracle-gated)
- **GTE projection setters** (later-99): `ov_set_geom_offset` (0x800846D0 SetGeomOffset) + `ov_set_geom_screen`
  (0x800846F0 SetGeomScreen) in engine/game_tomba2.c. Native writes CR24=OFX<<16, CR25=OFY<<16, CR26=H.
  VERIFIED byte-identical: logs OFX=160/OFY=120/H=350, and the rendered frame is **0-pixel-diff** vs the
  recomp body (PSXPORT_GEOM_RECOMP=1 A/B). The engine's projection config is now PC-native — the widescreen
  FOV lever (widen OFX + draw-env clip) lives in our code. (Also owned earlier: LoadImage upload
  ov_upload_image 0x80081218; LZ/group asset codecs.)
- **DrawOTag** (later-99): `ov_draw_otag` (0x80081560) → native `gpu_dma2_linked_list` (walk OT, decode
  each primitive, rasterize). The engine's per-frame DRAW SUBMISSION now goes straight through our native
  renderer, not the DMA-register emulation. VERIFIED **0-pixel-diff** vs recomp (PSXPORT_OT_RECOMP=1) at
  f1500 (514-poly scene). Next: PutDrawEnv (FUN_800815d0) + PutDispEnv (FUN_8008179c) → full native screen
  geometry, then enable widescreen (wide OFX + wide clip + wide render target).
- **Native projection — RTPS/RTPT in native C** (later-100, plan atomic-riding-sparkle Phase 1): the GTE
  per-vertex projection (matrix·vertex → view-space IR1/2/3, depth SZ, integer screen SX/SY) is now
  reimplemented in native C in `runtime/recomp/gte_beetle.c` (`proj_native_vertex`, mirroring
  mednafen/psx/gte.c: MultiplyMatrixByVector_PT + UNR `Divide`/`CalcRecip`/DivTable + TransformXY clamps).
  It ALSO emits the FLOAT data the integer GP0 packet throws away — subpixel screen (precise_x/y) +
  view-space pos + depth — which the renderer needs and the OT-reordered GP0 stream can't carry. **VERIFIED
  0-diff** vs Beetle's GTE on real gameplay (the field, `PSXPORT_AUTO_GAMEPLAY=1`): over 1.28M verts/frame,
  IR=0-diff, SZ=0-diff, SX/SY=0-diff, and our float screen rounds to the integer projection 100%. Probe:
  `PSXPORT_PROJPROBE=1` (read-only verifier, gameplay untouched — the real GTE still runs; this only
  cross-checks). **Gotcha (RE'd here):** Beetle's XY_FIFO push writes the new vertex into BOTH slots 2 & 3
  (`XY_FIFO(2)=XY_FIFO(3)` runs after `(3)=new`), so after an RTPT the 3 verts read back at DR12/DR13/DR14
  (DR15==DR14), NOT DR13-15; the Z_FIFO shifts cleanly so its verts are at DR17/18/19. This replaces the
  value-keyed PGXP-lite cache as the depth/subpixel source.
- **Projection+submit call sites pinned** (later-100b, probe `PSXPORT_DEBUG=rtpcaller` = RA histogram at each RTP
  + jal-target decode). The RTP-containing project-and-submit functions, by call frequency at the field:
  `0x80109C80` (4689, overlay) and `0x801099B4` (2025, overlay) DOMINATE; resident `0x8007FDB0` (1504, the
  triangle fn) + `0x8008007C` (880, the quad fn — projects the 4th vertex via RTPS, then the triangle via
  RTPT, so execution order ≠ packet order); `0x80027768` (16). So there are 5+ distinct projection routines
  and the two dominant ones live in **runtime-loaded overlay code (not statically disassemblable)** → owning
  each function's packet layout is impractical. BUT all of them write the projected SXY into the **shared
  primitive pool via the global `DAT_800bf544`** (decomp `FUN_8007fdb0`/`FUN_8008007c`: `DAT_800bf544[2/5/8]
  = getCopReg(2,0xc/0xd/0xe)` = XY_FIFO, then OT-link `*DAT_800bf544 = old|0x9000000` and advance). That base
  IS the OT node, i.e. == `s_cur_node` (masked) when DrawOTag later decodes the packet (gpu_native.c:1220).
  **DESIGN for attach-at-submission (function-agnostic, no layout/RE of the overlay fns needed):** at the
  `gte_op` RTP chokepoint read `P = mem_r32(0x800bf544)` (the packet being built; stable across a packet's
  multiple RTPs, advances when the packet is committed) and append the projected vertex's (int sx,sy + float
  screen/viewpos/depth) to a per-frame list keyed by `P & 0x1FFFFC`. In `gp0_exec` poly decode, for the
  packet at `s_cur_node`, match each packet vertex to its float by `(sx,sy)` among that packet's ≤4
  candidates — globally collision-free (packet address) and intra-packet collision-free (only 3-4 verts),
  handling the quad reorder and culled-prim pollution. Retires the value-keyed `pgxp_lookup` cache. Verify:
  per-vertex hit rate ~100% for 3D polys (a miss = a 2D/screen-space prim, the free 2D/3D class), and
  rendering 0-diff vs the current PGXP path. Then Phase 2 = PC-native render targets + real depth (view-Z).
- **Attach infrastructure BUILT + measured; capture point is the open piece** (later-100c, `PSXPORT_ATTACH`).
  Implemented in gte_beetle.c: `projprim_push/lookup/reset/has_node` (per-frame float store hashed by packet
  node) + `projprim_capture` (at `gte_op`, reuses the 0-diff `proj_native_vertex`), and a renderer-side
  verifier in gpu_native.c `gp0_exec` (per-poly-vtx hit/miss by `(s_cur_node, sx, sy)`, reset each present).
  **Finding:** capturing the node as `mem_r32(0x800bf544)` covers only the RESIDENT project-and-submit fns →
  **~30% poly-vtx hit rate, and ~95% of misses are NODE-ABSENT.** The dominant overlay projection fns write
  into the SAME primitive-buffer region (resident packets at ~phys 0xD3xxx, overlay packets at ~0xD9xxx) but
  via a DIFFERENT running pool-pointer global — so a single hardcoded pool pointer is the wrong (fragile,
  non-function-agnostic) capture point. **SOLVED (later-100d): capture by the SXY word's guest ADDRESS.** The
  projection fns store the packed XY_FIFO word to packet memory (resident via recomp stores, overlay via
  interp stores — BOTH funnel through `mem_w32`). Implemented (keyed by ADDRESS now, not node): at `gte_op`
  push each vertex's packed SXY + native float into a small pending ring (`s_pr`, 32 deep); `attach_store_hook`
  (called from `mem_w32` for stores into the prim-pool region phys 0xB0000–0xF0000, gated) match-and-consumes
  the pending ring oldest-first and records the float keyed by the store ADDRESS (`projprim_set`, last-write-
  wins so a culled-then-reused slot resolves to the surviving prim); the render side tracks each GP0 word's
  source guest address (`s_fifo_addr[]`, set from `s_gp0_src` in the `gpu_dma2_linked_list` OT walk) and
  `gp0_exec` looks the float up by each vertex word's address (`projprim_lookup`). **VERIFIED (PSXPORT_ATTACH,
  field):** poly-vtx hit **100%** of GTE-projected (3D) polys (f400/f600 = 0 miss); the only misses are
  op-2F axis-aligned textured UI quads = 2D screen-space prims with no projection — the FREE 2D/3D class (a
  miss with a valid OT addr = 2D; addr==0 = direct/FMV). Fully function/pool-agnostic (covers the dominant
  overlay projection fns), no global value-collision (the value-match is bounded to the tiny pending window;
  the RESULT is address-exact). The dormant value-keyed `pgxp_lookup` is now superseded. **NEXT: Phase 2** —
  feed `projprim_lookup`'s view-space Z into a PC-native render target + real depth buffer; route 3D (hit) vs
  2D (miss) prims into separate passes; composite.

### Native ownership plan (reimplement libgpu, keep recomp body as oracle via rec_set_override)
1. **IN PROGRESS — native classified display list (later-123).** The geometry submit fns now build a
   PC-native `NativePrim` (engine/native_dl.{h,c}: would-be packet words + per-vertex view-Z) instead of
   writing the GPU packet to guest RAM; they link only a zero-length ordering node into the guest OT.
   `gpu_dma2_linked_list` renders owned nodes from the native arena via gp0_exec, taking depth from the
   carried view-Z (no address bridge). DONE for POLY_GT3/GT4 + the byte-packed GT4 field emitter (the bulk
   of 3D world geometry); VERIFIED 4:3 + 16:9 byte-identical (PSXPORT_DL_GUESTPKT A/B). **Remaining for
   full DrawOTag ownership:** the sprite/tile/flat/line builders + env packets (the field's 389 rects + 11
   env still write guest packets via libgpu / inline AddPrim) — once those emit NativePrims too, the OT
   walk becomes a pure render of a fully-classified native scene graph and NO render data lives in guest
   RAM. The recomp libgpu stays callable for A/B diff (PSXPORT_SUBMIT_RECOMP).
2. Own **PutDrawEnv/PutDispEnv** + the GTE projection (FUN_800509B4) → native widescreen.
3. Own **MoveImage/LoadImage/StoreImage** (VRAM↔VRAM/CPU) → fixes reflection/copy effects (water, fades).
4. Own the **OT clear/build** + the entity-list walk (Phase-1) → native 60fps interpolation.

## Fades (intro cutscene "flashes full visibility on fade-in") — RE target
Symptom: during a story-cutscene fade-IN, one frame shows the scene at FULL brightness before the fade
takes over. Tomba2 has no GPU global brightness; a fade is either (a) the GTE color scalar (GPF, IR0 —
later-96: GPF scales baked vertex colors by a per-frame brightness/fade factor), or (b) a full-screen
semi-transparent TILE drawn over the scene. The flash = the fade factor/overlay is one frame late or
mis-initialized in our path. **To fix it properly we must own DrawOTag** (see plan #1): then we see the
fade primitive/var directly each frame and can compare its first-frame value vs the oracle. Tracked as
the concrete proof that we now understand the draw path.

## Scene accounting — native classified display list (OWNERSHIP step 1, later-99)
`PSXPORT_SCENEDUMP=N` (gpu_native.c `gpu_scene_dump`): a read-only walk of the same OT DrawOTag DMAs,
classifying every primitive (poly/rect/line/fill/VRAM-copy/upload/env) + tracking draw-area/offset, and
logging the categories where effects live (VRAM→VRAM copies, fills, large/semi overlays = fade tiles).
This is the port ACCOUNTING for every draw instead of blind GP0 rasterization. Findings (f1500 water/demo):
- **TWO DrawOTag passes per frame:** (1) a tiny setup OT — `FILL (0,0,0) 320x240 @ (0,256)` = back-buffer
  clear (y=256 is the parity-1 framebuffer); (2) the main OT = **514 polys + 389 rects + 11 env**, ~1 tiny
  (2x1) VRAM copy.
- **Water is ordinary TEXTURED GEOMETRY, not a framebuffer-reflection copy** (the only VRAM→VRAM copy is
  degenerate 2x1). REFUTES the earlier "reflection effect" hypothesis. The broken-water artifact the user
  saw was almost certainly the PGXP value-keyed vertex-smoothing trick tearing the water's dense textured
  grid — now disabled by default (bf69890). The water polys are among the 514; next attribute them by
  screen region + texpage/CLUT (extend SCENEDUMP with a region filter) to confirm vs the oracle.

## Issue root-causes (understood via the scene classifier + frame-stepping, later-99)
- **Broken water** — ROOT CAUSE: the PGXP value-keyed vertex-smoothing TRICK was tearing the water's
  dense textured grid (collisions snapped grid verts to wrong subpixel positions). Water is ordinary
  textured geometry (scene classifier: no reflection copy). FIX: PGXP default-off (bf69890). Not a game bug.
- **Intro story-cutscene "flashes full visibility on fade-in"** — the cutscene is the post-NewGame prologue
  (stage 0x8010637C; "Tomba jumps into the sea"). Frame-stepped the fade-in in the port's SW path: the
  panel (drawn as ~3 sprites/rects, NO full-screen overlay, NO fill) brightens as a **smooth monotonic
  ramp** (mean 0.006→0.32 over ~30f) — i.e. the engine fades by **ramping the sprites' modulation color**
  0→full, and it does so CORRECTLY. So the flash is NOT in the game's fade logic — it is renderer-side:
  the **VK present path** (1-frame-behind/batched present, or stale frame at the title→black→fade
  transition) or the **FMV→engine handoff** (./run.sh has FMV on; drive.py SW path with NO_FMV is clean).
  NEXT: reproduce on the VK path (PSXPORT_VK_SHOT across the prologue entry) to pin the exact present-path
  frame, then fix our presentation — a renderer bug we own, distinct from the engine.
  - **later: reproduced on VK + characterised (live drive, PSXPORT_AUTO_NEWGAME=2 + debug-server step).**
    The glitch is a 1-2 frame brightness OUTLIER in the dark fade-in: a single FULL-BRIGHT frame on the
    otherwise near-black ramp (caught once at displayed-mean 1→57→2 between black neighbours; also a
    black-then-bright pair in the user's run). **It is INTERMITTENT** — two full step-through passes
    showed a clean smooth ramp with no flash. It was DETERMINISTIC before the buffer-flip hack was
    removed (commit b1f029a) and became intermittent after → the locus is the **flip/present timing**,
    not the engine fade (SW ramps correctly). Working hypothesis: VK renders frame N's tee'd geometry at
    frame N+1's present (1 frame behind SW, which rasterizes during DrawOTag), and with the now-
    unconditional double-buffer flip there's a race in which buffer/modulation the swapchain scans out
    during the fast ramp → an occasional stale full-bright (pre-fade) or just-cleared (black) buffer.
    NEXT: catch it in-situ (the step-drive flags displayed>>buf0/buf256) and confirm whether the present
    sampled a buffer/region the current frame didn't draw; then make the VK present frame-accurate
    (present the SAME frame's geometry, not 1-behind) so swap + render stay in lockstep.
  - **Tooling for this (committed):** PSXPORT_AUTO_NEWGAME=2 boots straight to the prologue and auto-
    pauses; debug-server `pause`/`step`/`play` (also keyboard P / '.'), `vkshot`/`vkvram` (reliable while
    paused — the loop re-presents each tick), `swvkcap` + tools/swvk_diff.py + tools/perceptual.py.
  - **later: PINNED the symptom precisely (it is DETERMINISTIC on the first pass through the prologue
    fade, NOT a race — the earlier "intermittent" reading was stepping a stale frame range).** At the
    flash the display origin is (0,256) and **framebuffer (0,256) holds the cutscene scene at FULL
    brightness (mean ~57)** while framebuffer (0,0) holds the dim fade (~3); the next frame the display
    origin flips to (0,0) (dim). So the two double-buffers diverge wildly DURING the fade — one carries a
    full-bright render of the scene (looks like the post-fade image: "Tomba is living peacefully…"), the
    other the early-fade dim render. Confirmed `disp=(x,y)` via the debug-server `frame` command + per-
    buffer `vkvram`. The user confirms the SW renderer fades smoothly, so this is **VK-specific: one
    double-buffer retains / receives a full-bright render of the scene that isn't re-dimmed in lockstep
    with the other.** RULED OUT (tested, no effect on the flash): the prims>0 flip hack; moving the
    present after DrawOTag (1-frame batch offset); a synchronous present (vertex-buffer reuse race).
    NEXT (decisive, not yet done): drive the SW renderer (PSXPORT_SW_GPU=1) to the same prologue frame
    and read framebuffer (0,256) — if SW has it dim there, the bug is VK redrawing/retaining that buffer
    full-bright (likely the persistent s_tex + how the cutscene's per-buffer redraw maps to VK's
    upload/tee path); compare the two renderers' per-buffer brightness across the fade.
  - **later2: SW renderer CONFIRMED smooth (PSXPORT_SW_GPU=1, same drive): no brightness spike, ramp
    0→62; VK spikes to ~57 at the same frame. So definitively VK-specific.** Causes RULED OUT by A/B
    (flash unchanged): flip hack; present-after-DrawOTag (no 1-frame batch offset); synchronous present
    (vertex-buffer reuse race); the OT-order DEPTH ordering (PSXPORT_VK_NODEPTH=1 — flash persists).
    Remaining mechanism: VK's persistent s_tex framebuffer (0,256) carries a FULL-BRIGHT render of the
    cutscene scene during the dim early fade, while SW's (0,256) is dim — i.e. VK populates/retains that
    buffer differently from SW for the same GP0 (the geometry is tee'd to VK and rendered into s_tex; the
    background/clear is uploaded from s_vram). NEXT: at the flash frame, dump SW s_vram (0,256) AND VK
    s_tex (0,256) for the SAME captured GP0 (extend swvkcap to also raw-dump s_vram) and diff which
    primitives/region differ; check whether the cutscene actually redraws (0,256) every frame or leaves
    it stale (then VK retains the old full-bright render where SW's last redraw was dim).
  - **SOLVED (2026-06-17): ROOT CAUSE = VK lost OT-order accumulation of OVERLAPPING semi-transparent
    prims.** The fade is NOT GPF/modulation — the cutscene scene is drawn at FULL vertex colour every
    frame and a full-screen SUBTRACTIVE semi tile (blend mode 2, `scene − fg`) darkens it; `fg` ramps
    248→123 so the scene fades IN from black. The flash is the ONE handoff frame between the panel
    fade-OUT and the scene fade-IN, where TWO stacked full-screen subtractive tiles are emitted
    (`PSXPORT_SEMIDUMP`: `blend=2 col=(255) fullscreen` then `blend=2 col=(8) fullscreen`). SW
    rasterizes them sequentially → `scene−255−8 = black`. VK drew ALL semi prims in ONE pass sampling a
    single PRE-semi framebuffer snapshot, so the two tiles did NOT accumulate — each subtracted from the
    same bright scene and the later (−8) tile OVERWROTE the (−255) tile → `scene−8 ≈ full bright` → a
    one-frame flash on the buffer it drew (displayed the next frame at disp=(0,256)). Diagnosed with two
    new env-gated, renderer-independent probes in gpu_native.c: **`PSXPORT_FADEDBG="a:b"`** (per-frame
    max prim colour, semi colour range, bigsemi count, disp/draw origin) and **`PSXPORT_SEMIDUMP=frame`**
    (each semi prim's blend mode + colour + bbox). The GP0 colour ramp is identical under SW and VK, so
    these settle "engine vs renderer" without VK. **FIX (gpu_gpu.c + gpu_native.c):** partition the semi
    batch into overlap GROUPS (`gpu_gpu_semi_group(bbox)` per semi prim; a prim that overlaps the current
    group's accumulated bbox starts a new group) and draw each group with its OWN fresh framebuffer
    snapshot, so a later group blends against earlier groups' results — exactly the sequential PSX/SW
    order. Non-overlapping semis (e.g. water tiles) stay ONE group → no extra cost. **VERIFIED:** VK fade
    now ramps smoothly f345→f400 (mean 0→12.7) tracking SW frame-for-frame, no spike (was ~57 at the
    flash frame); residual VK/SW drift ~10% from f382 on is the known dither/subpixel-rounding residual,
    not the flash.

## In-game pause / Options menu — state machine (RESOLVED later-112; replaced by our menu)
The in-game **pause menu** ("Options / Load data / Quit game") runs as a **task in the GAME overlay**
(0x80108xxx), which is why the main MAIN.EXE decomp (`ram_f1000_all.c`) has no caller for the draw
functions — they're reached only from overlay code + a jump table. Layout:
- **Task body / page dispatcher** `0x8010810C`: reads the **page byte `task+0x6B`** (task ptr =
  `*(u32)0x1F800138`, the scheduler's current-task pointer), bounds-checks `< 0xC`, then jumps through a
  **12-entry function-pointer table at `0x801062EC`** (`handler = table[page]`).
  | page | handler | role |
  |---|---|---|
  | 1 | `0x8010829C`→`FUN_8007eae4` | **main pause menu** draw "Options / Load data / Quit game" |
  | 2 | `0x801084A4` | close (resets `task+0x6B=0`, clears `DAT_1f800136`) |
  | 3 | `0x801082C0`→**`FUN_8007b45c`** | **the Options submenu controller** (the menu we replace) |
- **Main-menu handler** (page 1) reads button **edges `DAT_800e7e68`** (u16, active-high; Up=0x10,
  Down=0x40 move cursor `DAT_800bf808`; **Cross=0x4000** confirms). On Cross over "Options" (cursor 0)
  it sets `task+0x6B = cursor+3 = 3` → next frame the dispatcher enters the Options controller.
- **Options controller `FUN_8007b45c`** (0x8007b45c, NOT in the decomp dump — disasm via
  `tools/recomp/decode.py`): draws "Select Options" (`FUN_8007f104`: **Messages / Sound / Screen adjust /
  Controls** — the options the user deemed not worth keeping), dispatches its own sub-screens by
  `task+0x50` (table `0x80016E78`, 5 entries). Exits: **Triangle (0x1000)** → `task+0x6B=2` (close);
  **Circle (0x2000)** in the list → `task+0x6B=1` (back to the pause menu), resets cursor, sets
  `DAT_1f800136=1`. Menu SFX = `FUN_80074590(id, a1, 0)` (Circle: `0x14,0xFFF7`; Triangle/back: `0x11,0`).
- **Replacement** (later-112, `engine/game_tomba2.c` `ov_options_menu`, gated `PSXPORT_UI`):
  `rec_set_override(0x8007B45C, …)` shows our ImGui overlay instead of the game's options and owns the
  same Circle→page1 / Triangle→page2 back-nav + SFX. Faithful fallback: if the overlay isn't up
  (headless/window-less) it super-calls the real `FUN_8007b45c`. Verified the hook is reached
  (`PSXPORT_DEBUG=ui` + AUTO_GAMEPLAY → forced Cross at the auto-appearing menu logs the hit).
- **Other menus nearby** (same draw lib, separate flows): `FUN_8007ee74` "Continue / Load data / Quit
  game" (death/continue), `FUN_8007ed5c` Save prompt, `FUN_8007ef60` "OK to quit game?".

## Engine ownership status → `docs/engine-ownership-audit.md`
The actionable map of what the native engine OWNS vs. what still runs **on the interpreter** (the "black
box"), why widescreen corrupts (clip+cull+2D layers stay 4:3 while we fake the FB wide), and the prioritized,
oracle-gated port plan to own the render/camera/submit layer for REAL widescreen. **Read it before render/
camera/widescreen work.** (Gameplay logic stays interpreted — the Beetle emulator is the oracle, by design.)

## Open RE items (next, in order)
1. ~~The entity list + its walk~~ — **DONE** (above): lists `DAT_800fb168`/`DAT_800f2624`, walk
   `FUN_8007a904`, node layout. Handlers are per-object fn pointers @ +0x1c (not a type-indexed table).
2. Find `FUN_8007a904`'s caller (frame placement) — it has no static caller (called via task/overlay).
   Confirm at runtime (objlog / a tap) that it's the per-logic-frame object driver.
3. Object node full lifecycle: spawn/link (`:55976–56167`) and free; the pool base/extent.
4. Camera basis + GTE projection setup (for native render submission, Phase 3).
5. Whether handlers call the cull/submit path (`FUN_8007712c`) in field scenes, or a different render
   path (the journal noted the cull dispatcher didn't fire in the demo). `PSXPORT_DEBUG=obj` answers this.
6. ~~Projection setup~~ — **DONE** (later-98): `gen_func_800509B4`, OFX/OFY/H = 160/120/350. Widescreen
   lever identified. Remaining: the draw-environment/clip-rect width setup (to widen the clip with OFX).
7. **Water draw path** (broken in port): NOT GTE-projected (separate layer). Identify via provenance +
   dual-core state-synced diff. The immediate user-visible regression.

## Dual-core differential harness (planned tooling — sync on GAME STATE, not frame number)
Per user direction: run the port and the Beetle oracle (`runtime/wide60rt`) and compare at the SAME game
state (e.g. attract/demo start), because their boot timings differ so frame numbers don't align. Sync via
an observable guest-RAM latch — e.g. the stage var `native_boot` prints as `stage=DEMO 0x801062E4`, or the
documented scene latch `0x800BE258==2`. Gate both cores to that latch, then diff GP0 stream / VRAM /
framebuffer there. Extends the existing `tools/drive.py` + `docs/diff-driver.md`.

## Move-and-collide (per-object physics) — RE TRACE + ownership analysis (2026-06-21)
Goal was to own a clean PC-native "pos += vel; gravity; query grid; clamp" integrator as `engine/
physics.cpp`. **Finding: Tomba!2 has NO single shared velocity+gravity integrator.** The move-and-collide
layer is a *family of angle+speed grid-SLIDE probes*, and the velocity→displacement + gravity step lives,
per-object, inside the state-machine handlers — there is no shared routine to lift cleanly.

### The owned collision-GRID query family (already native — `engine/collision.cpp`)
`FUN_80049968` grid row-pointer setup · `FUN_80047CBC` cell query/neighbor-walk · `FUN_800498C8` resolve
loop · `FUN_8004798C` per-step origin/index · `FUN_80031780` list-tail. All scratchpad-only (table base
`0x1F8001C8`, probe coords `0x1F8001BC/BE/C0`, result tag `0x1F8001A6`, cell cursors `0x1F8001E0/E8/EC`).
These are the GRID; the response/integration layer is the CONSUMER below.

`FUN_80048360` **cell-relative OFFSET transform** (`ov_grid_offset_48360`, `engine/grid_offset.cpp`) —
a hot grid LEAF (~5% of field interp time across its internal jump-table entries 0x80048360/410/594/630,
~14400 calls/run; chosen via the §F profiler this session). No args; returns `rec[7]` (slope lo byte) in
v0. Scratchpad-ONLY, no GTE, no render, **no callees**. Maps the probe into a cell-local 6-bit offset and
applies the cell tag's orientation, then re-accumulates onto the working coords:
- dx = `(sh[0x1BC]-sh[0x1AA]) & 0x3F` → `sh[0x1C2]`; dz = `(sh[0x1C0]-sh[0x1AC]) & 0x3F` → `sh[0x1C6]`.
- TAG = `rec[0]` (rec ptr = `w[0x1E0]`, latched by the query). **TAG&3** drives a pre-mirror (`^0x3F`),
  a post sign-negate, and a post re-mirror of (dx,dz) by quadrant (0:both / 1:dz·dx / 2:dx·dz / 3:none-mirror
  vs all-negate — the negate is the bitwise complement of the mirror map). **TAG&4** transposes dx/dz
  (applied before AND after the slope step). **TAG&8** selects the slope op on `rec[6]` (hi=a2, lo=a3):
  set → sheared `dz -= a3 + (((a2-a3)*dx)>>6)`; clear → divided `dz -= ((dx-a3)*a2)/(a3^0x3F)` (MIPS
  signed mult-lo + truncating div; the divisor is non-zero in practice). dx is zeroed across the slope.
- Tail: `sh[0x1BC]+=dx, sh[0x1C0]+=dz`; `sh[0x1C2]=(origDx+dx)`, `sh[0x1C6]=(origDz+dz)` then re-mirror/
  re-swap those by TAG&3 / TAG&4.
GOTCHAs: the TAG&3 quadrant map differs between the pre-mirror (q1→dz, q2→dx, q3→both) and the
sign-negate (q0→both, q1→dx, q2→dz, q3→none); the slope products are MIPS signed (low word); `origDx/origDz`
(t4/t5) are the PRE-transform offsets captured at entry. Verified via the `gridoffset` channel = full
main-RAM + scratchpad + v0 A/B vs `rec_super_call` (exact 0-diff, no stack-window exclusion since there are
no dispatched callees): **8000+ live field calls 0-diff** (press right 250 + left 250, all tag branches
exercised). Registered in game_tomba2.cpp.

### The move-and-collide RESPONSE family (consumers of grid_resolve `FUN_800498C8`)
Discovered by scanning `jal 0x800498C8` and their callers. All take a **displacement/speed + angle**, NOT a
velocity field, and derive the world step from the object's **angle field `+0x56`** via the engine's
fixed-point trig:
- `FUN_80046A44` — the canonical/most-general probe (callers: `FUN_80024448`, `FUN_8005D530`,
  `FUN_80062D8C`). Sig `(obj a0, speed a1, ystep a2, maxiter a3)`. Loops up to `a3` iterations: step =
  `rsin/rcos(angle 0x56)*speed >>12`; writes probe pos to scratch `0x1BC/1BE/1C0`; calls grid_resolve
  `0x800498C8`; on hit computes slide direction with **atan2 `FUN_80085690`**, picks a slide axis (tags
  0x4/0x8), sub-steps via helper `FUN_8004602C` (itself a grid walk over `0x1F8001E0`), then writes the
  resolved position back to guest **`+0x2E` (X), `+0x36` (Z)**, and `+0x32` (Y) on the floor-snap branch
  (`0x1F8001A6 & 0x400` → add `0x1F8001C4`). Returns `(tag>>9)&3`. Callees: `0x80083E80` rsin,
  `0x80083F50` rcos, `0x80085690` atan2, `0x800498C8` grid_resolve, `0x8004602C` slide helper.
- `FUN_8004766C` — resolve-in-place (15 callers, e.g. `0x8003FBC4`, `0x80041194`, `0x80055D5C`,
  `0x80069860`, `0x80080424`). Loads obj pos `+0x2E/+0x32/+0x36` → scratch, runs grid_setup(`+0x2A`)+
  grid_query, follows grid links updating layer field `+0x2A`, then `FUN_80048134`/`FUN_80048034`
  (sub-cell offset + link walk, scratchpad-only) and writes resolved pos back to `+0x2E/+0x36`. No velocity;
  pure snap-to-grid of the CURRENT position.
- Directional probes `FUN_800455C0`/`80045B30`/`80045CAC`/`800468AC`/`80046E2C`/`80047468`/`800492B0`/
  `80049418` (+ thin wrappers `FUN_80049250/80049280/8004951C/…` that pass `obj, dx, dy, angle 0x56`).
  Each tests one direction and snaps a single axis (`+0x32` Y for floor, `+0x2E/+0x36` for walls).

### Object physics fields (guest node)
Position: `+0x2E` X, `+0x32` Y, `+0x36` Z (all `sh`, fixed). Heading/angle: `+0x56` (12-bit, period 4096).
Velocity (read only in the SM handlers, e.g. `FUN_80024448`): `+0x66` (lh, X-vel), `+0x68` (lhu, Y-vel),
`+0x6A/+0x6C` seen as related. Floor/grid layer: `+0x2A`. Floor-type/state out: `+0x17D`, `+0x164`. The
integration `pos += f(vel); gravity` is done by each SM case *before* calling a probe — e.g. `FUN_80024448`
reads `+0x66/+0x68`, negates Y, picks dir from `+0x17E`, then calls `FUN_80046A44`.

### Engine fixed-point trig (used by the probes — keep dispatched)
`FUN_80083E80` = rsin, `FUN_80083F50` = rcos → core `FUN_80083EBC`: 4096-entry sine table at **`0x800A5AF0`**
with quadrant folding, 12-bit angle. `FUN_80085690` = fixed-point atan2. These are the angle primitives;
the probes' position output is byte-derived from them.

### Why physics.cpp was NOT shipped (per the "can't cleanly isolate → return the trace" clause)
The move-and-collide is intrinsically the iterative grid-SLIDE algorithm in `FUN_80046A44`, whose guest
position bytes (`+0x2E/+0x32/+0x36`) the still-recomp AI reads back. Reproducing those bytes EXACTLY (the
content-interface gate: full RAM + scratchpad 0-diff) requires reproducing the exact `>>12` fixed-point
rsin/rcos-table math, the atan2 slide pick, and the sub-step helper `FUN_8004602C`'s grid walk — i.e. a
MIPS-faithful transcription, which the methodology explicitly rejects as the deliverable ("a native function
that merely reproduces the PSX body byte-for-byte is still PSX-simulation"). There is no clean `pos+=vel;
gravity` integrator to lift, because that part is per-SM, not shared. A correct native ownership here means
owning a per-object STATE MACHINE end-to-end (its velocity/gravity update AND its probe call), which is a
different, larger unit than "a physics integration step" — to be scoped as an SM-handler port, not a shared
physics module. Recommended next target: own one concrete actor SM (e.g. `FUN_80024448`'s owner) including
its `+0x66/+0x68` velocity update + gravity + the `FUN_80046A44` call, gating on RAM+scratchpad 0-diff.

### OWNED — `FUN_80024448` actor move-and-collide SM step (PC-native, `engine/actor_sm_24448.cpp`)
Owned 2026-06-21. The control flow + branch decisions + guest field writes are PC-native; the three
fixed-point LEAVES it calls stay `rec_dispatch`ed (they compute exact `>>12` results the still-recomp AI/
render read back — transcribing them would be PSX-simulation). `sm24448verify` gate = full main-RAM +
scratchpad + v0 A/B vs `rec_super_call(0x80024448)`, **650+ invocations, 0 mismatches, 0 bad-opcode** (driven
field + walk; the SM fires as the FALLBACK move-collide path of caller `FUN_80024548`, see below).
- Body: read `+0x17E` (mode, signed) → probe `maxiter` = 37 (`<0`) else 74; read X-vel `+0x66` (lh→speed)
  and Y-vel `+0x68` (lhu, negated+sign-ext→ystep); clear `+0x17D`; call probe `FUN_80046A44(obj,xvel,
  -yvel,maxiter)` → tag `s1`. If `s1==0` return 0 (miss). On HIT: `+0x17D = (scratch 0x1F8001A6>>11)&3`
  (floor-type); call slide-finalize `FUN_80048654(obj)`; read resolved heading `scratch 0x1F8001A0` → store
  raw to `+0x140`, and to angle `+0x56` (or `(head-0x800)&0xFFF` when flip-flag `+0x147!=0`). Tail: if
  `s1==2 && (+0x17D&1)` → `+0x164=7` + sub-step `FUN_80024AF0(obj)`, else `+0x164=4`; store `+0x15C=s1`;
  clear `scratch 0x1F800084`; return 1.
- FIELD WRITES owned: `+0x17D`(sb floor-type), `+0x140`(sh heading), `+0x56`(sh angle), `+0x164`(sb state-
  out), `+0x15C`(sb tag), `scratch 0x1F800084`(sw 0). Leaves dispatched: `FUN_80046A44` probe,
  `FUN_80048654` slide-finalize (atan2/sqrt; writes obj `+0x48/4A/4C` + scratch `0x1A0/0x1A2`),
  `FUN_80024AF0` floor sub-step (rsin/rcos `0x80083E80/0x80083F50`).
- CALLER: sole `jal` from `FUN_80024548` @0x80024760 — only reached when scratch `0x1F800080==0` AND the
  per-mode handler (`jalr` via table `0x800AD22C[*0x800BF870]`) returned 0. So `FUN_80024448` is the
  generic move-collide FALLBACK of `FUN_80024548` (itself called from actor updates `FUN_80057A68`,
  `FUN_8005D530`). No jump-table indirection into `FUN_80024448` itself.
