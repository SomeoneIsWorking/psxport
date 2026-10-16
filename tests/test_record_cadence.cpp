// test_record_cadence.cpp — FramePresenter's record path at 60 fps: the in-between of the two last shown
// pictures, or the shown record on its cut, then N.
#include "frame_presenter.h"
#include "marker_render.h"
#include "record_raster_setup.h"
#include "render_mode.h"
#include "testutil.h"

#include <memory>
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

  void presentReal() override {
    calls.emplace_back("real");
    order.emplace_back("real");
    if (producers == nullptr) {
      CHECK(presenter_.composedRecord() == nullptr);
    } else if (const FrameRecord *record = presenter_.composedRecord()) {
      real = *record;
      realAdvances = presenter_.composedAdvances();
    }
    realComposed = presenter_.composedRecord() != nullptr;
  }
  void captureDiagnostic(uint64_t, bool) override {}
  void pace(psx::frame::FieldCadence cadence) override {
    paceParts.push_back(cadence.parts);
    order.emplace_back("pace");
  }
  bool interpolatesRecords() const override {
    return interpolates;
  }
  bool sealedFrameIsCut() override {
    return cut;
  }
  psx::gpu::RecordRect displayedBuffer() override {
    ++displayQueries;
    return display;
  }
  void presentInBetween() override {
    calls.emplace_back("in-between");
    order.emplace_back("in-between");
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
  RenderMode *displayPassMode() override {
    return &mode;
  }

  RenderMode mode;
  bool interpolates = true;
  int displayQueries = 0;
  bool cut = false;
  psx::gpu::RecordRect display = kBufferA;
  std::vector<std::string> calls;
  std::vector<int> paceParts;
  std::vector<std::string> order; // presents and paces, interleaved
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

// A record's sequence and the x its keyed triangle is drawn at.
struct Drawn {
  std::uint64_t sequence = 0;
  int x = 0;
};

FrameRecord recordAt(Drawn drawn, const psx::gpu::RecordRect &buffer = kBufferA) {
  DrawPrimitive primitive;
  primitive.state.clipX0 = buffer.x0;
  primitive.state.clipY0 = buffer.y0;
  primitive.state.clipX1 = buffer.x1 - 1;
  primitive.state.clipY1 = buffer.y1 - 1;
  primitive.vertexCount = 3;
  primitive.vertices[0].x = drawn.x;
  primitive.vertices[1].x = drawn.x + 4;
  primitive.vertices[2].x = drawn.x;
  primitive.key = kKey;
  FrameRecord record(drawn.sequence, true);
  record.append(primitive);
  return record;
}

// The keyed triangle in bucket 1 of table 0, drawn by the scope `serial`.
FrameRecord slottedRecordAt(Drawn drawn, std::uint32_t serial) {
  FrameRecord keyed = recordAt(drawn);
  auto primitive = std::get<DrawPrimitive>(keyed.entries()[0]);
  psx::test::require(primitive.key).serial = serial;
  primitive.slot = psx::present::OtSlot{0, 1};
  FrameRecord record(drawn.sequence, true);
  record.beginSlot({0, 1}, true);
  record.append(primitive);
  return record;
}

RecordKey ownedBy(std::uint32_t serial) {
  RecordKey owner = kKey;
  owner.serial = serial;
  return owner;
}

psx::present::FrameState savedAt(const FrameRecord &record, const RecordKey &owner, float x) {
  return marker::savedState(record, owner, x);
}

int shownX(const FrameRecord &record) {
  return std::get<DrawPrimitive>(record.entries()[0]).vertices[0].x;
}

void test_consecutive_records_present_the_shown_record_then_n(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt({0, 0}));
  CHECK(backend.calls == std::vector<std::string>{"real"});
  CHECK(backend.paceParts == std::vector<int>{1});

  backend.calls.clear();
  backend.paceParts.clear();
  presenter.commit(backend, 2, recordAt({1, 20}));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK(backend.paceParts == (std::vector<int>{2, 2}));
  CHECK_EQ(shownX(backend.shown), 20); // the shown record, unblended
  CHECK(backend.shownIsCurrent);
  CHECK(presenter.composedRecord() == nullptr);
}

