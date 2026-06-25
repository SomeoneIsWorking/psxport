# Issues #6 (HUD/UI opaque gray box) + #12 (cutscene mid-frame black bar) — RE + PC-native fix spec

READ-ONLY RE, 2026-06-21. No source modified. Driven headless (`PSXPORT_VK_HEADLESS=1 PSXPORT_REPL=1`)
with `PSXPORT_SETTINGS=<ini>` toggling aspect/ires, `shot` captures, `debug gp1/scene`. Reconciles the
prior static RE (engine/engine_ui_rect.cpp draft + issue #6 gh comments) with the user's 2026-06-21 hint
("HUD is PC-ported; the gray box is negatively affected by ires and wide") and the live evidence.

---

## TL;DR

- **#6 is TWO independent defects fused in one screenshot**, not one:
  1. **Opaque gray FILL** = the CLUT-inherit defect already RE'd (the panel/gauge is a textured-SEMI prim
     whose inherited CLUT is 0 → it samples STP=0 texels → `tritex.frag:52` correctly does NOT blend →
     opaque). This is *independent of ires/wide* (it reproduces, when it reproduces, identically at 4:3
     ires1). The draft's CLUT theory is the LIVE cause of the *grayness*. **Confirmed: it does NOT
     reproduce in any scene I could reach headless** (attract menu, in-game Options/Load/Quit menu, the
     narration cutscene) — panels were transparent at 4:3 ires1, 4:3 ires2, and 16:9 ires2. Reproduction
     is state-specific (the real pause menu / the AP "3" gauge) and the user must reach it live.
  2. **Jagged / ragged / torn box edges, "worse under wide"** = a SECOND, ires/wide-specific defect in the
     PC-native 2D layout: `ws_2d_anchor_off()` (gpu_native.cpp:121) anchors **each prim of a multi-slice
     panel independently** into left/center/right thirds, so a contiguous 9-slice panel gets its slices
     shoved to different X anchors under widescreen → the box tears apart. This is the part the user's hint
     points at, and it is NEW relative to the prior RE (which only chased the CLUT).
- **#12** (cutscene mid-frame black bar) = the scratch-FB present path **discards the PSX display vertical
  crop**. A cutscene letterboxes via the GP1-0x07 display vertical-range (a narrow `vy0..vy1`), but
  `frame_via_fb` present samples the **whole** FB `[0,FBH]` (gpu_gpu.cpp:1221) and ignores `s_disp_y`/the
  vertical range, so VRAM rows the PSX would have cropped to black bars become visible — a black/undrawn
  band lands mid-screen. Wide/ires-only (the 4:3 path honors the display region).

---

## #6 — HUD/UI opaque gray box

### Live evidence (what reproduces and what does NOT)
| Scene | Config | Gray box? | Shot |
|-------|--------|-----------|------|
| In-game Options/Load/Quit menu (f299) | 16:9 ires1(auto) | NO (text only, no panel) | iss6 baseline |
| same | 4:3 ires1 | NO | iss6_43_ires1 |
| same | 4:3 ires2 | NO | iss6_43_ires2 |
| same | 16:9 ires2 | NO | iss6_ires2 |
| narration cutscene (f160) | 4:3 ires1 / ires2 / 16:9 ires2 | NO panel (the gray "@/scroll" is the in-world LETTER object, intended art) | narr_43i1/43i2/169i2 |

`scratch/screenshots/iss6_*.png`, `narr_*.png`. **Matches the draft's & gh's note: "does not currently
reproduce (panel transparent)".** The user's screenshots (issue6_pausemenu_box.png 1296×860,
issue6_hud_gauge_box.png 550×412) are from a *different state* (the genuine pause/AP-gauge state, which
AUTO/headless does not reach — see docs/driving-the-game.md §5 "there is NO pause menu in these runs").
So #6 is verified-by-RE, not verified-by-live-repro in this session; the user must reach the live state.

