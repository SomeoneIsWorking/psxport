// test_record_cadence.cpp — FramePresenter's record path at 60 fps: the in-between of the two last shown
// pictures, or the shown record on its cut, then N.
#include "frame_presenter.h"
#include "marker_render.h"
#include "testutil.h"

#include <string>
#include <variant>
#include <vector>

namespace {

using psx::present::DrawPrimitive;
using psx::present::FrameRecord;
using psx::present::RecordKey;

constexpr psx::gpu::RecordRect kBufferA{0, 0, 320, 240};
constexpr psx::gpu::RecordRect kBufferB{0, 256, 320, 496};

class RecordBackend final : public psx::frame::FramePresentationBackend {
public:
  explicit RecordBackend(const psx::frame::FramePresenter &presenter) : presenter_(presenter) {}

  void emit(std::span<const RqItem>) override {}
  void presentReal() override {
    calls.emplace_back("real");
    if (producers == nullptr) {
      CHECK(presenter_.composedRecord() == nullptr);
    } else if (const FrameRecord *record = presenter_.composedRecord()) {
      real = *record;
      realAdvances = presenter_.composedAdvances();
    }
    realComposed = presenter_.composedRecord() != nullptr;
  }
  void captureDiagnostic(uint64_t, bool) override {}
  void pace(int, int parts) override {
    paceParts.push_back(parts);
  }
  void reconcile(uint64_t) override {}
  void beginLedgerFrame() override {}
  bool interpolatesRecords() const override {
    return true;
  }
  bool sealedFrameIsCut() override {
    return cut;
  }
  psx::gpu::RecordRect displayedBuffer() override {
    return display;
  }
  void presentInBetween() override {
    calls.emplace_back("in-between");
    const FrameRecord *record = presenter_.composedRecord();
    CHECK(record != nullptr);
    if (record != nullptr) {
      shown = *record;
      shownIsCurrent = record == &presenter_.currentRecord();
    }
    CHECK(!presenter_.composedAdvances());
  }
  const psx::present::StateProducers *stateProducers() override {
    return producers;
  }

  bool cut = false;
  psx::gpu::RecordRect display = kBufferA;
  std::vector<std::string> calls;
  std::vector<int> paceParts;
  FrameRecord shown;
  bool shownIsCurrent = false;
  const psx::present::StateProducers *producers = nullptr;
  FrameRecord real;
  bool realAdvances = false;
  bool realComposed = false;

private:
  const psx::frame::FramePresenter &presenter_;
};

constexpr RecordKey kKey{0x8001F798u, 0x80150000u, 0, 0};

FrameRecord recordAt(std::uint64_t sequence, int x, const psx::gpu::RecordRect &buffer = kBufferA) {
  DrawPrimitive primitive;
  primitive.state.clipX0 = buffer.x0;
  primitive.state.clipY0 = buffer.y0;
  primitive.state.clipX1 = buffer.x1 - 1;
  primitive.state.clipY1 = buffer.y1 - 1;
  primitive.vertexCount = 3;
  primitive.vertices[0].x = x;
  primitive.vertices[1].x = x + 4;
  primitive.vertices[2].x = x;
  primitive.key = kKey;
  FrameRecord record(sequence, true);
  record.append(primitive);
  return record;
}

// The keyed triangle in bucket 1 of table 0, drawn by the scope `serial`.
FrameRecord slottedRecordAt(std::uint64_t sequence, int x, std::uint32_t serial) {
  FrameRecord keyed = recordAt(sequence, x);
  auto primitive = std::get<DrawPrimitive>(keyed.entries()[0]);
  primitive.key->serial = serial;
  primitive.slot = psx::present::OtSlot{0, 1};
  FrameRecord record(sequence, true);
  record.beginSlot({0, 1}, true);
  record.append(primitive);
  return record;
}

psx::present::FrameState savedAt(const FrameRecord &record, std::uint32_t serial, float x) {
  psx::present::FrameStates states;
  RecordKey owner = kKey;
  owner.serial = serial;
  states.save(owner, marker::Marker{x, 1, 0});
  return states.collect(record);
}

int shownX(const FrameRecord &record) {
  return std::get<DrawPrimitive>(record.entries()[0]).vertices[0].x;
}

} // namespace