// A present waits for its deadline first, so the work before it never shifts it off that deadline.
void test_each_present_follows_its_pace(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt({0, 0}));
  CHECK(backend.order == (std::vector<std::string>{"pace", "real"}));
  backend.order.clear();
  presenter.commit(backend, 2, recordAt({1, 20}));
  CHECK(backend.order == (std::vector<std::string>{"pace", "in-between", "pace", "real"}));
}

void test_a_cut_shows_n_as_its_in_between(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt({0, 0}));
  backend.cut = true;
  presenter.commit(backend, 2, recordAt({1, 20}));
  CHECK(backend.calls == (std::vector<std::string>{"real", "in-between", "real"}));
  CHECK(backend.shownIsCurrent);
  CHECK_EQ(shownX(backend.shown), 20);
}

void test_a_record_that_does_not_follow_has_no_in_between(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt({0, 0}));
  presenter.commit(backend, 2, recordAt({2, 20}));
  presenter.commit(backend, 2, FrameRecord(3, false));
  CHECK(backend.calls == (std::vector<std::string>{"real", "real", "real"}));
}

// Each record draws one buffer while the other is displayed, so present N shows N-1.
void test_a_double_buffer_shows_the_previous_shown_record(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt({0, 0}, kBufferA));
  backend.display = kBufferA;
  presenter.commit(backend, 2, recordAt({1, 20}, kBufferB));
  CHECK(backend.calls == (std::vector<std::string>{"real", "real"}));

  backend.calls.clear();
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt({2, 40}, kBufferA));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 1u);
  CHECK_EQ(shownX(backend.shown), 20);

  backend.calls.clear();
  backend.display = kBufferA;
  backend.cut = true;
  presenter.commit(backend, 2, recordAt({3, 60}, kBufferB));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 2u);
  CHECK_EQ(shownX(backend.shown), 40);

  // Record 3 was sealed as a cut; it is shown at the next present, so that in-between is record 3.
  backend.calls.clear();
  backend.display = kBufferB;
  backend.cut = false;
  presenter.commit(backend, 2, recordAt({4, 80}, kBufferA));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 3u);
  CHECK_EQ(shownX(backend.shown), 60);
}

// Spyro 2's measured cadence: a walked record, then an empty one (the step that stopped at the
// limiter wait), each walk drawing the buffer not displayed. Present N shows the walk of N-2.
void test_a_double_buffer_with_empty_records_between_walks_shows_the_previous_walk(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt({0, 0}, kBufferA));
  presenter.commit(backend, 2, FrameRecord(1, true));
  backend.display = kBufferA;
  presenter.commit(backend, 2, recordAt({2, 20}, kBufferB));
  presenter.commit(backend, 2, FrameRecord(3, true));

  backend.calls.clear();
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt({4, 40}, kBufferA));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(backend.shown.sequence(), 2u);
  CHECK_EQ(shownX(backend.shown), 20);
}

// A cut sealed with a record that drew nothing still separates the drawing records around it.
void test_a_cut_on_an_empty_record_reaches_the_next_drawing_record(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt({0, 0}));
  backend.cut = true;
  presenter.commit(backend, 2, FrameRecord(1, true));
  backend.cut = false;
  backend.calls.clear();
  presenter.commit(backend, 2, recordAt({2, 20}));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK(backend.shownIsCurrent);
}

void test_an_incomplete_empty_record_breaks_the_pairing(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt({0, 0}));
  presenter.commit(backend, 2, FrameRecord(1, false));
  backend.calls.clear();
  presenter.commit(backend, 2, recordAt({2, 20}));
  CHECK(backend.calls == std::vector<std::string>{"real"});
}

void test_a_display_no_record_drew_has_no_in_between(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt({0, 0}));
  presenter.commit(backend, 2, recordAt({1, 20}));
  CHECK(backend.calls == (std::vector<std::string>{"real", "real"}));
}

