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
//     eng(c).actorTomba.interactWalk();          // per-frame aux-item interaction loop
//     eng(c).actorTomba.growthStep(mode);        // grow (1) / shrink (0) transformation
//     eng(c).actorTomba.velocityIntegrate(suppressY);  // move via dir*speed at G+0x44/48/4A/4C
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

  // Wire stepModeInteract/type8Interact/type7Interact/growthYSnap/frameTick into the global
  // override registry — postInteractWalk's own rec_dispatch(c, LEAF_TYPE_*) call sites are the
  // ONLY reachers of the 4 interactWalk addresses, and the native frameStartTickFaithful's
  // rec_dispatch(c, 0x8005950C) is the only core-A reacher of frameTick (the sole direct
  // func_8005950C caller is gen_func_80059D28 = the substrate frameStartTickFaithful, core-B only),
  // so a rec_dispatch-only registration (no shard_set_override setter) is sufficient for all 5.
  static void registerOverrides(Game* game);

private:
  // EngineOverrideFn-shaped trampolines for registerOverrides (need class access to the private
  // sub-handlers below).
  static void ov_stepModeInteract(Core* c);
  static void ov_type8Interact(Core* c);
  static void ov_type7Interact(Core* c);
  static void ov_growthYSnap(Core* c);
  static void ov_frameTick(Core* c);
  static void ov_turnBiasCompute(Core* c);
  static void ov_outerTransitionGate(Core* c);
  static void ov_outerTransitionCommit(Core* c);
  static void ov_assetReady(Core* c);
  // shard_set_override setter trampolines (psx_fallback-gated — see .cpp banner).
  static void gov_turnBiasCompute(Core* c);
  static void gov_outerTransitionGate(Core* c);
  static void gov_outerTransitionCommit(Core* c);
  static void gov_assetReady(Core* c);
  static void gov_matrixComposeAttached(Core* c);
  static void gov_enterOuterState0(Core* c);
  static void gov_mode0ActionGate(Core* c);
  static void gov_mode0WalkHandler(Core* c);
  static void gov_actionHandler8005ACC8(Core* c);
  static void gov_actionHandler8005AEE4(Core* c);
  static void gov_actionHandler8005F1B0(Core* c);
  static void gov_actionHandler800588BC(Core* c);
  static void gov_actionHandler800531DC(Core* c);
  static void gov_actionHandler800660AC(Core* c);
  static void gov_actionHandler8005EF48(Core* c);

  // Four unowned per-frame leaves ported byte-faithfully 2026-07-17 (guest ABI: a0=r4 etc; body
  // reads/writes c->r[] directly). gov_ trampolines wire them via engine_set_override_main.
  void proximityAngleWalk(Core* c);      // FUN_80053968 — aux-list angle-window proximity walk (frame 56)
  void limbFrameLoad(Core* c);           // FUN_80054790 — per-state limb frame-offset loader (frameless)
  void invincibilityFlashStep(Core* c);  // FUN_80060268 — invincibility/hit blink-flash cadence (frameless)
  void rampOffsetStep(Core* c);          // FUN_80063098 — +32 ramp folded into a0+66/+86, spawn at <1025 (frame 24)
  static void gov_proximityAngleWalk(Core* c);
  static void gov_limbFrameLoad(Core* c);
  static void gov_invincibilityFlashStep(Core* c);
  static void gov_rampOffsetStep(Core* c);

  // Sub-handlers of interactWalk — kept private since the type-dispatch loop is the only caller.
  void proximityCheck    (uint32_t item);     // FUN_80022060
  void type4GuardedCheck (uint32_t item);     // FUN_80114E74
  void subHitboxCheck    (uint32_t item);     // FUN_80022190

  // ----------------------------------------------------------------------------
  // Sub-handlers of postInteractWalk (band 0x80020000-0x8002FFFF; RE'd + drafted 2026-07-08,
  // UNWIRED — postInteractWalk's rec_dispatch call sites for these 4 leaves are left in place;
  // wiring is a future override-registry step, not done by this draft).
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
  //   (override registry; frameStartTickFaithful's `default: rec_dispatch(c, 0x8005950Cu)` now hits
  //   the native). frameTick's OWN logic is byte-exact: PSXPORT_SBS_MODE=full autonav ran 0
  //   sbs-div / 0 VIOLATION through f15600+ (was diverging at f158 before the register-faithfulness
  //   fix — see below). Of the 5 drafted sub-callees below, 4 (turnBiasCompute/outerTransitionGate/
  //   outerTransitionCommit/assetReady) are now ALSO wired+verified (2026-07-10 §9 promotion pass —
  //   see docs/findings/animation.md for the bugs that pass found: a MIPS branch-delay-slot misread
  //   in turnBiasCompute, a wrong gate constant in outerTransitionCommit, 7 missing r31 mirrors)
  //   and reached through their own overrides::install + engine_set_override_main registrations —
  //   frameTick's `rec_dispatch(c, addr)` call sites below are unchanged and pick them up
  //   automatically. resetLoadGate remains UNWIRED (dispatched to substrate, out of that pass's
  //   scope). Every dispatch sets the gen jal-site r31 constant first so substrate/native callees
  //   that spill ra byte-match core B.
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
  //   state 1 (when G+0 != 2 AND (G+0 & 4) == 0) or re-arms the counter to 1 (G+0x172=1)
  //   otherwise. RE-CORRECTED 2026-07-10 (§9 promotion pass): the 2026-07-08 draft's "CORRECTED"
  //   note below was itself wrong — it claimed ground-truth gen_func_80053FDC compares g0 against
  //   0 (reverting what it called a Ghidra decompiler error showing `!= 2`), but a fresh line-by-
  //   line re-diff of generated/shard_5.c:7749 (`r3=u8(G+0); if(r3==2) goto rearm;` — a literal
  //   constant 2, unambiguous in the recompiler's own C, not a Ghidra artifact) plus an SBS-
  //   confirmed fix (0-diff only after reverting to `!= 2`) proves gen genuinely gates on the
  //   literal 2, not 0: gen never special-cases g0==0 and DOES special-case g0==2. The pre-existing
  //   comment inverted this while "fixing" a Ghidra mismatch that wasn't actually wrong. See
  //   docs/findings/animation.md "turnBiasCompute/.../outerTransitionCommit ... §9 promotion"
  //   for the full writeup. Faithful port from ground truth. Guest frame: addiu sp,-32; spill
  //   s0,s1,ra (no s2 slot — a smaller frame than outerTransitionGate's).
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

  // ----------------------------------------------------------------------------
  // 2026-07-10 wide-RE dedicated pass — frameTick's two "too large for the first pass" direct
  // callees (docs/engine_re.md's 0x8005950C "Mapped-only" note). Both faithful 1:1 ports, UNWIRED
  // (frameTick's own rec_dispatch call sites for 0x80058648/0x800597AC still reach the substrate —
  // wiring is a future frontier-tier step). Both kept as LITERAL register-level transcriptions
  // (goto/label-preserving, not restructured into named C++ control flow) per fleet-workflow.md §9
  // — see the .cpp banners for concrete restructuring bugs this caught during drafting.
  // ----------------------------------------------------------------------------

  // enterOuterState0(mode) — guest FUN_80058648(G, mode). frameTick's case-0 (INIT) handler:
  //   (re)allocates Tomba's 17-record attach array via GraphicsBind::recordArrayInit (already
  //   native), resets a large block of per-frame G scratch fields, dispatches the still-substrate
  //   FUN_800682C4/FUN_80057FD4 plus the already-native growthStep, and — when mode==0 — an
  //   indirect jump-table dispatch keyed by DAT_800BF870 through one of two 32-entry tables
  //   (0x800A45B8 / 0x800C45B8; table CONTENTS not yet enumerated — a follow-up dedicated pass).
  //   Faithful from generated/shard_7.c:7739. Guest frame: addiu sp,-32; spill s0(G),s1,s2(mode),ra.
  void enterOuterState0(int32_t mode);

  // matrixComposeAttached() — guest FUN_800597AC(G). frameTick's matrix-compose tail (all cases):
  //   composes Tomba's own SRT, then — when G+0x8 (attach count) is nonzero — a per-attached-item
  //   pass over G+0xC0[i*4] (the SAME array GraphicsBind::recordArrayInit populates) applying each
  //   item's local rotation onto G's (or an "attach B" variant) matrix and accumulating the
  //   translate into item+0x2C/30/34. Faithful from generated/shard_5.c:8654. Guest frame:
  //   addiu sp,-64; spill r16-r23,r30(scratch, not fp),ra.
  void matrixComposeAttached();
  void mode0ActionGate();            // FUN_8005A910 — mode-0 (walk) action gate; picks FUN_8005A970 vs swim 0x80112B50
  void mode0WalkHandler();            // FUN_8005A970 — mode-0 normal walk handler (PORT_GEN verbatim, wired via 8005A910)
  void actionHandler8005ACC8();       // FUN_8005ACC8 — mode-N action handler (PORT_GEN verbatim)
  void actionHandler8005AEE4();       // FUN_8005AEE4 — mode-N action handler (PORT_GEN verbatim)
  void actionHandler8005F1B0();       // FUN_8005F1B0 — mode-N action handler (PORT_GEN verbatim)
  void actionHandler800588BC();       // FUN_800588BC — mode-N action leaf (PORT_GEN verbatim)
  void actionHandler800531DC();       // FUN_800531DC — mode-N action leaf (PORT_GEN verbatim)
  void actionHandler800660AC();       // FUN_800660AC — mode-N action leaf (PORT_GEN verbatim)
  void actionHandler8005EF48();       // FUN_8005EF48 — mode-N action handler (PORT_GEN verbatim)

  // ----------------------------------------------------------------------------
  // 2026-07-10 wide-RE pass — mode-N dispatch table A/B (FUN_80058918/FUN_80058F5C) case-target
  // cluster 0x80060064-0x80065374 (docs/engine_re.md's "NOT yet triaged" 10-address band). Both
  // tables pass G (r4) to every case target, so ALL of these read/write G's own fields — but as a
  // SEPARATE per-case sub-state machine keyed off G+0x6 (0/1/2/3...), distinct from the outer
  // G+0x5 mode selector that chose which of these got called. Field semantics past G+0x6/G+0x7
  // (the sub-state counter itself) are NOT derived this pass — offsets only, honestly unnamed.
  // FAITHFUL DRAFTS, UNWIRED, literal register-level transcription (goto/label-preserving) per
  // fleet-workflow.md §9. Every rec_dispatch callee target is a still-substrate leaf.
  // ----------------------------------------------------------------------------

  // caseAreaEntryHook_80065374() — guest FUN_80065374(G unused; frame-only). Table A/B case
  //   target. Reads NOT G but the current-area-id global DAT_800BF870 and fires exactly one of
  //   three area-specific one-shot hooks by area id, no-op for any other area:
  //     area==0 -> rec_dispatch(0x8010AECC)   area==5 -> rec_dispatch(0x80110CB8)
  //     area==6 -> rec_dispatch(0x80113E3C)   else    -> no-op
  //   All three targets are outside MAIN.EXE's own 0x80058xxx-0x80066xxx band (0x8010xxxx/
  //   0x8011xxxx), consistent with per-area overlay entry points. Faithful from
  //   generated/shard_2.c:7999. Guest frame: addiu sp,-24; spill ra only (no callee-saved regs —
  //   param in r4 is loaded but never read).
  void caseAreaEntryHook_80065374();

  // caseArea0EntryHook_800653F4(G) — guest FUN_800653F4(G). Table A/B case target. A 2-state
  //   sub-FSM on G+0x6: state 0 -> one-shot init (SceneTransition::resetSwap-style clear via
  //   rec_dispatch(0x80054198) i.e. func_80054198(G), then func_80054D14(G,64,0)), clears G+0x7,
  //   advances G+0x6 to 1, falls through; state 1 -> every call, if the current-area-id global
  //   DAT_800BF870 == 0, fires rec_dispatch(0x8010C780)(G) (area-0-specific hook, same overlay
  //   band as caseAreaEntryHook_80065374's targets) — does NOT itself advance G+0x6 further, so
  //   (per this function alone) it re-fires every frame while area==0 unless 0x8010C780 changes
  //   G+0x6 itself (unconfirmed — that callee is still substrate); state >=2 -> no-op.
  //   Faithful from generated/shard_3.c:16208. Guest frame: addiu sp,-24; spill s0(G),ra.
  void caseArea0EntryHook_800653F4(uint32_t G);

  // caseModeFsm_800620D0(G) — guest FUN_800620D0(G). Table A/B case target. 4-state sub-FSM on
  //   G+0x6 (values 0..3) with a 16-bit countdown timer at G+0x40 (offset 64):
  //     state 0 -> one-shot setup (clear G+0x146/326, resetSwap, func_80054D14(G,225,4), an SFX
  //       cue func_80074590(57,0,0), timer=30, advance to state 1)
  //     state 1 -> per-call tick: func_80055D5C(G), timer += 8 written back at G+0x32(50)... NOTE:
  //       this state's own body ALSO touches G+0x32/0x50 fields via func_8005444C/func_80056C00 —
  //       not independently confirmed field names, offsets only; on timer (G+0x40, signed 16-bit)
  //       hitting <=0: if G+0x29(41)==0 clears G+0x5/0x6 (returns FSM to idle) else calls
  //       func_80056D44(G,0); otherwise leaves state unchanged.
  //     state 2 (from table dispatch value 2, label L_800621E4) -> ANOTHER one-shot setup variant
  //       (clears byte at 0x800BF80E(-2034 off 0x800C0000), same resetSwap/func_80054D14/cue
  //       sequence with DIFFERENT operand (225,4 reused) then timer=30, G+0x6++) — note this
  //       branch is reached when G+0x6==2 per the case-target dispatch value 3 (see cpp for the
  //       exact branch-vs-label mapping, preserved literally from gen).
  //     state 3 -> per-call countdown at G+0x40 only; on it hitting exactly 0, writes G+0(byte at
  //       -2034) = 1, sets G+0x4=4, G+0x5=32, clears G+0x6/0x7 (re-idles).
  //   Faithful from generated/shard_4.c:8288. Guest frame: addiu sp,-32; spill s0(G),s1,ra.
  void caseModeFsm_800620D0(uint32_t G);

  // caseModeFsm_80061A7C(G) — guest FUN_80061A7C(G). Table A/B case target. Same G+0x6 sub-FSM
  //   shape as caseModeFsm_800620D0 (states 0..3, timer G+0x40) but with a companion 16-bit field
  //   at G+0x42(66) used as a secondary sub-timer/flag in states 2/3, plus a DAT_800BF87B(123)/
  //   DAT_800BF86A(14)-ish global-table decrement gated on G+0x42 bit0 in state 2's tail, and a
  //   direct re-entrant self-write at the end (writes G+0x0=3, G+0x172(370)=20, G+0x5/6/7=0, plus
  //   clearing byte 0x800BF7D9(-2039)) when both timers hit specific end conditions. Field
  //   semantics past the state byte are NOT derived this pass. Faithful from
  //   generated/shard_2.c:7732. Guest frame: addiu sp,-24; spill s0(G),ra.
  void caseModeFsm_80061A7C(uint32_t G);

  // caseModeFsm_80060064(G) — guest FUN_80060064(G). Table A/B case target. A DIFFERENT-shaped
  //   sub-FSM: outer switch on G+0x6 in {0,1,else}; state 0 branches AGAIN on G+0x7 in {0,1,else}:
  //     G+0x6==0,G+0x7==0 -> one-shot init (clear G+0x145/326, an animation-ish call
  //       func_80055E28(G,1), func_80055D5C(G), func_80054D14(G,22,3), advance G+0x7)
  //     G+0x6==0,G+0x7==1 -> func_80055E28(G,1)/func_80055D5C(G)/func_80076D68(G) (this last one
  //       is ActorTomba::frameTick's own leaf per its header comment — return value compared
  //       against G+0x7's PRE-increment value) then, if they match, a big committing block:
  //       clears G+0x7, sets G+0x145=(the matched value), G+0x50(80)=0, advances G+0x6, fires two
  //       SFX cues func_80074590(15,0,0)/func_80074590(37,0,0), func_80054D14(G,18,4),
  //       func_800538E0(G,G+44,0), func_80055F48(G,0), func_80055844(G) — then branches on that
  //       LAST call's return value to adjust G+0x4A(74) by -1408 or reload it from
  //       byte@G+0x165(357), finishing with a signed-divide-by-4-ish reshape of G+0x4A
  //       (`x + (x<<16>>18)`, i.e. `x + x/4` on the sign-extended 16-bit value).
  //     G+0x6==1 -> func_80055E28(G,1), func_80055FBC(G,byte@G+0x14A(330)), func_80056B48(G,1),
  //       func_80055D5C(G), func_80076D68(G); compares result to G+0x145(325): if ==2, calls
  //       func_800574E0(G,0) else func_800574E0(G,17), then func_80057C08(G,1-or-0).
  //   ALL paths tail-call func_800551C4(G) unconditionally before return (label L_8006024C is a
  //   shared tail every branch funnels through). Field semantics past the state bytes NOT derived
  //   this pass. Faithful from generated/shard_3.c:15659. Guest frame: addiu sp,-32; spill
  //   s0(G),s1,ra.
  void caseModeFsm_80060064(uint32_t G);

  // ----------------------------------------------------------------------------
  // 2026-07-10 wide-RE pass #2 — the 4 mapped-not-drafted leftovers of the 60064-65374 cluster.
  // Same table A/B case-cluster as above (G in r4, per-G sub-FSM on G+0x6). Two of the four
  // (0x800624B4 / 0x80060C60) are NOT leaves — each is itself a nested dispatch layer indexing a
  // small `.rodata` function-pointer array and switching on the loaded target. FAITHFUL DRAFTS,
  // UNWIRED, per fleet-workflow.md §9. See docs/engine_re.md's "third dispatch layer map" section
  // for the full case-target enumeration of both nested tables.
  // ----------------------------------------------------------------------------

  // caseModeFsm_8006228C(G) — guest FUN_8006228C(G). Table A/B case target. Same G+0x6 4-state
  //   sub-FSM shape as caseModeFsm_800620D0/80061A7C (states 0..3 gated by <2/==2/==3), but state 1
  //   is denser: every call does func_80055FBC(G, byte@G+327), func_80076D68(G), func_80056B48(G,0),
  //   func_80055D5C(G), G+0x32(50)+=8 then func_8005444C(G); if byte@G+0x29(41)!=0, decrements the
  //   16-bit G+0x40(64) timer and, when it lands on exactly the value whose low 16 bits sign-extend
  //   to 0 (`(timer<<16)==0`, i.e. timer wrapped through 0), fires func_8005A714(G) once; always
  //   tail-calls func_80056C00(G,1) and func_800551C4(G). It then independently ticks a SEPARATE
  //   8-bit counter at G+0x167(359) (set to 30 by both state-0-shaped inits): decrementing it every
  //   call, and only once it BOTTOMS OUT at 0 does it check byte@G+0x29(41) to either re-idle
  //   (G+5=G+6=0) or call func_80056D44(G,0) — i.e. this state never advances G+0x6 itself; only
  //   states 2/3's own G+0x6++ move the FSM forward. State-0 init: clear G+0x146(326), resetSwap,
  //   func_80054D14(G,224,4), SFX cue func_80074590(58,0,0), G+0x167(359)=30, G+7=0, G+0x40(64)=7,
  //   G+0x42(66)=0, G+6++ (falls through into state-1's body the same call). State 2: clears global
  //   byte 0x800BF80E (`0x800C0000-2034`), resetSwap, func_80054D14(G,223,4) — NOTE 223, not 224 —
  //   SFX cue func_80074590(58,0,0), timer=30, G+6++ (exits directly, no fallthrough). State 3:
  //   func_80076D68(G), func_800551C4(G), decrements G+0x40 timer; once it lands on the sign-extends-
  //   to-0 value, writes global byte 0x800BF80E=1, G+4=4, G+5=32, clears G+6/G+7 (re-idles);
  //   otherwise no-op. Faithful from generated/shard_5.c:9664. Guest frame: addiu sp,-32; spill
  //   s0(G, r16),s1(r17),ra — s1 holds the constant 1, never restored (dead scratch, matches gen).
  void caseModeFsm_8006228C(uint32_t G);

  // caseModeFsm_8006506C(G) — guest FUN_8006506C(G). Table A/B case target. Same outer G+0x6
  //   4-state shape (0..3, `<2`/`==2`/`==3` gates) as the drafted cluster, but state 0's init is
  //   GATED: reads `mem_r32(G+380) & (0x1088<<16|0x200)` and compares to 0x200 — if EQUAL, runs the
  //   usual resetSwap(func_80054198)/func_80054D14(G,64,3)/G+6=3,G+7=0 sequence then bails straight
  //   to the shared state-1 tail (L_8006535C is NOT it — falls into L_80065138's alternate path,
  //   see .cpp for the exact branch map); if NOT equal, it instead re-checks a bit-1 flag at
  //   byte@G+0x15C(348): set -> func_80054D14(G,64,3), G+6=2,G+7=0; clear -> func_80054D14(G,64,3),
  //   G+6++,G+7=0, falling into the shared tail. The shared tail (state-1-ish, reached from every
  //   init path AND from a direct G+6==1 dispatch) reads a hardware/DAT flag at 0x8004CED4
  //   (`32783<<16-12460`) bit4: set -> func_80054D14(G,65,0)/func_80062D8C(G,0), then picks an SFX
  //   band arg (3 or -2) off a scratchpad byte `mem_r16(0x1F800000+380)&3`; clear -> bit6 of that
  //   same DAT_8004CED4 gates func_80062D8C(G,1) vs (G,64) before the same SFX dispatch
  //   (func_80074590(<band>,0,-60)). Then func_80055824(G) — the same call caseModeFsm_80060064
  //   uses (an ActorTomba::frameTick leaf per its own header comment) — feeds a big committing block
  //   nearly identical to caseModeFsm_80060064's own commit tail (G+5=4,G+6=2,G+356=0,G+7=0,
  //   G+344(dword)=0,G+88(word)=0,G+64(timer)=8, SFX cue func_80074590(29,0,0), func_80055E28(G,0),
  //   func_80054D14(G, 2|(byte@G+330&1), 20)). State 2 (label L_800652B0): func_80076D68(G), reads
  //   DAT bit4 at 0x80047E68 (`32782<<16+32360`) OR'd with a func_80055824(G) fallback, and on
  //   either being truthy sets G+5=7,G+6=1(the r18 constant),G+7=0; else if bit6 of DAT_8004CED4
  //   set, G+0x32(50)+=16 via func_80062D8C(G,1), G+6=1. State 3 (label L_80065324):
  //   func_80076D68(G), func_80062D8C(G,129), then conditionally an SFX cue (-2 band) same as
  //   state-0's tail — does NOT itself advance G+0x6 (no self-write to G+6 in this branch).
  //   Faithful from generated/shard_1.c:12103. Guest frame: addiu sp,-32; spill
  //   s0(G,r16),s2(r18),s1(r17),ra.
  void caseModeFsm_8006506C(uint32_t G);

  // nestedDispatch_800624B4(G) — guest FUN_800624B4(G). Table A/B case target — **NOT a leaf**:
  //   after two unconditional side-effect writes (G+0x17B(379)=1, global byte 0x8008027A
  //   (`0x80080000+634`, i.e. `8064<<16 + 634`) =2), gates `byte@G+0x6 < 5` (exit straight to the
  //   epilogue if not) and, when true, indexes a **5-entry `.rodata` function-pointer table at
  //   0x800163DC** (`32769<<16 + 25628`, i.e. `0x8001CADC`... literal gen constant kept: base
  //   `0x80010000` region + 25628) with `byte@G+0x6`, landing on an internal 5-way switch over the
  //   loaded address. Case-target addresses (all still-substrate; see docs/engine_re.md's "third
  //   dispatch layer map" for the full RE of all 5 bodies): `0x8006250C` (init: sets byte@0x800BF7FA
  //   (`32780<<16-2040`, offset+6)=0, G+0=6, +1=1, conditionally resetSwap+func_800551C4, then
  //   func_80067EF4(G, byte@G+111)/func_8001CF2C()/func_80055D5C(G), G+6++, func_80076D68(G),
  //   func_800310F4(30,0) spawning-style call whose return (r16) OR's flag 0x80 into ret+0x28 when
  //   nonzero, SFX cue func_80074590(22,0,30), G+16(dword)=r16, G+0x40(64)=5); `0x800625D4`
  //   (timer@G+0x40 decrement, re-idle-ish clear of G+1 + advance G+6 once it lands on 0);
  //   `0x800625F8` (checks DAT_800FE0A0 (`32800<<16-7968`) word !=0, calls func_8001CF2C());
  //   `0x8006261C` (reads two lookup-table entries off DAT_8009D014(`32783<<16-12268`, a dword
  //   pointer) and a per-item table at `32784<<16-20112` indexed by `byte@G+382 & 15`, feeds
  //   func_80044CD4(G, entry>>11, entry2-entry) — return==0 re-enters the G+6++ tail, nonzero exits
  //   without advancing); `0x80062678` (checks scratchpad byte@0x1F8001AB(411)!=0 as a gate; if the
  //   dword at G+16 is set, writes ret+4=2,ret+5=0; func_80057FD4(G); G+1=1; conditionally
  //   func_80074F24(byte@0x1F80022C(-1936)) when scratchpad byte@G+16... (see cpp for the exact
  //   dword-vs-byte read at r16+311==1); G+0=3, G+4(dword)=DAT_8009D010(`8064<<16+572`, i.e.
  //   scratchpad+572), G+370(word)=30; conditionally clears r16+311 and byte 0x800BF7D9(-2039)).
  //   `default:` (dead per the `<5` gate) calls `rec_dispatch(c, r2)` and returns WITHOUT the
  //   epilogue — mirrored literally. Faithful from generated/shard_6.c:9622. Guest frame:
  //   addiu sp,-32; spill s1(G,r17),ra,s0(r16, only live inside cases 0x8006250C/80062678).
  void nestedDispatch_800624B4(uint32_t G);

  // nestedDispatch_80060C60(G) — guest FUN_80060C60(G). Table A/B case target — **NOT a leaf, same
  //   shape as nestedDispatch_800624B4**: an unconditional side-effect write (scratchpad byte
  //   0x1F80027B (`8064<<16+635`) =0), gates `byte@G+0x6 < 8` (exit to epilogue if not), then
  //   indexes an **8-entry `.rodata` function-pointer table at 0x800163BC** (`32769<<16 + 25596`)
  //   with `byte@G+0x6`, landing on an internal 8-way switch. At 792 gen-C lines this is by far the
  //   largest of the whole cluster and — per fleet-workflow.md §9's "honest map beats buggy draft"
  //   — this pass drafts ONLY the frame/gate/switch skeleton plus the smallest case body
  //   (`0x800611B0`, ~10 gen-C lines) faithfully; the other 7 case bodies (ranging ~42-191 gen-C
  //   lines each, several themselves multi-branch with nested sub-dispatches like case
  //   `0x80061010`'s call into the already-drafted `matrixComposeAttached()`) are MAPPED ONLY —
  //   see docs/engine_re.md's "third dispatch layer map" for line ranges + one-line shape notes per
  //   case, and the .cpp for a `// TODO(wide-RE): not transcribed` stub at each undrafted label
  //   (falls straight to the shared epilogue — NOT faithful, dead code only, safe because unwired).
  //   Drafted case `0x800611B0`: reads byte@G+327(0x147), picks 1792 or 256, writes G+332(word)=
  //   that value, G+334(word)=0, G+6++ (falls through to the next case's own entry per gen's own
  //   fallthrough — NOT taken here since this is the shortest/last-checked case in isolation; see
  //   cpp for the literal `goto` target, which the drafted body preserves even though the target
  //   label itself is a TODO stub). Faithful (for the drafted case + skeleton) from
  //   generated/shard_1.c:10924. Guest frame: addiu sp,-32; spill s0(G,r17 — note: this function
  //   uses r17 as G, NOT r16, unlike its siblings),ra; r16 is live-but-uninitialized-at-entry
  //   scratch used inside several (undrafted) case bodies, not part of the guest frame spill set.
  void nestedDispatch_80060C60(uint32_t G);
};
