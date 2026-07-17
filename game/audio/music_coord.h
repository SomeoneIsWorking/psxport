// class MusicCoord — DIALOG ↔ INGAME-MUSIC coordination subsystem owned by Engine.
//
// PROPER OOP: one instance per Core, embedded on Engine (`eng(c).musicCoord`). Back-pointer
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

  // PSXPORT_XA_DBG change-detector for tick()'s [coord] log line (song/xa-active/xa-loop last-seen).
  uint32_t mPrev = 0xDEAD;
  int      mPa = -1, mPl = -1;

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

  // fieldBgmDirector(): NATIVE field BGM director (currently unwired — the frame loop plays the
  //   recompiled libsnd path; this director played a HARDCODED song over everything from the menu
  //   on). Drives the PC-native synth from the LIVE area bundle (guest 0x80182000: 10 SEPs + the
  //   field VAB @+0x26b4): while the GAME stage (0x8010637C) is active AND the bundle is present,
  //   start the area BGM natively (once, on entry) and keep it playing; stop it when leaving the
  //   field. Song defaults to 8 (field theme); PSXPORT_FIELD_SONG=<0..9> overrides for auditioning.
  //   Gated by the `music` native gate; latch lives on Game (field_bgm_started).
  void fieldBgmDirector();

  // tick(): once-per-frame update (called from native_step_frame). Enforces "dialogs stop
  //   the ingame music" — stops a looping ingame-music clip while a dialog tone is up; resumes
  //   the remembered clip once the dialog ends and the XA stream is free (no voice playing).
  void tick();

  // voiceMixTick(voice_base): per-frame VOICE-CHANNEL VOLUME MIXER — port of FUN_80075824
  //   (RE'd via ghidra on 2026-07-03; docs/findings/audio.md). Ramps the voice channel's current
  //   volume (u16 at +0x2C) toward its target (u16 at +0x2A) by ±0x100/frame (±0x400 when
  //   scratchpad 0x1F800137 == 2), applies a boost when the scratchpad "boost" flag (0x1F80027E)
  //   is nonzero, folds in the ADSR-like second-stage smoother (u16 at +0x30 → +0x2E), and writes
  //   the packed 16-bit volume to voice[+0x10] and voice[+0x12], the pan words voice[+0x04] and
  //   voice[+0x06] to 0x3FFF, and OR's 0xC0 into voice[+0x00] (dirty flag).
  //   Special short-circuits:
  //     scratchpad[+0x19A] != 2 → shortcut: vol = base * 0x47FF >> 15 (boot / silence state).
  //     scratchpad[+0x137] == 1 → dialog mode: vol picks a full/scaled path from DAT_800BE0E4
  //     flags, and OR's 0x3 (extra flags) into voice[+0x00].
  //   Also drops the fade to the "far" level (writes DAT_800BE222 = 0x47FF and pings the SPU
  //   queue helper 0x800750D8) if the running vol falls below 0x10 while voice[+0x33] is armed.
  //   Callable on any voice base; the field frame calls it with voice_base = 0x800BE1F8 (the
  //   ambient/XA channel), per AreaSlots::updateTail (game/world/area_slots.cpp).
  void voiceMixTick(uint32_t voice_base);

  // setGain2(val): FUN_80075D24 — sets the ambient-voice second-stage gain the voiceMixTick
  //   smoother chases (0x800BE1F8+0x2E, "g2_target" in voiceMixTick's comment). val < 0: an
  //   INSTANT snap — writes -val into BOTH the target (+0x2E) AND the current (+0x30), so the
  //   smoother has nothing left to chase (no fade). val >= 0: a RAMPED set — clamps val to
  //   [0,0x1FFF] and writes only the target (+0x2E); voiceMixTick's `g2 += (target-g2)>>3` then
  //   eases toward it over subsequent frames. RE'd via Ghidra headless (scratch/decomp/cluster1.c:
  //   FUN_80075d24).
  void setGain2(int32_t val);

  // registerOverrides(): wires FUN_80075D24 into the global override registry (no static
  // `func_<addr>(c)` call site exists in the recompiled output — only reached via indirect
  // rec_dispatch).
  void registerOverrides();
};
