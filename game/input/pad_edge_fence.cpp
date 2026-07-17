// game/input/pad_edge_fence.cpp — Engine::padEdgeFence, the port of FUN_800788AC.
//
// WIRED 2026-07-16 (pad_edge_fence_install below): §9 line-by-line re-verify against
// gen_func_800788AC (generated/shard_3.c:17840) passed with ZERO diffs — every address constant
// (0x800ECF54/56 cur/prev, 0x1F80019A poll flag, 0x800BED88/8C queue cursor/countdown,
// 0x800E7E68 pressed, 0x800F23A4 released), the branch structure, both jal-site ra constants
// (0x80078940/0x80078978), the store order, and the a0=released tail-call argument all match.
// `Engine::frameUpdate()`'s `rec_dispatch(c, 0x800788ACu)` now routes here via the override.
//
// ROLE (already partly documented, see docs/engine_re.md "Per-frame fence FUN_800788ac"):
// the top-level engine loop's per-frame fence, called exactly once per logic frame
// (FUN_80050b08 step 2, override `ov_frame_update`). It:
//   1. stashes the previous frame's "cur" word as "prev" (0x800ECF54 -> 0x800ECF56),
//   2. runs a small countdown/refill state machine gated by scratchpad flag *0x1F80019A,
//      either popping an entry off a queue at *0x800BED88 (stride 4: u16 tag @+2, u16 value @+0)
//      when a countdown at *0x800BED8C hits zero, or (when the flag isn't 1) calling
//      FUN_800524B4(0) and storing ITS return into 0x800ECF54,
//   3. computes PRESSED = cur & ~prev into DAT_800E7E68 and RELEASED = prev & ~cur into
//      DAT_800F23A4 (confirmed edge outputs — cross-referenced by docs/engine_re.md and
//      game/ai/actor_zoned_attacker.cpp-family code that reads those two globals),
//   4. tail-calls FUN_8005229C (a CD/load sub-state-machine per docs/engine_re.md's region-8005
//      survey, XA audio cue queue family 0x800521F4/0x8005229C/0x8005245C).
//
// CONFIDENCE — the byte-level control flow below is CONFIRMED (transcribed 1:1 from
// generated/shard_3.c:17840 gen_func_800788AC, instruction-exact ground truth, cross-checked
// against tools/disas.py 0x800788AC --all 55). The SEMANTIC label of 0x800ECF54 ("cur") is
// MEDIUM confidence: docs/engine_re.md's existing summary calls it "cur pad state", but this RE
// pass shows the SAME word gets overwritten with either a queue-entry's u16 value OR the return
// of FUN_800524B4(0) (a function an EARLIER RE pass filed under "controller vibration/analog-
// config subsystem", 0x80052144-0x800527C8 — not independently re-confirmed here). Whatever its
// true source, DAT_800ECF54 is read/written as a flat 16-bit "current sample" and DAT_800ECF56 as
// the one-frame-delayed "previous sample" — that structural role is solid regardless of what
// produces the sample. FUN_800524B4 and FUN_8005229C themselves stay un-owned (rec_dispatch).
#include "core.h"
#include "game_ctx.h"
#include "core/engine.h"

void rec_dispatch(Core*, uint32_t);

#define CUR_PREV_BASE   0x800ECF54u   // +0 = cur (u16), +2 = prev (u16)
#define POLL_FLAG       0x1F80019Au   // scratchpad u8 — gates queue-pop vs FUN_800524B4(0) path
#define QUEUE_COUNTDOWN 0x800BED8Cu   // u16 countdown to next queue pop
#define QUEUE_CURSOR    0x800BED88u   // u32 pointer into the queue (4 bytes/entry: u16 @+0, u16 tag @+2)
#define PRESSED_OUT     0x800E7E68u   // u16 — cur & ~prev (newly-set bits)
#define RELEASED_OUT    0x800F23A4u   // u16 — prev & ~cur (newly-cleared bits)
#define FN_PAD_SAMPLE   0x800524B4u   // FUN_800524B4 — un-owned (see confidence note above)
#define FN_CD_SM        0x8005229Cu   // FUN_8005229C — un-owned CD/load sub-state-machine

