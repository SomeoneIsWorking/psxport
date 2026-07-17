// game/audio/audio_dispatch.cpp — AudioDispatch method bodies. See audio_dispatch.h for the
// per-method RE contracts; each body owns control flow + index/record decode, the XA/voice/libsnd
// leaves (FUN_8001CF2C / FUN_8001D2A8 / FUN_80074B44 / FUN_80074E48) stay substrate.
//
// FUN_8001CF2C re-confirmed 2026-07-08 (Ghidra decomp, scratch/decomp/cf2c_cluster.c):
//   FUN_80052010(2)          -> already native: PcScheduler::forceClose(2)
//   DAT_800be0e4 = 0         -> settle-flag clear (guest RAM write, mechanical)
//   FUN_8001cf00(0)          -> already native: Cd::toSpuMix(0)
//   do { } while(!FUN_80089e1c(9,0,0))
//     -> FUN_80089e1c is libcd's retry-loop CD-command dispatcher (3 attempts, calls
//        FUN_8008ac34 = CdControl-style command issue against the real CD-controller regs,
//        FUN_8008a6ec = CdSync-style completion poll). Command id 9 + the retry/backoff shape
//        is PSX CD-controller/BIOS protocol, not game logic — there is no "observable result"
//        to rebuild here beyond replaying real hardware handshake timing, which the PSX HLE
//        CD subsystem already does faithfully. Genuine PSX-hardware/BIOS leaf: STAYS SUBSTRATE.
//        (The two calls it makes that ARE game logic — forceClose, toSpuMix — are already owned
//        above it, so FUN_8001cf2c itself is now just a 2-line hardware-retry wrapper.)

#include "audio/audio_dispatch.h"
#include "game_ctx.h"
#include "core.h"
#include "core/engine.h"
#include "override_registry.h"   // overrides::install — the one native-override registry

// Byte-exact ready-frame ports below install by guest address into the ONE override registry so
// every caller (substrate included) reaches the native method. gen_func_* = the recomp oracle body.
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80075024(Core*);
extern void gen_func_80075070(Core*);
extern void func_800750D8(Core*);   // AudioDispatch::dispatch3Way leaf (native-owned; routed via dispatch)
extern void func_80075CEC(Core*);   // audio fade-target setter (FUN_80075CEC)

// AudioDispatch::dispatch3Way — native ownership of FUN_800750D8 (Ghidra decomp
// scratch/decomp/fun_800750d8_v2.c). Returns the flag/settle result via c->r[2] as well as via
// the C return value so the recomp `v0` semantics stay.
uint32_t AudioDispatch::dispatch3Way(uint32_t idx, uint32_t arg2) { Core* c = core;
  uint32_t v0;
  if (idx == 0xFFu) {
    v0 = (uint32_t)c->mem_r8(0x800BE0E4u) & 4u;
  } else if (idx == 0xFEu) {
    rec_dispatch(c, 0x8001CF2Cu);                 // engine tick / settle
    v0 = c->r[2];
  } else {
    voiceFetchBits(idx, arg2);
    v0 = 0;
  }
  c->r[2] = v0;
  return v0;
}

// AudioDispatch::voiceFetchBits — native ownership of FUN_8001D364 (Ghidra decomp
// scratch/decomp/fun_8001d364.c). See audio_dispatch.h for the field layout. Table addresses are
// 8 resident u16-array pointers (ptr-to-ptr; deref once for the table, then read two u16s per
// entry). The substrate leaf FUN_8001D2A8 (XA/voice fetch entry) stays dispatched — sister to
// zoneTransitionSetup which uses the 0x8009D110 record table with a different index shape.
void AudioDispatch::voiceFetchBits(uint32_t bits, uint32_t flag) { Core* c = core;
  static constexpr uint32_t TABLE_PTRS[8] = {
    0x8001005Cu, 0x80010060u, 0x80010064u, 0x80010068u,
    0x8001006Cu, 0x80010070u, 0x80010074u, 0x80010078u,
  };
  const uint32_t klass  = (bits >> 3) & 7u;         // "voice-class" selector — 0..7
  const uint32_t sub    = bits & 7u;                // per-class entry index — 0..7
  const uint32_t table  = c->mem_r32(TABLE_PTRS[klass]);
  const uint32_t entry  = table + sub * 4u;         // 2 u16 fields per entry (offset, count)
  const uint16_t offset = c->mem_r16(entry + 0u);
  const uint16_t count  = c->mem_r16(entry + 2u);
  const uint32_t base   = c->mem_r32(0x1F800224u) + (uint32_t)offset * 8u;
  const uint32_t tail   = base + ((uint32_t)count - 2u) * 8u;

  c->r[4] = klass;
  c->r[5] = base;
  c->r[6] = tail;
  c->r[7] = (flag & 1u) | 2u;
  rec_dispatch(c, 0x8001D2A8u);                     // XA/voice fetch entry (substrate)
}

