# Issue #14 — spiky-ball HUD: 3 balls bottom-LEFT (16:9) vs 1 ball bottom-CENTER (vanilla) — RE

READ-ONLY RE, 2026-06-21. No source committed-modified (a throwaway probe was built, run, and reverted;
`git status` clean). Driven headless (`PSXPORT_VK_HEADLESS=1 PSXPORT_REPL=1`), `newgame; skip 650; run 60`,
village field scene (global frame `s_frame==720`), `PSXPORT_SETTINGS` toggling aspect 4:3 vs 16:9.

---

## TL;DR

- The bug is **WIDESCREEN-ONLY**. At **4:3 the HUD is correct: ONE spiky ball, bottom-centre**
  (`scratch/screenshots/iss14_43.png`). At **16:9 it is THREE balls shoved to the bottom-LEFT**
  (`scratch/screenshots/iss14_169.png`). Same game, same emitted prims — the difference is entirely our
  widescreen present path. This is the strongest possible signal that it is OUR 2D-widescreen handling,
  not the game emitting three.
- The HUD ball is drawn by **three GP0 op-`0x65` textured-rect sprites** (textured rect, var size), guest
  nodes **`0x800BFE74` / `0x800BFE94` / `0x800BFEB4`**, all at **y=200, 32×24**, native x = **112 / 144 /
  176**, all sampling the *identical* texel region (uv0=(48,152), texpage (384,0), CLUT (480,211)). They
  are emitted byte-identically at 4:3 AND 16:9 (confirmed by `PSXPORT_POLYDUMP=720` in both inis) — **the
  game does NOT change its emission with aspect; aspect is purely our post-process.**
- **There are TWO distinct widescreen defects stacked here, and the headline is mostly defect (1):**
  1. **WRONG POSITION (bottom-LEFT instead of bottom-CENTRE).** The scratch-FB relocation **vertex
     shader** subtracts the PSX **drawing-area origin `i_da.x0` (= `s_da_x0` = 144 this frame)** from the
     2D HUD sprite X, then the wide path only adds back `margin` (54 px). Net the balls land at FB x =
     `(native - 144) + 54` = **22 / 54 / 86** → hard against the bottom-left. **This is NOT the
     `ws_2d_anchor_off` thirds logic** — those sprites correctly classify as the CENTRE third
     (anchor offset = 0, verified live); the displacement is the `i_da.x0` subtraction in the FB shader.
  2. **THREE balls instead of ONE.** Independently, the three sprites that **composite into a single
     ball at 4:3** stay **three separate balls in the scratch-FB path** even once re-centred (verified by
     a probe build). The 4:3-native path shows only the middle sprite's ball; the FB path renders all
     three. Root cause not fully closed (see "Open" below) but it is a render-ordering/overdraw
     difference between the native-VRAM present and the scratch-FB present, NOT extra emission.

So: **our anchor does not "triple" it — the game emits three sprites that on the real/4:3 path collapse to
one ball; our scratch-FB path both mis-positions them (left, via the `i_da.x0` subtraction) and fails to
collapse them (three balls).** Fix BOTH; do NOT hide two of the three.

---

## Live evidence

| Config | Shot | HUD result |
|--------|------|-----------|
| 4:3  (`aspect=0`) | `scratch/screenshots/iss14_43.png` (320×234) | **ONE ball, bottom-centre** (correct, matches vanilla) |
| 16:9 (`aspect=1`) | `scratch/screenshots/iss14_169.png` (428×240) | **THREE balls, bottom-LEFT** (the bug) |
| 16:9 + probe re-centre | `scratch/screenshots/iss14_169_testfix.png` | THREE balls, now bottom-CENTRE (defect 1 fixed, defect 2 remains) |

Cropped close-ups: `iss14_43_balls2.png` (one ball + chain), `iss14_169_bottom.png` (three balls left).
Reference issue images agree: `docs/reference/issues/issue14_three_spikyballs.png`,
`docs/reference/issues/issue13_water_ok.png` (both 16:9 runs → three balls bottom-left).