// FUN_800788AC — per-frame input-edge fence. See the file header above for the full RE + confidence
// notes.
void Engine::padEdgeFence() {
  Core* c = this->core;
  uint32_t saved_sp = c->r[29];
  uint32_t saved_ra = c->r[31];
  c->r[29] = saved_sp - 24;                 // addiu sp,sp,-24
  c->mem_w32(c->r[29] + 16, c->r[16]);      // sw s0,16(sp)  (LIVE incoming s0)
  c->mem_w32(c->r[29] + 20, saved_ra);      // sw ra,20(sp)
  c->r[16] = 0x800F0000u;                   // gen: s0 = 32783<<16, live across BOTH dispatches —
                                            // callees spill s0, so a stale native r16 diverges the
                                            // guest stack (wave-3 f12 lesson; restored from sp+16
                                            // in the epilogue below like gen)

  // prev := cur (BEFORE this frame's sample is written)
  uint16_t cur0 = c->mem_r16(CUR_PREV_BASE);
  c->mem_w16(CUR_PREV_BASE + 2, cur0);

  uint8_t pollFlag = c->mem_r8(POLL_FLAG);
  if (pollFlag != 1) {
    // Not in queue-poll mode: call FUN_800524B4(0), store its return into "cur".
    c->r[31] = 0x80078940u;
    c->r[4] = 0;
    c->r[2] = 1u;                           // live at the jal (the ==1 compare literal)
    c->r[3] = pollFlag;                     // live at the jal (the loaded poll byte)
    rec_dispatch(c, FN_PAD_SAMPLE);
    c->mem_w16(CUR_PREV_BASE, (uint16_t)c->r[2]);
  } else {
    uint16_t countdown = (uint16_t)(c->mem_r16(QUEUE_COUNTDOWN) - 1);
    c->mem_w16(QUEUE_COUNTDOWN, countdown);
    if (countdown == 0) {
      // Refill: pop one entry off the queue at *QUEUE_CURSOR.
      uint32_t entry = c->mem_r32(QUEUE_CURSOR);
      if (c->mem_r16(entry + 2) == 0) {
        // Tag field is 0 (terminator/padding slot) — skip it, advance +4.
        entry = entry + 4;
      }
      uint16_t val = c->mem_r16(entry + 0);
      c->mem_w16(CUR_PREV_BASE, val);
      uint16_t nextTag = c->mem_r16(entry + 2);
      c->mem_w32(QUEUE_CURSOR, entry + 4);
      c->mem_w16(QUEUE_COUNTDOWN, nextTag);
    }
    // countdown != 0: neither branch writes "cur" — it keeps the value just copied into "prev"
    // above, so pressed/released compute to 0 for this frame (still counting down).
  }

  uint16_t prev = c->mem_r16(CUR_PREV_BASE + 2);
  uint16_t cur  = c->mem_r16(CUR_PREV_BASE);
  uint16_t pressed  = (uint16_t)(cur & (uint16_t)~prev);
  uint16_t released = (uint16_t)(prev & (uint16_t)~cur);
  c->mem_w16(PRESSED_OUT, pressed);
  c->mem_w16(RELEASED_OUT, released);

  c->r[31] = 0x80078978u;                   // jal-site ra
  c->r[4]  = released;                      // a0 leftover from the release-mask compute — the gen
                                             // body calls FUN_8005229C with a0 still holding this
                                             // value (not explicitly reset), so it IS the argument.
  // LIVE caller-saved registers at the tail call — the callee's frame SPILLS these to the guest
  // stack, so leaving them stale diverges SBS even though no conforming code reads them (found at
  // wave-3 wiring: f12 two 2-byte diffs at 0x801FFFAA/CA, B=0x800F upper halves = gen's r2). Gen
  // values at the jal: r2 = 0x800F0000 (the released-store address base literal), r3 = ~cur32
  // (the released-mask scratch), r5 = 0x800E0000 (the pressed-store base, set on every path).
  c->r[2] = 0x800F0000u;
  c->r[3] = ~(uint32_t)cur;
  c->r[5] = 0x800E0000u;
  rec_dispatch(c, FN_CD_SM);                // FUN_8005229C — CD/load sub-state-machine tail-call

  c->r[31] = c->mem_r32(c->r[29] + 20);     // lw ra,20(sp)
  c->r[16] = c->mem_r32(c->r[29] + 16);     // lw s0,16(sp)
  c->r[29] = saved_sp;                      // addiu sp,sp,24
}

// Override wrapper + install (guest ABI is all-implicit — the fence takes no args, returns
// FUN_8005229C's v0 which the gen leaves in r2; the native body preserves that by not touching
// r2 after the tail dispatch).
namespace {
void ov_padEdgeFence(Core* c) { eng(c).padEdgeFence(); }
}
extern void gen_func_800788AC(Core*);
void pad_edge_fence_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x800788ACu, ov_padEdgeFence, gen_func_800788AC);
}
