// class ActorTomba — the PC-native Tomba actor: a NAMED-FIELD view + owned per-frame logic over
// the master G block at 0x800E7E80.
//
// Verified this pass: **G IS TOMBA'S NODE**. He is not dispatched by walkAll / walkList2 /
// walkAux — his state lives on the shared G block that pool_init sub-init FUN_8007A810 zeros
// (0x184 bytes at 0x800E7E80..0x800E8004). Per-frame he ticks via the per-area callback
// `Engine::modePerFrameDispatch` fires (seaside area 0 = `area_seaside_perframe`, which invokes
// `ActorTomba::interactWalk`).
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::actorTomba`. Back-pointer wired
// once by Core's constructor. Callers reach Tomba's methods through the object graph:
//
//     c->engine.actorTomba.interactWalk();          // per-frame aux-item interaction loop
//     c->engine.actorTomba.growthStep(mode);        // grow (1) / shrink (0) transformation
//     c->engine.actorTomba.velocityIntegrate(suppressY);  // move via dir*speed at G+0x44/48/4A/4C
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// SceneTransition, Sop, ObjectList, ParallaxBg, ScreenFade.
//
// The methods below preserve their pre-class doc-comments — see the .cpp for the full RE.
#pragma once
#include <cstdint>
class Core;

class ActorTomba {
public:
  Core* core = nullptr;

  // Guest address of Tomba's node = the master G block.
  static constexpr uint32_t G_ADDR = 0x800E7E80u;

  // ----------------------------------------------------------------------------
  // Per-frame
  // ----------------------------------------------------------------------------
  // interactWalk — guest FUN_80022760. Walks the aux render list at *0x1F800154 and dispatches
  //   per-item collision checks against G. Early-outs on the "Tomba disabled" gates
  //   (G+0x16E == 0 / 0x800BF80D != 0 / G+0x17E & 0x200). Type-keyed dispatch to
  //   `proximityCheck` (types 0/1/2/3/7), `type4GuardedCheck` (type 4 subtype 2),
  //   `subHitboxCheck` (type 6).
  void interactWalk();

  // postInteractWalk — guest FUN_801130C4. Runs immediately after `interactWalk` (default-mode
  //   branch only). Walks a DIFFERENT list — the *0x1F80013C / *0x1F800144 pair — and per item
  //   dispatches by type:
  //     item[0xC] == 9 (special) → FUN_80111304(G, item) IF !(G+0x17E & 0x8200), continue
  //     item[2] == 3             → FUN_8010E258(G, item)
  //     item[2] == 4             → detailed guarded state-transition (see cpp)
  //     item[2] == 7             → FUN_800235A0(G, item)
  //     item[2] == 8             → FUN_800205CC(G, item)
  //     item[2] in {0xF, 0x14, 0x56} → FUN_80020364(G, item, 0)
  //     item[2] == 0x13          → FUN_8010EA80(G, item)
  //     item[2] == 0x2F          → FUN_80020364(G, item, 2)
  //   Sub-handler leaves stay substrate. Ghidra decomp scratch/decomp/fun_801130c4.c.
  void postInteractWalk();

  // ----------------------------------------------------------------------------
  // Growth / shrink (Tomba's transformation state)
  // ----------------------------------------------------------------------------
  // growthStep(mode) — guest FUN_80057DC0. Toggles G+0x17E bit 0x8000 (grown flag) with a
  //   G+0x32 Y-position ±0x46 compensation so his feet stay on the ground; then rescales the
  //   bounds/physics fields at G+0xB8/BA/BC (Q12 world scale), G+0x80/82/84/86 (bounding box),
  //   G+0x62/64/66/68 (physics constants), and DAT_800E802A by `1/(mode+1)`. Called by
  //   Engine::gStateMutate cases 6 (grow, mode=1) and 7 (shrink, mode=0).
  void growthStep(int32_t mode);

  // ----------------------------------------------------------------------------
  // Movement
  // ----------------------------------------------------------------------------
  // velocityIntegrate(suppressY) — guest FUN_80056B48. Integrates a per-frame velocity into
  //   Tomba's 16.16 master position: posX += dirX*speed (G+0x2C += (s16 G+0x48)*(s16 G+0x44)),
  //   posZ similarly (G+0x34 / G+0x4C), and posY (G+0x30 / G+0x4A) unless `suppressY` is set.
  //   Tail: if flag363 (G+0x16B) == 0 AND flag97 (G+0x61) == 0, dispatch the stop/settle helper
  //   FUN_80054650(G, 0); otherwise clear flag95 (G+0x5F) bit 0x04.
  void velocityIntegrate(bool suppressY);

  // settleStep(mode) — guest FUN_80054650. The "stop/settle helper" `velocityIntegrate` tail-
  //   dispatches when Tomba isn't blocked (flag363 & flag97 clear). Sets DAT_1F800258 = 0, clears
  //   G+0x5F bit 0x04, and if G+0x16B (flag363) is 0 runs a two-way grid probe (FUN_8004954C —
  //   substrate) with a `sVar3 = probe_offset / 2` derived from G+0x62 or from the item hooked
  //   at G+0x10 (obj+0x86 - obj+0x84 - (G+0x32 - obj+0x32)). On probe hit stamps G+0x60=1 and
  //   G+0x5F = (flag from G+0x149 bit 0x4 → G+0x147 else G+0x149&1) + 4, returns 1. Sets DAT_
  //   1F800258 marks a "sink" fallback that flips G+0x5F to (5 - G+0x147). Returns 1 on hit else 0
  //   (via c->r[2]). Ghidra decomp scratch/decomp/footstep_hunt.c.
  uint32_t settleStep(int32_t mode);

  // postFrameWaterCheck() — guest FUN_8010E904 (final call in area_seaside_perframe).
  //   Water/sea gating: if seaside water mode 0x800BF816 is engaged, snap Tomba's Y (G+0x32) to
  //   the water surface (0x800BF812 minus G+0x62); otherwise, if not paused, run FUN_8010E408(G)
  //   (substrate). Also fires the area-exit trigger (0x1F800137 / 0x800BF80F / 0x800BF83A /
  //   0x800BF839 / 0x1F800236) when Tomba is off-map (G+0x32 < -0xE74 AND G+0x36 < 0x1451) in
  //   water-mode 2. NO SFX. Ghidra decomp scratch/decomp/tomba_postframe_10e904.c.
  void postFrameWaterCheck();

private:
  // Sub-handlers of interactWalk — kept private since the type-dispatch loop is the only caller.
  void proximityCheck    (uint32_t item);     // FUN_80022060
  void type4GuardedCheck (uint32_t item);     // FUN_80114E74
  void subHitboxCheck    (uint32_t item);     // FUN_80022190

  // ----------------------------------------------------------------------------
  // Sub-handlers of postInteractWalk (band 0x80020000-0x8002FFFF; RE'd + drafted 2026-07-08,
  // UNWIRED — postInteractWalk's rec_dispatch call sites for these 4 leaves are left in place;
  // wiring is a future EngineOverrides/shard_set_override step, not done by this draft).
  // ----------------------------------------------------------------------------

  // stepModeInteract(item, mode) — guest FUN_80020364. postInteractWalk case 0xF/0x14/0x56
  //   (mode=0) and case 0x2F (mode=2). Guest frame: addiu sp,-40 / spill s0,s1,s2,s3,ra.
  //   Gate: return 0 if G+0x17E & 0x200 (paused). Else FUN_8001F40C(G,item,a2=1) (shared
  //   proximity+step leaf, same one case-4's type4GuardedCheck-adjacent LEAF_TYPE_4_PROX_STEP
  //   uses) — if result v0 < 0, no hit (return 0). If G+0x144==1 && v0<2 ("just transitioned"
  //   state): when G+0x17E bit 0x8000 (grown) is CLEAR, call FUN_8001FDB4(item,1,0x10,0x20)
  //   (alt-tag stamp) then return 1; when SET, call FUN_8001F054(G,item) and return 1 only if
  //   (mode & 3) != 0, else fall through to return 1 with no call. Else (steady-state): if
  //   (mode & 0x3F) != 0, rotate G/item apart along the FUN_8001F40C heading using
  //   Trig::rsin/rcos(0x1F80009C-cached heading) scaled by (G+0x80 + item+0x80) — mirrors
  //   proximityCheck's own trig-offset shape; which side moves depends on (mode & 0x7F)==1 vs
  //   (G[0] & 4)==0. Then per (mode & 0x40)/(mode & 0x80) bit pattern: either a plain "return 2"
  //   push-apart, a state-machine bump into G[5]=0x13 (returns 2 or 3), or (mode&0x40 set) an
  //   announcer-cue + G-state stamp (0/2/3/6/0x172/0x173/0x2B) returning 4. Faithful draft from
  //   generated/shard_7.c:1379 (ground truth) + Ghidra scratch/decomp/region_8002.c.
  uint8_t stepModeInteract(uint32_t item, uint32_t mode);

  // type8Interact(item) — guest FUN_800205CC. postInteractWalk case 8. Guest frame:
  //   addiu sp,-32 / spill s0,s1,s2,ra (s1=G, s2=item — mapped from a0/a1 at entry). Gate:
  //   if item[0]==5: fire FUN_8001F830() (a niladic substrate cue) when !(G+0x17E&0x200) and
  //   G+0x78==0. Else if G+0x17E bit 0x8000 set: delegate whole-hog to FUN_8001EC3C(G,item)
  //   (grown-state variant, fully substrate). Else: FUN_8001F40C(G,item,a2=0) proximity/step;
  //   v0<0 -> no hit. If item[0]==1: on a "just-triggered" transition (G+0x144==1 && v0<2) push
  //   item via FUN_8001FDB4(item,-2,3,0x1E); else (not paused) branch on (v0&1): even -> rotate
  //   G onto item using the same rsin/rcos(heading)*[G+0x80,item+0x80] trig-offset shape as
  //   proximityCheck, stamp G+0x60=1 and G+0x5F=Trig::angleCmp(heading,G+0x140,1)+2; odd (v0==1,
  //   item[0x145]&1==0) -> reset G's walk-state fields (0x145/0x4A/0x4B/0x50/0x51/0x148=0,
  //   0x29=1) and re-derive G+0x32 from item+0x84/0x32/0x2E, then (if G+0x78==0 and
  //   DAT_800BF816==0) snap G+0x32 by ±0x46/±0x8C per the G+0x17E sign — the SAME growth-offset
  //   snap `growthYSnap` performs, inlined here for the "just left growth transition" case.
  //   Else (item[0]!=1 && !=5, gate !(G+0x17E&0x200) && (item[0x145]&1)==0): stamp item+0x29=1.
  //   Faithful draft from generated/shard_0.c:1112 (ground truth) + Ghidra scratch/decomp/
  //   region_8002.c.
  void type8Interact(uint32_t item);

  // type7Interact(item) — guest FUN_800235A0. postInteractWalk case 7. Guest frame:
  //   addiu sp,-32 / spill s0,s1,ra (s0=G, s1=item). FUN_8001F40C(G,item,a2=1) proximity/step;
  //   if v0 < 0, return 0 (no hit). Else pick a FUN_8001FF7C(G,item,mode,flag) call: mode is
  //   always `item` (a1); flag is 4 when G+0x164==0x0C else 1 — matches postFrameWaterCheck's
  //   own "0x0C special state" convention. Returns 1.
  //   Faithful draft from generated/shard_4.c:1267 (ground truth, matches Ghidra 1:1).
  uint8_t type7Interact(uint32_t item);

  // growthYSnap() — guest FUN_80022C78 (= postFrameWaterCheck's LEAF_WATER_SPLASH). Leaf, no
  //   guest-stack frame. Operates on G only (a0=G): stamp G+0x29=1, G+0x145=0, G+0x4A=0 (u16),
  //   G+0x50=0 (u16), G+0x148=0 (walk/collision-frame reset — same fields type8Interact's
  //   "just left growth" branch also clears). If G+0x78==0 (not frozen) AND DAT_800BF816==0
  //   (dry land): read G+0x17E sign to pick the growth-offset constant (0x8C when grown/negative
  //   flag, else 0x46 — the SAME constants growthStep's Y-compensation uses); if G+0x84 (u16)
  //   already equals that constant, no-op; else re-snap G+0x32 = G+0x84 + (G+0x32 - constant).
  //   Ties growthStep's grow/shrink transform to Tomba's actual on-ground Y after a growth-state
  //   change settles. Faithful draft from generated/shard_0.c:1466 (ground truth, matches
  //   Ghidra 1:1).
  void growthYSnap();
};
