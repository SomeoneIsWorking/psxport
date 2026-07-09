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
class Game;

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

  // Wire stepModeInteract/type8Interact/type7Interact/growthYSnap/frameTick into `game`'s
  // EngineOverrides — postInteractWalk's own rec_dispatch(c, LEAF_TYPE_*) call sites are the ONLY
  // reachers of the 4 interactWalk addresses, and the native frameStartTickFaithful's
  // rec_dispatch(c, 0x8005950C) is the only core-A reacher of frameTick (the sole direct
  // func_8005950C caller is gen_func_80059D28 = the substrate frameStartTickFaithful, core-B only),
  // so EngineOverrides alone is sufficient for all 5; no shard_set_override dual-wire is needed.
  static void registerOverrides(Game* game);

private:
  // EngineOverrideFn-shaped trampolines for registerOverrides (need class access to the private
  // sub-handlers below).
  static void ov_stepModeInteract(Core* c);
  static void ov_type8Interact(Core* c);
  static void ov_type7Interact(Core* c);
  static void ov_growthYSnap(Core* c);
  static void ov_frameTick(Core* c);

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
  //   (dry land): read G+0x17E sign to pick the growth-offset constant (0x46 when grown/negative
  //   flag, else 0x8C — the SAME constants growthStep's Y-compensation uses); if G+0x84 (u16)
  //   already equals that constant, no-op; else re-snap G+0x32 = G+0x84 + (G+0x32 - constant).
  //   Ties growthStep's grow/shrink transform to Tomba's actual on-ground Y after a growth-state
  //   change settles. Faithful draft from generated/shard_0.c:1466 (ground truth, matches
  //   Ghidra 1:1). CORRECTED 2026-07-08: the original draft had the 0x46/0x8C polarity swapped —
  //   fixed by direct trace of gen_func_80022C78 (see actor_tomba.cpp).
  void growthYSnap();

