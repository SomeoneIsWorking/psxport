// game/render/hud_gauge_emitter.h — native port of the HUD gauge emitter, FUN_8004FD30 (frame
// entry) + FUN_8004FB4C (per-item leaf). Self-contained: reads only the fixed HUD-gauge table at
// 0x800BF548 and emits its packets straight into the shared GPU packet pool. See
// hud_gauge_emitter.cpp for the RE trace and docs/findings/render.md's census note ("(2) FUN_
// 8004FD30+FUN_8004FB4C (self-contained HUD gauge emitter, loop identity = 0x800BF548 stride-0x8C
// index)").
//
// Same faithful-substrate-mirror carve-out as WidescreenMarginQuad/OverlayGt3Gt4: this is the
// SUBSTRATE's own packet-pool + OT writer, not pc_render. Every guest write below is part of the
// byte-exact state SBS compares.
#pragma once
struct Core;
class  Game;

class HudGaugeEmitter {
public:
  // FUN_8004FD30() -> void. Frame entry: draws the two fixed HUD viewport/panel DR_AREA clip
  // rects, walks the 0x800BF548 gauge-item table (stride 0x8C, count at +8) calling emitItem()
  // per record.
  static void emitFrame(Core* c);

  // FUN_8004FB4C(record=a0) -> void. Per-item leaf: forwards two sub-descriptor fields to the
  // still-substrate segment-layout leaf FUN_8004EB94, optionally emits a per-item DR_AREA clip
  // box, then hands off to the still-substrate digit/label leaf FUN_8005019C.
  static void emitItem(Core* c);

  static void registerOverrides(Game* game);
};
