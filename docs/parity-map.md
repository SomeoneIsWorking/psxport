# SBS parity map — is each ported unit byte-exact to recomp_path? (managed by tools/parity.py)

Durable ledger for Job #1 (byte-exact pc_faithful). One `## ` block per ported unit.
`tools/parity.py` = summary · `tools/parity.py <words>` = search · `tools/parity.py check` = gate.

**Status:** 19 verified · 5 partial · 3 n/a

## ActorTomba::actionHandler800531DC (FUN_800531DC)
- **status:** verified
- **frames:** 27150
- **gate:** port_check PASS + MIRROR_VERIFY OK + 0 sbs-div; combat + watch-cut f27150
- **evidence:** 58809b1c

## ActorTomba::actionHandler800588BC (FUN_800588BC)
- **status:** verified
- **frames:** 21390
- **gate:** port_check PASS + MIRROR_VERIFY OK + 0 sbs-div; combat + watch-cut f21390
- **evidence:** 71b5d764

## ActorTomba::actionHandler8005ACC8 (FUN_8005ACC8)
- **status:** verified
- **frames:** 20040
- **gate:** port_check PASS + MIRROR_VERIFY OK (469 hits) + 0 sbs-div; combat f5700 + watch-cut f20040
- **evidence:** 2a94898c

## ActorTomba::actionHandler8005AEE4 (FUN_8005AEE4)
- **status:** verified
- **frames:** 21390
- **gate:** port_check PASS + MIRROR_VERIFY OK + 0 sbs-div; combat + watch-cut f21390
- **evidence:** 71b5d764

## ActorTomba::enterOuterState0 (FUN_80058648)
- **status:** verified
- **frames:** 19740
- **gate:** MIRROR_VERIFY pass#1 OK + combat clean-exit 0-diff f4000 + watch-cut 0-diff f19740
- **evidence:** c47d3690

## ActorTomba::matrixComposeAttached (FUN_800597AC)
- **status:** verified
- **frames:** 18900
- **gate:** 11713 MIRROR_VERIFY passes + 0 sbs-div/6000 combat frames; watch-cut 0-diff f18900
- **evidence:** 537dac98

## ActorTomba::mode0ActionGate (FUN_8005A910)
- **status:** verified
- **frames:** 20580
- **gate:** MIRROR_VERIFY OK all invocations + 0 sbs-div; combat 0-diff + watch-cut f20580
- **evidence:** 0bb8cb9d

## ActorTomba::mode0WalkHandler (FUN_8005A970)
- **status:** verified
- **frames:** 20040
- **gate:** port_check PASS + MIRROR_VERIFY OK + 0 sbs-div; combat 0-diff f5400 + watch-cut f20040
- **evidence:** d4ace056

## Core::guestMemset (FUN_8009A420)
- **status:** verified
- **frames:** 18120
- **gate:** same wave gate; ovhit 2247/2247; §9 n<=0 return-0 fix
- **evidence:** 7db286a8

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

## Dialog glyph tap FUN_8007CC00 (Panel::pushDialogGlyphs)
- **status:** verified
- **frames:** 19590
- **gate:** SBS-full combat f5460 + watch-cut f19590 0-diff; hut replay bubble identical via tap (panelq box=800EEA60 count=17)
- **evidence:** 916ddfc0

## Engine::padEdgeFence (FUN_800788AC)
- **status:** verified
- **frames:** 18120
- **gate:** combat clean-exit 0-diff f4500 + ovhit 4500/4500; watch-cut 0-diff f18120
- **evidence:** 7db286a8

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

## framework-agnostic-p1.7c
- **status:** verified
- **frames:** 450
- **gate:** PSXPORT_SBS_MODE=full PSXPORT_VK_HEADLESS=1 PSXPORT_AUTO_SKIP=1 ./scratch/bin/tomba2_port
- **evidence:** 58fc5f76

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

## TileGridLayer scrollStep+emit (0x8011534C/0x80115598)
- **status:** verified
- **frames:** 20820
- **gate:** SBS-full combat 0-diff f5850 + watch-cut 0-diff f20820; ovhit native=oracle=3878 both addrs
- **evidence:** cd278ce4

## ActorTomba::actionHandler8005EF48 (FUN_8005EF48)
- **status:** partial
- **gate:** port_check equivalence (verbatim) + wired 0-diff; mode unreached by autonav — runtime-unexercised
- **evidence:** 58809b1c

## ActorTomba::actionHandler8005F1B0 (FUN_8005F1B0)
- **status:** partial
- **gate:** port_check equivalence (verbatim) + wired 0-diff, but mode unreached by combat/field/cutscene autonav — runtime-unexercised
- **evidence:** 71b5d764

## ActorTomba::actionHandler800660AC (FUN_800660AC)
- **status:** partial
- **gate:** port_check equivalence (verbatim) + wired 0-diff; mode unreached by autonav — runtime-unexercised
- **evidence:** 58809b1c

## Gauge text-row tap FUN_8004EB94
- **status:** partial
- **gate:** registered, 2-leg 0-diff, but ovhit 0/0 (no gauge item in autonav) — host push math unexercised; needs gauge-popping drive + USER eyeball
- **evidence:** 7a48eb15

## Icon glyph tap FUN_80078988
- **status:** partial
- **gate:** 2-leg 0-diff with tap registered; icon strings not exercised in autonav legs — needs an icon-showing drive + USER eyeball
- **evidence:** 916ddfc0

## Billboard picture dual-emit (rq_push_ft4_record @ billboardEmit/submitQuad)
- **status:** n/a
- **gate:** host-only queue push (zero guest writes); 2-leg 0-diff f5520/f20610 with pushes live; USER play-test pending (#65)
- **evidence:** 8988b389

## Font::glyphQueuePush (glyphEmit dual-emit host half)
- **status:** n/a
- **gate:** host-only queue push, zero guest writes; glyphEmit faithful body previously verified
- **evidence:** 0c711055

## Render::dialogTextNative
- **scope:** field/hut dialog TEXT producer (pc_render overlay)
- **status:** n/a
- **gate:** RETIRED 916ddfc0 — superseded by the FUN_8007CC00 tap
- **evidence:** 916ddfc0
- **owner:** game/render/render_walk.cpp
- **notes:** read-only pc_render producer — writes ONLY host memory, never guest RAM; parity N/A by construction (DisplayPassGuard enforces). Correctness = USER eyeball, not SBS.
