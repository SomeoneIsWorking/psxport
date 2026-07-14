// game/render/widescreen_margin_quad.h — native port of the widescreen-margin OT.GT4 quad
// emitter (FUN_8013CDD4, a00 overlay). See widescreen_margin_quad.cpp for the RE trace and
// docs/findings/render.md "0x8013CDD4 port — ambiguity SETTLED" for the record-layout proof.
//
// Same faithful-substrate-mirror carve-out as OverlayGt3Gt4 (overlay_gt3gt4.h): this is the
// SUBSTRATE's own GTE + OT + packet-pool writer, not pc_render. It executes underneath on both
// SBS cores and every guest write below is part of the byte-exact state SBS compares.
#pragma once
struct Core;
class  Game;

class WidescreenMarginQuad {
public:
  // FUN_8013CDD4(obj=a0) -> void. Walks obj's single margin-node + its 36-byte-stride quad
  // record array, GTE-transforms each into a POLY_GT4 packet, and links it into the OT.
  static void emit(Core* c);

  static void registerOverrides(Game* game);
};
