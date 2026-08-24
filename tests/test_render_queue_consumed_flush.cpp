// A RenderQueue keeps its consumed payload until the next push so that starting a new queue is lazy.
// That retained storage is not another submission: a later empty DrawOTag/flush must not capture it
// again. This drives the shipping queue and FramePresenter together, without a GPU or game window.
#include "game.h"
#include "render_queue.h"
#include "testutil.h"

#include <memory>

namespace {

void push_hud_item(RenderQueue &queue) {
  RqItem *item = queue.push();
  CHECK(item != nullptr);
  *item = RqItem{};
  item->layer = RQ_HUD;
  item->seq = queue.seq - 1;
}

void test_consumed_flush_is_empty_until_another_push() {
  const auto game = std::make_unique<Game>();
  RenderQueue &queue = game->rq;

  push_hud_item(queue);
  queue.flush(&game->core);
  CHECK_EQ(game->presentation.capturedCount(), 1);
  CHECK_EQ(queue.mLedger.captured[RQ_HUD], 1L);
  CHECK_EQ(queue.consumed, 1);

  // No producer pushed between these draw boundaries. The retained item is storage for lazy reset,
  // not a second frame submission.
  queue.flush(&game->core);
  CHECK_EQ(game->presentation.capturedCount(), 1);
  CHECK_EQ(queue.mLedger.captured[RQ_HUD], 1L);
  CHECK_EQ(queue.consumed, 1);

  // The next real push must still perform the lazy reset and start a fresh one-item queue.
  push_hud_item(queue);
  CHECK_EQ(queue.n, 1);
  CHECK_EQ(queue.consumed, 0);
  queue.flush(&game->core);
  CHECK_EQ(game->presentation.capturedCount(), 2);
  CHECK_EQ(queue.mLedger.captured[RQ_HUD], 2L);
  CHECK_EQ(queue.consumed, 1);
}

} // namespace

int main() {
  RUN(consumed_flush_is_empty_until_another_push);
  return pt_summary();
}