static void test_consecutive_records_present_the_blend_then_n(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt(0, 0));
  CHECK(backend.calls == std::vector<std::string>{"real"});
  CHECK(backend.paceParts == std::vector<int>{1});

  backend.calls.clear();
  backend.paceParts.clear();
  presenter.commit(backend, 2, recordAt(1, 20));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK(backend.paceParts == (std::vector<int>{2, 2}));
  CHECK_EQ(shownX(backend.shown), 10);
  CHECK(!backend.shownIsCurrent);
  CHECK(presenter.composedRecord() == nullptr);
}

static void test_a_cut_shows_n_as_its_in_between(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt(0, 0));
  backend.cut = true;
  presenter.commit(backend, 2, recordAt(1, 20));
  CHECK(backend.calls == (std::vector<std::string>{"real", "in-between", "real"}));
  CHECK(backend.shownIsCurrent);
  CHECK_EQ(shownX(backend.shown), 20);
}

static void test_a_record_that_does_not_follow_has_no_in_between(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt(0, 0));
  presenter.commit(backend, 2, recordAt(2, 20));
  presenter.commit(backend, 2, FrameRecord(3, false));
  CHECK(backend.calls == (std::vector<std::string>{"real", "real", "real"}));
}

// Each record draws one buffer while the other is displayed, so present N shows N-1.
static void test_a_double_buffer_blends_the_two_shown_records(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt(0, 0, kBufferA));
  backend.display = kBufferA;
  presenter.commit(backend, 2, recordAt(1, 20, kBufferB));
  CHECK(backend.calls == (std::vector<std::string>{"real", "real"}));

  backend.calls.clear();
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt(2, 40, kBufferA));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 1u);
  CHECK_EQ(shownX(backend.shown), 10);

  backend.calls.clear();
  backend.display = kBufferA;
  backend.cut = true;
  presenter.commit(backend, 2, recordAt(3, 60, kBufferB));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 2u);
  CHECK_EQ(shownX(backend.shown), 30);

  // Record 3 was sealed as a cut; it is shown at the next present, so that in-between is record 3.
  backend.calls.clear();
  backend.display = kBufferB;
  backend.cut = false;
  presenter.commit(backend, 2, recordAt(4, 80, kBufferA));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 3u);
  CHECK_EQ(shownX(backend.shown), 60);
}

// Spyro 2's measured cadence: a walked record, then an empty one (the step that stopped at the
// limiter wait), each walk drawing the buffer not displayed. Present N shows the walk of N-2.
static void test_a_double_buffer_with_empty_records_between_walks_blends_the_two_shown_walks(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt(0, 0, kBufferA));
  presenter.commit(backend, 2, FrameRecord(1, true));
  backend.display = kBufferA;
  presenter.commit(backend, 2, recordAt(2, 20, kBufferB));
  presenter.commit(backend, 2, FrameRecord(3, true));

  backend.calls.clear();
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt(4, 40, kBufferA));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 2u);
  CHECK_EQ(shownX(backend.shown), 10);
}

// A cut sealed with a record that drew nothing still separates the drawing records around it.
static void test_a_cut_on_an_empty_record_reaches_the_next_drawing_record(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt(0, 0));
  backend.cut = true;
  presenter.commit(backend, 2, FrameRecord(1, true));
  backend.cut = false;
  backend.calls.clear();
  presenter.commit(backend, 2, recordAt(2, 20));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK(backend.shownIsCurrent);
}

static void test_an_incomplete_empty_record_breaks_the_pairing(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt(0, 0));
  presenter.commit(backend, 2, FrameRecord(1, false));
  backend.calls.clear();
  presenter.commit(backend, 2, recordAt(2, 20));
  CHECK(backend.calls == std::vector<std::string>{"real"});
}

