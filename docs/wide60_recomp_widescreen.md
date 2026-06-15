# wide60 — widescreen for the native recomp port (Tomba! 2)

> **⚠️ CORRECTION (2026-06-15, journal later-55 — READ THIS FIRST).** The central premise below
> (§1/§5: "we own present, so widen the framebuffer to 427 and present square pixels, NO stretch")
> is **architecturally impossible for Tomba2** and is falsified by measurement:
> - **VRAM is fully packed.** Two 320-wide framebuffers sit at (0,0)/(0,256); the **texture atlas
>   fills x≥320 right next to them** (`scratch/screenshots/vram_f5000.png`). There is NO horizontal
>   room to widen the FB — drawing into x∈[320,427) corrupts the live textures. §5's "copy a
>   WS_WIDTH-wide region centered on the display origin" copies the texture atlas as the screen's
>   right third (garbage).
> - **The §1 IR1×0.75 multiply SQUISHES** (0.75× horizontal); presented 1:1 it's a thin world. It is
>   only correct paired with a downstream **1.333× stretch** — i.e. the very Beetle hack this doc set
>   out to replace. The prototype `proj_widen.c` numbers actually demonstrate the squish.
>
> **What real hor+ widescreen requires here** (none autonomous-completable): (1) squish-stretch
> (Beetle hack — the only one that fits packed VRAM), or (2) a separate wide off-screen FB backing
> store in our rasterizer, or (3) relocating the texture atlas. **All three still require the §2
> per-game cull-cone widen** (the cull is world/camera-space, so no present trick recovers the edge
> geometry), which is the risky change that caused walk-through ghosts and needs the user's eyes.
> §2 (cull RE) and the measurement tooling below still stand; §1/§5's "better-than-Beetle, no-stretch"
> framing does not. Treat this doc's mechanism sections as the cull/HUD/terrain RE record, not the
> present strategy.

**Status:** design + RE hook map. No runtime code changed by this doc (another stream owns
`gte_beetle.c` / `gpu_native.c`). Prototype proving the projection math: `scratch/wsdev/proj_widen.c`.

**Scope reframe (read first).** The project's *prior* widescreen notes (journal 2026-06-13
"display enhancements", 2026-06-12 "GTE tagging + cull-cone patch") targeted the **Beetle
emulator** and its **GTE-scale hack** (`beetle_psx_widescreen_hack` / the
`widescreen_hack_aspect_ratio` multiply in `gte.c` `TransformXY`). That hack *narrows* the
projected X by ~0.75 on the assumption the present layer then **stretches** a 4:3 image out to
16:9. **None of that carries over as-is to this port.** The port is a static recompilation that
**owns** the GTE projection (`runtime/recomp/gte_beetle.c`, Beetle `gte.c` compiled as-is), its
**own software rasterizer** (`runtime/recomp/gpu_native.c`), and the **present + SDL window**
(`gpu_present` / `present_window` in `gpu_native.c`). So widescreen is done **properly**: widen
*our* horizontal FOV in the GTE and present a *genuinely wider* framebuffer at correct pixel
aspect — no stretch, no squish. The emulator-era `widescreen_hack` global stays OFF; we do not
reuse it (details in §1).

What *does* carry over: the **culling RE** (the six `slti` cull sites, the per-object cull
dispatcher `0x8007712C`) and the hard lesson that a *naive* cull-widen corrupted gameplay
(objects drawn but walk-through / blinking). §2 designs around that.

---

## 0. The aspect target and the one constant

PSX 4:3 active display for this title is **320×240** (the game programs GP1(0x08)→320 wide,
GP1(0x06/0x07) ≈ 240 lines; see `gpu_native.c` `gpu_gp1`, `s_disp_w/s_disp_h`). To go 16:9 at
the same vertical FOV we widen the horizontal field by 4:3→16:9 = **(16/9)/(4/3) = 4/3 ≈
1.333×**, i.e. a virtual width of **320 × 4/3 = 426.7 → 427 px**.

Define one tunable:

```
WS_RATIO = (4.0/3.0) / target_aspect          // 0.75 for 16:9, 0.5625 for 21:9
WS_WIDTH = round(W43 / WS_RATIO)               // 427 for 16:9, 569 for 21:9
```

`WS_RATIO` is the **screen-X scale** applied to the GTE's IR1 contribution; `WS_WIDTH` is the
**widened viewport width** the rasterizer and present use. These are inverse: scale X by
`WS_RATIO` about the center, render into a viewport `1/WS_RATIO` wider. Note `WS_RATIO` for 16:9
is **0.75 — numerically identical to Beetle's `widescreen_hack_aspect_ratio` for 16:9** — but
the *meaning* is opposite: Beetle relies on a downstream stretch; **we keep pixels square and
widen the viewport instead** (§1, §5). Knob: `PSXPORT_WS_ASPECT` (default off = 4:3; `16:9`,
`21:9`).

---

## 1. PROJECTION WIDENING — the GTE tap

### The RTPS math (where the FOV lives)
Beetle `gte.c` `TransformXY` (the projection step shared by RTPS and RTPT), line **1300**:

```c
SET_MAC(0, F((int64_t)OFX + IR1 * h_div_sz * ((widescreen_hack) ? widescreen_hack_aspect_ratio : 1.00)) >> 16);
//          ^OFX (CR24, 16.16 center-X)  ^IR1 (cam-space X)  ^h_div_sz = H/Z (proj.plane / depth)
```