### Defect 1 — opaque GRAY (CLUT-inherit) — the LIVE cause of the grayness
Already fully RE'd; restating with file:line for completeness.
- Emitter: **`FUN_8007e1b8`** (universal UI rect emitter). Panel = `FUN_8007eae4` (page-1 9-slice,
  handler `0x8010829C` off table `0x801062EC`); descriptor `desc+2 = 0x8000` → SEMI set, CLUT field = 0
  ("inherit"). At `0x8007e26c..0x8007e27c` the emitter **skips the CLUT store** (`prim+0xE`) when
  `desc+2 & 0x7fff == 0`. (Full RE in `engine/engine_ui_rect.cpp` lines 16-54.)
- On PSX the prim re-uses the GP0-latched CLUT of the previously drawn UI prim; our stateless per-prim
  submit carries no latch, so the inherit prim ships with a stale/zero CLUT.
- The shader is **correct and must stay**: `tritex.frag:40` discards CLUT-index-0 texels; `tritex.frag:41`
  reads `stp = (texel>>15)&1`; `tritex.frag:52` `if (v_clut.z != 0 && (mode==3 || stp==1))` — a textured
  semi prim blends **only** where the texel STP bit is set. A wrong CLUT → texels with STP=0 → no blend →
  the raw (gray) texel is written opaque. **Do NOT relax this gate** (would break per-texel-STP sprites).
- Root cause: the engine does not bind the UI atlas CLUT explicitly for inherit-descriptor rects. The
  draft `engine/engine_ui_rect.cpp` "re-stamp inherit prims with the last-bound CLUT" is the FORBIDDEN
  fix (it reproduces the PSX GP0 latch) and is NOT wired into the build — do not ship it.

### Defect 2 — jagged/torn box under WIDE — the ires/wide interaction the user flagged
- **`ws_2d_anchor_off(bx0,bx1,is_bg)` — gpu_native.cpp:121-129**, applied per-prim at
  **gpu_native.cpp:820-824** (poly path) and **:973-974** (sprite path) via `ws_2d_local_x`.
- It classifies a 2D prim by **its own bbox center** into native-320 thirds (L≈107, R≈213) and shifts the
  WHOLE prim by `-margin` (left third), `+margin` (right third), or `0` (center). Comment at :113-119
  explicitly designed this for *independent* HUD elements ("corner HUD hugs its side").
