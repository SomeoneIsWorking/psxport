#include "guest_draw_rows.h"
#include "testutil.h"

using psx::gpu::RecentDrawRows;
using psx::gpu::RowSpan;

// Spyro 2's double buffer: draw rows 12..227 for the window at (0,0) and 240..455 for the one at
// (0,228). Whichever buffer is displayed, its own draw rows are the newest span inside its window,
// even though the newest span overall belongs to the other buffer.
static void test_each_window_finds_its_own_buffer(void) {
  RecentDrawRows rows;
  rows.note({12, 228});
  rows.note({240, 456});
  CHECK(rows.newestIntersecting({0, 240}) == (RowSpan{12, 228}));
  CHECK(rows.newestIntersecting({228, 468}) == (RowSpan{240, 456}));
}

// Re-issuing a held span moves it to the front instead of taking a second slot, so a buffer that
// re-states its own area every field cannot evict the other buffer's span.
static void test_reissue_does_not_evict(void) {
  RecentDrawRows rows;
  rows.note({12, 228});
  for (int i = 0; i < 2 * RecentDrawRows::kCapacity; ++i) {
    rows.note({240, 456});
  }
  CHECK(rows.newestIntersecting({0, 240}) == (RowSpan{12, 228}));
}

// The newest intersecting span wins: a full-window area set during a transition is superseded by the
// letterboxed one the guest draws with afterwards.
static void test_newest_intersecting_wins(void) {
  RecentDrawRows rows;
  rows.note({0, 240});
  rows.note({12, 228});
  CHECK(rows.newestIntersecting({0, 240}) == (RowSpan{12, 228}));
}

// Past capacity the oldest span is dropped, and a window nothing was drawn into finds nothing.
static void test_oldest_is_evicted_and_untouched_window_is_empty(void) {
  RecentDrawRows rows;
  for (int i = 0; i <= RecentDrawRows::kCapacity; ++i) {
    rows.note({i * 10, i * 10 + 5});
  }
  CHECK(!rows.newestIntersecting({0, 5}).has_value());
  CHECK(rows.newestIntersecting({40, 45}) == (RowSpan{40, 45}));
  CHECK(!rows.newestIntersecting({100, 200}).has_value());
}

int main(void) {
  RUN(each_window_finds_its_own_buffer);
  RUN(reissue_does_not_evict);
  RUN(newest_intersecting_wins);
  RUN(oldest_is_evicted_and_untouched_window_is_empty);
  return pt_summary();
}
