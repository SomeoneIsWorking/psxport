# Issues #7 + #11 — FMV-skip → stale-overlay family (root cause + PC-native fix spec)

READ-ONLY RE. No source was modified. Cited file:line + guest addresses throughout.

- **#7** "Skipping the 2nd FMV briefly corrupts the title/menu" — menu glyphs
  (NewGame/LoadGame/Options/StartGame) garble with yellow-highlight bleed for a frame or two,
  then snap clean.
- **#11** "SCEA copyright + broken title menu appear after skipping the opening FMV" — (1) the
  SCEA "Sony Computer Entertainment…" white text is drawn over an in-engine Tomba close-up
  (`issue11_scea_in_scene.png`); (2) the title menu renders broken — overlapping/mispositioned
  text + yellow highlight boxes (`issue11_title_menu_broken.png`).

**Verdict: #7 and #11 are ONE bug** — the same stale-`s_vram` mechanism, seen at two points in the
boot sequence (opening FMV vs second FMV) and at two stages of the title's own asset-load ramp.

---

## 0. Repro (headless, reached)

`PSXPORT_VK_HEADLESS=1` forces `skip_fmv` (`native_boot.cpp:624`), so to exercise the skip path I ran
FMVs **on** but capped to 3 decoded frames each (`PSXPORT_FMV_MAXFRAMES=3`) + `PSXPORT_SCEA_SKIP=1`,
which is behaviorally identical to a user Start-skip: the FMV player breaks out early
(`native_fmv.cpp:763`) leaving its last partial frame in VRAM, exactly as the skip break does
(`native_fmv.cpp:720`, `:759`).

```
PSXPORT_VK_HEADLESS=1 PSXPORT_NO_FMV=0 PSXPORT_SCEA_SKIP=1 PSXPORT_FMV_MAXFRAMES=3 \
PSXPORT_NOAUDIO=1 PSXPORT_REPL=1 PSXPORT_NATIVE_FRAMES=100000 \
scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE "<disc.chd>"   # REPL: run 5 / shot / run 60 / shot
```

Evidence captured (`scratch/screenshots/`):
- `postfmv_title.png` (title entry, ~frame 5): **near-white blank** — the title's full-screen
  background sprites have NOT yet been uploaded/drawn; the present samples the stale white SCEA fill.
- `title60.png` (~frame 65): **correct TOMBA!2 title** — self-corrected once the bg sprites + menu
  text landed.
- `postfmv_vram.png` (VRAM @ present-frame 2): the displayed FB region (0,0)–(320,240) is the
  **white SCEA fill** the splash composite left there; the rest of VRAM is still black.
- Stage timeline (`PSXPORT_DEBUG` task log): DEMO stage (0x801062E4) enters at frame ~6 with sub-SM
  `s48=1` walking `s4a=0..7` (the title asset loader) until `s48=2` (title running) at frame ~14.
  Between title-entry (~f5) and asset-ready (~f14) is the corruption window. `time out in strNext()`
  at f13 is the game's own StrPlayer giving up on the FMV we already played natively.

---

## 1. The FMV-skip control path (file:line)

**Boot order** (`boot.cpp:85` → `native_stub.cpp:231` → `native_boot.cpp:611`):

1. `native_stub_run` runs the disc boot stub SCUS_944.54 (interpreted). The stub draws the SCEA
   splash; we render it **PC-native** by compositing the 4bpp text directly into the displayed
   framebuffer `s_vram(0,0)`:
   - `GpuState::scea_splash_composite(fade)` — `gpu_native.cpp:1539`. Source text = a 4bpp page at
     VRAM **(832,256)**, CLUT at **(880,511)** (`gpu_native.cpp:1544`); it writes the decoded white
     glyphs into `s_vram` rect rows y=200, x≈24..600 (`gpu_native.cpp:1542-1556`). **CLUT index 0 is
     the WHITE fill** (`gpu_native.cpp:1552` comment), so the splash paints a large near-white region
     into the display FB.
   - Driven each present by `ov_stub_vsync` (`native_stub.cpp:170`, `:184`).
