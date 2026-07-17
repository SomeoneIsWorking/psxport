// game/audio/music_coord.cpp — class MusicCoord — dialog-vs-ingame-music coordination (PC mod,
// instant-CD-safe). Owned by Engine; see music_coord.h for the OOP shape.
//
// The ingame/area background music is a LOOPING XA clip; a dialog uses sequenced "dialog-tone"
// songs (current-song byte 0x800bed80 in 4..7 — regular/worry/etc, user-identified) plus
// one-shot voice clips, all on the single XA stream. On real hardware the area-music start fires
// from the gameplay state machine only AFTER the CD-paced scene load, by which time the dialog
// has the stream/gate, so the looping music never overlapped the dialog tone. With our instant
// CD reads the area-music start fires immediately (during the dialog gap), so the loop overlaps
// the dialog tone — the audible bug. Mod: while a dialog tone is the current song, keep the
// looping ingame music suppressed and remembered; resume it once the dialog ends. One-shot voice
// clips are unaffected (they ARE the dialog audio). Deferred-music state lives on the instance:
// c->game->cd.{pending_music,pm_chan,pm_start,pm_end}.
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "c_subsys.h"
#include "cfg.h"
#include "music_coord.h"
#include "native_gate.h"   // fieldBgmDirector's `music` gate
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "guest_abi.h"     // GuestFrame — voiceMixTick mirrors FUN_80075824's 32-byte frame
#include <stdio.h>
#include <string.h>        // memcmp (fieldBgmDirector bundle validation)
#include <stdlib.h>        // atoi (PSXPORT_FIELD_SONG)

void rec_dispatch(Core*, uint32_t);   // still-substrate leaves called from voiceMixTick

bool MusicCoord::dialogToneActive() {
  Core* c = this->core;
  uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF;
  return s >= 4 && s <= 7;
}

// PC-added helper (NOT a port of any FUN_XXXX): snap the game's CD-volume fade CURRENT (0x800be224)
// to 0 AND zero the SPU CDVOL L/R registers directly, so THIS frame's XA mix is silent and the
// per-frame audio ramp (voiceMixTick above) then climbs the volume back to target (~2s fade-in).
// Fixes "music starts too loud" on instant-CD replays — real hardware masked this because the
// area music only started after the CD-paced scene load, by which point the fade had been armed.
// Caller MUST only invoke when the music is the sole XA user — CDVOL also scales the dialog voice.
// Zeroing 0x800be224 alone is NOT enough: the ramp already ran this frame with the OLD `cur` before
// this snap, mixing one frame at full volume. So also poke the SPU CDVOL register directly here.
void MusicCoord::musicFadeIn() {
  Core* c = this->core;
  c->mem_w16(0x800be224, 0);   // game's fade CURRENT = 0 (its per-frame ramp climbs it back up)
  c->mem_w16(0x1f801db0, 0);   // SPU CDVOL L = 0 NOW, so THIS frame's XA mix is silent (no loud blip)
  c->mem_w16(0x1f801db2, 0);   // SPU CDVOL R = 0
}

// Called from the BGM-start override (ov_sound_play_bgm) right after the song index is written, i.e.
// at the instant a dialog tone begins — synchronously, before this frame's audio mix. Stops the
// looping ingame music so it can't leak a frame past the dialog start (the per-frame tick() below
// would otherwise stop it one frame late, after the mix). Keeps it remembered (c->game->cd
// .pending_music) so tick() resumes it when the dialog ends.
void MusicCoord::cutIfDialog() {
  if (dialogToneActive() && xa_stream_is_looping(&core->game->xa)) xa_stream_stop(&core->game->xa);
}

