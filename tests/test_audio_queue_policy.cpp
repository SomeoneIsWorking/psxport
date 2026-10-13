#include "audio_queue_policy.h"
#include "testutil.h"

#include <cmath>

namespace {

constexpr double kBytesPerSecond = AudioQueuePolicy::kBytesPerSecond;
constexpr double kFieldSeconds = 1001.0 / 60000.0;
// The device takes one 1024-frame period at a time.
constexpr double kPeriodSeconds = 1024.0 / AudioQueuePolicy::kSampleRateHz;

double ms(double bytes) {
  return bytes * 1000.0 / kBytesPerSecond;
}

// One field producer and a period-pulling device, driven through the shipping steer. `clockError` is
// the device clock's rate error against the producer's.
struct Playback {
  AudioQueueSteer steer;
  double queued = 0;
  double ratio = 1.0;
  double nextPull = 0;
  double now = 0;
  double minRatio = 1.0;
  double maxRatio = 1.0;
  int drops = 0;
  int empties = 0;

  explicit Playback(double startBytes) : queued(startBytes) {
    steer.reset(static_cast<int>(startBytes));
  }

  void field(double clockError) {
    const double fieldBytes = kBytesPerSecond * kFieldSeconds;
    now += kFieldSeconds;
    while (nextPull <= now) {
      queued -= kPeriodSeconds * kBytesPerSecond * ratio * (1.0 + clockError);
      if (queued <= 0) {
        queued = 0;
        ++empties;
      }
      nextPull += kPeriodSeconds;
    }
    if (AudioQueuePolicy::shouldDrop(static_cast<int>(queued))) {
      ++drops;
      return;
    }
    queued += fieldBytes;
    ratio = steer.observe(static_cast<int>(queued));
    minRatio = std::fmin(minRatio, ratio);
    maxRatio = std::fmax(maxRatio, ratio);
  }

  void run(double seconds, double clockError) {
    const double end = now + seconds;
    while (now < end) {
      field(clockError);
    }
  }
};

} // namespace

static void test_buffer_sizes_are_bounded_pcm_durations(void) {
  CHECK_EQ(AudioQueuePolicy::kPlaybackTargetBytes, 15876);
  CHECK_EQ(AudioQueuePolicy::kQueueLimitBytes, 31752);
  CHECK(AudioQueuePolicy::kPlaybackTargetBytes < AudioQueuePolicy::kQueueLimitBytes);
}

static void test_playback_starts_only_after_the_target_is_primed(void) {
  CHECK(!AudioQueuePolicy::readyToStart(AudioQueuePolicy::kPlaybackTargetBytes - 1));
  CHECK(AudioQueuePolicy::readyToStart(AudioQueuePolicy::kPlaybackTargetBytes));
}

static void test_only_a_confirmed_started_empty_queue_reprimes(void) {
  CHECK(!AudioQueuePolicy::needsReprime(false, 0));
  CHECK(!AudioQueuePolicy::needsReprime(true, 1));
  CHECK(AudioQueuePolicy::needsReprime(true, 0));
}

static void test_queue_limit_drops_only_excess_audio(void) {
  CHECK(!AudioQueuePolicy::shouldDrop(AudioQueuePolicy::kQueueLimitBytes - 1));
  CHECK(AudioQueuePolicy::shouldDrop(AudioQueuePolicy::kQueueLimitBytes));
}

static void test_ratio_is_unity_at_the_target_and_bounded_beyond_it(void) {
  CHECK(AudioQueuePolicy::consumptionRatio(AudioQueuePolicy::kPlaybackTargetBytes) == 1.0);
  CHECK(AudioQueuePolicy::consumptionRatio(1e9) == 1.0 + AudioQueuePolicy::kMaxRateTrim);
  CHECK(AudioQueuePolicy::consumptionRatio(0) == 1.0 - AudioQueuePolicy::kMaxRateTrim);
}

// A queue filled to the drop limit (an unpaced boot burst) drains to the target without a drop.
static void test_a_full_queue_converges_to_the_target(void) {
  Playback playback(AudioQueuePolicy::kQueueLimitBytes - 4000);
  playback.run(60.0, 0.0);
  CHECK(std::fabs(ms(playback.queued) - AudioQueuePolicy::kPlaybackTargetMs) < 12.0);
  CHECK(std::fabs(ms(playback.steer.level()) - AudioQueuePolicy::kPlaybackTargetMs) < 5.0);
  CHECK_EQ(playback.drops, 0);
  CHECK_EQ(playback.empties, 0);
  CHECK(playback.maxRatio <= 1.0 + AudioQueuePolicy::kMaxRateTrim);
}

// A hitch that ate most of the queue refills it to the target.
static void test_a_drained_queue_refills_to_the_target(void) {
  Playback playback(AudioQueuePolicy::kBytesPerSecond * 30 / 1000);
  playback.run(60.0, 0.0);
  CHECK(std::fabs(ms(playback.steer.level()) - AudioQueuePolicy::kPlaybackTargetMs) < 5.0);
  CHECK(playback.minRatio >= 1.0 - AudioQueuePolicy::kMaxRateTrim);
}

// At the target with a 200 ppm clock mismatch either way, the steady state neither drops nor empties
// and the trim stays a small fraction of its range.
static void test_steady_state_holds_the_target_through_clock_drift(void) {
  for (const double error : {200e-6, -200e-6}) {
    Playback playback(AudioQueuePolicy::kPlaybackTargetBytes);
    playback.run(600.0, error);
    CHECK_EQ(playback.drops, 0);
    CHECK_EQ(playback.empties, 0);
    CHECK(std::fabs(ms(playback.steer.level()) - AudioQueuePolicy::kPlaybackTargetMs) < 10.0);
    CHECK(std::fabs(playback.ratio - 1.0) < 0.001);
  }
}

int main(void) {
  RUN(buffer_sizes_are_bounded_pcm_durations);
  RUN(playback_starts_only_after_the_target_is_primed);
  RUN(only_a_confirmed_started_empty_queue_reprimes);
  RUN(queue_limit_drops_only_excess_audio);
  RUN(ratio_is_unity_at_the_target_and_bounded_beyond_it);
  RUN(a_full_queue_converges_to_the_target);
  RUN(a_drained_queue_refills_to_the_target);
  RUN(steady_state_holds_the_target_through_clock_drift);
  return pt_summary();
}
