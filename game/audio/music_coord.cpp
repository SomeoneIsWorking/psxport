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
#include "game.h"
#include "c_subsys.h"
#include "cfg.h"
#include "music_coord.h"
#include "native_gate.h"   // fieldBgmDirector's `music` gate
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
  if (dialogToneActive() && xa_stream_is_looping()) xa_stream_stop();
}

// Per-frame VOICE-CHANNEL VOLUME MIXER — port of FUN_80075824 (RE'd via ghidra 2026-07-03). Called
// from Engine::areaUpdateTail (game/scene/engine_stage.cpp) with voice_base = 0x800BE1F8 (the
// ambient/XA channel). Was previously — INCORRECTLY — wired to musicFadeIn() there, which merely
// zeros the fade current + SPU CDVOL; that call was based on a wrong reading of what FUN_80075824
// does. The RE'd body is a full ramp+boost+dialog-branch mixer (see header for the shape). SBS
// gameplay mode surfaced the resulting divergence at 0x800BE208/A (voice[+0x10]/[+0x12], the packed
// volume word). Both writes are done every frame; if they lag or don't fire, the audio state
// diverges downstream.
void MusicCoord::voiceMixTick(uint32_t voice_base) {
  Core* c = this->core;
  const uint32_t V = voice_base;
  const uint8_t  boost_flag = c->mem_r8(0x1F80027Eu);         // scratchpad boost byte
  const uint8_t  state      = c->mem_r8(0x1F80019Au);         // scratchpad "active audio" state
  const uint8_t  cutMode    = c->mem_r8(0x1F800137u);         // 0/1/2 — dialog/cut mode
  int32_t vol;                                                // computed 16-bit volume result

  if (state != 2) {
    // Boot / silence path: scale base by a fixed 0x47FF / 0x7FFF ratio (~89%), no ramp.
    int16_t base = (int16_t)c->mem_r16(V + 0x28u);
    vol = ((int32_t)base * 0x47FFu) >> 15;
  } else if (cutMode == 1) {
    // Dialog mode: pick full-blast (0x7FFF), a base-scaled 0x7FFF, or a table-derived value
    // depending on which bit is set in DAT_800BE0E4. Then OR extra flags into voice[+0x00].
    vol = 0x7FFF;
    const uint8_t flags = c->mem_r8(0x800BE0E4u);
    if ((flags & 0x02u) == 0) {
      if ((flags & 0x08u) == 0) {
        int16_t base = (int16_t)c->mem_r16(V + 0x28u);
        vol = ((int32_t)base * 0x7FFFu) >> 15;
      } else {
        uint32_t x = (uint32_t)c->mem_r8(0x800FB165u);
        vol = (int32_t)((x * 0x7FFFu) / 9u);
      }
    }
    c->mem_w32(V + 0u, c->mem_r32(V + 0u) | 0x3u);
  } else {
    // Normal ramp: cur (+0x2C) chases target (+0x2A) by ±step (0x100 or 0x400 in fast-cut mode).
    int32_t cur    = (int16_t)c->mem_r16(V + 0x2Cu);
    int32_t target = (int16_t)c->mem_r16(V + 0x2Au);
    int32_t step   = (cutMode == 2) ? 0x400 : 0x100;
    if (cur < target) {
      cur += step; if (target < cur) cur = target;
    } else if (target < cur) {
      cur -= step; if (cur < target) cur = target;
    }
    c->mem_w16(V + 0x2Cu, (uint16_t)cur);

    int16_t base = (int16_t)c->mem_r16(V + 0x28u);
    vol = ((int32_t)base * cur) >> 15;
    if (boost_flag != 0) {
      vol = (vol * 5) >> 2;
      if (vol > 0x7FFE) vol = 0x7FFF;
    }
    // Low-volume + armed(+0x33) + scratchpad[+0x27A]==0 → drop fade target to 0x47FF and ping the
    // SPU queue helper 0x800750D8. Clears the arm flag.
    if (vol < 0x11 && c->mem_r8(V + 0x33u) != 0 && c->mem_r8(0x1F80027Au) == 0) {
      c->mem_w16(0x800BE222u, 0x47FFu);                      // FUN_80075CEC(0x47FF): fade target
      c->r[4] = c->mem_r8(0x1F80023Bu); c->r[5] = 1;
      rec_dispatch(c, 0x800750D8u);                          // SPU queue helper (still substrate)
      c->mem_w8(V + 0x33u, 0);                                // disarm
    }
    // Smoother pass on the second-stage gain (+0x30 chases +0x2E by delta>>3).
    int32_t g2_cur    = (uint16_t)c->mem_r16(V + 0x30u);
    int32_t g2_target = (int16_t)c->mem_r16(V + 0x2Eu);
    int32_t g2 = g2_cur + ((g2_target - g2_cur) >> 3);
    c->mem_w16(V + 0x30u, (uint16_t)g2);
    vol = (vol * (int16_t)g2) >> 13;
  }

  // Common tail: write packed vol to both +0x10/+0x12, pan to 0x3FFF at +0x04/+0x06, dirty bit.
  c->mem_w16(V + 0x10u, (uint16_t)vol);
  c->mem_w16(V + 0x12u, (uint16_t)vol);
  c->mem_w16(V + 0x04u, 0x3FFFu);
  c->mem_w16(V + 0x06u, 0x3FFFu);
  c->mem_w32(V + 0u, c->mem_r32(V + 0u) | 0xC0u);
}

// Called once per frame (native_step_frame). Enforces "dialogs stop the ingame music":
// stop a looping ingame-music clip while a dialog tone is up; resume the remembered clip once
// the dialog ends and the XA stream is free (no voice playing).
void MusicCoord::tick() {
  Core* c = this->core;
  if (cfg_str("PSXPORT_XA_DBG")) {
    uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF; int a = xa_stream_is_active(), l = xa_stream_is_looping();
    if (s != mPrev || a != mPa || l != mPl) {
      fprintf(stderr, "[coord] song=%u tone=%d xa_active=%d loop=%d pending=%d\n",
              s, dialogToneActive(), a, l, c->game->cd.pending_music);
      mPrev = s; mPa = a; mPl = l;
    }
  }
  if (dialogToneActive()) {
    if (xa_stream_is_looping()) xa_stream_stop();    // dialog up: silence ingame music (kept pending)
  } else if (c->game->cd.pending_music && !xa_stream_is_active()) {
    xa_stream_play(c->game->cd.pm_chan, c->game->cd.pm_start, c->game->cd.pm_end, 1);   // dialog over: resume ingame music
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
    if (started) { c->game->music_list.stop(); started = 0; }
    return;
  }
  if (started) {
    // Restart if the song fully drained (one-shot tail) so the field stays scored.
    if (c->game->music_list.nowPlaying() < 0) started = 0; else return;
  }
  // Validate the bundle is loaded before starting (area data may not be in yet right after a load).
  const uint8_t* b = c->ram + 0x182000u;             // guest 0x80182000 (area bundle)
  if (memcmp(b + 0x30, "pQES", 4) || memcmp(b + 0x26b4, "pBAV", 4)) return;
  int song = 8;                                   // default field theme (longest area seq)
  if (const char* s = cfg_str("PSXPORT_FIELD_SONG")) { int v = atoi(s); if (v >= 0 && v < 10) song = v; }
  if (c->game->music_list.playArea(b, 0x50000, song) == 0) started = 1;
}
