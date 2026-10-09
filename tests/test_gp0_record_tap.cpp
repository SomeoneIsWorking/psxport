// test_gp0_record_tap — the FrameRecord the GPU device seals: decoded entries, their source address,
// sequence numbers, and work that straddles a seal.

#include "gpu_device.h"
#include "testutil.h"

#include <cstdint>
#include <variant>

namespace {

using psx::gpu::GpuDevice;
using psx::present::DrawPrimitive;
using psx::present::FrameRecord;

constexpr std::uint32_t xy(int x, int y) {
  return (static_cast<std::uint32_t>(x) & 0x7FFu) | ((static_cast<std::uint32_t>(y) & 0x7FFu) << 16);
}

void drawEverywhere(GpuDevice &device) {
  device.gp0(0xE3000000u);
  device.gp0(0xE4000000u | 1023u | (511u << 10));
}

} // namespace

static void test_polygon_is_decoded_after_offset(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  drawEverywhere(device);
  device.gp0(0xE5000000u | 10u | (20u << 11));
  device.sealRecord();
  device.gp0(0x20112233u, 0x80010000u);
  device.gp0(xy(1, 2), 0x80010004u);
  device.gp0(xy(-5, 40), 0x80010008u);
  device.gp0(xy(30, 7), 0x8001000Cu);
  const FrameRecord record = device.sealRecord();
  CHECK(record.complete());
  CHECK_EQ(record.entries().size(), 1u);
  const auto *primitive = std::get_if<DrawPrimitive>(&record.entries()[0]);
  CHECK(primitive != nullptr);
  CHECK(primitive->kind == psx::present::PrimitiveKind::Polygon);
  CHECK_EQ(primitive->vertexCount, 3);
  CHECK_EQ(primitive->sourceAddress, 0x80010000u);
  CHECK_EQ(primitive->vertices[1].x, 5);
  CHECK_EQ(primitive->vertices[1].y, 60);
  CHECK_EQ(primitive->vertices[0].r, 0x33);
  CHECK_EQ(primitive->vertices[0].b, 0x11);
  CHECK(!primitive->key.has_value());
}

// part counts primitives inside one OT packet; a second packet under the same key repeats part 0.
static void test_part_restarts_per_packet(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  drawEverywhere(device);
  device.sealRecord();
  const psx::present::RecordKey key{0x80010000u, 0x80150000u, 3, 0};
  device.beginPacket();
  for (int triangle = 0; triangle < 2; triangle++) {
    device.gp0(0x20112233u, 0x80100000u, key);
    device.gp0(xy(1, 2), 0x80100004u, key);
    device.gp0(xy(5, 40), 0x80100008u, key);
    device.gp0(xy(30, 7), 0x8010000Cu, key);
  }
  device.beginPacket();
  device.gp0(0x20112233u, 0x80100100u, key);
  device.gp0(xy(1, 2), 0x80100104u, key);
  device.gp0(xy(5, 40), 0x80100108u, key);
  device.gp0(xy(30, 7), 0x8010010Cu, key);
  const FrameRecord record = device.sealRecord();
  CHECK_EQ(record.entries().size(), 3u);
  const unsigned expected[] = {0, 1, 0};
  for (std::size_t i = 0; i < record.entries().size() && i < 3; i++) {
    const auto *primitive = std::get_if<DrawPrimitive>(&record.entries()[i]);
    CHECK(primitive != nullptr && primitive->key.has_value());
    if (primitive != nullptr && primitive->key.has_value()) {
      CHECK_EQ(primitive->key->part, expected[i]);
    }
  }
}

static void test_transfers_in_execution_order(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  device.sealRecord();
  device.gp0(0x02FF0000u);
  device.gp0(xy(37, 4));
  device.gp0(xy(20, 3));
  device.gp0(0x80000000u);
  device.gp0(xy(0, 0));
  device.gp0(xy(100, 100));
  device.gp0(xy(8, 8));
  device.gp0(0xA0000000u, 0x80020000u);
  device.gp0(xy(5, 6));
  device.gp0(xy(3, 1));
  device.gp0(0x00020001u);
  device.gp0(0x00000003u);
  const FrameRecord record = device.sealRecord();
  CHECK(record.complete());
  CHECK_EQ(record.entries().size(), 3u);
  const auto *fill = std::get_if<psx::present::VramFill>(&record.entries()[0]);
  CHECK(fill != nullptr);
  CHECK_EQ(fill->x, 32);
  CHECK_EQ(fill->width, 32);
  CHECK_EQ(fill->value, 31u << 10);
  CHECK(std::holds_alternative<psx::present::VramCopy>(record.entries()[1]));
  const auto *upload = std::get_if<psx::present::VramUpload>(&record.entries()[2]);
  CHECK(upload != nullptr);
  CHECK_EQ(upload->sourceAddress, 0x80020000u);
  const auto pixels = record.pixels(*upload);
  CHECK_EQ(pixels.size(), 3u);
  CHECK_EQ(pixels[0], 1u);
  CHECK_EQ(pixels[1], 2u);
  CHECK_EQ(pixels[2], 3u);
}