// AudioDispatch::settleField — native ownership of FUN_80074BC4 (Ghidra decomp
// scratch/decomp/fun_80074bc4.c). Every callee stays substrate (libsnd wrappers with no PC-native
// equivalent yet).
void AudioDispatch::settleField() { Core* c = core;
  c->mem_w8(0x1F80027Eu, 0);            // audio-cue scratchpad flag
  rec_dispatch(c, 0x8001CF2Cu);         // engine tick / VBlank settle (substrate)
  rec_dispatch(c, 0x80074B44u);         // tail voices reset             (substrate)
  rec_dispatch(c, 0x80074E48u);         // SsSeqClose + full voice reset (substrate)
}

// AudioDispatch::selectState — native ownership of FUN_800750A4 (Ghidra decomp
// scratch/decomp/area_load_leaves.c): dispatch3Way(idx, 1) then publish idx to the state-index
// byte. Used by Engine::areaLoadState's state 2 (idx=4).
void AudioDispatch::selectState(uint8_t idx) { Core* c = core;   // FUN_800750A4
  dispatch3Way(idx, 1);
  c->mem_w8(0x1F80023Bu, idx);
}

// AudioDispatch::zoneTransitionSetup — native ownership of the tiny dispatcher FUN_8001D71C. See
// the class doc-comment (audio_dispatch.h) for the record layout + branch semantics. Ghidra decomp
// scratch/decomp/sop_tail_8001d71c.c; widths spot-checked against tools/disas.py 0x8001d71c.
void AudioDispatch::zoneTransitionSetup(int16_t idx) { Core* c = core;
  if (idx < 0) {
    rec_dispatch(c, 0x8001CF2Cu);                     // engine tick (still substrate; SYNC)
    return;
  }

  // Record table @0x8009D110, stride 6 bytes.
  const uint32_t recAddr = 0x8009D110u + (uint32_t)(uint16_t)idx * 6u;
  const uint8_t  track  = c->mem_r8 (recAddr + 0u);
  const uint8_t  group  = c->mem_r8 (recAddr + 1u);
  const uint16_t offRaw = c->mem_r16(recAddr + 2u);   // base offset scalar (×8 later)
  const uint16_t spanRaw= c->mem_r16(recAddr + 4u);   // record span scalar (×8 later)

  // Silent-track fast path — audio subsystem ready-latch already at 1 → just queue state 2.
  if (group == 0 && c->mem_r8(0x800FB162u) == 1) {
    c->mem_w8(0x800BE0E4u, 2);
    return;
  }

  const uint32_t base = c->mem_r32(0x1F800220u) + (uint32_t)offRaw * 8u;
  const uint32_t tail = base + (uint32_t)spanRaw * 8u;
  c->r[4] = (uint32_t)track;
  c->r[5] = base;
  c->r[6] = tail;
  c->r[7] = (uint32_t)group;
  rec_dispatch(c, 0x8001D2A8u);                       // XA/voice fetch entry (still substrate)
}

