# SBS parity map — is each ported unit byte-exact to recomp_path? (managed by tools/parity.py)

Durable ledger for Job #1 (byte-exact pc_faithful). One `## ` block per ported unit.
`tools/parity.py` = summary · `tools/parity.py <words>` = search · `tools/parity.py check` = gate.

**Status:** 2 verified · 1 n/a

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

## Render::dialogTextNative
- **scope:** field/hut dialog TEXT producer (pc_render overlay)
- **status:** n/a
- **owner:** game/render/render_walk.cpp
- **notes:** read-only pc_render producer — writes ONLY host memory, never guest RAM; parity N/A by construction (DisplayPassGuard enforces). Correctness = USER eyeball, not SBS.