Screen-X = `OFX + IR1·(H/Z)`. So three equivalent levers widen the horizontal FOV:
- **scale the IR1 term** by `WS_RATIO` (what we choose — surgical, touches only X),
- scale `H` (changes both X *and* the depth divide DQ/Z used for fog/Z — wrong, do not),
- shift `OFX` (only re-centers, doesn't change FOV).

`h_div_sz` is shared by the X and Y writes (line 1303 does Y with the same `h_div_sz` and **no**
multiplier) — scaling only the X line keeps vertical FOV intact, which is exactly the
"hor+ widescreen" we want (more seen left/right, same top/bottom).

### Why we do NOT reuse `widescreen_hack`
The recomp build compiles `gte.c` **without `-DPSXPORT_HOOKS`** (that guard is the dead
emulator capture path; confirmed absent from `run.sh:89` / `tools/recomp/build.sh:26`).
`runtime/recomp/gte_beetle.c` hard-sets `widescreen_hack = 0` and
`widescreen_hack_aspect_ratio_setting = 0` as faithful-first externs. The stock hack's 0.75
multiply assumes a downstream horizontal **stretch** of a 4:3 framebuffer; our present does no
such stretch (§5), so enabling the stock hack alone would render the world *narrowed* into a
4:3 viewport — squished, the exact "Tomba2 widescreen caveat" the journal warned about. We need
the multiply **plus** a wider viewport. The cleanest implementation is therefore *not* to flip
the existing flag but to add a dedicated tap.

### The exact tap (recommended)
Do **not** edit `gte.c` (vendored). Put the widen in **`runtime/recomp/gte_beetle.c`**, which is
ours and already the adapter for `gte.c`. Two viable mechanisms:

1. **Preferred — set Beetle's own scalar, repurposed.** `gte_beetle.c` already owns
   `widescreen_hack` / `widescreen_hack_aspect_ratio_setting`. Set `widescreen_hack = 1` and pick
   the `_setting` whose ratio equals `WS_RATIO` (case 1 = 0.75 = 16:9; case 5 = 0.55 ≈ 21:9).
   That activates line 1300/1317's multiply with **no edit to `gte.c`** — the global is defined in
   `gte_beetle.c`. Then pair it with the viewport widen in §5. This reuses the exact, oracle-tested
   multiply path; the only change vs the emulator era is that *we widen the viewport instead of
   stretching*, so the result is correct-aspect rather than squished.
   - Caveat: Beetle's table is coarse (0.80/0.75/0.66/0.63/0.60/0.55/0.37). For an arbitrary
     aspect, prefer mechanism 2.

2. **Exact — a thin RTPS/RTPT override scaling IR1.** If a precise/arbitrary `WS_RATIO` is wanted,
   wrap `gte_op` in `gte_beetle.c`: for RTPS (`0x01`/`cop2 0x180001`) and RTPT (`0x30`), the
   projection writes XY_FIFO. Scaling at the *register* level is fragile (IR1 is reused by other
   ops). Cleaner: keep mechanism 1's flag path but expose `widescreen_hack_aspect_ratio` as a
   *float we set directly* (it is a local in `TransformXY` today — would need a one-line change in
   `gte.c` to read an extern). Since the rule is "don't edit `gte.c`," **mechanism 1 is the
   recommendation**; revisit only if a non-tabulated aspect is required.

**Tap summary:** `runtime/recomp/gte_beetle.c`, in `gte_init()` (or a new `gte_set_widescreen()`
called from boot): `widescreen_hack = ws_enabled; widescreen_hack_aspect_ratio_setting =
ws_setting;`. Effective math becomes `SX = OFX + IR1·(H/Z)·WS_RATIO`, exactly line 1300.

### Prototype evidence
`scratch/wsdev/proj_widen.c` reimplements line 1300 with `WS_RATIO=0.7494` and a `+53px` viewport
recenter (427 vs 320). Output: vertices at camera-X `IR1∈{-200,+160,+200}` that project to
`SX∈{-40,320,360}` — **off-screen at 4:3** — land at `SX∈{63,332,362}` **inside [0,427)** —
**visible at 16:9**. That extra-visible band is precisely the new FOV, and precisely what the
game's culling will pop-in/out of if the cull cone isn't widened to match (§2).

---

## 2. CULLING FIX (per-game) — widen visibility WITHOUT touching logic

### The cull RE (carried over, addresses verified)
The game culls per-object against a view cone *narrower than the screen* in the overlay
enqueue-for-draw function `~0x80077100–0x800776E0`. The test is a signed compare
`slti $v0,$v1,IMM` where **`$v1` = a cos-scaled dot/distance** and `IMM` = a per-LOD threshold;
`$v1 < IMM ⇒ culled`. Six sites (from `runtime/games/tomba2.cpp` `kCullSites`, with their
original instruction words as overlay signatures):

| site PC      | instr word   | stock IMM | meaning            |
|--------------|--------------|-----------|--------------------|
| `0x800772D4` | `0x28620370` | `0x370`   | `slti v0,v1,0x370` |
| `0x80077368` | `0x28620358` | `0x358`   | `slti v0,v1,0x358` |
| `0x80077414` | `0x28620358` | `0x358`   | `slti v0,v1,0x358` |
| `0x800774A8` | `0x28620370` | `0x370`   |                    |
| `0x8007753C` | `0x28620350` | `0x350`   |                    |
| `0x800775D0` | `0x28620368` | `0x368`   |                    |

The universal per-object **cull/LOD dispatcher is `0x8007712C`** (a0 = object*, once per logic
frame for every live drawable; `ObjectCull` hook, `0x00051400` signature). The cull-cone test
above lives downstream of it.

### Why the emulator-era widen broke gameplay (the lesson)
The old `CullSlti` override recomputed each `slti` against a **0.72×-widened** threshold and
redirected past the stock instruction. **Result (user-reported ground truth, journal
2026-06-13): objects blinked in/out AND became walk-through** — visible but with no collision.
**Root cause:** these six `slti` sites are **not purely a *visibility* test** — at least some
gate the object's *gameplay activation* (collision/logic setup). Widening them forced the engine
to *draw* objects whose collision/logic state was never initialized → rendered ghosts at the
widened boundary, flickering as they crossed it. The six sites "are NOT all confirmed to be the
same cull test" (journal) — that ambiguity is the trap. It was gated OFF
(`PSXPORT_T2_CULLWIDEN=1` to re-enable for RE only).

### The correct native design: separate visibility from activation
The fix is **not** to widen all six `slti` thresholds blindly. It is to **widen only the
*visibility/draw-enqueue* test, leaving the *gameplay-active* test at its stock 4:3 value**, so
logic/collision behaves byte-identically to the unmodified game and only the *render* cone grows.

Concrete plan, in order of preference:

1. **Classify the six sites first (RE, do before any widen).** For each `slti` site, determine
   whether the branch it feeds leads to (a) *skip-draw only* (enqueue path) or (b) *skip-the-whole-
   object* (which also skips collision/logic setup). Method: at `0x8007712C` (`ObjectCull`,
   `a0`=object) log, per object per frame, which of the six sites fired and whether that object's
   collision/active flags (the object struct fields the journal mapped: type `+0x0c`, visible flag
   `+0x01`, pos `+0x2e/0x32/0x36`, pool base `~0x800EF478` stride `0xC4`) were subsequently
   populated. The sites that, when "kept", leave collision uninitialized are **activation gates —
   do NOT widen these.** The sites that only suppress the draw packet are **pure visibility — widen
   these.**
2. **Widen only the visibility sites.** For each confirmed pure-visibility `slti`, the native
   override is the same mechanism as `CullSlti` but applied to a *vetted subset*: compute
   `v0 = (int32_t)v1 < WIDE_IMM` and redirect to `pc+4` in the running region
   (`(pc+4) | (region bits)`), RAM untouched, instruction-word-signature-gated for overlay safety.
   The widened `WIDE_IMM` should be scaled by **exactly `WS_RATIO`** (0.75 for 16:9), *not* the old
   hand-tuned 0.72 — the cone must widen to match the projection's new FOV, which §1 fixes at
   `WS_RATIO`. (0.72 was an eyeballed value; tie it to the same constant so projection and cull
   stay consistent across aspect settings.)
3. **If a site turns out to gate BOTH draw and logic (inseparable), leave it at stock.** Then that
   object class simply pops in at the stock 4:3 boundary even in widescreen — an *acceptable,
   non-corrupting* limitation (it looks like the original game at the screen edge), vastly
   preferable to walk-through ghosts. Document which classes are affected.

**Net rule:** *only the visibility test moves; the gameplay-active test never moves.* This is the
direct answer to the journal's warning. Implementation lives in **`runtime/games/games_tomba2.c`**
(the per-game module) via `rec_set_override`/hook on the vetted site PCs — the override
infrastructure already supports redirect hooks (the `tomba2.cpp` `CullSlti` is the template;
port it, restricted to the vetted subset and using `WS_RATIO`).

**Hook for classification:** `0x8007712C` (dispatcher) + the six `slti` PCs above. Gate behind
`PSXPORT_WS_CULL` during RE; enable by default only after the visibility/activation split is
verified per site.

---

## 3. HUD / 2D — re-anchor to the widened edges, never stretch

### Where the HUD is drawn
2D HUD/overlay elements are **CPU-positioned**, not GTE-projected: the game writes their screen
coordinates directly into GP0 rectangle/sprite packets (`op 0x60–0x7F`) and untextured rects /
fills. In `gpu_native.c` these flow through `gp0_exec` (the `op>=0x60 && op<=0x7F` sprite/rect
branch, line ~154; and `op==0x02` fill). Their X is the literal `cx(xy)` from the packet **plus
`s_off_x`** (the GP0 `E5` draw-offset) — it never passes through OFX/IR1, so §1's projection
widen does **not** move them. Good: that means we can re-anchor them independently.

### The re-anchor design
At 16:9 the framebuffer is `WS_WIDTH` (427) wide but the game still positions HUD as if the
screen were 320 wide. Two failure modes to avoid: (a) leaving them at raw X → HUD bunched in the
**left** 320 px with empty space on the right; (b) scaling them by `WS_WIDTH/320` → **stretched**
HUD (the journal's explicit "must not stretch"). Correct behavior = **edge-anchoring**:

- Elements near **screen-left** (raw `X < 160`) keep their distance from the **left** edge: `X' = X`.
- Elements near **screen-right** (raw `X ≥ 160`) keep their distance from the **right** edge:
  `X' = X + (WS_WIDTH - 320)`.
- Centered elements (a HUD piece spanning the middle, or full-width bars) are centered in the new
  width: `X' = X + (WS_WIDTH - 320)/2`.

The left/right split point (160 = old center) is the simplest heuristic; per-element anchor
classification (left / right / center / stretch-bar) can be refined per HUD piece during RE if
the 160-split mis-files something. Y is unchanged (vertical size is unchanged).

### Where to apply it
**Do not** bake this into the rasterizer's generic sprite path (it must not know about aspect).
Two clean options:
1. **Per-game GP0 filter (preferred):** in `games_tomba2.c`, intercept the HUD draw routine(s)
   — the libgpu sprite wrappers the game uses for HUD (the `0x80080xxx/0x80081xxx` libgpu
   wrappers the journal flagged as the per-frame display path, e.g. `FUN_8008179C` and the
   PutDispEnv family) — and apply the anchor transform to the packet's X before it reaches
   `gpu_gp0`. Requires identifying *which* draw calls are HUD vs world-2D (RE: HUD is drawn with a
   stable draw-env / fixed Z-order after the 3D scene; tag by the routine/call-site).
2. **Rasterizer-side anchor flag:** add a "this primitive is a screen-space 2D sprite" signal
   from the per-game layer and let `gpu_native.c` apply the X-anchor only for flagged prims. More
   invasive to shared code (another stream owns it) — option 1 keeps the per-game logic in the
   per-game file.

**RE task to pin first:** enumerate the HUD draw call-sites (life bar, item icons, dialog boxes).
The `0x80080xxx/0x80081xxx` libgpu wrappers are the entry; log GP0 sprite/rect packets per frame
and identify the fixed-position UI vs the world. Hook point candidate: the per-frame display
driver `FUN_8008179C` (journal: "PutDispEnv etc.", state-0 driver in the StrPlayer/main loop).

---

## 4. TERRAIN — the CPU-projected geometry

### The problem
The journal flags Tomba2's **terrain as CPU-projected** (not per-vertex GTE), so §1's GTE widen
**will not widen the terrain** — only the GTE-projected characters/models/objects widen, and the
terrain would stay 4:3-projected → a horizontal seam between widened world objects and
un-widened ground. This is the single hardest piece and the most likely "known limitation."

### Options
1. **If the terrain's CPU projection uses the *same* OFX/H/Z parameters** (i.e. the CPU code
   reimplements the perspective divide with the GTE's OFX/H), then the same `WS_RATIO` X-scale
   applies — find the CPU projection routine and scale its screen-X output by `WS_RATIO` about the
   center, identically to §1. RE task: locate the terrain transform (journal: "RLE/decode loop at
   `0x80044E14`", "demo tunnel mesh appears CPU-computed"; the terrain mesh transform is a separate
   routine — likely reads the same camera matrix the GTE uses). This is the *correct* fix and keeps
   terrain consistent with world objects.
2. **If terrain is GTE-projected after all** (the "CPU-projected" claim was from the emulator-era
   RE and may be partly wrong — re-verify on the running port; GTE RTPS now actually fires here,
   unlike the emulator-era 2D-only intro): then it widens automatically via §1 and there is no
   extra work. **Verify this first** before assuming the hard path — it may be free.
3. **Known limitation (fallback):** if the terrain projection can't be located/scaled cleanly,
   ship widescreen with world-objects + HUD widened and terrain at 4:3 FOV, documented. This is a
   visible seam and is the *least* acceptable outcome — pursue (1)/(2) first.

**RE task to pin first:** confirm whether terrain vertices pass through GTE RTPS/RTPT on the
running port (instrument `gte_op` in `gte_beetle.c` for RTPS/RTPT counts during gameplay with
terrain on screen). If yes → option 2 (free). If no → find the CPU projection routine → option 1.

---

## 5. PRESENT / ASPECT — widen the framebuffer, keep pixels square

### Current state
`gpu_native.c`: `s_disp_w`/`s_disp_h` come from GP1(0x08/0x07) (320×240). `present_window`
creates the SDL texture at `s_disp_w × s_disp_h` and `SDL_RenderCopy`s it to the window (3×
scale today), copying the displayed VRAM region `(s_disp_x,s_disp_y)`–`+s_disp_w×s_disp_h`.

### The widescreen present
The game still programs a 320-wide display region (it doesn't know about widescreen), but §1
makes the GTE project world geometry across a **427-px-wide** span centered on OFX. So:

- **VRAM scanout width must widen to `WS_WIDTH` (427).** The rasterizer draws into VRAM at the
  widened X coordinates (projection produces X up to ~427 about center after the viewport
  recenter). The present must therefore copy a `WS_WIDTH`-wide region, not 320. Because the game's
  GP1(0x08) still says 320, the present layer needs the widescreen width from *our* config
  (`PSXPORT_WS_ASPECT`), not from GP1. Concretely: `present_width = ws_enabled ? WS_WIDTH :
  s_disp_w;` and the scanout copy loop / SDL texture use `present_width`.
- **The display-area X origin shifts left** by `(WS_WIDTH-320)/2` so the widened image is centered
  on the same VRAM content the game composed (the viewport recenter from §1's `+53px` is the
  mirror of this). Equivalently: keep VRAM authoring centered and have present copy
  `[s_disp_x-(WS_WIDTH-320)/2 .. +WS_WIDTH]`.
- **Pixel aspect stays square; the *window* aspect widens.** The SDL window/texture become
  `WS_WIDTH × s_disp_h` (427×240) and present at the true 16:9 window aspect (e.g. `WS_WIDTH*scale ×
  s_disp_h*scale*?`). To get a correct 16:9 *picture* with square-ish PSX pixels: window aspect =
  `WS_WIDTH / s_disp_h` after the title's pixel-aspect correction (PSX 320×240 is ~4:3 i.e. pixels
  ~1:1; 427×240 → 16:9). **No horizontal stretch of a 4:3 buffer** — this is the key difference
  from the Beetle hack, which stretched 320→16:9 and squished the world to compensate. Here the
  buffer *is* wider, so nothing is squished.
- **SDL specifics:** in `present_window`, gate the texture (re)creation on `present_width` instead
  of `s_disp_w` (line ~279 `s_tex_w != s_disp_w`), size the scanout buffer/loop to `present_width`,
  and let `SDL_RenderCopy(s_ren, s_tex, NULL, NULL)` fill the resizable window (the window is
  already `RESIZABLE`; set its initial size to `WS_WIDTH*3 × s_disp_h*3`). Keep
  `SDL_HINT_RENDER_SCALE_QUALITY=nearest` (no bilinear, per prior user pref) — or letterbox via
  `SDL_RenderSetLogicalSize(WS_WIDTH, s_disp_h)` so resize preserves 16:9.

This all lives in **`gpu_native.c`** (`present_window` + the `s_disp_w`-driven scanout in
`gpu_present`) — owned by another stream; this doc specifies *what* it must do, not an edit.

---

## 6. CONCRETE HOOK LIST

| # | Subsystem | File / function | Address / symbol | Action |
|---|-----------|-----------------|------------------|--------|
| 1 | Projection widen | `runtime/recomp/gte_beetle.c` `gte_init()` (or new `gte_set_widescreen()`) | sets `widescreen_hack`, `widescreen_hack_aspect_ratio_setting`; effective tap = `gte.c` `TransformXY` **line 1300** (`OFX + IR1·h_div_sz·ratio`) | enable the X-scale = `WS_RATIO` (0.75 @16:9). Pair with hook #5. **Do not edit `gte.c`.** |
| 2 | Cull classify | `runtime/games/games_tomba2.c` | dispatcher `0x8007712C` (a0=object); object pool base `~0x800EF478` stride `0xC4`, flags `+0x01`/`+0x0c` | RE hook: per object, which of the 6 `slti` fired + whether collision/active got set → split visibility vs activation |
| 3 | Cull widen (visibility only) | `runtime/games/games_tomba2.c` (port `CullSlti` from `tomba2.cpp`, vetted subset) | the **visibility-only** subset of `0x800772D4 / 0x80077368 / 0x80077414 / 0x800774A8 / 0x8007753C / 0x800775D0` (instr words `0x2862xxxx`) | redirect-past-`slti` override, `WIDE_IMM = stock·WS_RATIO`, RAM untouched, sig-gated. **Never widen activation-gating sites.** |
| 4 | HUD re-anchor | `runtime/games/games_tomba2.c` (per-game GP0 X filter) | HUD draw path: libgpu wrappers `0x80080xxx/0x80081xxx`, per-frame display driver `FUN_8008179C`; packets = GP0 `0x60–0x7F` / `0x02` in `gpu_native.c` `gp0_exec` | left-anchor (`X<160`), right-anchor (`X≥160 ⇒ X+ (WS_WIDTH-320)`), center bars; **no stretch** |
| 5 | Present / aspect | `runtime/recomp/gpu_native.c` `present_window` + `gpu_present` scanout | `s_disp_w/s_disp_x` (line ~34, 259, 279, 286–291, 311–314) | copy `WS_WIDTH`-wide region centered on display origin; SDL texture/window at `WS_WIDTH×s_disp_h`; square pixels, no stretch; `RenderSetLogicalSize` for letterbox |
| 6 | Terrain | `runtime/games/games_tomba2.c` + RE | RTPS/RTPT counts via `gte_op` in `gte_beetle.c`; CPU terrain xform near `0x80044E14` (re-verify) | **first** check if terrain is GTE-projected (then free); else scale CPU screen-X by `WS_RATIO` |
| — | Config | one tunable | `PSXPORT_WS_ASPECT` (off / `16:9` / `21:9`) → `WS_RATIO`, `WS_WIDTH` | drives #1, #3, #4, #5, #6 consistently |

### Build / guard notes
- Recomp build compiles `gte.c` **without `PSXPORT_HOOKS`** (`run.sh:89`, `tools/recomp/build.sh:26`).
  The emulator capture taps (`psxport_capture_rtp` etc.) are inert here — ignore them.
- All per-game logic goes in `runtime/recomp/games_tomba2.c` (note: the file is under `runtime/recomp/`,
  not `runtime/games/`; `runtime/games/tomba2.cpp` is the **old emulator-era** module and is the RE
  source-of-record for the cull addresses, not a build input to the port).
- Gate everything behind `PSXPORT_WS_ASPECT` (and `PSXPORT_WS_CULL` during cull RE) so the faithful
  4:3 boot/diff path is byte-identical when widescreen is off.

---

## 7. Prior emulator-era assumptions that do NOT carry over

- **"Widescreen = `beetle_psx_widescreen_hack` + report aspect via av_info."** Dead. There is no
  libretro av_info / core-options layer in the port; we own present (`gpu_native.c`). The
  `widescreen_hack` global still exists (in `gte_beetle.c`) and its 0.75 multiply is *reusable as the
  X-scale*, but the **stretch-the-4:3-buffer** half of that design is replaced by **widen-the-viewport**
  (§5). Same multiplier, opposite present strategy.
- **"internal_resolution upscale + widescreen both break the wide60 capture coords."** Irrelevant —
  that was the Beetle harness capturing NATIVE screen coords; the port rasterizes ourselves at our
  chosen width, so there is no capture-coordinate mismatch to manage.
- **"Cull-widen 0.72× via `CullSlti` on all six sites."** Partially carries: the *addresses* are
  correct and reused, but (a) the scale must be `WS_RATIO` (0.75 @16:9) tied to the projection, not a
  free 0.72, and (b) it **must not** be applied to all six blindly — the visibility/activation split
  (§2) is mandatory or the walk-through/blink regression returns. Default OFF until the split is RE'd.
- **"Tomba2 terrain is CPU-projected (won't widen)."** Re-verify on the running port before treating
  it as fact — GTE RTPS now actually executes during gameplay (it didn't during the emulator-era 2D
  intro RE), so terrain *may* be GTE-projected and widen for free (§4).
- **"Present at core-reported 4:3 with letterbox."** Replaced by widescreen-aware present width (§5).
```