### Emitter (identical in 4:3 and 16:9), `s_frame==720`, `PSXPORT_POLYDUMP=720`
```
node=800BFE74 SPRITE op=65 tex=1 semi=0 clut=(480,211) tp=(384,0) at=(144,200) 32x24 uv0=(48,152) off=(0,0)
node=800BFE94 SPRITE op=65 tex=1 semi=0 clut=(480,211) tp=(384,0) at=(112,200) 32x24 uv0=(48,152) off=(0,0)
node=800BFEB4 SPRITE op=65 tex=1 semi=0 clut=(480,211) tp=(384,0) at=(176,200) 32x24 uv0=(48,152) off=(0,0)
```
Three distinct guest nodes (0x20 apart), 32 px stride, identical sampling.

### Final FB X (16:9), throwaway probe printed at the sprite emit (`gpu_native.cpp` sprite path):
```
op65 native_x=112 w=32 s_off_x=0 bg=0 X=112 XL=112 XR=144 da_x0=144 anchor=0 wide=428 had3d=1
op65 native_x=144 w=32 s_off_x=0 bg=0 X=144 XL=144 XR=176 da_x0=144 anchor=0 wide=428 had3d=1
op65 native_x=176 w=32 s_off_x=0 bg=0 X=176 XL=176 XR=208 da_x0=144 anchor=0 wide=428 had3d=1
```
- `anchor=0` for ALL three → `ws_2d_anchor_off` correctly puts them in the centre third (centres 128/160/192
  are all inside L=106..R=213). The thirds logic is NOT the cause.
- `da_x0=144`. The FB-relocation shader does `fx = (i_pos.x - i_da.x0)*ss + fb_x0`
  (`runtime/recomp/shaders_vk/tritex.vert:34-35`), with `fb_x0 = margin*ss = 54`
  (`runtime/recomp/gpu_gpu.cpp:807-808`, `push_wide`). So:
  - ball 112 → (112−144)+54 = **22**
  - ball 144 → (144−144)+54 = **54**
  - ball 176 → (176−144)+54 = **86**
  These exactly match the three detected ball clusters in the 16:9 shot (centres ≈ 38/70/102).

---

## Root cause

### Defect 1 — bottom-LEFT (the headline position bug)
`runtime/recomp/shaders_vk/tritex.vert:30-39` — the scratch-FB relocation branch (taken only when
`frame_via_fb()`, i.e. wide or ires>1) computes `local = i_pos - i_da.xy` and `fx = local.x*ss + fb_x0`.
This subtraction of the PSX **drawing-area top-left** (GP0 `E3`, `gpu_native.cpp:1126`,
`s_da_x0 = 144` this frame) is correct for 3D geometry (which is expressed relative to the active draw
buffer) but **WRONG for these 2D HUD sprites**, whose screen X (112..176, `s_off_x=0`) is *screen-space*,
not buffer-relative. Subtracting 144 and adding back only `margin=54` shifts the whole HUD strip left by
`144-54 = 90` px → bottom-left.

At **4:3** the shader takes the `else` branch (`tritex.vert:40-42`, `fx = i_pos.x`, no `i_da.x0`
subtraction) → the sprites stay at native 112/144/176 → correct centre placement. That asymmetry is the
entire 4:3-vs-16:9 position difference. (A throwaway probe that pre-added `s_da_x0` back to the HUD sprite
X in the wide path moved the balls from bottom-left to bottom-centre — confirming the subtraction is the
position bug. See `iss14_169_testfix.png`.)

### Defect 2 — THREE balls instead of ONE (secondary, not fully closed)
Even after re-centring (probe), the scratch-FB path renders three balls 32 px apart, whereas the 4:3
native-VRAM present shows only one (the middle sprite). The three sprites are byte-identical and 32 px
apart in BOTH paths, so the difference is in render ordering / overdraw between the two present paths:
- 4:3 (no FB): sprites raster into the live VRAM display buffer; only the middle ball survives to the
  presented region (the x=112 and x=176 sprites are not visible — overdrawn or clipped at raster). Result:
  one ball.
- 16:9 (scratch FB): the FB present preserves all three (the FB is filled by relocating tee'd content;
  the HUD sprites go RQ_HUD / RQ_OM_2D_FG ordered by submission). Result: three balls.
The exact reason the 4:3 path drops two of the three (later HUD/world overdraw vs FB ordering, or a
display-region clip at the buffer edge) is **not yet pinned** and needs a raw-VRAM read at 4:3 to confirm
whether two balls are physically absent from VRAM (overdrawn at raster) or merely outside the sampled
display window. This is the one open thread.

---

