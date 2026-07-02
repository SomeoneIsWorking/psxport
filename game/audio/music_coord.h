// class MusicCoord — DIALOG ↔ INGAME-MUSIC coordination subsystem owned by Engine.
//
// PROPER OOP: one instance per Core, embedded on Engine (`c->engine.musicCoord`). Back-pointer
// `core` wired at Core construction time (same pattern as Font / Asset / Placement). Subsystem,
// not a math library — MusicCoord owns the dialog-vs-music coordination behavior (which song-
// index range means "a dialog is playing", how the looping area BGM ducks under it, the fade-in
// ramp on resume) even though the class has no fields today.
//
// SCOPE: the four free functions previously living in game/audio/music_dialog_coord.cpp —
// dialog_tone_active, music_fade_in, xa_music_cut_if_dialog, xa_dialog_coord — each taking Core*
// and returning behaviour on the shared XA stream + guest fade-volume state.
#pragma once
#include <stdint.h>
class Core;

class MusicCoord {
public:
  Core* core = nullptr;

  // dialogToneActive(): true iff the CURRENT-SONG index (0x800bed80) is in [4, 7] — the
  //   dialog-tone range (regular/worry/etc, user-identified). Cheap read, safe every frame.
  bool dialogToneActive();

  // musicFadeIn(): snap the game's CD-volume fade CURRENT (0x800be224) to 0 AND zero the SPU
  //   CDVOL L/R registers directly (0x1F801DB0/DB2), so this frame's XA mix is silent and the
  //   game's per-frame ramp then climbs volume back to target (~2s fade-in). Caller MUST only
  //   invoke this when the music is the sole XA user — CDVOL also scales the dialog voice.
  void musicFadeIn();

  // cutIfDialog(): if a dialog tone is currently active and a looping ingame-music clip is
  //   playing, stop the clip so it can't leak a frame past the dialog start. Called from the
  //   BGM-start hook (sound.cpp ov_sound_play_bgm) synchronously with the dialog-tone song
  //   index becoming current. Kept pending so tick() resumes it when the dialog ends.
  void cutIfDialog();

  // tick(): once-per-frame update (called from native_scheduler_step). Enforces "dialogs stop
  //   the ingame music" — stops a looping ingame-music clip while a dialog tone is up; resumes
  //   the remembered clip once the dialog ends and the XA stream is free (no voice playing).
  void tick();
};
