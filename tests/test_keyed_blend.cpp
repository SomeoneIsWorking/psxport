// test_keyed_blend.cpp — the in-between record: keyed vertices blended from N-1, everything else N.
#include "keyed_blend.h"
#include "testutil.h"

#include <variant>

namespace {

using psx::present::DrawPrimitive;
using psx::present::FrameRecord;
using psx::present::PrimitiveKind;
using psx::present::RecordKey;
using psx::present::VramFill;

DrawPrimitive triangle(int x, int y, std::optional<RecordKey> key) {
  DrawPrimitive primitive;
  primitive.kind = PrimitiveKind::Polygon;
  primitive.vertexCount = 3;
  primitive.vertices[0] = {x, y, 10, 20, 30, 1, 2};
  primitive.vertices[1] = {x + 8, y, 10, 20, 30, 3, 4};
  primitive.vertices[2] = {x, y + 8, 10, 20, 30, 5, 6};
  primitive.key = key;
  return primitive;
}

DrawPrimitive sprite(int x, int y, std::optional<RecordKey> key) {
  DrawPrimitive primitive;
  primitive.kind = PrimitiveKind::Sprite;
  primitive.vertexCount = 1;
  primitive.width = 16;
  primitive.height = 16;
  primitive.vertices[0] = {x, y, 0, 0, 0, 0, 0};
  primitive.key = key;
  return primitive;
}

const DrawPrimitive &primitiveAt(const FrameRecord &record, std::size_t index) {
  return std::get<DrawPrimitive>(record.entries()[index]);
}

constexpr RecordKey kKey{0x8001F798u, 0x80150000u, 0, 0};
constexpr RecordKey kOtherKey{0x8001F798u, 0x80150000u, 1, 0};

} // namespace

static void test_a_paired_primitive_is_blended_and_keeps_its_current_attributes(void) {
  FrameRecord previous(1, true);
  previous.append(triangle(10, 100, kKey));
  FrameRecord current(2, true);
  DrawPrimitive moved = triangle(20, 110, kKey);
  moved.vertices[0].r = 99;
  moved.vertices[0].u = 7;
  current.append(moved);
  const FrameRecord blended = psx::present::keyedBlend(previous, current, 0.5f);
  const DrawPrimitive &out = primitiveAt(blended, 0);
  CHECK_EQ(out.vertices[0].x, 15);
  CHECK_EQ(out.vertices[0].y, 105);
  CHECK_EQ(out.vertices[1].x, 23);
  CHECK_EQ(out.vertices[0].r, 99);
  CHECK_EQ(out.vertices[0].u, 7);
  CHECK(out.vertices[0].subX == 0.0f);
  CHECK_EQ(blended.sequence(), current.sequence());
}

static void test_half_pixel_positions_keep_their_fraction_on_polygons_only(void) {
  FrameRecord previous(1, true);
  previous.append(triangle(10, 100, kKey));
  previous.append(sprite(10, 50, kOtherKey));
  FrameRecord current(2, true);
  current.append(triangle(11, 101, kKey));
  current.append(sprite(11, 51, kOtherKey));
  const FrameRecord blended = psx::present::keyedBlend(previous, current, 0.5f);
  const DrawPrimitive &polygon = primitiveAt(blended, 0);
  CHECK_EQ(polygon.vertices[0].x, 10);
  CHECK(polygon.vertices[0].subX == 0.5f);
  CHECK(polygon.vertices[0].subY == 0.5f);
  const DrawPrimitive &rect = primitiveAt(blended, 1);
  CHECK_EQ(rect.vertices[0].x, 11);
  CHECK(rect.vertices[0].subX == 0.0f);
}

static void test_unkeyed_and_unpaired_primitives_are_drawn_as_in_n(void) {
  FrameRecord previous(1, true);
  previous.append(triangle(0, 0, std::nullopt));
  previous.append(triangle(0, 0, kOtherKey));
  FrameRecord current(2, true);
  current.append(triangle(40, 40, std::nullopt));
  current.append(triangle(40, 40, kKey));
  VramFill fill;
  fill.width = 16;
  fill.height = 4;
  fill.value = 0x1234;
  current.append(fill);
  const FrameRecord blended = psx::present::keyedBlend(previous, current, 0.5f);
  CHECK(blended == current);
}

static void test_a_key_used_twice_is_not_blended(void) {
  FrameRecord previous(1, true);
  previous.append(triangle(0, 0, kKey));
  previous.append(triangle(100, 0, kKey));
  previous.append(triangle(50, 0, kOtherKey));
  FrameRecord current(2, true);
  current.append(triangle(10, 0, kKey));
  current.append(triangle(110, 0, kKey));
  current.append(triangle(60, 0, kOtherKey));
  const FrameRecord blended = psx::present::keyedBlend(previous, current, 0.5f);
  CHECK_EQ(primitiveAt(blended, 0).vertices[0].x, 10);
  CHECK_EQ(primitiveAt(blended, 1).vertices[0].x, 110);
  CHECK_EQ(primitiveAt(blended, 2).vertices[0].x, 55);
}

static void test_a_topology_mismatch_is_not_blended(void) {
  FrameRecord previous(1, true);
  DrawPrimitive quad = triangle(0, 0, kKey);
  quad.vertexCount = 4;
  previous.append(quad);
  DrawPrimitive otherPage = triangle(0, 0, kOtherKey);
  otherPage.state.texPageX = 64;
  previous.append(otherPage);
  FrameRecord current(2, true);
  current.append(triangle(20, 20, kKey));
  current.append(triangle(20, 20, kOtherKey));
  const FrameRecord blended = psx::present::keyedBlend(previous, current, 0.5f);
  CHECK(blended == current);
}

static void test_double_buffered_offsets_blend_in_drawing_area_space(void) {
  FrameRecord previous(1, true);
  DrawPrimitive back = triangle(10, 356, kKey);
  back.state.offsetY = 256;
  previous.append(back);
  FrameRecord current(2, true);
  current.append(triangle(20, 110, kKey));
  const DrawPrimitive &out = primitiveAt(psx::present::keyedBlend(previous, current, 0.5f), 0);
  CHECK_EQ(out.vertices[0].x, 15);
  CHECK_EQ(out.vertices[0].y, 105);
}

static void test_t_one_is_n(void) {
  FrameRecord previous(1, true);
  previous.append(triangle(3, 9, kKey));
  previous.append(sprite(-7, 2, kOtherKey));
  FrameRecord current(2, true);
  current.append(triangle(250, -31, kKey));
  current.append(sprite(17, 300, kOtherKey));
  current.append(triangle(1, 1, std::nullopt));
  CHECK(psx::present::keyedBlend(previous, current, 1.0f) == current);
}

int main(void) {
  RUN(a_paired_primitive_is_blended_and_keeps_its_current_attributes);
  RUN(half_pixel_positions_keep_their_fraction_on_polygons_only);
  RUN(unkeyed_and_unpaired_primitives_are_drawn_as_in_n);
  RUN(a_key_used_twice_is_not_blended);
  RUN(a_topology_mismatch_is_not_blended);
  RUN(double_buffered_offsets_blend_in_drawing_area_space);
  RUN(t_one_is_n);
  return pt_summary();
}
