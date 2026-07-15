# RE + port frontier — is each step REAL (re-verified) or a HACK? (managed by tools/portmap.py)

The RE dependency chain. `## ` block per step. Work `portmap.py next`; kill `portmap.py hacks`.
Detail lives in docs/port-progress.md; this is the queryable real-vs-hack frontier.

**Status:** 3 verified · 1 ported-unverified · 1 todo

## title-frontend — DEMO stage s0..s7 + menu logic
- **scope:** 0x801062E4 stage; Demo::s0..s7; sub-machines 0x8010696C/0x80106AC4
- **status:** verified
- **order:** 10
- **owner:** game/scene/demo.cpp
- **notes:** s2SubMachine owned this session (last unowned title sub-machine); SBS 0-diff (see parity.py)

## title-render — logo+menu+cursor geometry
- **scope:** titleNative + shared data-driven menu emitter (menuChrome/menuItemsAndCursor/emitMenuFt4)
- **status:** verified
- **order:** 11
- **deps:** title-frontend — DEMO stage s0..s7 + menu logic
- **owner:** game/render/render_walk.cpp
- **notes:** now DATA-DRIVEN (reads guest FT4 templates via FUN_8007e1b8 reproduction) — hand-decoded constants retired; title pc-vs-psx RMSE 0.000 (no regression). Shared with s3MenuNative (#2b).

## newgame-sop-intro (#2b)
- **scope:** DEMO sm[0x48]==3 = the s3 MENU page (logos + FT4 items 0x90/0x91 + cursor). NOT SOP narration.
- **status:** verified
- **order:** 20
- **deps:** title-render — logo+menu+cursor geometry
- **owner:** game/render/render_walk.cpp (s3MenuNative + shared menuChrome/menuItemsAndCursor/emitMenuFt4)
- **notes:** BUILT + VERIFIED: data-driven menu emitter reproduces FUN_8007e1b8/FUN_80106824 reading guest templates; s3 menu pc-vs-psx RMSE 0.000, no crash at s48=3. Draw order: items then cursor (cursor on top in overlap).

## field-world (sceneNative)
- **scope:** 0x8010637C GAME field: terrain+entities+objects+backdrop, real depth
- **status:** ported-unverified
- **order:** 30
- **deps:** title-frontend — DEMO stage s0..s7 + menu logic
- **owner:** game/render/render_walk.cpp (sceneNative)
- **notes:** renders; not SBS-gated this session — add a parity entry when driven under SBS

## field-2D layer (#3b)
- **scope:** field HUD/dialog/billboards/op-0x7C sprites — the free-roam blocker
- **status:** todo
- **order:** 31
- **deps:** field-world (sceneNative)
- **owner:** -
- **notes:** Track A: 60 op-0x7C sprites need a native producer + billboard poly-count false-positive; Track B: font->queue producer (dialogTextNative landed the dialog-text slice)