static void test_a_display_no_record_drew_has_no_in_between(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt(0, 0));
  presenter.commit(backend, 2, recordAt(1, 20));
  CHECK(backend.calls == (std::vector<std::string>{"real", "real"}));
}

static void test_every_present_draws_the_producers_render_at_its_t(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  const psx::present::StateProducers renders = marker::renders(kKey.producer);
  backend.producers = &renders;
  const FrameRecord first = slottedRecordAt(0, 0, 1);
  presenter.commit(backend, 2, first, savedAt(first, 1, 0.0f));
  CHECK(backend.calls == std::vector<std::string>{"real"});
  CHECK_EQ(shownX(backend.real), 0);
  CHECK(backend.realAdvances);

  backend.calls.clear();
  const FrameRecord second = slottedRecordAt(1, 20, 2);
  presenter.commit(backend, 2, second, savedAt(second, 2, 20.0f));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(shownX(backend.shown), 10);
  CHECK_EQ(shownX(backend.real), 20);
  CHECK(backend.realAdvances);
  CHECK(presenter.composedRecord() == nullptr);
}

// A record after the shown one that overwrites the displayed buffer (a clear under a whole-VRAM draw area) makes the
// shown picture stale: the present is the device's, not the composed record.
static void test_a_later_record_over_the_displayed_buffer_is_not_composed_over(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  const psx::present::StateProducers renders = marker::renders(kKey.producer);
  backend.producers = &renders;
  const FrameRecord first = slottedRecordAt(0, 0, 1);
  presenter.commit(backend, 2, first, savedAt(first, 1, 0.0f));
  CHECK(backend.realComposed);

  DrawPrimitive clear;
  clear.kind = psx::present::PrimitiveKind::Sprite;
  clear.vertexCount = 1;
  clear.width = kBufferA.x1;
  clear.height = kBufferA.y1;
  clear.state.clipX1 = 1023;
  clear.state.clipY1 = 511;
  FrameRecord second(1, true);
  second.append(clear);
  presenter.commit(backend, 2, second, psx::present::FrameState());
  CHECK(!backend.realComposed);
}

// A copy onto its own source (the guest's 2x1 fence) changes no pixel: the shown picture stays composed.
static void test_a_copy_onto_itself_leaves_the_shown_picture_composed(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  const psx::present::StateProducers renders = marker::renders(kKey.producer);
  backend.producers = &renders;
  const FrameRecord first = slottedRecordAt(0, 0, 1);
  presenter.commit(backend, 2, first, savedAt(first, 1, 0.0f));
  CHECK(backend.realComposed);

  psx::present::VramCopy fence;
  fence.srcX = 0;
  fence.srcY = 0;
  fence.dstX = 0;
  fence.dstY = 0;
  fence.width = 2;
  fence.height = 1;
  FrameRecord second(1, true);
  second.append(fence);
  presenter.commit(backend, 2, second, psx::present::FrameState());
  CHECK(backend.realComposed);
}

int main(void) {
  RUN(consecutive_records_present_the_blend_then_n);
  RUN(a_cut_shows_n_as_its_in_between);
  RUN(a_record_that_does_not_follow_has_no_in_between);
  RUN(a_double_buffer_blends_the_two_shown_records);
  RUN(a_double_buffer_with_empty_records_between_walks_blends_the_two_shown_walks);
  RUN(a_cut_on_an_empty_record_reaches_the_next_drawing_record);
  RUN(an_incomplete_empty_record_breaks_the_pairing);
  RUN(a_display_no_record_drew_has_no_in_between);
  RUN(every_present_draws_the_producers_render_at_its_t);
  RUN(a_later_record_over_the_displayed_buffer_is_not_composed_over);
  RUN(a_copy_onto_itself_leaves_the_shown_picture_composed);
  return pt_summary();
}
