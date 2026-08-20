// test_spu_mix_optional_hook.cpp — the game-music mix step must tolerate a port that HAS no game
// music engine.
//
// THE DEFECT THIS GATES. SpuAudio::frameEx mixed the game's native music on top of the SPU output
// with `if (game) game->core.hooks->audioMixFrame(...)` — a check on the GAME, not on the HOOK.
// `audioMixFrame` is optional: psxport's own convention is that a port binds its GameHooks by name
// and every hook it has not stood up stays null (the framework does test the pointer for other
// optional hooks, e.g. devWarpAreaEnter in native_boot.cpp). The first consumer without a native
// music engine therefore called through a null pointer the instant anything drove the mixer —
// observed in spider1 as SIGSEGV at spu_audio.cpp:176 the moment per-field audio was wired up,
// backtrace bottoming out at address 0.
//
// Both classes are exercised, because a guard that never lets the hook run would "pass" the null
// case while silently disabling game music everywhere:
//   * hook absent  -> no call, buffer left exactly as the SPU rendered it
//   * hook present -> called exactly once, with the buffer and frame count it was given
//
// Hermetic: spu_mix_game_audio touches neither the SPU nor a device, and neither the null case nor
// the stub hook below dereferences `Core*`, so a null Core is a legitimate input here.
#include "testutil.h"

#include <stdint.h>

#include "game_iface.h"
#include "spu_audio.h"

namespace {

int g_calls = 0;
int g_frames = -1;
int16_t *g_buf = nullptr;

void stub_mix(Core *c, int16_t *buf, int frames) {
  (void)c;
  g_calls++;
  g_frames = frames;
  g_buf = buf;
  for (int i = 0; i < frames * 2; i++) {
    buf[i] = (int16_t)(buf[i] + 100);
  }
}

} // namespace

// A port with no music engine of its own: the hook is null and the mix step must do nothing at all.
static void test_null_hook_is_a_no_op(void) {
  GameHooks hooks{}; // value-initialised: every hook, audioMixFrame included, null
  CHECK(hooks.audioMixFrame == nullptr);

  int16_t buf[8];
  for (int i = 0; i < 8; i++) {
    buf[i] = (int16_t)(i * 7);
  }
  int16_t want[8];
  for (int i = 0; i < 8; i++) {
    want[i] = (int16_t)(i * 7);
  }

  g_calls = 0;
  spu_mix_game_audio(nullptr, &hooks, buf, 4); // 4 stereo frames = 8 samples

  CHECK_EQ(g_calls, 0);
  CHECK_MEM_EQ(buf, want, sizeof want);
}

// A null hook TABLE is the same contract — nothing to mix, and nothing to dereference.
static void test_null_hooks_table_is_a_no_op(void) {
  int16_t buf[4] = {1, 2, 3, 4};
  int16_t want[4] = {1, 2, 3, 4};

  g_calls = 0;
  spu_mix_game_audio(nullptr, nullptr, buf, 2);

  CHECK_EQ(g_calls, 0);
  CHECK_MEM_EQ(buf, want, sizeof want);
}

// The other class: a port that DOES have a music engine must still get its hook called, once, with
// the buffer and frame count it was handed. Without this case a guard of `if (false)` would pass.
static void test_present_hook_is_called_once(void) {
  GameHooks hooks{};
  hooks.audioMixFrame = &stub_mix;

  int16_t buf[6] = {0, 0, 0, 0, 0, 0};
  int16_t want[6] = {100, 100, 100, 100, 100, 100};

  g_calls = 0;
  g_frames = -1;
  g_buf = nullptr;
  spu_mix_game_audio(nullptr, &hooks, buf, 3);

  CHECK_EQ(g_calls, 1);
  CHECK_EQ(g_frames, 3);
  CHECK(g_buf == buf);
  CHECK_MEM_EQ(buf, want, sizeof want);
}

int main(void) {
  RUN(null_hook_is_a_no_op);
  RUN(null_hooks_table_is_a_no_op);
  RUN(present_hook_is_called_once);
  return pt_summary();
}
