// A pause transition must preserve the completed native composite before the pause frame can clear or
// replace the live render target. The policy has no SDL dependency: GPU ownership supplies the source
// and executes the copy, while this state machine makes the capture request one-shot and per Game.
#include "native_composite_capture.h"
#include "testutil.h"

namespace {

constexpr NativeCompositeExtent kNative{1024, 512};
constexpr NativeCompositeExtent kIres{3072, 1536};

void test_capture_waits_for_a_completed_native_composite() {
  NativeCompositeCapture capture;
  CHECK(!capture.request());
  CHECK(!capture.plan().copy);
  CHECK(!capture.valid());

  capture.noteCompletedComposite(NativeCompositeSource::Native, kNative);
  CHECK(capture.request());
  const NativeCompositeCapturePlan plan = capture.plan();
  CHECK(plan.copy);
  CHECK(plan.allocate);
  CHECK(plan.source == NativeCompositeSource::Native);
  CHECK_EQ(plan.extent.width, kNative.width);
  CHECK_EQ(plan.extent.height, kNative.height);
}

void test_capture_is_one_shot_and_retains_its_source_extent() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(NativeCompositeSource::Native, kNative);
  CHECK(capture.request());
  CHECK(capture.didCapture(capture.plan()));

  CHECK(capture.valid());
  CHECK_EQ(capture.extent().width, kNative.width);
  CHECK_EQ(capture.extent().height, kNative.height);
  CHECK(!capture.plan().copy);

  CHECK(capture.request());
  const NativeCompositeCapturePlan second = capture.plan();
  CHECK(second.copy);
  CHECK(!second.allocate);
}

void test_scale_change_reallocates_only_the_capture_target() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(NativeCompositeSource::Native, kNative);
  CHECK(capture.request());
  CHECK(capture.didCapture(capture.plan()));

  capture.noteCompletedComposite(NativeCompositeSource::Ires, kIres);
  CHECK(capture.request());
  const NativeCompositeCapturePlan plan = capture.plan();
  CHECK(plan.copy);
  CHECK(plan.allocate);
  CHECK(plan.source == NativeCompositeSource::Ires);
  CHECK_EQ(plan.extent.width, kIres.width);
  CHECK_EQ(plan.extent.height, kIres.height);
}

void test_pending_capture_keeps_its_completed_source() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(NativeCompositeSource::Native, kNative);
  CHECK(capture.request());

  // The following scene may finish before the next presentation fence executes the copy. It must
  // become the source for a later request, not retarget this pending pause backdrop.
  capture.noteCompletedComposite(NativeCompositeSource::Ires, kIres);
  const NativeCompositeCapturePlan plan = capture.plan();
  CHECK(plan.copy);
  CHECK(plan.source == NativeCompositeSource::Native);
  CHECK_EQ(plan.extent.width, kNative.width);
  CHECK_EQ(plan.extent.height, kNative.height);
  CHECK(capture.didCapture(plan));

  CHECK(capture.request());
  const NativeCompositeCapturePlan next = capture.plan();
  CHECK(next.source == NativeCompositeSource::Ires);
  CHECK_EQ(next.extent.width, kIres.width);
  CHECK_EQ(next.extent.height, kIres.height);
}

void test_per_game_capture_state_does_not_leak() {
  NativeCompositeCapture first;
  NativeCompositeCapture second;
  first.noteCompletedComposite(NativeCompositeSource::Native, kNative);
  CHECK(first.request());
  CHECK(first.didCapture(first.plan()));

  CHECK(first.valid());
  CHECK(!second.valid());
  CHECK(!second.plan().copy);
}

void test_capture_refuses_a_stale_plan_and_renderer_teardown() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(NativeCompositeSource::Native, kNative);
  CHECK(capture.request());
  NativeCompositeCapturePlan stale = capture.plan();
  stale.extent = kIres;
  CHECK(!capture.didCapture(stale));
  CHECK(capture.requested());
  CHECK(!capture.valid());

  capture.resetResource();
  CHECK(!capture.requested());
  CHECK(!capture.valid());
  CHECK(!capture.plan().copy);
  CHECK(!capture.request());
}

} // namespace

int main() {
  RUN(capture_waits_for_a_completed_native_composite);
  RUN(capture_is_one_shot_and_retains_its_source_extent);
  RUN(scale_change_reallocates_only_the_capture_target);
  RUN(pending_capture_keeps_its_completed_source);
  RUN(per_game_capture_state_does_not_leak);
  RUN(capture_refuses_a_stale_plan_and_renderer_teardown);
  return pt_summary();
}