namespace {

// Typed lens over the guest voice-channel control block (the ambient/XA channel @0x800BE1F8).
// Reads/writes go straight through to guest RAM — named fields instead of raw `V + 0xNN` offsets.
// Paired stores (volume/pan land on L and R halves) keep the substrate's store order.
struct VoiceChannel {
  Core* c;
  uint32_t base;
  uint32_t flags() const          { return c->mem_r32(base); }
  void     setFlags(uint32_t v)   { c->mem_w32(base, v); }
  void     setPan(uint16_t v)     { c->mem_w16(base + 0x06u, v); c->mem_w16(base + 0x04u, v); }
  void     setVolume(uint16_t v)  { c->mem_w16(base + 0x12u, v); c->mem_w16(base + 0x10u, v); }
  int16_t  baseVol() const        { return (int16_t)c->mem_r16(base + 0x28u); }
  int16_t  fadeTarget() const     { return (int16_t)c->mem_r16(base + 0x2Au); }
  int16_t  fadeCur() const        { return (int16_t)c->mem_r16(base + 0x2Cu); }
  void     setFadeCur(int32_t v)  { c->mem_w16(base + 0x2Cu, (uint16_t)v); }
  int16_t  gain2Target() const    { return (int16_t)c->mem_r16(base + 0x2Eu); }
  uint16_t gain2Cur() const       { return (uint16_t)c->mem_r16(base + 0x30u); }
  void     setGain2Cur(int32_t v) { c->mem_w16(base + 0x30u, (uint16_t)v); }
  bool     lowVolArmed() const    { return c->mem_r8(base + 0x33u) != 0; }
  void     disarmLowVol()         { c->mem_w8(base + 0x33u, 0); }
};

// Audio-global guest state read by the mixer (scratchpad bytes + dialog control words).
constexpr uint32_t kSpAudioState = 0x1F80019Au;  // 2 = audio engine active
constexpr uint32_t kSpCutMode    = 0x1F800137u;  // 0 normal / 1 dialog / 2 fast-cut
constexpr uint32_t kSpBoost      = 0x1F80027Eu;  // nonzero = +25% volume boost
constexpr uint32_t kSpQueueGate  = 0x1F80027Au;  // must be 0 for the low-volume SPU ping
constexpr uint32_t kSpQueueIdx   = 0x1F80023Bu;  // SPU queue slot for the ping
constexpr uint32_t kDialogFlags  = 0x800BE0E4u;  // dialog volume-source select bits
constexpr uint32_t kDialogLevel  = 0x800FB165u;  // dialog table level (0..9)

// Guest leaves the mixer calls (jal-site ra constants from abi_extract 0x80075824):
constexpr uint32_t kFnSetFadeTarget = 0x80075CECu;  // FUN_80075CEC(target)
constexpr uint32_t kFnSpuQueuePing  = 0x800750D8u;  // FUN_800750D8(idx, 1)

}  // namespace

