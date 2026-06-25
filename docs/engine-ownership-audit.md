# Engine ownership audit — what the native engine owns vs. what still runs on the interpreter

Written 2026-06-18 (later-114) in response to: "the game becomes completely corrupted [in widescreen],
which tells me most of the game engine is still a black box — port the render/camera layer."

**Architecture (do not get this wrong — later-103):** the port is **interpreter-only**. The real MAIN.EXE
+ runtime overlays are loaded into guest RAM and executed by the **flat interpreter** (`interp.c`
`interp_flat`; `rec_coro_run`/`rec_interp` are thin wrappers). The static recompiler (`tools/recomp/emit.py`
→ `generated/`) is an **OFFLINE analysis aid only** (`PSXPORT_RECOMP=1`) — it is NOT linked into the runtime
and NOT in the execution path. So everything that isn't a PC-native override **runs on the interpreter**, not
recompiled code. "Owning" a function = replacing its interpreted execution with native C via
`rec_set_override`. The **verification ORACLE is the Beetle emulator** (`runtime/wide60rt`), not recomp.

**Direction (user-confirmed, later-115): FULL ownership of the engine layer — NO faking.** Own the entire
**render + camera + submit + loop pipeline** natively, and **retire every renderer-side fake**: the
FB-widescreen hack (`push_wide` / `WIDE_OFF` / the wide scratch FB) and the sprite-anchor FB hack
(`gpu_gpu_sprite_anchor_dx`) all go away once the engine itself produces a genuinely wider frame. Widescreen
and PC effects must come from a real wider frustum, not a post-stretch. **Gameplay logic (per-entity AI,
physics, rules) STAYS running on the interpreter (the real PSX code in guest RAM)** — it is our bit-exact
oracle, not a black box; reimplementing it by hand would be enormous and throw away what proves each native
piece correct. This audit maps what is owned, what is NOT, why widescreen corrupts, and the order to close
the gaps. **Read `docs/engine_re.md` first** — this is its actionable index, not a re-derivation.

## How "ownership" works (so the plan is concrete)
Every render call the game makes (executing on our interpreter) goes through **Sony libgpu**, dispatched via
a fn-ptr table at **`0x800A5998`** (the "GPU sys" struct). We take a function over with
`rec_set_override(addr, native_fn)`; `rec_super_call` re-runs the ORIGINAL function on the interpreter, so it
stays callable for A/B comparison. A function is "owned" only when its native version is **0-pixel / 0-state
diff vs. the original (interpreted) path AND the Beetle oracle on real gameplay**. The legacy-named
`PSXPORT_*_RECOMP=1` flags just **disable the native override** (run the original on the interpreter) for
that A/B. Verify with the oracle, never by eye — `docs/gfx-debug.md`.

---

## A. OWNED today (native C, oracle-verified) — `engine/game_tomba2.c` overrides
| addr | libgpu / engine fn | native override | verified |
|---|---|---|---|
| `0x800788AC` | per-frame fence (input + sub-tick) | `ov_frame_update` | yes (drives audio/present) |
| `0x80081560` | **DrawOTag** (the per-frame draw kick) | `ov_draw_otag`→`gpu_dma2_linked_list` | 0-diff @f1500 (514 polys) |
| `0x800846D0` | SetGeomOffset (OFX/OFY) | `ov_set_geom_offset` | 0-diff (OFX160/OFY120) |
| `0x800846F0` | SetGeomScreen (H/FOV) | `ov_set_geom_screen` | 0-diff (H=350) |
| `0x80081218` | LoadImage (CPU→VRAM upload) | `ov_upload_image` | yes |
| `0x80044D8C`/`0x80044E84` | LZ decompress / group unpack | `ov_lz_decompress`/`ov_unpack_group` | yes |
| `0x8007FDB0`/`0x8008007C`/`0x80027768` | POLY_GT3/GT4/GT4-bp **submit (resident)** | `ov_submit_poly_*` + autodetect | native submit path |
| `0x8007712C` | per-object cull/LOD dispatcher | `ov_object_cull` (gated) | tap only (fps60/objlog) |
| (GTE) | **RTPS/RTPT per-vertex projection** | `proj_native_vertex` (gte_beetle.c) | **0-diff, 1.28M v/frame** |
| (GTE store) | per-vertex float depth attach | `projprim_*` by SXY address | **100% hit on 3D polys** |
| `0x8007B45C` | in-game Options menu | `ov_options_menu` (this session) | reached + overlay |

So we own: the draw KICK, the projection MATH (+ true per-vertex depth, function-agnostic), the resident
submit emitters, asset upload, and the frame fence. **The projection numbers and depth are solved.**

---

## B. NOT OWNED — still running on the interpreter (the black box), by subsystem
These functions still execute as the original PSX code on the flat interpreter (no native override yet).
Ordered by how directly each blocks faithful widescreen/effects.

### B1. Screen geometry: PutDrawEnv + PutDispEnv  ← THE widescreen blocker
- **`0x800815D0` PutDrawEnv** (draw-area CLIP rect + draw offset) and **`0x8008179C` PutDispEnv** (display
  area, GP1) still run on the interpreter. They set the **320-wide clip**. Our current "widescreen" never widens this —
  it renders the native 4:3 output into a wider scratch FB and spreads vertices in a shader. That is the
  hack. Owning these two (+ widening the clip to match a wider OFX) makes the GPU itself accept a wider frame.