void test_every_present_draws_the_producers_render_at_its_t(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  const psx::present::StateProducerTable renders = marker::renders(kKey.producer);
  backend.producers = &renders;
  const FrameRecord first = slottedRecordAt({0, 0}, 1);
  presenter.commit(backend, 2, first, savedAt(first, ownedBy(1), 0.0f));
  CHECK(backend.calls == std::vector<std::string>{"real"});
  CHECK_EQ(shownX(backend.real), 0);
  CHECK(backend.realAdvances);

  backend.calls.clear();
  const FrameRecord second = slottedRecordAt({1, 20}, 2);
  presenter.commit(backend, 2, second, savedAt(second, ownedBy(2), 20.0f));
  CHECK(backend.calls == (std::vector<std::string>{"in-between", "real"}));
  CHECK_EQ(shownX(backend.shown), 10);
  CHECK_EQ(shownX(backend.real), 20);
  CHECK(backend.realAdvances);
  CHECK(presenter.composedRecord() == nullptr);
}

// A record after the shown one that overwrites the displayed buffer (a clear under a whole-VRAM draw area) makes the
// shown picture stale: the present is the device's, not the composed record.
void test_a_later_record_over_the_displayed_buffer_is_not_composed_over(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  const psx::present::StateProducerTable renders = marker::renders(kKey.producer);
  backend.producers = &renders;
  const FrameRecord first = slottedRecordAt({0, 0}, 1);
  presenter.commit(backend, 2, first, savedAt(first, ownedBy(1), 0.0f));
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
void test_a_copy_onto_itself_leaves_the_shown_picture_composed(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  const psx::present::StateProducerTable renders = marker::renders(kKey.producer);
  backend.producers = &renders;
  const FrameRecord first = slottedRecordAt({0, 0}, 1);
  presenter.commit(backend, 2, first, savedAt(first, ownedBy(1), 0.0f));
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

FrameRecord fillOf(std::uint64_t sequence, const psx::gpu::RecordRect &buffer) {
  psx::present::VramFill fill;
  fill.x = buffer.x0;
  fill.y = buffer.y0;
  fill.width = buffer.x1 - buffer.x0;
  fill.height = buffer.y1 - buffer.y0;
  FrameRecord record(sequence, true);
  record.append(fill);
  return record;
}

// Tomba 2's stage handoff: passes that draw nothing fill each buffer black, then display it. The buffer a
// fill overwrote shows the device's picture, not the last record that drew it.
void test_a_fill_in_an_earlier_record_that_drew_nothing_makes_the_buffer_stale(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.display = kBufferB;
  presenter.commit(backend, 2, recordAt({0, 0}, kBufferA));
  backend.display = kBufferA;
  presenter.commit(backend, 2, recordAt({1, 20}, kBufferB));
  backend.display = kBufferB;
  presenter.commit(backend, 2, fillOf(2, kBufferA));
  backend.calls.clear();
  backend.display = kBufferA;
  presenter.commit(backend, 2, fillOf(3, kBufferB));
  CHECK(backend.calls == std::vector<std::string>{"real"});
  CHECK_EQ(presenter.stability().backwards, 0u);
}

// The play check reads these: every record present after the first brings its in-between, and a
// record that drew nothing leaves the shown record where it was, which is a repeat.
void test_stability_counts_in_betweens_and_repeats(void) {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  presenter.commit(backend, 2, recordAt({0, 0}));
  presenter.commit(backend, 2, recordAt({1, 20}));
  presenter.commit(backend, 2, recordAt({2, 40}));
  CHECK_EQ(presenter.stability().recordPresents, 3u);
  CHECK_EQ(presenter.stability().inBetweens, 2u);
  CHECK_EQ(presenter.stability().withoutInBetween, 1u);
  CHECK_EQ(presenter.stability().repeats, 0u);
  presenter.commit(backend, 2, FrameRecord(3, true));
  CHECK_EQ(presenter.stability().repeats, 1u);
  CHECK_EQ(presenter.stability().backwards, 0u);
}

class ArmedProbe final : public psx::present::StateProducer {
public:
  explicit ArmedProbe(const RenderMode &mode) : mode_(mode) {}
  void render(const psx::present::StateInterval &, psx::present::PrimitiveSink &) const override {
    ++renders;
    armedEvery = armedEvery && mode_.displayPassArmed();
  }

  mutable int renders = 0;
  mutable bool armedEvery = true;

private:
  const RenderMode &mode_;
};

// Producer renders run inside the display pass, so a render that stores to guest memory aborts.
// A 30 fps Record present with nothing saved does not look for a picture to compose over.
void test_a_frame_without_saved_state_is_not_composed() {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  const psx::present::StateProducerTable renders = marker::renders(kKey.producer);
  backend.producers = &renders;
  backend.interpolates = false;
  presenter.commit(backend, 1, recordAt({1, 10}));
  CHECK_EQ(backend.displayQueries, 0);
  CHECK(!backend.realComposed);
}

void test_producer_renders_run_inside_the_display_pass() {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  psx::present::StateProducerTable renders;
  auto probe = std::make_unique<ArmedProbe>(backend.mode);
  const ArmedProbe &seen = *probe;
  renders.install(kKey.producer, std::move(probe));
  backend.producers = &renders;
  const FrameRecord first = slottedRecordAt({0, 0}, 1);
  presenter.commit(backend, 2, first, savedAt(first, ownedBy(1), 0.0f));
  const FrameRecord second = slottedRecordAt({1, 20}, 2);
  presenter.commit(backend, 2, second, savedAt(second, ownedBy(2), 20.0f));

  CHECK(seen.renders >= 3);
  CHECK(seen.armedEvery);
  CHECK(!backend.mode.displayPassArmed());
}

// The ledger reconciles each composed t = 1 present: a render that drew nothing for its recorded
// primitives is a dropped producer, the faithful render is not.
void test_the_ledger_counts_a_render_that_drew_nothing() {
  psx::frame::FramePresenter presenter;
  RecordBackend backend(presenter);
  backend.interpolates = false;
  psx::present::StateProducerTable silent;
  silent.install(kKey.producer, std::make_unique<ArmedProbe>(backend.mode));
  backend.producers = &silent;
  const FrameRecord first = slottedRecordAt({0, 0}, 1);
  presenter.commit(backend, 1, first, savedAt(first, ownedBy(1), 0.0f));
  CHECK_EQ(static_cast<int>(presenter.ledger().framesReconciled()), 1);
  CHECK_EQ(static_cast<int>(presenter.ledger().framesDropped()), 1);

  psx::frame::FramePresenter faithful;
  RecordBackend markerBackend(faithful);
  markerBackend.interpolates = false;
  const psx::present::StateProducerTable renders = marker::renders(kKey.producer);
  markerBackend.producers = &renders;
  faithful.commit(markerBackend, 1, first, savedAt(first, ownedBy(1), 0.0f));
  CHECK_EQ(static_cast<int>(faithful.ledger().framesReconciled()), 1);
  CHECK_EQ(static_cast<int>(faithful.ledger().framesDropped()), 0);
}

} // namespace

int main(void) {
  RUN(the_ledger_counts_a_render_that_drew_nothing);
  RUN(producer_renders_run_inside_the_display_pass);
  RUN(a_frame_without_saved_state_is_not_composed);
  RUN(stability_counts_in_betweens_and_repeats);
  RUN(a_fill_in_an_earlier_record_that_drew_nothing_makes_the_buffer_stale);
  RUN(consecutive_records_present_the_shown_record_then_n);
  RUN(each_present_follows_its_pace);
  RUN(a_cut_shows_n_as_its_in_between);
  RUN(a_record_that_does_not_follow_has_no_in_between);
  RUN(a_double_buffer_shows_the_previous_shown_record);
  RUN(a_double_buffer_with_empty_records_between_walks_shows_the_previous_walk);
  RUN(a_cut_on_an_empty_record_reaches_the_next_drawing_record);
  RUN(an_incomplete_empty_record_breaks_the_pairing);
  RUN(a_display_no_record_drew_has_no_in_between);
  RUN(every_present_draws_the_producers_render_at_its_t);
  RUN(a_later_record_over_the_displayed_buffer_is_not_composed_over);
  RUN(a_copy_onto_itself_leaves_the_shown_picture_composed);
  return psx::test::summary();
}
