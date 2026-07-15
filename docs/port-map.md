# RE + port frontier — is each step REAL (re-verified) or a HACK? (managed by tools/portmap.py)

The RE dependency chain. `## ` block per step. Work `portmap.py next`; kill `portmap.py hacks`.
Detail lives in docs/port-progress.md; this is the queryable real-vs-hack frontier.

**Status:** 2 verified · 1 ported-unverified · 2 todo

## title-frontend — DEMO stage s0..s7 + menu logic
- **scope:** 0x801062E4 stage; Demo::s0..s7; sub-machines 0x8010696C/0x80106AC4
- **status:** verified
- **order:** 10
- **owner:** game/scene/demo.cpp
- **notes:** s2SubMachine owned this session (last unowned title sub-machine); SBS 0-diff (see parity.py)

## title-render — logo+menu+cursor geometry
- **scope:** titleNative: FUN_80106690 logos + FUN_80106824 items/cursor via FUN_8007e1b8 templates
- **status:** verified
- **order:** 11
- **deps:** title-frontend — DEMO stage s0..s7 + menu logic
- **owner:** game/render/render_walk.cpp (titleNative)
- **notes:** geometry RE-verified against dumped emitters (items templates 0x8e/0x8f, cursor 0x98 + table 0x80107704); prior 'empirical' constants proven exact

## newgame-sop-intro (#2b)
- **scope:** DEMO sm[0x48]==3 = Demo::s3 MENU (logos + FT4 templates 0x90/0x91 + cursor), NOT the SOP narration
- **status:** todo
- **order:** 20
- **deps:** title-render — logo+menu+cursor geometry
- **owner:** -
- **notes:** RE DONE (findings/render.md '#2b'): it's a 2nd menu page structurally identical to titleNative; positions/widths validated, ground truth captured (ram_s3menu.bin, s3menu_ref.ppm). REMAINING: decode FUN_8007e1b8 UV/clut (calibrate on 0x8e), build s3-menu producer, verify by pc-vs-psx pixel-diff. Do NOT route to sceneNative/renderSopNarration.

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
