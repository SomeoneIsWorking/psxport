// game_hooks_opt.h — guarded accessors for OPTIONAL GameHooks entries.
//
// WHY THIS EXISTS. GameHooks is a vtable of callbacks a game MAY supply. Two of the three consumers
// bind it with DESIGNATED INITIALISERS, so every hook they do not name is value-initialised to
// nullptr — a game opts out of a hook by saying nothing. That is the intended design, and it means
// EVERY optional hook is null on some real port right now.
//
// The framework kept calling them anyway. Three times, each found by a crash rather than a test, and
// each invisible in the reference consumer because Tomba!2 supplies every hook:
//   * audioMixFrame   — spyro died on its first audio frame.
//   * renderFadeState — spyro died on its first present.
//   * audioNowPlayingName — spider1 and spyro died when ESC opened the debug overlay, because
//     rmlui_overlay's refreshReadouts() guarded on `game` but not on the hook. The user saw it as
//     "ESC closes the game" and as "the RmlUi overlay only works in Tomba!2": one defect, two
//     symptoms, in two games.
//
// So the null check is NOT left to each call site. An optional hook gets ONE accessor here that
// answers safely when the game opted out, and every caller goes through it. A call site that reaches
// `hooks->thing(...)` directly for an optional hook is the bug, not a style preference.
//
// WHAT "SAFELY" MEANS: the absent case must be a DEFINED answer that the caller can render — a null
// name reads as "nothing playing", a no-op action does nothing — never a sentinel the caller has to
// remember to test, and never a fabricated value that makes an unsupported feature look supported.
//
// SHAPE: each accessor takes the hooks TABLE explicitly rather than reaching through `Core`, matching
// spu_mix_game_audio (tests/test_spu_mix_optional_hook.cpp). That is what makes the guard testable at
// all — `Core` has a real constructor that cannot be stood up in a hermetic test, so an accessor which
// only took `Core*` could never have its null cases exercised, and an unexercised guard is how this
// bug class keeps coming back. The caller reads `game->core.hooks` (a plain member read) and passes it.
#pragma once

class Core;
struct FadeState;
struct GameHooks;

// Read the optional game-owned fade state. A title with no legacy hook table, or no fade hook in
// that table, is exactly the default zero/no-fade state. Keeping both cases here prevents renderer
// call sites from dereferencing the compatibility table directly.
FadeState game_render_fade_state(Core *c, const GameHooks *hooks);

// Currently-playing Sound-Test track name, or nullptr when nothing is playing OR the game has no
// Sound Test at all. The overlay renders both as "stopped", which is exactly right: a game without a
// sound-test catalogue genuinely has nothing playing.
const char *game_audio_now_playing_name(Core *c, const GameHooks *hooks);

// Sound-Test action: play catalogued track (>= 0) or stop (< 0). Silently does nothing when the game
// has no Sound Test — the menu row is inert rather than fatal. `track` reaches the game unchanged;
// the negative "stop" value is meaningful and must not be clamped.
void game_audio_sound_test_play(Core *c, const GameHooks *hooks, int track);