// Per-frame VOICE-CHANNEL VOLUME MIXER — port of FUN_80075824 (RE'd via ghidra 2026-07-03). Called
// from AreaSlots::updateTail (game/world/area_slots.cpp) with voice_base = 0x800BE1F8 (the
// ambient/XA channel). Was previously — INCORRECTLY — wired to musicFadeIn() there, which merely
// zeros the fade current + SPU CDVOL; that call was based on a wrong reading of what FUN_80075824
// does. The RE'd body is a full ramp+boost+dialog-branch mixer (see header for the shape). SBS
// gameplay mode surfaced the resulting divergence at 0x800BE208/A (voice[+0x10]/[+0x12], the packed
// volume word). Both writes are done every frame; if they lag or don't fire, the audio state
// diverges downstream.
void MusicCoord::voiceMixTick(uint32_t voice_base) {
  Core* c = this->core;
  // FUN_80075824's guest frame (abi_extract --contract): 32 B; r17@sp+20, r31@sp+24, r16@sp+16.
  // The spills are guest-visible bytes — the hut-entry SBS diverge @f389 (0x801FE918..923) was
  // exactly these three words missing on the native path. r17 carries the channel base and r16
  // the running volume; both stay live in the register file so dispatched leaves spill the true
  // values (see guest_abi.h).
  static constexpr GuestFrameSpill kSpills[] = {{17, 20}, {31, 24}, {16, 16}};
  GuestFrame<32, 3> frameGuard(c, kSpills);
  GuestReg<17> chanReg(c);
  GuestReg<16> volReg(c);
  chanReg = voice_base;

  VoiceChannel voice{c, voice_base};
  const uint8_t state   = c->mem_r8(kSpAudioState);
  const uint8_t cutMode = c->mem_r8(kSpCutMode);
  const uint8_t boost   = c->mem_r8(kSpBoost);
  int32_t vol;                                     // computed 16-bit volume result
  // PSXPORT_DEBUG=vmt — voiceMixTick trace (RE/SBS diagnostic: docs/findings/audio.md "pc_skip vs
  // oracle: SPU register stream divergences"). Under SBS the two Games are separate Core/RAM
  // instances; this + the paired [gain2] trace in setGain2() below let a session correlate each
  // core's ramp/smoother state and setGain2 call cadence directly, instead of re-deriving it by
  // hand every time this bug class resurfaces.
  cfg_logf("vmt", "f%u %s state=%u cut=%u boost=%u cur=%d tgt=%d g2cur=%u g2tgt=%d base=%d",
           c->game->timing.logicFrame, c->game->pc_skip ? "A(skip)" : "B(oracle)", state, cutMode,
           boost, voice.fadeCur(), voice.fadeTarget(), voice.gain2Cur(), voice.gain2Target(),
           voice.baseVol());

  if (state != 2) {
    // Boot / silence: ~89% of base (0x47FF/0x7FFF), no ramp. (gen computes this with shifts —
    // no hi/lo side-effect, so plain arithmetic is faithful here.)
    vol = (voice.baseVol() * 0x47FF) >> 15;
  } else if (cutMode == 1) {
    // Dialog: full blast, a scaled table level, or scaled base — selected by the dialog flags.
    const uint8_t flags = c->mem_r8(kDialogFlags);
    if (flags & 0x02u) {
      vol = 0x7FFF;
    } else if (flags & 0x08u) {
      // Table level / 9 — the game divides via the 0x38E38E39 magic multiply (hi/lo visible).
      int32_t level = (int32_t)c->mem_r8(kDialogLevel) * 0x7FFF;
      guest_mult(c, level, (int32_t)0x38E38E39);
      vol = ((int32_t)c->hi >> 1) - (level >> 31);
    } else {
      vol = (int32_t)(guest_mult(c, voice.baseVol(), 0x7FFF) >> 15);
    }
    voice.setFlags(voice.flags() | 0x3u);
  } else {
    // Normal ramp: fadeCur chases fadeTarget by ±step (0x400 in fast-cut mode, else 0x100).
    int32_t cur    = voice.fadeCur();
    int32_t target = voice.fadeTarget();
    int32_t step   = (cutMode == 2) ? 0x400 : 0x100;
    if (cur < target) {
      cur += step; if (target < cur) cur = target;
    } else if (target < cur) {
      cur -= step; if (cur < target) cur = target;
    }
    voice.setFadeCur(cur);

    vol = (int32_t)(guest_mult(c, voice.baseVol(), cur) >> 15);
    if (boost != 0) {
      vol = (vol * 5) >> 2;
      if (vol > 0x7FFE) vol = 0x7FFF;
    }
    // Faded out while armed (and the SPU queue gate open): reset the fade target to 0x47FF and
    // ping the SPU queue, then disarm.
    if (vol < 0x11 && voice.lowVolArmed() && c->mem_r8(kSpQueueGate) == 0) {
      volReg = (uint32_t)vol;                      // live across the guest calls
      guest_fn(c, kFnSetFadeTarget, 0x800759B4u, 0x47FF);
      guest_fn(c, kFnSpuQueuePing, 0x800759C4u, c->mem_r8(kSpQueueIdx), 1);
      voice.disarmLowVol();
    }
    // Second-stage gain smoother: gain2Cur chases gain2Target by delta>>3. The game reads the
    // current gain SIGNED for the delta but UNSIGNED for the accumulate — mirrored.
    int32_t g2 = voice.gain2Cur() + (((int32_t)voice.gain2Target() - (int16_t)voice.gain2Cur()) >> 3);
    voice.setGain2Cur(g2);
    vol = (int32_t)(guest_mult(c, vol, (int16_t)g2) >> 13);
  }

  // Publish: packed volume to both channel halves, pan centered, dirty bits. v0/v1 mirror gen's
  // register outputs (the flags word and the pan constant).
  volReg = (uint32_t)vol;
  const uint32_t flags = voice.flags() | 0xC0u;
  voice.setVolume((uint16_t)vol);
  voice.setPan(0x3FFFu);
  voice.setFlags(flags);
  c->r[2] = flags;
  c->r[3] = 0x3FFFu;
}

