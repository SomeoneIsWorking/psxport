// test_frame_composer.cpp — the frame at t: produced objects drawn by their render in their OT slots.
#include "emission_scope.h"
#include "frame_composer.h"
#include "frame_state.h"
#include "marker_render.h"
#include "testutil.h"

#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace {

using psx::present::DrawPrimitive;
using psx::present::FrameRecord;
using psx::present::FrameState;
using psx::present::FrameStates;
using psx::present::OtSlot;
using psx::present::RecordKey;

constexpr std::uint32_t kProducer = 0x80010000u;
constexpr std::uint32_t kObject = 0x80150000u;
constexpr int kOffsetX = 16;

using marker::Marker;

psx::present::StateProducers producers() {
  return marker::renders(kProducer);
}

DrawPrimitive triangle(int x, std::optional<RecordKey> key, std::uint32_t bucket) {
  DrawPrimitive primitive;
  primitive.vertexCount = 3;
  primitive.state.offsetX = kOffsetX;
  primitive.state.clipX1 = 319;
  primitive.state.clipY1 = 239;
  primitive.vertices[0].x = x + kOffsetX;
  primitive.vertices[1].x = x + 8 + kOffsetX;
  primitive.vertices[2].x = x + kOffsetX;
  primitive.vertices[1].y = 0;
  primitive.vertices[2].y = 8;
  primitive.key = key;
  primitive.slot = OtSlot{0, bucket};
  return primitive;
}

// Buckets 2, 1, 0 in walk order (a reverse-cleared table): A in 2, the object in 1, B in 0.
FrameRecord recordWithObject(std::uint64_t sequence, int objectX, std::uint32_t serial) {
  FrameRecord record(sequence, true);
  record.beginSlot({0, 2}, true);
  record.append(triangle(100, std::nullopt, 2));
  record.beginSlot({0, 1}, true);
  record.append(triangle(objectX, RecordKey{kProducer, kObject, 0, 0, serial}, 1));
  record.beginSlot({0, 0}, true);
  record.append(triangle(200, std::nullopt, 0));
  return record;
}

FrameState stateFor(const FrameRecord &record, std::uint32_t serial, Marker marker) {
  FrameStates states;
  states.save(RecordKey{kProducer, kObject, 0, 0, serial}, marker);
  return states.collect(record);
}

const DrawPrimitive &primitiveAt(const FrameRecord &record, std::size_t index) {
  return std::get<DrawPrimitive>(record.entries()[index]);
}

} // namespace

static void test_a_record_with_no_produced_object_is_the_frame(void) {
  const auto renders = producers();
  FrameRecord record(1, true);
  record.append(triangle(10, std::nullopt, 0));
  CHECK(!psx::present::composeFrame(record, nullptr, FrameState(), 0.5f, renders).has_value());
}

static void test_the_render_at_t_replaces_the_object_in_its_slot(void) {
  const auto renders = producers();
  const FrameRecord before = recordWithObject(1, 20, 7);
  const FrameRecord shown = recordWithObject(2, 40, 9);
  const FrameState from = stateFor(before, 7, {20.0f, 1, 0});
  const FrameState to = stateFor(shown, 9, {40.0f, 1, 0});
  const auto composed = psx::present::composeFrame(shown, &from, to, 0.5f, renders);
  CHECK(composed.has_value());
  CHECK_EQ(composed->entries().size(), static_cast<std::size_t>(3));
  CHECK_EQ(primitiveAt(*composed, 0).vertices[0].x, 100 + kOffsetX);
  CHECK_EQ(primitiveAt(*composed, 1).vertices[0].x, 30 + kOffsetX);
  CHECK_EQ(primitiveAt(*composed, 1).state.clipX1, 319);
  CHECK_EQ(primitiveAt(*composed, 2).vertices[0].x, 200 + kOffsetX);
}

static void test_at_t_one_the_render_draws_the_recorded_frame(void) {
  const auto renders = producers();
  const FrameRecord shown = recordWithObject(2, 40, 9);
  const FrameState to = stateFor(shown, 9, {40.0f, 1, 0});
  const auto composed = psx::present::composeFrame(shown, nullptr, to, 1.0f, renders);
  CHECK(composed.has_value());
  CHECK_EQ(composed->entries().size(), shown.entries().size());
  for (std::size_t i = 0; i < shown.entries().size(); i++) {
    const DrawPrimitive &a = primitiveAt(shown, i);
    const DrawPrimitive &b = primitiveAt(*composed, i);
    CHECK(a.vertices == b.vertices);
    CHECK(a.state == b.state);
    CHECK(a.slot == b.slot);
  }
}