## Why the prior `issue6_12_re.md` ws_2d_anchor theory does NOT apply here
The #6 RE blamed `ws_2d_anchor_off` (gpu_native.cpp:121-129) splitting a multi-slice panel across
left/centre/right thirds. That is a real defect for *panels that straddle the third boundaries*, but the
spiky-ball sprites all sit in the CENTRE third (`anchor=0`, verified) — so the thirds logic does NOT move
them. The ball displacement is a *different* widescreen defect: the FB shader's `i_da.x0` subtraction.
Don't conflate the two.

---

## Fix location + approach (PC-native: HUD owns its own screen-space layout)

The engine owns its 2D layer; a HUD element must land at a screen-anchored position independent of the
PSX draw-buffer origin and independent of ires/wide scaling. Two cooperating fixes:

### Fix 1 (position) — do NOT subtract the PSX draw-area origin from 2D HUD sprites in the FB path
The `i_da.x0` subtraction in `tritex.vert:34` is correct for 3D (buffer-relative) and wrong for
screen-space 2D HUD. Options, in order of preference:
1. **Lay HUD out in engine screen-space, not PSX buffer-space.** When the HUD emitter is owned native,
   place the ball strip at a screen anchor (bottom-centre) in the engine's own 2D coordinate system and
   feed the FB present a coordinate that already accounts for centring — never run it through the
   `i_pos - i_da.xy` buffer-relative transform meant for 3D.
2. **Short of owning the emitter:** in the sprite path (`gpu_native.cpp` ~line 991, the
   `gpu_gpu_wide_engine() && s_prev_had3d` block), make the 2D HUD sprite coordinates buffer-relative
   *before* they reach the shader so the shader's `i_da.x0` subtraction is correct — i.e. pass HUD sprite
   X already expressed in the same origin the shader subtracts (add `s_da_x0` back to the HUD-anchored X,
   or pass `i_da.x0 = 0` for these prims). The probe that added `s_da_x0` confirms this re-centres them.
   The PC-native intent: a centre-anchored HUD element = native X + `margin`, with NO `i_da.x0` term.

### Fix 2 (count) — make the three sprites composite to one ball in the FB path, same as 4:3
Do **NOT** "hide two of the three" — that is a bandaid. Once Fix 1 re-centres them, give the FB present
the SAME ordering/overdraw behaviour the native-VRAM path has (the engine's own 2D HUD layer/sort), so
whatever collapses the three sprites to one ball at 4:3 also happens in the wide path. This requires first
closing the Defect-2 open thread (why 4:3 drops two of the three): if they are overdrawn by a later prim
at 4:3, the FB path must apply the same overdraw/order; if the three are MEANT to be one ball (three tiles
of one wider glyph that overlap on the real path), the engine should treat them as one logical HUD rect.

**Files / lines**
- `runtime/recomp/shaders_vk/tritex.vert:30-39` — FB relocation `fx = (i_pos.x - i_da.x0)*ss + fb_x0`
  (the `i_da.x0` subtraction is Defect 1's mechanism).
- `runtime/recomp/gpu_gpu.cpp:801-809` — `push_wide`: `fb_x0 = margin*ss` (the only re-centring added back).
- `runtime/recomp/gpu_native.cpp:990-994` — sprite 2D wide block (`ws_2d_local_x`); the place to make HUD
  sprite X buffer-relative / cancel the `i_da.x0` term (Fix 1 option 2).
- `runtime/recomp/gpu_native.cpp:121-129` — `ws_2d_anchor_off` thirds (NOT the cause here; anchor=0).
- `runtime/recomp/gpu_native.cpp:1126` — GP0 `E3` sets `s_da_x0` (=144 this frame).
- `runtime/recomp/gpu_native.cpp:984-998` — HUD sprite RQ band (RQ_HUD / RQ_OM_2D_FG, submission order).

**USER eyeball after fix:** at 16:9 the spiky-ball HUD is ONE ball at bottom-CENTRE (matching the 4:3
shot), at ires>1 too; 4:3 unchanged.

---

## Open thread (one)
Defect 2 (3→1) is not fully root-caused: confirm via a raw 4:3 VRAM read at y≈200, x 112-208 whether the
x=112 and x=176 balls are physically absent from VRAM (overdrawn at raster) or merely outside the sampled
display region. That determines whether the FB-path fix is "match the overdraw order" or "treat the three
sprites as one logical HUD rect." Defect 1 (position) is fully proven and has a verified probe fix.
