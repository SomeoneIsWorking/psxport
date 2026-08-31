// A pause transition must preserve the completed native composite before the pause frame can clear or
// replace the live render target. The policy has no SDL dependency: GPU ownership supplies the source
// and executes the copy, while this state machine makes the capture request one-shot and per Game.
#include "native_composite_capture.h"
#include "testutil.h"

namespace {

constexpr NativeCompositeExtent kNative{1024, 512};
constexpr NativeCompositeExtent kIres{3072, 1536};
constexpr NativeCompositeFrame kNativeFrame{NativeCompositeSource::Native, kNative, {192, 96, 640, 360}, 1};
constexpr NativeCompositeFrame kIresFrame{NativeCompositeSource::Ires, kIres, {576, 288, 1920, 1080}, 3};

void test_capture_waits_for_a_completed_native_composite() {
  NativeCompositeCapture capture;
  CHECK(!capture.request());
  CHECK(!capture.plan().copy);
  CHECK(!capture.valid());

  capture.noteCompletedComposite(kNativeFrame);
  CHECK(capture.request());
  const NativeCompositeCapturePlan plan = capture.plan();
  CHECK(plan.copy);
  CHECK(plan.allocate);
  CHECK(plan.frame == kNativeFrame);
  CHECK_EQ(plan.captureExtent.width, kNativeFrame.sourceRect.width);
  CHECK_EQ(plan.captureExtent.height, kNativeFrame.sourceRect.height);
}

void test_capture_is_one_shot_and_retains_its_source_extent() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(kNativeFrame);
  CHECK(capture.request());
  CHECK(capture.didCapture(capture.plan()));

  CHECK(capture.valid());
  CHECK_EQ(capture.extent().width, kNativeFrame.sourceRect.width);
  CHECK_EQ(capture.extent().height, kNativeFrame.sourceRect.height);
  CHECK(!capture.plan().copy);

  CHECK(capture.request());
  const NativeCompositeCapturePlan second = capture.plan();
  CHECK(second.copy);
  CHECK(!second.allocate);
}

void test_scale_change_reallocates_only_the_capture_target() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(kNativeFrame);
  CHECK(capture.request());
  CHECK(capture.didCapture(capture.plan()));

  capture.noteCompletedComposite(kIresFrame);
  CHECK(capture.request());
  const NativeCompositeCapturePlan plan = capture.plan();
  CHECK(plan.copy);
  CHECK(plan.allocate);
  CHECK(plan.frame == kIresFrame);
  CHECK_EQ(plan.captureExtent.width, kIresFrame.sourceRect.width);
  CHECK_EQ(plan.captureExtent.height, kIresFrame.sourceRect.height);
}

void test_pending_capture_keeps_its_completed_source() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(kNativeFrame);
  CHECK(capture.request());

  // The following scene may finish before the next presentation fence executes the copy. It must
  // become the source for a later request, not retarget this pending pause backdrop.
  capture.noteCompletedComposite(kIresFrame);
  const NativeCompositeCapturePlan plan = capture.plan();
  CHECK(plan.copy);
  CHECK(plan.frame == kNativeFrame);
  CHECK(capture.didCapture(plan));

  CHECK(capture.request());
  const NativeCompositeCapturePlan next = capture.plan();
  CHECK(next.frame == kIresFrame);
}

void test_per_game_capture_state_does_not_leak() {
  NativeCompositeCapture first;
  NativeCompositeCapture second;
  first.noteCompletedComposite(kNativeFrame);
  CHECK(first.request());
  CHECK(first.didCapture(first.plan()));

  CHECK(first.valid());
  CHECK(!second.valid());
  CHECK(!second.plan().copy);
}

void test_capture_refuses_a_stale_plan_and_renderer_teardown() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(kNativeFrame);
  CHECK(capture.request());
  NativeCompositeCapturePlan stale = capture.plan();
  stale.frame = kIresFrame;
  CHECK(!capture.didCapture(stale));
  CHECK(capture.requested());
  CHECK(!capture.valid());

  capture.resetResource();
  CHECK(!capture.requested());
  CHECK(!capture.valid());
  CHECK(!capture.plan().copy);
  CHECK(!capture.request());
}

void test_capture_uses_the_completed_display_rect_once_as_the_next_base() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(kNativeFrame);
  CHECK(capture.request());
  CHECK(capture.didCapture(capture.plan()));

  const NativeCompositeBasePlan base = capture.takeBase(kNativeFrame);
  CHECK(base.blit);
  CHECK(!base.refused);
  CHECK(base.destination == kNativeFrame.sourceRect);
  CHECK(!capture.valid());
  CHECK(!capture.takeBase(kNativeFrame).blit);
}

void test_capture_refuses_an_incompatible_next_target_without_retaining_stale_base() {
  NativeCompositeCapture capture;
  capture.noteCompletedComposite(kIresFrame);
  CHECK(capture.request());
  CHECK(capture.didCapture(capture.plan()));

  NativeCompositeFrame incompatible = kIresFrame;
  incompatible.sourceRect.x += 3;
  const NativeCompositeBasePlan refused = capture.takeBase(incompatible);
  CHECK(!refused.blit);
  CHECK(refused.refused);
  CHECK(!capture.valid());
  CHECK(!capture.takeBase(kIresFrame).blit);
}

} // namespace

int main() {
  RUN(capture_waits_for_a_completed_native_composite);
  RUN(capture_is_one_shot_and_retains_its_source_extent);
  RUN(scale_change_reallocates_only_the_capture_target);
  RUN(pending_capture_keeps_its_completed_source);
  RUN(per_game_capture_state_does_not_leak);
  RUN(capture_refuses_a_stale_plan_and_renderer_teardown);
  RUN(capture_uses_the_completed_display_rect_once_as_the_next_base);
  RUN(capture_refuses_an_incompatible_next_target_without_retaining_stale_base);
  return pt_summary();
}