static void test_sequence_and_pending_work(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  const std::uint64_t first = device.sealRecord().sequence();
  CHECK(!device.hasUnsealedWork());
  device.gp0(0x02000000u);
  device.gp0(xy(0, 0));
  device.gp0(xy(16, 1));
  CHECK(device.hasUnsealedWork());
  CHECK_EQ(device.sealRecord().sequence(), first + 1);
  CHECK(!device.hasUnsealedWork());
}

// Pixels already in VRAM belong to the sealed record, which then cannot be replayed; the next record
// carries the whole upload.
static void test_seal_inside_an_upload(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  device.sealRecord();
  device.gp0(0xA0000000u);
  device.gp0(xy(0, 0));
  device.gp0(xy(4, 1));
  device.gp0(0x00020001u);
  const FrameRecord during = device.sealRecord();
  device.gp0(0x00040003u);
  const FrameRecord after = device.sealRecord();
  CHECK(!during.complete());
  CHECK(after.complete());
  CHECK_EQ(after.entries().size(), 1u);
  const auto *upload = std::get_if<psx::present::VramUpload>(&after.entries()[0]);
  CHECK(upload != nullptr);
  CHECK_EQ(after.pixels(*upload).size(), 4u);
}

// A quad's second triangle executes in the next record, which then cannot be replayed alone.
static void test_seal_between_quad_triangles(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  drawEverywhere(device);
  device.sealRecord();
  device.gp0(0x28FFFFFFu);
  device.gp0(xy(0, 0));
  device.gp0(xy(10, 0));
  device.gp0(xy(0, 10));
  const FrameRecord first = device.sealRecord();
  device.gp0(xy(10, 10));
  const FrameRecord second = device.sealRecord();
  CHECK(first.complete());
  CHECK(!second.complete());
  const FrameRecord third = device.sealRecord();
  CHECK(third.complete());
}

// A keyed textured draw that samples pixels the frame already wrote is recorded as an upload of its
// result; the upload keeps the key so the composer knows the object is not only primitives.
static void test_a_feedback_upload_keeps_the_primitives_key(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  drawEverywhere(device);
  device.gp0(0xE1000100u);
  device.sealRecord();
  device.gp0(0x02FFFFFFu);
  device.gp0(xy(0, 0));
  device.gp0(xy(64, 64));
  const psx::present::RecordKey key{0x80010000u, 0x80150000u, 5, 0};
  device.beginPacket();
  device.gp0(0x24808080u, 0x80100000u, key);
  device.gp0(xy(100, 100), 0x80100004u, key);
  device.gp0(0x00000000u, 0x80100008u, key);
  device.gp0(xy(120, 100), 0x8010000Cu, key);
  device.gp0(0x0100001Fu, 0x80100010u, key);
  device.gp0(xy(100, 120), 0x80100014u, key);
  device.gp0(0x00001F00u, 0x80100018u, key);
  const FrameRecord record = device.sealRecord();
  CHECK_EQ(record.entries().size(), 2u);
  const auto *upload =
      record.entries().size() > 1 ? std::get_if<psx::present::VramUpload>(&record.entries()[1]) : nullptr;
  CHECK(upload != nullptr);
  CHECK(upload != nullptr && upload->key.has_value());
  CHECK(upload != nullptr && upload->key && upload->key->object == key.object);
}

int main(void) {
  RUN(polygon_is_decoded_after_offset);
  RUN(part_restarts_per_packet);
  RUN(transfers_in_execution_order);
  RUN(sequence_and_pending_work);
  RUN(seal_inside_an_upload);
  RUN(seal_between_quad_triangles);
  RUN(a_feedback_upload_keeps_the_primitives_key);
  return pt_summary();
}