2. Stub LoadExec → `ov_loadexec` (`native_stub.cpp:76`) longjmps out → `native_boot_run`
   (`native_stub.cpp:254`).
3. `native_boot_run` plays the intro FMVs **before** crt0 (`native_boot.cpp:627-630`):
   `native_fmv_play(c,"MOVIE/LOGO.STR")` then `"MOVIE/OP.STR"`. Each frame is uploaded to **VRAM(0,0)**
   and presented by `present_rgb555` (`native_fmv.cpp:535-551`, `gpu_gp0(0xA0000000)` dest (0,0)).
4. `rec_dispatch(crt0)` → `ov_game_main` (`native_boot.cpp:634`, `:361`) → native frame loop → DEMO
   stage 0x801062E4 builds the title.

**What the skip does** (the actual skip trigger is in the FMV player, NOT a dedicated skip routine):
- `fmv_pace` (`native_fmv.cpp:644`) polls the pad each FMV frame; a Start **edge** sets `pressed`
  (`native_fmv.cpp:646-648`, `:654`). The play loop breaks on it (`native_fmv.cpp:720` for an audio
  sector, `:759` after a video frame), sets `skipped=1`, and **returns the frame count**
  (`native_fmv.cpp:766`, `:771`).

**What is NOT done on skip (the bug surface):**
- The FMV player does **no teardown**: it leaves its last (possibly partial) decoded frame in
  `s_vram(0,0)` and the disp-origin pointing at it. There is no clear/black-fill, no re-upload.
- The **SCEA text asset stays resident** in VRAM at (832,256)/CLUT(880,511) — nothing ever tears it
  down — and the **white SCEA fill it composited into `s_vram(0,0)` stays there** too. Nothing clears
  `s_vram` between the splash/FMV and the title (verified: no `memset(s_vram…)`/clear on this path).
- Control returns straight to MAIN's DEMO stage, which begins **loading** the title assets over
  several frames; the first title frames present **before** the full-screen background is uploaded.

**Ordering vs the first menu frame:** title-entry (~f5/f6) precedes title-assets-ready (~f14). The
present each frame is `present_window()` = `blit_src(s_vram, disp)` (`gpu_native.cpp:1296`), and the
VK path rasterizes the title's 2D sprites/text **on top of** that blitted `s_vram`
(`gpu_native.cpp:1409`, VK batch). So for ~8 frames the title composites its partial 2D prims over
**stale `s_vram`** (the white SCEA fill / FMV last frame).

---

## 2. #11 root cause — SCEA copyright + broken menu after skipping the OPENING FMV

The opening FMV (`OP.STR`) is the Tomba close-up. Skipping it (or it ending early) leaves:
- its **last frame in `s_vram(0,0)`** (the Tomba close-up the user sees), AND
- the **SCEA white fill still in `s_vram(0,0)`** from the stub splash, plus the resident SCEA text
  page at (832,256). The splash fill was never cleared, so the white "Sony Computer Entertainment…"
  glyphs (rows y≈200) sit composited into the same display FB the FMV partially overwrote — wherever
  the short FMV frame didn't fully overwrite, the SCEA white text shows through, painted over the
  Tomba image. That is `issue11_scea_in_scene.png`.

When MAIN's DEMO stage then enters and starts drawing the title **before its background sprites are
uploaded** (the ~f5→f14 window), the title's menu-text/highlight prims (op-0x65 sprites + text polys)
draw over that stale `s_vram` with **no opaque background behind them** → overlapping/mispositioned
white-on-white glyphs with the yellow selection highlight boxes exposed: `issue11_title_menu_broken.png`.

Root cause stated as the unit of work: **the FMV-skip / FMV-end path returns to the front-end without
(a) clearing the display framebuffer `s_vram` and (b) ensuring the title's opaque background is present
before the first title frame is shown.** The persistent SCEA fill + FMV last frame are then revealed
through the title's still-incomplete 2D layer. This is NOT a render-order/OT bug (later-178 fixed the
title's E1→sprite order); the title *content* is correct once loaded — the defect is stale pixels
under it during the load ramp.

