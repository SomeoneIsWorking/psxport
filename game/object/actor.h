// game/object/actor.h — Actor: a NAMED-FIELD view over a guest object node.
//
// The engine's per-object BEHAVIOR handlers (game/ai/beh_*) were transcribed from Ghidra output as free
// functions indexed by magic byte offsets — `obj[+0x42]`, `obj[+0x60]`, `obj[+0x80]`. That's
// black-box-transcription shape (per [[feedback_re_first_not_black_box]]): reading the code doesn't tell
// you what field 0x42 IS, so every diagnostic on it has to re-derive the semantic model. This class is the
// first move toward FIXING that: an Actor holds a Core* and a guest object address, and exposes typed
// getters/setters for the fields we've RE'd — so a handler reads `a.state()` / `a.setOscPhase(0)` /
// `a.setBoxX(0x1e)` instead of `mem_r8(obj + 4)` / `mem_w16(obj + 0x42, 0)` / `mem_w16(obj + 0x80, 0x1e)`.
//
// USAGE: create one at the top of a beh_ handler: `Actor a(c, c->r[4]);` and use its methods. The wrapper
// is trivially cheap (two pointer-sized fields, no allocation); no state is mirrored — every access goes
// through c->mem_r*/mem_w* so guest state stays authoritative and no synchronization issue exists.
//
// FIELD COVERAGE: this file grows FIELD BY FIELD as more beh_ handlers get RE'd. Each field addition
// documents WHERE the semantic was learned (which handler / which sub-behavior / what SBS finding named
// it). Do NOT define fields speculatively — only when a handler's RE actually resolved the semantic.
#pragma once
#include <cstdint>
#include "core.h"

class Actor {
public:
  Actor(Core* core, uint32_t addr) : c(core), obj(addr) {}

  // The raw guest node pointer (for legacy inline `mem_r*(obj+X)` in code paths that still index unnamed
  // fields, and for `rec_dispatch` sub-behavior calls that take the node in c->r[4]). Prefer named
  // accessors — this is the escape hatch for the not-yet-RE'd offsets.
  uint32_t addr() const { return obj; }

  // ── Lifecycle & state ────────────────────────────────────────────────────────────────────────────
  // state (obj+4, u8): per-object state machine byte. Common convention observed across the beh_
  //   handlers: 0 = init (allocate render record, seed fields, advance to 1), 1 = active (per-frame
  //   tick), 2/3 = despawn (call spawn.despawn(node)), >=4 = no-op. Named from
  //   beh_typed_table_seed_gate (FUN_80133C14) state-machine RE: sibling handlers FUN_800739AC /
  //   FUN_80073CD8 share this convention.
  uint8_t state() const               { return c->mem_r8(obj + 0x04); }
  void    setState(uint8_t v)         { c->mem_w8(obj + 0x04, v); }

  // alive (obj+0, u8): live/render-enable flag. Handlers set this to 1 alongside state=1 (Active) in
  //   the init state. Semantics beyond "set-when-active" not yet fully RE'd — treat as "handler-set
  //   live bit" for now.
  uint8_t alive() const               { return c->mem_r8(obj + 0x00); }
  void    setAlive(uint8_t v)         { c->mem_w8(obj + 0x00, v); }

  // renderMode (obj+1, u8): a secondary state byte written by beh_typed_table_seed_gate in the
  //   "scene gate == 0x22" branch (state 1) — set to the current state value (1). Purpose beyond the
  //   handler's local use not yet RE'd; kept named because the write site is coupled to renderMode
  //   semantics (see below).
  void    setRenderMode(uint8_t v)    { c->mem_w8(obj + 0x01, v); }

  // type (obj+3, u8): per-object TYPE index used as key into per-type data tables. In
  //   beh_typed_table_seed_gate this indexes the halfword TBL_A6E4 (per-type seed for oscBase).
  uint8_t type() const                { return c->mem_r8(obj + 0x03); }

