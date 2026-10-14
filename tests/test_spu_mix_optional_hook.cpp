// The game-music mix must tolerate a port with no music engine: spider1 crashed through a null
// audioMixFrame the moment per-field audio was wired. Both classes are exercised, because a guard that
// never lets the hook run would pass the null case while silencing game music everywhere.
#include "testutil.h"

#include <cstdint>
#include <memory>

#include "game.h"
#include "game_iface.h"

namespace {

int g_calls = 0;
int g_frames = -1;
int16_t *g_buf = nullptr;

void stub_mix(Core *, int16_t *buf, int frames) {
  g_calls++;
  g_frames = frames;
  g_buf = buf;
  for (int i = 0; i < frames * 2; i++) {
    buf[i] = static_cast<int16_t>(buf[i] + 100);
  }
}

const char *stub_name(Core *) {
  return nullptr;
}

// A Sound Test without a music engine: TitleAudio exists, its mix leaves the SPU output alone.
void test_absent_mix_hook_leaves_the_spu_output_alone() {
  const GameConfig config{};
  GameHooks hooks{};
  hooks.audioNowPlayingName = stub_name;
  LegacyGameRuntimeAdapter runtime(config, hooks);
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  psx::game::TitleAudio *audio = runtime.audio(game->core);
  CHECK(audio != nullptr);
  if (!audio) {
    return;
  }

  int16_t buf[8] = {0, 7, 14, 21, 28, 35, 42, 49};
  const int16_t want[8] = {0, 7, 14, 21, 28, 35, 42, 49};
  g_calls = 0;
  audio->mixFrame(buf, 4);

  CHECK_EQ(g_calls, 0);
  CHECK_MEM_EQ(buf, want, sizeof want);
}

void test_present_mix_hook_is_called_once() {
  const GameConfig config{};
  GameHooks hooks{};
  hooks.audioMixFrame = stub_mix;
  LegacyGameRuntimeAdapter runtime(config, hooks);
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  psx::game::TitleAudio *audio = runtime.audio(game->core);
  CHECK(audio != nullptr);
  if (!audio) {
    return;
  }

  int16_t buf[6] = {0, 0, 0, 0, 0, 0};
  const int16_t want[6] = {100, 100, 100, 100, 100, 100};
  g_calls = 0;
  g_frames = -1;
  g_buf = nullptr;
  audio->mixFrame(buf, 3);

  CHECK_EQ(g_calls, 1);
  CHECK_EQ(g_frames, 3);
  CHECK(g_buf == buf);
  CHECK_MEM_EQ(buf, want, sizeof want);
}

} // namespace

int main(void) {
  RUN(absent_mix_hook_leaves_the_spu_output_alone);
  RUN(present_mix_hook_is_called_once);
  return pt_summary();
}
