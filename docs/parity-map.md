# SBS parity map — is each ported unit byte-exact to recomp_path? (managed by tools/parity.py)

Durable ledger for Job #1 (byte-exact pc_faithful). One `## ` block per ported unit.
`tools/parity.py` = summary · `tools/parity.py <words>` = search · `tools/parity.py check` = gate.

**Status:** 6 verified · 2 n/a

## DEMO/title front-end (whole-scene)
- **scope:** stage 0x801062E4 boot->title menu (s48 handoff + menu hold + cursor)
- **status:** verified
- **frames:** 10890
- **gate:** PSXPORT_SBS_MODE=gameplay ... (both cores psx_render; isolates gameplay logic from render)
- **evidence:** f2d1b8af 2026-07-15
- **owner:** game/scene/demo.cpp
- **notes:** gameplay mode used because pc_render's fail-fast would abort core A on an unbuilt scene; render is guest-RAM-neutral so this is a valid logic gate

## Demo::s2SubMachine
- **scope:** 0x8010696C (title New/Load cursor sub-machine)
- **status:** verified
- **frames:** 10890
- **gate:** PSXPORT_REPL=1 PSXPORT_VK_HEADLESS=1 PSXPORT_NOAUDIO=1 PSXPORT_SBS_MODE=gameplay ./scratch/bin/tomba2_port  (drive title menu; expect A/B identical)
- **evidence:** f2d1b8af 2026-07-15 — 10890 frames 0-diff incl. cursor input; dispatch confirms override fires
- **owner:** game/scene/demo.cpp (Demo::s2SubMachine)
- **notes:** byte-exact frame 32 per abi_extract; twin of s3SubMachine

## Engine::walkStart (early-exit frame mirror)
- **status:** verified
- **frames:** 23850
- **gate:** PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_WATCH_CUT=1 (0 sbs-div through f23850) + AUTONAV=combat (0 through f7290)
- **evidence:** 1ff117a1

## field entry + scripted hold (logic)
- **scope:** stage 0x8010637C GAME field entry via hut-entry replay; guest RAM+scratchpad
- **status:** verified
- **frames:** 8220
- **gate:** PSXPORT_NOWINDOW=1 PSXPORT_VK_HEADLESS=1 PSXPORT_NOAUDIO=1 PSXPORT_SBS_MODE=gameplay PSXPORT_SBS_AUTONAV=1 PSXPORT_PAD_REPLAY=replays/scene-transitions/hut-entry-alt.pad ./scratch/bin/tomba2_port
- **evidence:** ffec2399 2026-07-15 — 8220 frames 0-diff; both cores field-rendering @f216
- **owner:** game/render/render_walk.cpp (sceneNative reads this state)
- **notes:** covers field ENTRY + scripted-caught hold (autonav did NOT reach free-roam control); free-roam + sceneNative RENDER correctness (eyeball) still uncovered — see portmap field-world

## Panel taps FUN_8004FFB4/8005019C (gen + native quad push)
- **status:** verified
- **frames:** 20970
- **gate:** SBS-full watch-cut 0-diff f20970 + combat f5970 (taps run gen bodies; push half host-only)
- **evidence:** 77b7bcdb

## ScreenFade leaf tap FUN_8007E9C8
- **status:** verified
- **frames:** 23850
- **gate:** same two legs; THUNK_FORCE_GEN A/B exonerated tap; ovhit native=32 newgame->narration
- **evidence:** 7a282422

## Font::glyphQueuePush (glyphEmit dual-emit host half)
- **status:** n/a
- **gate:** host-only queue push, zero guest writes; glyphEmit faithful body previously verified
- **evidence:** 0c711055

## Render::dialogTextNative
- **scope:** field/hut dialog TEXT producer (pc_render overlay)
- **status:** n/a
- **owner:** game/render/render_walk.cpp
- **notes:** read-only pc_render producer — writes ONLY host memory, never guest RAM; parity N/A by construction (DisplayPassGuard enforces). Correctness = USER eyeball, not SBS.