public:
  // ----------------------------------------------------------------------------
  // Per-frame G-block driver (0x8005950C cascade) — RE'd 2026-07-08 wide-RE pass. See
  // docs/engine_re.md's "ActorTomba G-block" section for the full call-graph writeup of the
  // ~99-function 0x80052xxx-0x8005Fxxx region this drives, and docs/code-map.md for addresses.
  // ----------------------------------------------------------------------------
  // frameTick() — guest FUN_8005950C. Tomba's own per-frame G-block driver, called directly
  //   from the already-native Engine::frameStartTick/frameStartTickFaithful (game/core/
  //   engine.cpp, the `default: target = 0x8005950Cu` arm of its mode-keyed dispatch) whenever
  //   0x800BF870 isn't one of the 4 area-specific overlay modes (2/3/7/20). Dispatches on the
  //   OUTER state byte G+4 (0-7; >=8 is unreachable — the guest's own jump table has exactly 8
  //   entries and any value >=8 returns immediately with no-op):
  //     0 = INIT       -> enterOuterState0 (still-substrate FUN_80058648, mode=0)
  //     1 = ACTIVE     -> turnBiasCompute + FUN_80058918 (mode-N dispatch table A, still
  //                       substrate) + matrix-compose (FUN_800597AC, still substrate) +
  //                       outerTransitionCommit(mode=0)
  //     2 = COMMITTING -> FUN_80067CA4 (still substrate) + matrix-compose
  //     3 = (unused jump-table slot) -> no-op
  //     4 = ACTIVE_ALT -> same shape as case 1, but the "turn-suppress mask" pair
  //                       (0x800ECF54/0x800E7E68) is restored from 0x1F800166/0x1F800190
  //                       instead of being cleared to 0 when unpaused, dispatches
  //                       FUN_80058F5C (mode-N dispatch table B, the near-duplicate sibling of
  //                       table A) instead of table A, and just TICKS outerTransitionGate()
  //                       instead of committing a new target
  //     5,6 = SCRIPTED -> direct dispatch into already-substrate cutscene-ish leaves
  //                       0x8018BD30 / 0x8018BE40 (outside this RE region) + matrix-compose
  //     7 = LOAD-WAIT  -> a 3-state (G+5: 0/1/2) sub-machine that kicks a load (FUN_8001CF2C),
  //                       polls asset-readiness (assetReady, guest FUN_80045580), and on commit
  //                       resets to state 1 (ACTIVE) and stamps an anim-pointer's mode fields
  //                       (*0x1F800138 + 0x4C/0x4E)
  //   0x800ECF54/0x800E7E68 (the SAME "turn-suppress mask" pair beh_actor_tomba_proximity_
  //   combat's enemy-engage tables write) are save/restored around cases 1 and 4 so their
  //   per-frame masking is scoped to just the sub-dispatch each wraps. Faithful 1:1 port from
  //   gen_func_8005950C (generated/shard_4.c:7624 — ground truth; Ghidra's own decompile of
  //   this function matched it exactly, cross-checked line-by-line against the recompiled C).
  //   Guest frame: addiu sp,-32; spill s0(<-a0=G),s1,s2,ra. WIRED + SBS-VERIFIED 2026-07-09
  //   (EngineOverrides; frameStartTickFaithful's `default: rec_dispatch(c, 0x8005950Cu)` now hits
  //   the native). frameTick's OWN logic is byte-exact: PSXPORT_SBS_MODE=full autonav ran 0
  //   sbs-div / 0 VIOLATION through f15600+ (was diverging at f158 before the register-faithfulness
  //   fix — see below). The 5 drafted sub-callees below (turnBiasCompute/outerTransitionGate/
  //   outerTransitionCommit/assetReady/resetLoadGate) are NOT called from frameTick yet —
  //   line-by-line verification vs generated/shard_*.c found real transcription bugs in the first
  //   two checked (turnBiasCompute: 0x800-threshold branches swapped; frameTick's own case-1
  //   skipClear: 0x800BF80F condition inverted, fixed in frameTick). Per fleet-workflow.md §9
  //   ("drafts are untrusted"), all 5 are dispatched to the SUBSTRATE from frameTick so SBS gates
  //   only frameTick's own logic; each gets its own verify+fix+wire pass later. Every dispatch
  //   sets the gen jal-site r31 constant first so substrate callees that spill ra byte-match core B.
  //   REGISTER-FAITHFULNESS (the f158 root cause): gen keeps r17/r18 live across the case-1/4/7
  //   callees (r17=ECF54_saved/r18=E7E68_saved in cases 1,4; r17=sub/r18=1 in case 7). The case
  //   callees func_800597AC (spills r18), func_80053FDC (spills r17), func_80076D68 (spills
  //   r17+r18) read those register values via their own spill slots — so frameTick MUST write
  //   c->r[17]/c->r[18] at the same points gen does (using C++ locals alone leaves the stale
  //   caller values in the registers → the callee spills diverge). Same pattern as the
  //   NodeXform register-faithfulness fix (docs/findings/animation.md).
  void frameTick();