  // ── Handler-local counters/echoes (u8) ───────────────────────────────────────────────────────────
  // counterA (obj+0x29, u8): u8 counter zero-cleared at init by beh_typed_table_seed_gate. Not yet
  //   traced to a writer that gives it semantics.
  void    setCounterA(uint8_t v)      { c->mem_w8(obj + 0x29, v); }
  // sceneMode (obj+0x2A, u8): per-handler scene-mode tag. beh_typed_table_seed_gate seeds this to 0x22
  //   at init, and its state-1 gate branches on the global scene-phase byte 0x800E7EAA == 0x22. That
  //   coupling names it: "this actor participates when the scene is in the 0x22 phase".
  void    setSceneMode(uint8_t v)     { c->mem_w8(obj + 0x2A, v); }
  // counterB (obj+0x46, u8): u8 counter zero-cleared at init. Same status as counterA — named as a
  //   counter because it sits next to other zeroed counters; exact semantics deferred.
  void    setCounterB(uint8_t v)      { c->mem_w8(obj + 0x46, v); }

  // ── Oscillation / motion (u16 / i16) ─────────────────────────────────────────────────────────────
  // oscBase (obj+0x32, u16): per-type oscillation base parameter. Seeded from the per-type halfword
  //   table 0x8014A6E4 keyed by type() at init. Read by state-1 sub-behaviors that compose per-frame
  //   motion (RE'd next arc).
  void    setOscBase(uint16_t v)      { c->mem_w16(obj + 0x32, v); }
  // oscPhase (obj+0x42, i16): SBS TARGET-#4 accumulator — the angle-space fixed-point phase that
  //   feeds the state-1 body's Trig::rcos oscillator via ov_a00_gen_801337E4 (accumulates +68 +
  //   rand()>>8 each active tick; the PRNG at FUN_8009A450 seeds off 0x80105EE8 is the almost-certain
  //   divergence source per docs/findings/sbs.md target-#4 upstream probe). beh_typed_table_seed_gate
  //   zero-clears this at init (state 0).
  void    setOscPhase(int16_t v)      { c->mem_w16(obj + 0x42, (uint16_t)v); }

  // ── Trigger box / range params ───────────────────────────────────────────────────────────────────
  // triggerParam (obj+0x60, i16): the signed per-object parameter passed as a1 into the state-1
  //   sub-behavior FUN_800778E4 (spatial trigger check). beh_typed_table_seed_gate seeds it to -200
  //   at init — reads as a Y-offset / trigger distance in world units, but the exact axis has not
  //   been RE'd yet.
  int16_t triggerParam() const        { return (int16_t)c->mem_r16(obj + 0x60); }
  void    setTriggerParam(int16_t v)  { c->mem_w16(obj + 0x60, (uint16_t)v); }
  // stateEcho (obj+0x62, u16): mirror of the state byte written in non-triggered gate paths in
  //   beh_typed_table_seed_gate state 1. Reads as a "last-tick state" latch for a downstream
  //   consumer we haven't RE'd yet — named for its write site's semantic, not the consumer's.
  void    setStateEcho(uint16_t v)    { c->mem_w16(obj + 0x62, v); }

  // ── Bounding box / trigger volume (u16 × 4) ──────────────────────────────────────────────────────
  // Seeded at init by beh_typed_table_seed_gate to (30, 60, 50, 100). Reads as a bounding-box or
  // radius-tuple parameterizing a trigger volume; the exact interpretation (AABB half-extents vs
  // XYZ+radius) will be pinned down when the state-1 sub-behavior that CONSUMES it is RE'd.
  void    setBoxX(uint16_t v)         { c->mem_w16(obj + 0x80, v); }
  void    setBoxY(uint16_t v)         { c->mem_w16(obj + 0x82, v); }
  void    setBoxZ(uint16_t v)         { c->mem_w16(obj + 0x84, v); }
  void    setBoxW(uint16_t v)         { c->mem_w16(obj + 0x86, v); }

private:
  Core*    c;
  uint32_t obj;
};

// Global SCENE-PHASE gate byte. Read every frame by beh_typed_table_seed_gate (and other cull-record
// handlers) to gate their state-1 behavior. Semantics (advanced by what? represents what phase?) not
// yet RE'd — named for the observation that beh_typed_table_seed_gate has sceneMode=0x22 (obj+0x2A)
// and dispatches on scene_phase()==0x22 in its state-1 body.
inline uint8_t scene_phase(Core* c) { return c->mem_r8(0x800E7EAAu); }

// Per-type OSCILLATION BASE table: 8014A6E4[type] -> u16 seeded into Actor::setOscBase() at init by
// beh_typed_table_seed_gate. Named for its state-machine role, not its content — content is per-type
// tuning data.
inline uint16_t osc_base_table(Core* c, uint8_t type) {
  return c->mem_r16(0x8014A6E4u + (uint32_t)type * 2u);
}
