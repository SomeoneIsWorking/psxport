// test_optional_hook_guards.cpp — an OPTIONAL GameHooks entry must be safe to leave null.
//
// THE DEFECT THIS GATES, which has now happened THREE times. GameHooks is a vtable of callbacks a game
// MAY supply. spider1 and spyro bind by DESIGNATED INITIALISER, so every hook they do not list is
// value-initialised to nullptr — and spyro spells two of them out as `nullptr` explicitly. The
// framework then called them without checking, jumping to address 0:
//
//   1. audioMixFrame    — segfaulted spyro on its first audio frame (fixed: spu_mix_game_audio).
//   2. renderFadeState  — segfaulted spyro on its first present (fixed in show_composite).
//   3. audioNowPlayingName / audioSoundTestPlay — THIS ONE. rmlui_overlay's refreshReadouts() guards
//      on `game` but not on the hook, so pressing ESC to open the debug menu in spider1 or spyro
//      called through a null pointer and the process died. To the user that reads as "ESC closes the
//      game", and as "the RmlUi overlay only works in Tomba!2" — one defect, two symptoms.
//
// Each was found by a crash rather than by a test, and each was invisible in the reference consumer
// because Tomba!2 supplies every hook. So the guard lives in ONE place now (game_hooks_opt.h) and this
// file is what keeps it honest.
//
// BOTH CLASSES, for every accessor: the ABSENT case must be a safe, defined answer (null / no-op) AND
// the PRESENT case must actually reach the game. A guard that never calls through would pass a
// null-only test while silently disabling the feature — that is why the present-hook cases assert the
// call count and the arguments, not merely that nothing crashed.
#include "testutil.h"

#include <stdint.h>
#include <string.h>

#include "game_iface.h"
#include "game_hooks_opt.h"

// `Core` is never CONSTRUCTED here — it has a real constructor that a hermetic test cannot stand up.
// The accessors take the hooks table explicitly for exactly that reason, so the Core pointer is only
// ever FORWARDED, never dereferenced, and the spies record it to prove it arrives unchanged. A
// sentinel address stands in for a real Core (same shape as the audioMixFrame test).
class Core;
static Core* const kFakeCore = reinterpret_cast<Core*>(0xC0FFEE00u);

// ---- spies -------------------------------------------------------------------------------------
static int  s_name_calls = 0;
static int  s_play_calls = 0;
static int  s_play_track = -999;
static Core* s_name_core  = nullptr;
static Core* s_play_core  = nullptr;

static const char* spy_now_playing(Core* c) { s_name_calls++; s_name_core = c; return "Track 07"; }
static void        spy_sound_test(Core* c, int track) { s_play_calls++; s_play_core = c; s_play_track = track; }

static void reset_spies(void) {
  s_name_calls = s_play_calls = 0; s_play_track = -999;
  s_name_core = s_play_core = nullptr;
}

// A null hooks TABLE is a different failure from a null hook IN the table — a port that never installs
// hooks at all reaches the same call sites, so both are asserted separately.
static void test_absent_hook_is_a_safe_answer_not_a_jump_to_zero(void) {
  GameHooks hooks{};            // every entry value-initialised to nullptr — the spider1/spyro shape
  CHECK(hooks.audioNowPlayingName == nullptr);
  CHECK(hooks.audioSoundTestPlay  == nullptr);
  reset_spies();

  CHECK(game_audio_now_playing_name(nullptr, &hooks) == nullptr);   // "nothing playing", not a crash
  game_audio_sound_test_play(nullptr, &hooks, 3);                   // silent no-op
  CHECK_EQ(s_play_calls, 0);
}

static void test_a_null_hooks_table_is_also_safe(void) {
  reset_spies();
  CHECK(game_audio_now_playing_name(nullptr, nullptr) == nullptr);
  game_audio_sound_test_play(nullptr, nullptr, 0);
  CHECK_EQ(s_play_calls, 0);
}

// THE OTHER CLASS. A guard that swallowed every call would pass the tests above while quietly
// disabling the Sound Test in the one game that has it, so assert the hook is reached, exactly once,
// with the arguments the caller passed.
static void test_a_present_hook_is_actually_called(void) {
  GameHooks hooks{};
  hooks.audioNowPlayingName = spy_now_playing;
  hooks.audioSoundTestPlay  = spy_sound_test;
  reset_spies();

  const char* nm = game_audio_now_playing_name(kFakeCore, &hooks);
  CHECK(nm != nullptr);
  CHECK(strcmp(nm, "Track 07") == 0);
  CHECK_EQ(s_name_calls, 1);
  CHECK(s_name_core == kFakeCore);      // the Core is forwarded, not dropped or rewritten

  game_audio_sound_test_play(kFakeCore, &hooks, 7);
  CHECK_EQ(s_play_calls, 1);
  CHECK_EQ(s_play_track, 7);
  CHECK(s_play_core == kFakeCore);

  // The stop action is track < 0 and must reach the game unchanged — a guard that clamped or dropped
  // it would make "stop" silently do nothing.
  game_audio_sound_test_play(kFakeCore, &hooks, -1);
  CHECK_EQ(s_play_calls, 2);
  CHECK_EQ(s_play_track, -1);
}

int main(void) {
  RUN(absent_hook_is_a_safe_answer_not_a_jump_to_zero);
  RUN(a_null_hooks_table_is_also_safe);
  RUN(a_present_hook_is_actually_called);
  return pt_summary();
}
