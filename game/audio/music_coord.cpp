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
#include <stdio.h>

// cd_override.cpp: enable/disable CD->SPU mixing (libsnd SpuSetCommonAttr via FUN_8001cf00(1)).
// Stays defined there (still called locally by voice_play); tick() below also needs it for
// the resumed-music path, so it is non-static there rather than duplicated here.
void cd_to_spu_mix(Core* c, int on);

bool MusicCoord::dialogToneActive() {
  Core* c = this->core;
  uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF;
  return s >= 4 && s <= 7;
}

// Fade the ingame music IN from silence using the GAME'S OWN CD-volume ramp. The game keeps a
// CD-volume fade pair: target DAT_800be222 and current DAT_800be224 (the SpuCommonAttr CDVOL the
// per-frame audio update FUN_80075824 ramps current->target by 0x100/frame, then writes to SPU
// reg 0x1B0/0x1B2, which Beetle spu.c uses to scale the XA — see spu.c CDVol). At steady state
// both sit at 0x7fff (full). On real hardware the area music only started after the CD-paced
// scene load, by which point this fade had been (re)armed; with instant CD the music (re)starts
// at the steady full level — the "starts too loud" bug. Mod: snap the fade CURRENT to 0 (leave the
// target), so the game's own ramp climbs it back up = a ~2s fade-in. Caller MUST only do this when
// the music is the sole XA user — CDVOL also scales the dialog VOICE (chan22), so zeroing it while
// a voice plays would mute the voice too.
//
// Snapping only `cur` is NOT enough: the game's per-frame ramp FUN_80075824 writes the SPU CDVOL
// register (0x1F801DB0/DB2 — Beetle spu.c case 0x30/0x32, CDVol[0]/[1]) from `cur` ONCE per frame,
// and on the music-(re)start frame it already ran with the OLD (full) `cur` before this snap — so
// that one frame still mixes the freshly-started XA at full volume (verified: PSXPORT_XA_DBG showed
// CDVOL=25480 for the start frame, then 198 and climbing). That leading full-volume frame is the
// audible "1-frame blip" / "starts loud then drops to zero then climbs". So ALSO write the SPU CDVOL
// register to 0 directly here, muting the start frame's mix; the next frame the ramp rewrites it from
// cur=0 and climbs. mem_w16 to 0x1F801DBx routes through io_write -> spu_write -> SPU_Write (mem.c).
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

// Called once per frame (native_scheduler_step). Enforces "dialogs stop the ingame music":
// stop a looping ingame-music clip while a dialog tone is up; resume the remembered clip once
// the dialog ends and the XA stream is free (no voice playing).
void MusicCoord::tick() {
  Core* c = this->core;
  if (cfg_str("PSXPORT_XA_DBG")) {
    static uint32_t prev = 0xDEAD; static int pa = -1, pl = -1;
    uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF; int a = xa_stream_is_active(), l = xa_stream_is_looping();
    if (s != prev || a != pa || l != pl) {
      fprintf(stderr, "[coord] song=%u tone=%d xa_active=%d loop=%d pending=%d\n",
              s, dialogToneActive(), a, l, c->game->cd.pending_music);
      prev = s; pa = a; pl = l;
    }
  }
  if (dialogToneActive()) {
    if (xa_stream_is_looping()) xa_stream_stop();    // dialog up: silence ingame music (kept pending)
  } else if (c->game->cd.pending_music && !xa_stream_is_active()) {
    xa_stream_play(c->game->cd.pm_chan, c->game->cd.pm_start, c->game->cd.pm_end, 1);   // dialog over: resume ingame music
    c->mem_w16(0x801fe0e0, 2);
    cd_to_spu_mix(c, 1);
    musicFadeIn();                                    // resumed music fades in from 0 (no voice now)
  }
}
