// test_emit_memory.cpp — a body's memory is the guest's or host bytes, and a render's writes never reach the guest.
#include "emit_memory.h"
#include "game.h"
#include "testutil.h"

#include <array>
#include <memory>

namespace {

using psx::present::EmitMemory;
using psx::present::HostMemory;

constexpr std::uint32_t kGuest = 0x80100000u;
constexpr std::uint32_t kHost = 0x80180000u;

std::span<const std::byte> bytesOf(const std::array<std::uint8_t, 4> &words) {
  return std::as_bytes(std::span(words));
}

} // namespace

static void test_a_guest_memory_reads_and_writes_the_core(void) {
  auto game = std::make_unique<Game>();
  const EmitMemory memory(game->core);
  CHECK(!memory.hosted());
  memory.mem_w32(kGuest, 0x11223344u);
  memory.mem_w16(kGuest + 4u, 0xFFFEu);
  memory.mem_w8(kGuest + 6u, 0x80u);
  CHECK_EQ(game->core.mem_r32(kGuest), 0x11223344u);
  CHECK_EQ(memory.mem_r16(kGuest + 4u), 0xFFFEu);
  CHECK_EQ(memory.mem_r16s(kGuest + 4u), -2);
  CHECK_EQ(memory.mem_r8s(kGuest + 6u), -128);
}

static void test_a_hosted_body_writes_host_bytes_and_leaves_the_guest_alone(void) {
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  core.mem_w32(kHost, 0xAAAAAAAAu);
  HostMemory host;
  host.zero(kHost, 16u);
  const EmitMemory memory(core, host);
  CHECK(memory.hosted());
  memory.mem_w32(kHost, 0x01020304u);
  CHECK_EQ(memory.mem_r32(kHost), 0x01020304u);
  CHECK_EQ(memory.mem_r8(kHost + 3u), 0x01u);
  CHECK_EQ(core.mem_r32(kHost), 0xAAAAAAAAu);
}

// A read outside every host range is level data and comes from the guest.
static void test_a_read_outside_the_host_ranges_falls_through_to_the_guest(void) {
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  core.mem_w32(kGuest, 0x5566u);
  HostMemory host;
  host.zero(kHost, 4u);
  const EmitMemory memory(core, host);
  CHECK_EQ(memory.mem_r32(kGuest), 0x5566u);
}

static void test_a_range_provided_from_bytes_is_found_through_every_segment(void) {
  HostMemory host;
  const std::array<std::uint8_t, 4> data{1, 2, 3, 4};
  host.provide(kHost, bytesOf(data));
  std::uint32_t word = 0;
  CHECK(host.read(kHost, &word, sizeof(word)));
  CHECK_EQ(word, 0x04030201u);
  CHECK(host.read(kHost & 0x1FFFFFFFu, &word, sizeof(word)));
  CHECK(host.read(0xA0000000u | (kHost & 0x1FFFFFFFu), &word, sizeof(word)));
  CHECK(!host.read(kHost + 4u, &word, sizeof(word)));
  CHECK(!host.read(kHost - 1u, &word, 1u));
  CHECK_EQ(host.view(kHost + 1u, 2u).size(), 2u);
}

static void test_ranges_in_any_order_are_each_found(void) {
  HostMemory host;
  host.zero(kHost + 0x100u, 16u);
  host.zero(kHost, 16u);
  host.zero(kHost + 0x80u, 16u);
  const std::uint32_t value = 7u;
  host.write(kHost + 0x80u, &value, sizeof(value));
  std::uint32_t back = 0;
  CHECK(host.read(kHost + 0x80u, &back, sizeof(back)));
  CHECK_EQ(back, 7u);
  CHECK(host.read(kHost + 0x100u, &back, sizeof(back)));
  CHECK_EQ(back, 0u);
}

static void test_a_hosted_gte_point_store_writes_host_bytes_only(void) {
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  gte_write_data(14, 0x00340012u);
  HostMemory host;
  host.zero(kHost, 16u);
  const EmitMemory memory(core, host);
  memory.storeGteXy(kHost + 8u, 14);
  CHECK_EQ(memory.mem_r32(kHost + 8u), 0x00340012u);
  CHECK_EQ(core.mem_r32(kHost + 8u), 0u);
}

int main(void) {
  RUN(a_guest_memory_reads_and_writes_the_core);
  RUN(a_hosted_body_writes_host_bytes_and_leaves_the_guest_alone);
  RUN(a_read_outside_the_host_ranges_falls_through_to_the_guest);
  RUN(a_range_provided_from_bytes_is_found_through_every_segment);
  RUN(ranges_in_any_order_are_each_found);
  RUN(a_hosted_gte_point_store_writes_host_bytes_only);
  return pt_summary();
}
