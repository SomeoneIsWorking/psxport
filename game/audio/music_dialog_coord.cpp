// music_dialog_coord.cpp — dialog-vs-ingame-music coordination (PC mod, instant-CD-safe). Moved
// from cd_override.cpp (2026-07 restructure): this is game AUDIO/DIALOG DESIGN behavior (which
// song-index range means "a dialog is playing", how the looping area BGM ducks under it, the
// fade-in ramp on resume), not CD-controller HLE. See cd_override.cpp for the CD-command/read HLE
// this calls into.
//
// ---- dialog-vs-ingame-music coordination (PC mod, instant-CD-safe) ----------------------------
// The ingame/area background music is a LOOPING XA clip; a dialog uses sequenced "dialog-tone"
// songs (current-song byte 0x800bed80 in 4..7 — regular/worry/etc, user-identified) plus
// one-shot voice clips, all on the single XA stream. On real hardware the area-music start fires
// from the gameplay state machine only AFTER the CD-paced scene load, by which time the dialog
// has the stream/gate, so the looping music never overlapped the dialog tone. With our instant
// CD reads the area-music start fires immediately (during the dialog gap), so the loop overlaps
// the dialog tone — the audible bug. Mod: while a dialog tone is the current song, keep the
// looping ingame music suppressed and remembered; resume it once the dialog ends. One-shot voice
// clips are unaffected (they ARE the dialog audio).
// deferred-music state lives on the instance: c->game->cd.{pending_music,pm_chan,pm_start,pm_end}
#include "core.h"
#include "game.h"
#include "c_subsys.h"
#include "cfg.h"
#include <stdio.h>

// cd_override.cpp: enable/disable CD->SPU mixing (libsnd SpuSetCommonAttr via FUN_8001cf00(1)).
// Stays defined there (still called locally by ov_voice_play); xa_dialog_coord below also needs
// it for the resumed-music path, so it is non-static there rather than duplicated here.
void cd_to_spu_mix(Core* c, int on);

int dialog_tone_active(Core* c) {
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
void music_fade_in(Core* c) {
  c->mem_w16(0x800be224, 0);   // game's fade CURRENT = 0 (its per-frame ramp climbs it back up)
  c->mem_w16(0x1f801db0, 0);   // SPU CDVOL L = 0 NOW, so THIS frame's XA mix is silent (no loud blip)
  c->mem_w16(0x1f801db2, 0);   // SPU CDVOL R = 0
}

// Called from the BGM-start override (ov_bgm_start) right after the song index is written, i.e. at
// the instant a dialog tone begins — synchronously, before this frame's audio mix. Stops the looping
// ingame music so it can't leak a frame past the dialog start (the per-frame xa_dialog_coord below
// would otherwise stop it one frame late, after the mix). Keeps it remembered (s_pending_music) so
// xa_dialog_coord resumes it when the dialog ends.
void xa_music_cut_if_dialog(Core* c) {
  if (dialog_tone_active(c) && xa_stream_is_looping()) xa_stream_stop();
}

// Called once per frame (native_scheduler_step). Enforces "dialogs stop the ingame music":
// stop a looping ingame-music clip while a dialog tone is up; resume the remembered clip once
// the dialog ends and the XA stream is free (no voice playing).
void xa_dialog_coord(Core* c) {
  if (cfg_str("PSXPORT_XA_DBG")) {
    static uint32_t prev = 0xDEAD; static int pa = -1, pl = -1;
    uint32_t s = c->mem_r16(0x800bed80) & 0xFFFF; int a = xa_stream_is_active(), l = xa_stream_is_looping();
    if (s != prev || a != pa || l != pl) {
      fprintf(stderr, "[coord] song=%u tone=%d xa_active=%d loop=%d pending=%d\n",
              s, dialog_tone_active(c), a, l, c->game->cd.pending_music);
      prev = s; pa = a; pl = l;
    }
  }
  if (dialog_tone_active(c)) {
    if (xa_stream_is_looping()) xa_stream_stop();    // dialog up: silence ingame music (kept pending)
  } else if (c->game->cd.pending_music && !xa_stream_is_active()) {
    xa_stream_play(c->game->cd.pm_chan, c->game->cd.pm_start, c->game->cd.pm_end, 1);   // dialog over: resume ingame music
    c->mem_w16(0x801fe0e0, 2);
    cd_to_spu_mix(c, 1);
    music_fade_in(c);                                      // resumed music fades in from 0 (no voice now)
  }
}