### B2. Projection CONFIG: gen_func_800509B4
- **`0x800509B4`** is the engine's projection setup; it CALLS the SetGeomOffset/Screen leaves we own, but we
  do not override IT. True widescreen = override it to set **OFX=214 (16:9) / wider** and keep OFY/H, so the
  owned `proj_native_vertex` projects across the wider screen. (FOV math already proven; this is the lever.)

### B3. Frustum CULL (the corruption cause for "missing/garbage sides")
- The game **culls geometry to its 4:3 frustum** (cull dispatcher `0x8007712C`, camera fwd vector
  `_DAT_1f8000e8/ea/ec`, NCLIP backface). When we force the image wider, geometry the game already culled
  on the sides simply isn't submitted → empty/garbled side bands. Owning the cull (widen the frustum test to
  the target aspect) is what makes a wide view show *real* extra world instead of garbage.

### B4. Screen-space 2D layers: water / sky backdrop + HUD sprites
- Water + sky are **NOT GTE-projected** — fixed screen-space layers (`engine_re.md` "Water + sky"). HUD/UI
  sprites (GP0 `0x60-0x7F`) likewise bypass the GTE. In widescreen these must be widened/edge-anchored at the
  SOURCE (their submit), not via the FB shader hack `gpu_gpu_sprite_anchor_dx`. Currently interpreted + FB-hack.

### B5. The dominant projection-and-submit OVERLAY emitters
- `0x80109C80` (4689/frame) and `0x801099B4` (2025/frame) are **runtime-loaded overlay code**, not statically
  disassemblable, and emit most field polys. We own their DEPTH function-agnostically (attach-by-address,
  100%), but the **packet build/submit still runs interpreted**. Fine for faithful render; matters if widescreen
  needs per-emitter awareness. Likely handled generically by B1–B3, not per-function.

### B6. VRAM copies + OT lifecycle (lower urgency)
- **MoveImage `0x800812D8`**, **StoreImage `0x80081278`**, **ClearImage `0x800810F0/180`**, **ClearOTagR
  `0x80081458`** (OT clear/build) — still interpreted. Own these for fade/copy effects and native OT building
  (also the path to clean 60fps interpolation). Not a widescreen blocker.

### B7. Camera basis / per-object transform load (Phase 3)
- The per-object rotation matrix → GTE CR0-7 before each RTP (96/54 static ctc2 sites) still runs interpreted. Owning
  it is the native entity-transform path (interpolation), not needed for widescreen.

---

## C. Why widescreen "completely corrupts" — root cause, stated plainly
It is **not** that gameplay logic is unknown. It is that we **fake** the wide view at the framebuffer while
the engine still renders, clips, and culls for 4:3:
1. **Clip stays 320** (B1) — the GPU was told to draw a 4:3 frame; we stretch it after.
2. **Frustum cull stays 4:3** (B3) — side geometry is never submitted → empty/garbled edges.
3. **2D water/sky/HUD are screen-space** (B4) — they don't follow the FB spread → misaligned bands.
The fix is to widen the view **at the source** (B2 OFX + B1 clip + B3 frustum + B4 2D anchoring), so the
game produces a genuine wide frame that our already-owned projection + depth render correctly. The renderer-
side spread (`push_wide`/`WIDE_OFF`/the aspect FB) becomes unnecessary once the source is wide. **(Before
any of this: reproduce + characterize the corruption with the render tooling, not eyeballing — see E.)**

---

## D. Prioritized port plan (highest leverage first; each gated + oracle-diffed)
1. **Diagnose first.** Reproduce the wide corruption headless/windowed; classify the garbled prims with
   `PSXPORT_SCENEDUMP` + `PSXPORT_PROVAT="x,y"` (which emitter, texpage/CLUT, is3d) and compare to the oracle
   at the SAME game-state. Confirm the B1/B3/B4 split empirically before writing the fix. **No assumptions.**
2. **Own PutDrawEnv + PutDispEnv (B1).** Native clip/offset/display packets; widen clip to target aspect.
   Verify 0-diff at 4:3 (`PSXPORT_ENV_RECOMP=1` A/B) before widening.
3. **Own gen_func_800509B4 (B2).** Override to drive OFX/clip from the chosen aspect (4:3/16:9/21:9/auto from
   `g_mods.aspect`). Verify 4:3 still 0-diff; then a wide frame projects via the owned `proj_native_vertex`.
4. **Own the frustum cull (B3).** Widen the horizontal frustum test to the aspect so side geometry is
   submitted. Verify 4:3 cull set == oracle (no over/under-draw), then wide shows real extra world.
5. **Own the 2D layers (B4).** Native water/sky/HUD submit with aspect-correct extents/anchoring (retire the
   FB-shader `WIDE_OFF`/`sprite_anchor_dx` hack once the source is wide).
6. **Then retire the FB widescreen hack** (`push_wide`/wide scratch FB) — keep only native internal-res scaling.
7. (Later, not widescreen) Own MoveImage/StoreImage/ClearImage/ClearOTagR (B6) for fades/copies + native OT;
   then camera-basis/per-object transform (B7) for native interpolation.

## E. Verification protocol (every step)
- **Faithful-first:** each new override keeps the original (interpreted) path callable via rec_super_call; a `PSXPORT_*_RECOMP=1` flag A/B-toggles it.
- **Gate = 0-diff vs oracle on real gameplay** (RAM/GP0/VRAM diff via `tools/drive.py` + Beetle; render-diff
  via `gpu_differ`), at 4:3 BEFORE enabling any widen. Widescreen "looks right" is never the gate.
- Default stays faithful (no mod forces a render change); widescreen is opt-in until it is 0-diff at 4:3 and
  artifact-free wide.