Evidence: `postfmv_title.png` = near-white blank at title entry (stale SCEA fill, no bg yet);
`postfmv_vram.png` = white SCEA fill occupying the (0,0) display region; task log = DEMO sub-SM
`s48=1 s4a=0..7` loading until `s48=2` at ~f14.

---

## 3. #7 root cause — skipping the 2nd FMV briefly garbles the menu, then self-corrects

Identical mechanism, different entry point. The "second FMV" is `OP.STR` (LOGO.STR = Whoopee logo is
the first). Skipping it returns to the DEMO/title with stale `s_vram` (FMV last frame + SCEA fill).
The title then re-loads/re-uploads its font texture + CLUT and its two full-screen background sprites
over the next several frames (DEMO sub-SM `s4a` ramp, ~f5→f14). During that ramp:
- the menu **font glyphs** sample VRAM CLUT/texpage that is **mid-upload / stale** → garbled glyphs;
- the **yellow selection-highlight** rect draws over the not-yet-opaque background → "yellow-highlight
  bleed";
once the background sprites + final font/CLUT upload complete (`s48==2`), the present shows the clean
title (`title60.png`). That is exactly the "garbled for a frame or two, then snaps clean" report —
**self-correcting because the title's own asset upload eventually overwrites the stale VRAM.** The
hypothesis in the issue ("first frames sample stale VRAM left by the FMV before the font+CLUT upload
completes") is **confirmed**.

---

## 4. Shared root cause?

**Yes — one bug.** Both are "the FMV-skip/end path hands back to the front-end with stale `s_vram`
(SCEA white fill + FMV last frame) and a resident SCEA text asset, and the title presents its 2D layer
before its opaque background + font/CLUT are fully uploaded." #11 = the worst-case first frames (SCEA
text + Tomba image both visible, menu drawn over nothing); #7 = the milder tail of the same window (a
frame or two of garbled glyphs). #11's SCEA-over-Tomba is just the most legible stale artifact because
the SCEA white fill is large and high-contrast; #7's is the font/CLUT mid-upload flavor of the same gap.

---

## 5. PC-native FIX PLAN (engine-owned sequencing — no sleep/retry)

The engine OWNS boot presentation; make the hand-off deterministic so no stale pixel can ever be
presented. Two independent, additive fixes — do BOTH (the second is the real guarantee):

### Fix A — the FMV/skip path tears down its own overlay (cheap, removes the SCEA+FMV residue)
On EVERY exit of `native_fmv_play_lba` (normal end AND `skipped` break), before returning, **clear the
displayed framebuffer to opaque black and present once**, so no FMV last-frame / SCEA fill survives
into the front-end:
- Add an explicit `gpu_clear_display(core)` (new tiny helper in `gpu_native.cpp`: `memset` the
  `s_vram` display rect `[s_disp_x..+s_disp_w] x [s_disp_y..+s_disp_h]` to 0, then `gpu_present`),
  called at the two break sites and the normal return in `native_fmv_play_lba`
  (`native_fmv.cpp:720/759/766`).
- Also clear it **once in `native_boot_run` after the FMV calls return** (`native_boot.cpp:630`),
  covering the SCEA-fill case (the SCEA white fill is still in `s_vram` even if NO FMV ran, e.g.
  LOGO/OP both skipped instantly). This is the targeted kill for `issue11_scea_in_scene.png`.
- The **resident SCEA text page** at (832,256)/CLUT(880,511) need not be wiped (it's off the display
  region and the title overwrites that VRAM when it uploads its atlas) — clearing the **display FB** is
  what removes the visible artifact. (If a stale-VRAM sampler ever re-reads (832,256), zeroing that
  4bpp page in the same clear is a clean belt-and-braces; keep it minimal.)

