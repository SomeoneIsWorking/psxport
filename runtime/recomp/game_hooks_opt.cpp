// game_hooks_opt.cpp — impl. See game_hooks_opt.h for why optional hooks are never called directly.
#include "game_hooks_opt.h"
#include "core.h"
#include "game_iface.h"

const char* game_audio_now_playing_name(Core* c, const GameHooks* hooks) {
  if (!hooks || !hooks->audioNowPlayingName) return nullptr;
  return hooks->audioNowPlayingName(c);
}

void game_audio_sound_test_play(Core* c, const GameHooks* hooks, int track) {
  if (!hooks || !hooks->audioSoundTestPlay) return;
  hooks->audioSoundTestPlay(c, track);
}

bool game_fps60_read_scene_cam(Core* c, const GameHooks* hooks, float R[3][3], float T[3]) {
  if (!hooks || !hooks->fps60ReadSceneCam) return false;
  hooks->fps60ReadSceneCam(c, R, T);
  return true;
}