static void test_a_cut_or_a_birth_renders_the_shown_state(void) {
  const auto renders = producers();
  const FrameRecord shown = recordWithObject(2, 40, 9);
  const FrameState to = stateFor(shown, 9, {40.0f, 1, 0});
  const auto cut = psx::present::composeFrame(shown, nullptr, to, 0.5f, renders);
  CHECK(cut && primitiveAt(*cut, 1).vertices[0].x == 40 + kOffsetX);
  const FrameState none;
  CHECK(!psx::present::composeFrame(shown, &to, none, 0.5f, renders).has_value());
  const auto born = psx::present::composeFrame(shown, &none, to, 0.5f, renders);
  CHECK(born && primitiveAt(*born, 1).vertices[0].x == 40 + kOffsetX);
}

static void test_a_render_into_another_slot_goes_to_that_slots_head(void) {
  const auto renders = producers();
  const FrameRecord shown = recordWithObject(2, 40, 9);
  const FrameState to = stateFor(shown, 9, {40.0f, 0, 0});
  const auto composed = psx::present::composeFrame(shown, nullptr, to, 1.0f, renders);
  CHECK(composed.has_value());
  CHECK_EQ(composed->entries().size(), static_cast<std::size_t>(3));
  CHECK_EQ(primitiveAt(*composed, 0).vertices[0].x, 100 + kOffsetX);
  CHECK(primitiveAt(*composed, 1).slot == (OtSlot{0, 0}));
  CHECK_EQ(primitiveAt(*composed, 1).vertices[0].x, 40 + kOffsetX);
  CHECK_EQ(primitiveAt(*composed, 2).vertices[0].x, 200 + kOffsetX);
}

// The guest changed draw mode before bucket 0, so a primitive placed there draws under that mode.
static void test_a_placed_primitive_takes_the_environment_where_it_lands(void) {
  const auto renders = producers();
  FrameRecord shown = recordWithObject(2, 40, 9);
  auto &after = std::get<DrawPrimitive>(shown.entries()[2]);
  after.state.dither = true;
  after.state.clipX1 = 255;
  const FrameState to = stateFor(shown, 9, {40.0f, 0, 0});
  const auto composed = psx::present::composeFrame(shown, nullptr, to, 1.0f, renders);
  CHECK(composed.has_value());
  CHECK(composed && primitiveAt(*composed, 1).state.dither);
  CHECK(composed && primitiveAt(*composed, 1).state.clipX1 == 255);
}

// Walked high to low: bucket 5 comes before bucket 2, and an empty bucket between 1 and 0 does not exist,
// so one past the last walked bucket goes after the table's last entry.
static void test_an_empty_slot_goes_where_the_walk_order_puts_it(void) {
  const auto renders = producers();
  const FrameRecord shown = recordWithObject(2, 40, 9);
  const auto first = psx::present::composeFrame(shown, nullptr, stateFor(shown, 9, {60.0f, 5, 0}), 1.0f, renders);
  CHECK(first.has_value());
  CHECK(first && primitiveAt(*first, 0).slot == (OtSlot{0, 5}));
  CHECK(first && primitiveAt(*first, 1).vertices[0].x == 100 + kOffsetX);
}

static void test_a_table_the_record_never_walked_keeps_the_objects_entries(void) {
  const auto renders = producers();
  FrameRecord shown(2, true);
  shown.append(triangle(40, RecordKey{kProducer, kObject, 0, 0, 9}, 1));
  const FrameState to = stateFor(shown, 9, {60.0f, 3, 0});
  CHECK(!psx::present::composeFrame(shown, nullptr, to, 1.0f, renders).has_value());
}

static void test_a_clut_resolves_to_one_the_record_sampled(void) {
  const auto renders = producers();
  FrameRecord shown = recordWithObject(2, 40, 9);
  DrawPrimitive textured = triangle(150, std::nullopt, 0);
  textured.textured = true;
  textured.clutWord = 0x1234;
  textured.clutOffset = shown.appendClut(std::vector<std::uint16_t>(16, 0x7FFF));
  shown.append(textured);
  const FrameState sampled = stateFor(shown, 9, {40.0f, 1, 0x1234});
  const auto composed = psx::present::composeFrame(shown, nullptr, sampled, 1.0f, renders);
  CHECK(composed.has_value());
  CHECK_EQ(primitiveAt(*composed, 1).clutOffset, textured.clutOffset);
  const FrameState unsampled = stateFor(shown, 9, {40.0f, 1, 0x4321});
  CHECK(!psx::present::composeFrame(shown, nullptr, unsampled, 1.0f, renders).has_value());
}