private:
  // ----------------------------------------------------------------------------
  // frameTick()'s immediate callees RE'd + drafted this pass (0x8005950C's direct sub-tree).
  // ----------------------------------------------------------------------------
  // turnBiasCompute(c, facing) — guest FUN_80055C9C. Frameless leaf (a0/G unused — only
  //   a1=facing heading + fixed globals DAT_800E806C/DAT_1F8000F2/DAT_800E805A). Computes a
  //   facing-vs-cached-view-heading delta, gated by a mode byte (DAT_800E806C==5 = a wide/menu
  //   variant with its own delta formula) and a "close" threshold (2048 / 2560 / 1536 depending
  //   on DAT_800E805A bit 0x800), then stamps a (turn-in, turn-out) bias-magnitude pair to
  //   0x1F80016C/0x1F80016E — the SAME turn-bias slots beh_actor_tomba_proximity_combat's
  //   enemy-engage tables also write (Tomba's own per-frame counterpart of that enemy nudge).
  //   Faithful port from gen_func_80055C9C (generated/shard_1.c:9208, ground truth) — all 3
  //   goto-chains fully traced and consolidated into one boolean; safe to restructure (unlike a
  //   dense/DAG-shaped function) because every path was hand-verified against the recompiled C.
  static void turnBiasCompute(Core* c, int16_t facing);

  // outerTransitionGate() — guest FUN_80053E50(G). The gate outerTransitionCommit (and case 4)
  //   call first: bails (false) while G+0x16E (a pending-frame counter) is still positive.
  //   Otherwise clears 0x800BF81E, runs Engine::gStateMutate(G, 0xB) (already-native), and when
  //   G+0x164==1 (a specific interaction-slot state) OR the global "busy" latch 0x800BF80D is
  //   clear, resets the walk-state (G+0=3, G+0x16E/0x170/0x146 cleared) and commits outer-state
  //   2 (G+4=2, G+5=1, G+6=0, 0x800BF80D=1) plus spawns a stop-motion task (FUN_800312D4).
  //   Returns true once any path completes handling (frameTick's callers use it purely as a
  //   bail-early check). Faithful port from gen_func_80053E50 (generated/shard_4.c:7161, ground
  //   truth). Guest frame: addiu sp,-32; spill s0,s1,s2,ra.
  bool outerTransitionGate();

  // outerTransitionCommit(mode) — guest FUN_80053FDC(G, mode). Calls outerTransitionGate() as
  //   its own first gate. If G+0x16E (pending counter) != G+0x170 (target), fires the
  //   transition cue + gStateMutate(0xB) + conditional walk-reset, then commits a NEW target
  //   (G+0x172=0x5A, G+0x170=G+0x16E) UNLESS the outer state is already 2 (mode!=1 branch only)
  //   or a "cutscene/dialogue" flag (G+0 & 0xC) is set (in which case it fires an SFX cue +
  //   spawns a task instead and returns without touching G+0). If they're already equal,
  //   decrements the settle counter G+0x172 and — on it hitting zero — either commits walk-
  //   state 1 (when G+0 != 0 AND (G+0 & 4) == 0) or re-arms the counter to 1 (G+0x172=1)
  //   otherwise. CORRECTED 2026-07-08: Ghidra's own decompile of this last branch showed
  //   `*param_1 != 2`; ground-truth gen_func_80053FDC compares against 0, not 2 (see
  //   actor_tomba.cpp for the register trace) — a real Ghidra decompiler error caught by
  //   cross-checking generated/shard_5.c:7749. Faithful port from that ground truth. Guest
  //   frame: addiu sp,-32; spill s0,s1,ra (no s2 slot — a smaller frame than
  //   outerTransitionGate's).
  void outerTransitionCommit(int32_t mode);

  // assetReady(c, slot) — guest FUN_80045580. Frameless-except-ra leaf: looks up a per-slot
  //   4-byte record at (&DAT_800BE11C)[slot] and forwards it (plus 2 fixed table pointers
  //   0x8018A000/DAT_800A3EC8) to the substrate loader-status leaf FUN_80044CD4, returning
  //   whether it reported a positive (>0) status. Faithful port from gen_func_80045580
  //   (generated/shard_6.c:6274, ground truth — matches Ghidra 1:1).
  static bool assetReady(Core* c, int32_t slot);

  // resetLoadGate(c) — guest FUN_80042310. Frameless-except-ra leaf: fires a niladic substrate
  //   cue (FUN_8001CF78), an SFX/cue trigger (FUN_80074590(0x7F,0,0)), clears the pause latch
  //   0x1F800137, then forwards the current area/mode byte (DAT_800BF870) into FUN_80074F24
  //   (an already-substrate "commit area mode" leaf). Faithful port from gen_func_80042310
  //   (generated/shard_5.c:5613, ground truth — matches Ghidra 1:1).
  static void resetLoadGate(Core* c);
};
