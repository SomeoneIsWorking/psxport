#include "audio_queue_policy.h"
#include "testutil.h"

static void test_buffer_sizes_are_bounded_pcm_durations(void) {
  CHECK_EQ(AudioQueuePolicy::kPlaybackCushionBytes, 31752);
  CHECK_EQ(AudioQueuePolicy::kQueueLimitBytes, 63504);
  CHECK(AudioQueuePolicy::kPlaybackCushionBytes < AudioQueuePolicy::kQueueLimitBytes);
}

static void test_playback_starts_only_after_the_cushion_is_primed(void) {
  CHECK(!AudioQueuePolicy::readyToStart(AudioQueuePolicy::kPlaybackCushionBytes - 1));
  CHECK(AudioQueuePolicy::readyToStart(AudioQueuePolicy::kPlaybackCushionBytes));
}

static void test_only_a_confirmed_started_empty_queue_reprimes(void) {
  CHECK(!AudioQueuePolicy::needsReprime(false, 0));
  CHECK(!AudioQueuePolicy::needsReprime(true, 1));
  CHECK(AudioQueuePolicy::needsReprime(true, 0));
}

static void test_queue_limit_drops_only_excess_catch_up_audio(void) {
  CHECK(!AudioQueuePolicy::shouldDrop(AudioQueuePolicy::kQueueLimitBytes - 1));
  CHECK(AudioQueuePolicy::shouldDrop(AudioQueuePolicy::kQueueLimitBytes));
}

int main(void) {
  RUN(buffer_sizes_are_bounded_pcm_durations);
  RUN(playback_starts_only_after_the_cushion_is_primed);
  RUN(only_a_confirmed_started_empty_queue_reprimes);
  RUN(queue_limit_drops_only_excess_catch_up_audio);
  return pt_summary();
}