### Fix B — the title never presents before its opaque background is ready (the real guarantee)
Fix A removes the SCEA/FMV residue, but a title frame drawn before its background sprite uploads would
still show black-under-text for a frame or two. Make the front-end's first presented frame deterministic:
- The DEMO/title sub-SM gates "assets loaded" at `task0+0x48 == 2` (it walks `s4a=0..7` while loading,
  `native_boot.cpp` task log; sub-SM at stage 0x801062E4). **Until the title background is uploaded,
  present a clean black frame instead of the half-built 2D layer** — i.e. the engine suppresses the
  title's 2D draw/compose while `s48 < 2` (or, equivalently, force the display FB to black for those
  frames). This is engine-owned sequencing keyed on the stage's OWN load-complete signal, NOT a timer
  or a sleep.
- Equivalently and more robustly: give the title an **explicit opaque background clear** as the FIRST
  thing the title composes each frame (a full-screen black/backdrop fill in `s_vram` before the menu
  text), so menu text/highlights never composite over stale or empty VRAM. This is the PC-native "draw
  your own background every frame" rule and removes the dependence on upload timing entirely.

Recommended: **A + the background-clear half of B.** A is the direct kill for the SCEA-on-Tomba
artifact; the per-frame opaque background in B removes the font/CLUT-mid-upload garble window for #7.
Neither uses a sleep, retry, or magic frame count — A is unconditional teardown at a real exit point,
B is an unconditional opaque background owned by the front-end.

### What the USER must eyeball (agent cannot self-verify a render)
Build, then on a **windowed** run press Start to skip during LOGO.STR and again during OP.STR, and
confirm:
1. No SCEA "Sony Computer Entertainment…" text ever appears over the Tomba close-up or the title.
2. The title menu (NewGame/LoadGame/Options/StartGame) shows clean from its first visible frame — no
   garbled glyphs, no yellow-highlight bleed, no white-on-white overlap.
3. The normal (un-skipped) FMV→title transition is unchanged (no new black flash beyond one clean
   black hand-off frame).
Capture comparable headless shots with the repro in §0 (`postfmv_title` should now be black or the
correct title, never near-white) and send for eyeball.

---

## 6. Reachability

**Reachable headless** (§0) — root cause confirmed on the running port, not a static guess. The only
caveat is that `PSXPORT_VK_HEADLESS` forces FMV-skip, so the repro uses `PSXPORT_NO_FMV=0` +
`PSXPORT_FMV_MAXFRAMES=3` to recreate the early-break state; the live Start-skip path
(`native_fmv.cpp:720/759`) takes the identical return path, so the captured stale-`s_vram` state is the
same one a user sees. Final visual sign-off is the windowed eyeball in §5.

## Key file:line index
- FMV-skip trigger + early-return (no teardown): `runtime/recomp/native_fmv.cpp:644-658` (pace/skip),
  `:710-771` (`native_fmv_play_lba` loop + breaks + return), `:535-551` (`present_rgb555` → VRAM(0,0)).
- Boot order / FMV calls / skip env: `runtime/recomp/native_boot.cpp:611-638`, FMVs at `:627-630`,
  `skip_fmv` at `:624`; native frame loop `:439-597`.
- SCEA splash composite (white-fill text into `s_vram(0,0)`): `runtime/recomp/gpu_native.cpp:1539-1558`;
  driver `runtime/recomp/native_stub.cpp:121-188` (`ov_stub_vsync`), `:199-213` (`ov_stub_cdread` hold),
  hand-off `:76`, `:231-255`.
- Present samples stale `s_vram`: `gpu_native.cpp:1296` (`present_window`), `:1270-1295` (`blit_src`),
  `:1400-1510` (`gpu_present_ex`) — no `s_vram` clear anywhere on the boot→title path.
- Stage gate for "title assets loaded": DEMO stage `0x801062E4`, sub-SM `task0+0x48==2` (task log,
  `native_boot.cpp` `[native_boot] frame … stage=DEMO sm[48=…]`).
- Prior context: `docs/journal.md` later-179 (SCEA PC-native render), later-178 (title E1→sprite order),
  later-46 (never-present-an-undrawn-buffer).
