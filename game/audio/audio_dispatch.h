// game/audio/audio_dispatch.h — PC-native audio DISPATCH / SETTLE cluster.
//
// PROPER OOP: one instance per Core, embedded on Engine, reached as
// `c->engine.audioDispatch.method(args)`. Back-pointer wired at Core construction (same pattern
// as Sfx / MusicCoord).
//
// SCOPE: the four field-audio control-flow primitives — the 3-way dispatcher (FUN_800750D8), the
// bit-packed XA/voice fetch selector (FUN_8001D364), the field audio SETTLE (FUN_80074BC4), and
// the zone-transition record dispatcher (FUN_8001D71C). Each owns control flow + index/record
// decode natively; the XA/voice/libsnd leaves stay substrate.
#pragma once
#include <cstdint>
struct Core;

class AudioDispatch {
public:
  Core* core = nullptr;

  // dispatch3Way(idx): guest FUN_800750D8. 3-branch dispatcher used by area-machine state 0
  //   (idx = 0x2C) and Pool::selectStateIndex (idx = s0):
  //     idx == 0xFF → return (DAT_800BE0E4 & 4) — an "audio-armed" flag test.
  //     idx == 0xFE → return FUN_8001CF2C's return value — settle-poll.
  //     else       → run voiceFetchBits(idx, second_arg) (substrate leaf), return 0.
  //   Ghidra decomp scratch/decomp/fun_800750d8_v2.c. Return in c->r[2] preserved.
  uint32_t dispatch3Way(uint32_t idx, uint32_t arg2 = 0);

  // voiceFetchBits(bits, flag): guest FUN_8001D364. Bit-packed XA/voice fetch selector.
  //   bits[3..5] pick one of 8 per-class descriptor tables (0x8001005C..0x80010078); bits[0..2]
  //   index a (u16 offset, u16 count) pair inside that table. Delegates to substrate
  //   FUN_8001D2A8(class, base, tail, flag|2) where base = *0x1F800224 + offset*8 and
  //   tail = base + (count-2)*8. Sister to zoneTransitionSetup (same substrate leaf,
  //   different index shape). Ghidra decomp scratch/decomp/fun_8001d364.c.
  void voiceFetchBits(uint32_t bits, uint32_t flag);

  // settleField(): guest FUN_80074BC4. 4-step audio state SETTLE fired by Engine::fieldRun's
  //   state 3 (init hand-off) / state 1's mode-3 branch / state 6 (transition entry): clears the
  //   scratchpad audio-cue flag @0x1F80027E, runs the engine-tick settle FUN_8001CF2C (task-2
  //   kill + VBlank sync), then two voice-table cleanups (FUN_80074B44 = tail voices reset;
  //   FUN_80074E48 = SsSeqClose + full voice reset). The 3 leaves all wrap SPU/BIOS libsnd APIs
  //   (SsSeqClose / SsSetMVol / SsAudUpdate) that don't have PC-native equivalents yet, so they
  //   stay dispatched. Ghidra decomp scratch/decomp/fun_80074bc4.c + scratch/decomp/74bc4_subs.c.
  void settleField();

  // zoneTransitionSetup(idx): tiny dispatcher at guest FUN_8001D71C. Called from Sop::fieldUpdate
  //   (idx=0xE, the tail's zone-transition setup after the intro scroller finishes) and from
  //   Engine::fadeSequencer (idx=11, the fade-in-init step). Ghidra decomp
  //   scratch/decomp/sop_tail_8001d71c.c.
  //     - idx < 0                       : dispatches the engine tick FUN_8001CF2C (still substrate).
  //     - idx >= 0                      : looks up a 6-byte record at 0x8009D110[idx] with fields
  //         +0 u8 track    +1 u8 group  +2 u16 baseOff  +4 u16 span
  //       and either short-circuits to `*0x800BE0E4 = 2` when `group==0 && DAT_800FB162==1`
  //       (silent-track fast path — the audio subsystem is already at ready-state 1), or dispatches
  //       FUN_8001D2A8(track, base, base+span, group) where `base = *0x1F800220 + baseOff*8`
  //       (still substrate — the XA/CD voice fetch entry-point that the xa_stream.c comment refers
  //       to). Control flow + record decode owned native; the two leaves stay dispatched.
  void zoneTransitionSetup(int16_t idx);
};
