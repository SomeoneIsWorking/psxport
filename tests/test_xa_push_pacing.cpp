// A push-mode XA stream fed at the disc rate plays at its true pitch from the start, and a clip
// shorter than the playback backlog still plays.
#include "testutil.h"
#include "xa_push_pacing.h"
#include "xa_state.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace {

constexpr uint32_t kPullsPerSecond = 44100;
constexpr uint32_t kPullsPerSector = 2352; // 2016 stereo frames at 37.8 kHz
constexpr double kNominalStep = 37800.0 / 44100.0;

std::array<uint8_t, 2352> stereoAudioSector() {
  std::array<uint8_t, 2352> raw{};
  raw[15] = 2u;    // Mode 2
  raw[18] = 0x64u; // real-time, form 2, audio
  raw[19] = 0x01u; // stereo, 37.8 kHz, 4-bit
  return raw;
}

std::unique_ptr<XaState> pushStream() {
  auto xa = std::make_unique<XaState>();
  xa_state_init(xa.get());
  xa->push_mode = 1;
  xa->active = 1;
  xa_bind_state(xa.get());
  return xa;
}

void test_a_stream_fed_at_the_disc_rate_plays_at_its_true_pitch() {
  auto xa = pushStream();
  const auto sector = stereoAudioSector();
  std::array<int32_t, 2> sample{};
  double rdAfterFirstSecond = 0.0;
  for (uint32_t pull = 0; pull < 3u * kPullsPerSecond; ++pull) {
    if (pull % kPullsPerSector == 0u) {
      CHECK_EQ(xa_push_audio_sector(xa.get(), sector.data(), pull / kPullsPerSector), 2016);
    }
    CDC_GetCDAudioSample(sample.data());
    if (pull + 1u == kPullsPerSecond) {
      rdAfterFirstSecond = xa->rd;
    }
  }
  const double pitch = (xa->rd - rdAfterFirstSecond) / (2.0 * kPullsPerSecond) / kNominalStep - 1.0;
  CHECK(std::fabs(pitch) < 0.002);
  CHECK(xa->wr - static_cast<uint32_t>(xa->rd) < 2u * XaPushPacing::kTargetBacklogFrames);
}

void test_a_clip_shorter_than_the_backlog_still_plays() {
  auto xa = pushStream();
  const auto sector = stereoAudioSector();
  std::array<int32_t, 2> sample{};
  CHECK_EQ(xa_push_audio_sector(xa.get(), sector.data(), 0u), 2016);
  for (uint32_t pull = 0; pull < kPullsPerSecond; ++pull) {
    CDC_GetCDAudioSample(sample.data());
  }
  CHECK(xa->rd >= 2015.0);
}

} // namespace

int main() {
  RUN(a_stream_fed_at_the_disc_rate_plays_at_its_true_pitch);
  RUN(a_clip_shorter_than_the_backlog_still_plays);
  return pt_summary();
}
