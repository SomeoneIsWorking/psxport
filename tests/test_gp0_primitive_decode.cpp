// test_gp0_primitive_decode — a packet decoded on its own matches what the device recorded for it,
// once the draw offset is applied.
#include "gp0_primitive_decode.h"
#include "gpu_device.h"
#include "testutil.h"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace {

using psx::gpu::GpuDevice;
using psx::present::DrawPrimitive;

constexpr int kOffsetX = 10;
constexpr int kOffsetY = 20;

constexpr std::uint32_t xy(int x, int y) {
  return (static_cast<std::uint32_t>(x) & 0x7FFu) | ((static_cast<std::uint32_t>(y) & 0x7FFu) << 16);
}

constexpr std::uint32_t uv(int u, int v, std::uint16_t attribute) {
  return static_cast<std::uint32_t>(u) | (static_cast<std::uint32_t>(v) << 8) |
         (static_cast<std::uint32_t>(attribute) << 16);
}

std::optional<DrawPrimitive> recorded(const std::vector<std::uint32_t> &packet) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  device.gp0(0xE3000000u);
  device.gp0(0xE4000000u | 1023u | (511u << 10));
  device.gp0(0xE5000000u | static_cast<std::uint32_t>(kOffsetX) | (static_cast<std::uint32_t>(kOffsetY) << 11));
  device.sealRecord();
  for (std::uint32_t word : packet) {
    device.gp0(word);
  }
  const psx::present::FrameRecord record = device.sealRecord();
  if (record.entries().size() != 1u) {
    return std::nullopt;
  }
  return std::get<DrawPrimitive>(record.entries()[0]);
}

void checkMatches(const std::vector<std::uint32_t> &packet) {
  const std::optional<DrawPrimitive> drawn = recorded(packet);
  const auto decoded = psx::gpu::decodePacketPrimitive(packet);
  CHECK(drawn.has_value());
  CHECK(decoded.has_value());
  if (!drawn || !decoded) {
    return;
  }
  const DrawPrimitive &device = *drawn;
  CHECK(decoded->kind == device.kind);
  CHECK_EQ(decoded->vertexCount, device.vertexCount);
  CHECK_EQ(decoded->textured, device.textured);
  CHECK_EQ(decoded->gouraud, device.gouraud);
  CHECK_EQ(decoded->semiTransparent, device.semiTransparent);
  CHECK_EQ(decoded->modulate, device.modulate);
  CHECK_EQ(decoded->width, device.width);
  CHECK_EQ(decoded->height, device.height);
  CHECK_EQ(decoded->clutWord, device.clutWord);
  for (int v = 0; v < device.vertexCount; v++) {
    psx::present::RecordVertex shifted = decoded->vertices[static_cast<std::size_t>(v)];
    shifted.x += kOffsetX;
    shifted.y += kOffsetY;
    CHECK(shifted == device.vertices[static_cast<std::size_t>(v)]);
  }
  if (device.kind == psx::present::PrimitiveKind::Polygon && device.textured) {
    CHECK_EQ(decoded->state.texPageX, device.state.texPageX);
    CHECK_EQ(decoded->state.texPageY, device.state.texPageY);
    CHECK_EQ(decoded->state.texMode, device.state.texMode);
    CHECK_EQ(decoded->state.blendMode, device.state.blendMode);
  }
}

} // namespace

static void test_a_flat_textured_triangle(void) {
  const std::uint16_t clut = (300u << 6) | 3u;
  const std::uint16_t page = 5u | (1u << 4) | (2u << 5) | (1u << 7);
  checkMatches({0x26808080u, xy(1, 2), uv(4, 5, clut), xy(60, 8), uv(40, 5, page), xy(9, 70), uv(4, 50, 0)});
}

static void test_a_gouraud_textured_quad(void) {
  const std::uint16_t clut = (480u << 6) | 7u;
  const std::uint16_t page = 9u | (0u << 7);
  checkMatches({0x3C102030u,
                xy(0, 0),
                uv(0, 0, clut),
                0x00405060u,
                xy(50, 0),
                uv(31, 0, page),
                0x00708090u,
                xy(0, 40),
                uv(0, 31, 0),
                0x00A0B0C0u,
                xy(50, 40),
                uv(31, 31, 0)});
}

static void test_a_flat_untextured_quad(void) {
  checkMatches({0x28FF0000u, xy(3, 3), xy(80, 5), xy(4, 60), xy(70, 66)});
}

static void test_a_textured_sprite(void) {
  const std::uint16_t clut = (500u << 6) | 1u;
  checkMatches({0x64808080u, xy(30, 40), uv(8, 16, clut), xy(24, 18)});
}

static void test_a_fixed_size_sprite(void) {
  checkMatches({0x78FFFFFFu, xy(-2, 100)});
}

static void test_a_gouraud_line(void) {
  checkMatches({0x52FFFFFFu, xy(10, 20), 0x00808080u, xy(40, -6)});
}

static void test_a_flat_line(void) {
  checkMatches({0x40204060u, xy(1, 2), xy(30, 40)});
}

static void test_other_commands_decode_to_nothing(void) {
  CHECK(!psx::gpu::decodePacketPrimitive(std::vector<std::uint32_t>{0x02000000u, xy(0, 0), xy(16, 16)}).has_value());
  CHECK(!psx::gpu::decodePacketPrimitive(std::vector<std::uint32_t>{0x20FFFFFFu, xy(0, 0)}).has_value());
  CHECK(!psx::gpu::decodePacketPrimitive(std::vector<std::uint32_t>{0x48FFFFFFu, xy(0, 0), xy(8, 8)}).has_value());
}

int main() {
  RUN(a_flat_textured_triangle);
  RUN(a_gouraud_textured_quad);
  RUN(a_flat_untextured_quad);
  RUN(a_textured_sprite);
  RUN(a_fixed_size_sprite);
  RUN(a_gouraud_line);
  RUN(a_flat_line);
  RUN(other_commands_decode_to_nothing);
  return pt_summary();
}