- A **multi-slice panel / multi-sprite gauge is several prims** (the 9-slice `FUN_8007eae4`: corners +
  edges + center; the gauge's arc segments). Under widescreen (`margin>0`) the slices that fall in
  different thirds get **different anchor offsets**, so the single contiguous box is **pulled apart** at
  the third boundaries → the "ragged left/bottom edges / torn box shape" the user reported. At 4:3
  `margin<=0` → returns input unchanged (no tearing) — which is why the defect is **wide-specific**.
- ires alone (4:3, ires>1, `margin==0`) does NOT tear (`ws_2d_anchor_off` no-ops); ires only scales. So
  the user's "ires and wide" most strictly means **wide** for the tearing; ires can co-occur because the
  defaults persist `aspect=16:9` + `ires_auto`.

### Reconciliation
The prior RE (CLUT) and the new hint (wide-tearing) are **both right about different symptoms**: CLUT =
the grayness/opacity, `ws_2d_anchor_off` = the torn/ragged shape under wide. The LIVE cause of the *box
being gray* is the CLUT-inherit; the LIVE cause of it being *torn and worse under wide* is the per-prim
thirds anchoring.

---

## #12 — cutscene horizontal black bar across the middle

### Evidence
- issue12 image is **1982×256** (a wide, short crop of a macOS window — rounded corners visible), i.e. a
  **wide** cutscene capture. Scene content runs continuously above AND below a full-width black band that
  sits mid-frame, vertically offset — the band is *inside* the rendered frame, not a window border.
- Could not reach the specific cutscene headless (the narration/intro segment I reached uses a stable
  `320×234 @ 0,0` display region with only GP1-0x04 issued — no vertical-range change in that window;
  `scratch/screenshots/cut_f80/f160.png`). So #12 is a static hypothesis + a named live repro below.

### Code path + root cause
- 4:3 / non-FB present samples the **PSX display region** `[s_disp_x, s_disp_y, s_disp_w, s_disp_h]`,
  honoring the display vertical-range set by **GP1 0x07** (gpu_native.cpp:1215-1220: `s_disp_vy0/vy1` →
  `s_disp_h = vy1-vy0`) and the display-area start `s_disp_y` (GP1 0x05, :1214). A cutscene that
  letterboxes by **narrowing the displayed vertical range** (showing only `vy0..vy1` of a taller VRAM
  buffer) is reproduced correctly here — the cropped rows simply aren't sampled.
- The **scratch-FB present path** (wide/ires, `frame_via_fb()`) OVERRIDES the sampled region to the full
  FB and throws the display crop away:
  - windowed present: **gpu_gpu.cpp:1220-1221** `int via_fb = frame_via_fb(); if (via_fb) { sx=0;
    sy=FB_Y0; w=FBW(); h=FBH(); }`
  - headless: **gpu_gpu.cpp:1205** and shot/dump: **:1647**, all `{ sx=0; sy=FB_Y0; w=FBW(); h=FBH(); }`
  - i.e. it samples the WHOLE scratch FB `[0,FBH]` and never consults `s_disp_y` / `s_disp_vy0..vy1`.
- The scratch FB is filled by relocating the 3D content (tritex.vert:30-39, `fy = FB_Y0 + (y - da_y0)*ss`)
  over the full draw area. VRAM rows that the cutscene's display vertical-range would have **clipped off
  to black bars** are NOT clipped in the FB present — they render straight through. Whatever sits in those
  rows (undrawn/black, or a second buffer's content) appears as a **mid-frame black band**, with the scene
  above/below offset because the present is showing rows the PSX would never have displayed.
- Net: **the engine's letterbox (the PSX display vertical crop) is lost on the wide/ires FB path**, so a
  cutscene that letterboxes via vertical-range shows the un-cropped rows → a stray black band mid-frame.
  This is exactly an "inherited a PSX display decision and then dropped it under scaling" bug, and it is
  wide/ires-specific (4:3 honors the crop).

### Live repro the user must run
Reach the cutscene that shows the bar (the one in issue12) **at 16:9** and compare to **4:3**:
the bar should VANISH at 4:3 (display crop honored) and APPEAR at 16:9/ires>1 (FB present ignores the
crop). Enable `debug gp1` and look for a GP1-0x07 with a narrow `vy0..vy1` (and/or a GP1-0x05 `s_disp_y`
shift) on the cutscene frames — that confirms the cutscene letterboxes via the display range.

---

## PC-native FIX PLANs (engine owns its 2D layer + letterbox; do NOT replicate PSX latches/OT)

### #6 Defect 1 (opaque gray) — explicit UI-atlas CLUT, engine-owned
The engine must OWN the UI atlas CLUT and bind it explicitly for inherit-descriptor rects, instead of
inheriting a GP0 latch. Concretely (when the menu page handler `0x8010829C` / the HUD gauge emitter is
owned native): the engine knows which font/UI atlas the panel/gauge samples → set the prim's CLUT
(`prim+0xE`) from the engine's own UI-atlas CLUT id at emit time. This is engine state, not a re-stamp of
"whatever the hardware last latched". Until the page handler is owned, the panel stays as recomp and the
defect is latent (it currently does not reproduce). Do **not** wire the draft `engine_ui_rect.cpp` (its
last-bound-CLUT re-stamp is the forbidden PSX-latch replica). Do **not** relax `tritex.frag:52`.
**USER eyeball:** after the fix, reach the real pause menu + the AP "3" gauge and confirm the backing is
transparent (the gauge/text composite over the world, no gray fill) at 4:3 AND 16:9.

### #6 Defect 2 (torn box under wide) — anchor a panel as ONE unit, not per-slice
`ws_2d_anchor_off` must not split a contiguous multi-prim UI element. The engine owns 2D layout, so a
panel/gauge is one logical UI rect; the correct PC-native rule is to compute the anchor offset from the
**whole element's** bbox (or its owning UI node) ONCE and apply that SAME offset to every slice — never
per-prim thirds. Options, in order of preference:
1. When the menu/HUD emitter is owned native, lay the panel out in engine UI-space and anchor the element
   as a unit (its slices keep their relative offsets); apply one X shift for the element.
2. Short of owning the emitter: key the anchor offset by the UI element's OT-node (the slices of one
   `FUN_8007eae4` panel share a node) so all slices of one panel get one offset — i.e. cache
   `ws_2d_anchor_off` per `s_cur_node` for the frame instead of recomputing per prim. This removes the
   third-boundary tearing without per-prim guessing.
   (Either way: the rule is "one UI element → one anchor", replacing the per-prim center-thirds at
   gpu_native.cpp:121-129 / its call sites :824 and :973-974.)
**USER eyeball:** at 16:9 confirm the pause panel and the gauge box are RIGID rectangles (no ragged
edges / no slices shoved to the frame edges), and HUD corner elements still hug their side.

### #12 (mid-frame black bar) — honor the engine's letterbox on the FB present path
The engine owns its present/letterbox. The FB present must respect the cutscene's intended displayed
region instead of blasting the whole FB. The PC-native fix: when `frame_via_fb()`, map the PSX display
vertical-range into the scaled FB and sample/clip to THAT, not `[0,FBH]`:
- top of the sampled FB region = `FB_Y0 + (s_disp_y_eff - da_y0)*ires` (where `s_disp_y_eff` accounts for
  GP1-0x05 start and the GP1-0x07 `vy0`), height = `(displayed_lines)*ires`, instead of `sy=FB_Y0;
  h=FBH()` at gpu_gpu.cpp:1221 (and the headless/shot/dump twins at :1205 and :1647).
- Equivalently and more PC-native: have the engine draw real letterbox bars itself in its 2D layer (top
  and bottom of the *display* aspect) and never present FB rows outside the cutscene's display window.
  The bars then sit at the frame edges by construction, regardless of ires/wide.
Do NOT special-case "this one cutscene"; the bug is the FB present dropping the display crop for ALL
wide/ires frames — fix the region math once.
**USER eyeball:** the cutscene that showed the mid-frame bar now letterboxes at the top/bottom only
(bars at the edges, no stray mid-frame band) at 16:9 and ires>1; 4:3 unchanged.

---

## Key file:line index
- `tritex.frag:40,41,52` — CLUT-0 discard, STP read, textured-semi STP gate (correct; do not change).
- `engine/engine_ui_rect.cpp:16-54` — RE of `FUN_8007e1b8` CLUT-inherit; :1-7 the forbidden re-stamp.
- `runtime/recomp/gpu_native.cpp:121-129` — `ws_2d_anchor_off` per-prim thirds anchoring (#6 tearing).
- `runtime/recomp/gpu_native.cpp:824, 973-974` — `ws_2d_local_x` call sites (poly + sprite 2D widen).
- `runtime/recomp/gpu_native.cpp:55-58` — `bg_2d` backdrop-vs-HUD coverage classifier.
- `runtime/recomp/gpu_native.cpp:1214-1224` — GP1 0x05/0x07/0x08 set the PSX display region/vertical crop.
- `runtime/recomp/gpu_gpu.cpp:318-325` — `use_fb`/`frame_via_fb` (scratch FB only on 3D + wide/ires).
- `runtime/recomp/gpu_gpu.cpp:1205, 1220-1221, 1647` — FB present samples `[0,FBH]`, ignores display crop (#12).
- `runtime/recomp/shaders_vk/tritex.vert:30-39` — FB relocation `fy = FB_Y0 + (y-da_y0)*ires`.
- `runtime/recomp/shaders_vk/present.frag:8-16` — present samples a single contiguous `disp` rect.

## Caveats
- #6 grayness and #12 bar were NOT live-reproduced this session (state unreachable headless); both are
  RE+evidence-grounded hypotheses with named live repros for the user. The #6 *tearing* mechanism
  (`ws_2d_anchor_off` per-prim) is statically certain for any multi-slice panel under wide.
- Default `psxport_settings.ini` ships `aspect=16:9 ires_auto=1`, so the user almost always runs the
  wide/ires path — consistent with "the gray box is negatively affected by ires and wide".