static void test_states_follow_the_scope_that_wrote_the_packets(void) {
  psx::present::EmissionScope scope;
  FrameStates states;
  RecordKey owner;
  {
    psx::present::EmissionScope::Guard guard(scope, kProducer, kObject, 0);
    owner = scope.current();
    states.save(owner, Marker{40.0f, 1, 0});
    {
      auto part = scope.element(3);
      CHECK_EQ(scope.current().serial, owner.serial);
    }
  }
  // Built in one logic frame, walked in the next.
  states.endFrame();
  const FrameRecord walked = recordWithObject(2, 40, owner.serial);
  CHECK(states.collect(walked).find({kProducer, kObject}).has_value());
  for (std::uint32_t i = 0; i < FrameStates::kRetainedFrames; i++) {
    states.endFrame();
  }
  CHECK(!states.collect(walked).find({kProducer, kObject}).has_value());
}

static void test_an_object_drawn_from_two_scopes_is_ambiguous(void) {
  const auto renders = producers();
  FrameRecord shown = recordWithObject(2, 40, 9);
  shown.append(triangle(60, RecordKey{kProducer, kObject, 0, 0, 10}, 0));
  FrameStates states;
  states.save(RecordKey{kProducer, kObject, 0, 0, 9}, Marker{40.0f, 1, 0});
  states.save(RecordKey{kProducer, kObject, 0, 0, 10}, Marker{60.0f, 0, 0});
  const FrameState to = states.collect(shown);
  CHECK(!to.find({kProducer, kObject}).has_value());
  CHECK(!psx::present::composeFrame(shown, nullptr, to, 1.0f, renders).has_value());
}

// Two triangles of the object in one slot, the guest's draw mode changing between them.
class PairRender final : public psx::present::StateProducer {
public:
  void render(std::span<const std::byte>,
              std::span<const std::byte>,
              float,
              psx::present::PrimitiveSink &sink) const override {
    for (const float x : {40.0f, 60.0f}) {
      DrawPrimitive primitive;
      primitive.vertexCount = 3;
      psx::present::placeVertex(primitive.vertices[0], x, 0.0f, primitive.kind);
      sink.emit(OtSlot{0, 1}, primitive);
    }
  }
};

static void test_the_nth_primitive_in_a_slot_takes_the_nth_entrys_environment(void) {
  psx::present::StateProducers renders;
  renders.install(kProducer, std::make_unique<PairRender>());
  FrameRecord shown(2, true);
  shown.beginSlot({0, 1}, true);
  const RecordKey key{kProducer, kObject, 0, 0, 9};
  shown.append(triangle(40, key, 1));
  DrawPrimitive dithered = triangle(60, key, 1);
  dithered.state.dither = true;
  shown.append(dithered);
  const FrameState to = stateFor(shown, 9, {0.0f, 1, 0});
  const auto composed = psx::present::composeFrame(shown, nullptr, to, 1.0f, renders);
  CHECK(composed.has_value());
  CHECK(composed && composed->entries().size() == 2u);
  CHECK(composed && !primitiveAt(*composed, 0).state.dither);
  CHECK(composed && primitiveAt(*composed, 1).state.dither);
  CHECK(composed && primitiveAt(*composed, 1).vertices[0].x == 60 + kOffsetX);
}

// A face that drew over its own texture became an upload of its pixels; the object's render would draw
// it again over them, so the object stays as the guest drew it.
static void test_an_object_with_a_baked_upload_keeps_its_entries(void) {
  const auto renders = producers();
  FrameRecord shown = recordWithObject(2, 40, 9);
  psx::present::VramUpload baked;
  baked.width = 1;
  baked.height = 1;
  baked.key = RecordKey{kProducer, kObject, 1, 0, 9};
  shown.appendUpload(baked, std::vector<std::uint16_t>{0x1234});
  const FrameState to = stateFor(shown, 9, {60.0f, 3, 0});
  CHECK(!psx::present::composeFrame(shown, nullptr, to, 1.0f, renders).has_value());
}

int main() {
  RUN(a_record_with_no_produced_object_is_the_frame);
  RUN(the_render_at_t_replaces_the_object_in_its_slot);
  RUN(at_t_one_the_render_draws_the_recorded_frame);
  RUN(a_cut_or_a_birth_renders_the_shown_state);
  RUN(a_render_into_another_slot_goes_to_that_slots_head);
  RUN(a_placed_primitive_takes_the_environment_where_it_lands);
  RUN(an_empty_slot_goes_where_the_walk_order_puts_it);
  RUN(a_table_the_record_never_walked_keeps_the_objects_entries);
  RUN(a_clut_resolves_to_one_the_record_sampled);
  RUN(states_follow_the_scope_that_wrote_the_packets);
  RUN(an_object_drawn_from_two_scopes_is_ambiguous);
  RUN(the_nth_primitive_in_a_slot_takes_the_nth_entrys_environment);
  RUN(an_object_with_a_baked_upload_keeps_its_entries);
  return pt_summary();
}