// AudioDispatch::selectStateRemap — native ownership of FUN_80075024. Maps the incoming slot to a
// state index (19 when slot == 5, else 20), runs dispatch3Way(stateIdx, 1) via the FUN_800750D8
// leaf, publishes stateIdx to the scratchpad state-index byte 0x1F80023B (same tail as selectState)
// and clears the text/audio state byte 0x800BE22B. READY-FRAME leaf: the gen body descends sp by 24
// and spills ra/s0 (r31/r16) at sp+20/+16 with their LIVE incoming values, restoring before return
// — the native port mirrors that guest stack frame exactly (see docs/faithful-execution.md,
// game/player/collision.cpp::flatNormal).
// ORACLE: gen_func_80075024
void AudioDispatch::selectStateRemap(uint8_t slot) {
  Core* c = this->core;
  c->r[4] = slot;
  c->r[29] = c->r[29] + (uint32_t)-24;               // addiu sp,-0x18 — descend the guest frame
  c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);   // sw s0,0x10(sp) — LIVE incoming s0
  c->r[16] = c->r[0] + (uint32_t)20;                 // stateIdx = 20 (default)
  c->r[4] = c->r[4] & 255u;
  c->r[2] = c->r[0] + (uint32_t)5;
  { int _t = (c->r[4] != c->r[2]); c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]); if (_t) goto L_80075044; }  // sw ra,0x14(sp)
  c->r[16] = c->r[0] + (uint32_t)19;                 // slot == 5 → stateIdx = 19
L_80075044:;
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80075050u;
  c->r[5] = c->r[0] + (uint32_t)1; func_800750D8(c); // dispatch3Way(stateIdx, 1)
  c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));  // lw ra,0x14(sp)
  c->r[2] = (uint32_t)8064u << 16;
  c->mem_w8((c->r[2] + (uint32_t)571), (uint8_t)c->r[16]);   // 0x1F80023B = stateIdx (state-index byte)
  c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));  // lw s0,0x10(sp)
  c->r[2] = (uint32_t)32780u << 16;
  c->mem_w8((c->r[2] + (uint32_t)-7637), (uint8_t)c->r[0]);  // 0x800BE22B = 0 (text/audio state byte)
  c->r[29] = c->r[29] + (uint32_t)24; return;        // addiu sp,0x18 — ascend the guest frame
}

// AudioDispatch::publishStateFade — native ownership of FUN_80075070. Publishes the incoming index
// to the scratchpad state-index byte 0x1F80023B, sets the text/audio state byte 0x800BE22B = 1, then
// runs the audio fade-target setter FUN_80075CEC(0) (still substrate). READY-FRAME leaf: the gen
// body descends sp by 24 and spills ra (r31) at sp+16 with its LIVE incoming value, restoring before
// return — the native port mirrors that guest stack frame exactly.
// ORACLE: gen_func_80075070
void AudioDispatch::publishStateFade(uint8_t idx) {
  Core* c = this->core;
  c->r[4] = idx;
  c->r[29] = c->r[29] + (uint32_t)-24;               // addiu sp,-0x18 — descend the guest frame
  c->r[2] = (uint32_t)8064u << 16;
  c->r[3] = (uint32_t)32780u << 16;
  c->mem_w8((c->r[2] + (uint32_t)571), (uint8_t)c->r[4]);    // 0x1F80023B = idx (state-index byte)
  c->r[2] = c->r[0] + (uint32_t)1;
  c->r[4] = c->r[0] + c->r[0];                       // fade-target arg = 0
  c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);   // sw ra,0x10(sp) — LIVE incoming ra
  c->r[31] = 0x80075094u;
  c->mem_w8((c->r[3] + (uint32_t)-7637), (uint8_t)c->r[2]); func_80075CEC(c);  // 0x800BE22B = 1; audioFadeTarget(0)
  c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));  // lw ra,0x10(sp)
  c->r[29] = c->r[29] + (uint32_t)24; return;        // addiu sp,0x18 — ascend the guest frame
}

// eov_* wrappers — guest-ABI adapters (args in c->r[4], return in c->r[2]). One per leaf.
static void eov_audioSelectStateRemap(Core* c) { eng(c).audioDispatch.selectStateRemap((uint8_t)c->r[4]); }
static void eov_audioPublishStateFade(Core* c) { eng(c).audioDispatch.publishStateFade((uint8_t)c->r[4]); }

void AudioDispatch::registerOverrides() {
  using overrides::install;
  install(0x80075024u, "AudioDispatch::selectStateRemap", eov_audioSelectStateRemap, gen_func_80075024, shard_set_override);
  install(0x80075070u, "AudioDispatch::publishStateFade", eov_audioPublishStateFade, gen_func_80075070, shard_set_override);
}