// MusicCoord::setGain2 — FUN_80075D24 body. See music_coord.h for the RE contract. Always targets
// the fixed ambient-voice control block at 0x800BE1F8 (same S5 base as voiceMixTick/updateTail).
void MusicCoord::setGain2(int32_t val) {
  Core* c = this->core;
  const uint32_t V = 0x800BE1F8u;
  cfg_logf("vmt", "[gain2] f%u %s val=%d",
           c->game->timing.logicFrame, c->game->pc_skip ? "A(skip)" : "B(oracle)", val);
  if (val < 0) {
    uint16_t snap = (uint16_t)(-(int16_t)val);
    c->mem_w16(V + 0x2Eu, snap);   // target
    c->mem_w16(V + 0x30u, snap);   // current — instant snap, no smoothing left to do
    return;
  }
  if (val > 0x1FFF) val = 0x1FFF;
  c->mem_w16(V + 0x2Eu, (uint16_t)val);   // target only — the per-frame smoother eases toward it
}

static void eov_musicCoordSetGain2(Core* c) {
  int32_t val = (int32_t)c->r[4];
  eng(c).musicCoord.setGain2(val);
  // Mirror gen_func_80075D24's register outputs (shard_1.c): r3 = 0x800BE1F8 (the voice block addr,
  // computed as 32780<<16 + (-7688)); r2 = the last value the substrate's branch leaves in r2:
  //   negative val: r2 = -val;  positive <8192: r2 = 1;  positive >=8192: r2 = 0.
  c->r[3] = 0x800BE1F8u;
  if (val < 0) c->r[2] = (uint32_t)(-val);
  else         c->r[2] = (val < 8192) ? 1u : 0u;
}

extern void gen_func_80075D24(Core*);

void MusicCoord::registerOverrides() {
  overrides::install(0x80075D24u, "MusicCoord::setGain2", eov_musicCoordSetGain2, gen_func_80075D24);
}

// Called once per frame (native_step_frame). Enforces "dialogs stop the ingame music":
// stop a looping ingame-music clip while a dialog tone is up; resume the remembered clip once
// the dialog ends and the XA stream is free (no voice playing).
void MusicCoord::tick() {
  Core* c = this->core;
  if (cfg_str("PSXPORT_XA_DBG")) {
    uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF; int a = xa_stream_is_active(&c->game->xa), l = xa_stream_is_looping(&c->game->xa);
    if (s != mPrev || a != mPa || l != mPl) {
      fprintf(stderr, "[coord] song=%u tone=%d xa_active=%d loop=%d pending=%d\n",
              s, dialogToneActive(), a, l, c->game->cd.pending_music);
      mPrev = s; mPa = a; mPl = l;
    }
  }
  if (dialogToneActive()) {
    if (xa_stream_is_looping(&c->game->xa)) xa_stream_stop(&c->game->xa);    // dialog up: silence ingame music (kept pending)
  } else if (c->game->cd.pending_music && !xa_stream_is_active(&c->game->xa)) {
    xa_stream_play(&c->game->xa, c->game->cd.pm_chan, c->game->cd.pm_start, c->game->cd.pm_end, 1);   // dialog over: resume ingame music
    c->mem_w16(0x801fe0e0, 2);
    c->game->cd.toSpuMix(1);
    musicFadeIn();                                    // resumed music fades in from 0 (no voice now)
  }
}

// NATIVE field BGM director — see music_coord.h. Currently unwired from the frame loop (the
// recompiled libsnd path is the music path); kept as the native-synth driver for the area bundle.
void MusicCoord::fieldBgmDirector() {
  Core* c = core;
  if (!c->game->native_gates.get("music")) return;   // gated off -> recomp libsnd is the (oracle) music path
  // Are we in the field (GAME stage running)? The stage cell holds the active stage's task-0 entry.
  uint32_t stage = c->mem_r32(0x801fe00c);
  int in_field = (stage == 0x8010637Cu);
  int& started = c->game->field_bgm_started;
  if (!in_field) {
    if (started) { gctx(c)->music_list.stop(); started = 0; }
    return;
  }
  if (started) {
    // Restart if the song fully drained (one-shot tail) so the field stays scored.
    if (gctx(c)->music_list.nowPlaying() < 0) started = 0; else return;
  }
  // Validate the bundle is loaded before starting (area data may not be in yet right after a load).
  const uint8_t* b = c->ram + 0x182000u;             // guest 0x80182000 (area bundle)
  if (memcmp(b + 0x30, "pQES", 4) || memcmp(b + 0x26b4, "pBAV", 4)) return;
  int song = 8;                                   // default field theme (longest area seq)
  if (const char* s = cfg_str("PSXPORT_FIELD_SONG")) { int v = atoi(s); if (v >= 0 && v < 10) song = v; }
  if (gctx(c)->music_list.playArea(b, 0x50000, song) == 0) started = 1;
}
