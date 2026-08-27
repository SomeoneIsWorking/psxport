// The pixel probe must answer three separate questions with the shipping owners: which triangles
// cover a native pixel, which texel the renderer's captured page/CLUT state samples, and which opaque
// item wins the renderer's GREATER_OR_EQUAL D32 contest versus the source-authored PSX OT order.
#include "testutil.h"

#include "game.h"
#include "gpu_native_internal.h"
#include "gpu_vk.h"
#include "render_queue.h"

#include <cstring>
#include <memory>

namespace {

RqItem triangle() {
  RqItem item{};
  item.nv = 3;
  item.mode = 3;
  item.da_x0 = 0;
  item.da_y0 = 0;
  item.da_x1 = 31;
  item.da_y1 = 31;
  item.xs[0] = 0;
  item.ys[0] = 0;
  item.xs[1] = 8;
  item.ys[1] = 0;
  item.xs[2] = 0;
  item.ys[2] = 8;
  item.depth[0] = 0.25f;
  item.depth[1] = 0.50f;
  item.depth[2] = 0.75f;
  return item;
}

void test_coverage_rejects_degenerate_and_clipped_faces() {
  GpuState gpu;
  RqItem item = triangle();
  const RqPixelSample inside = rq_probe_item_pixel(gpu, item, 1, 1);
  CHECK(inside.covered);
  CHECK(inside.writes);
  CHECK_EQ(inside.triangle, 0);
  CHECK(inside.interpolated_depth > item.depth[0]);
  CHECK(inside.interpolated_depth < item.depth[2]);

  CHECK(!rq_probe_item_pixel(gpu, item, 12, 12).covered);
  item.da_x1 = 0;
  CHECK(!rq_probe_item_pixel(gpu, item, 1, 1).covered);
  item = triangle();
  for (int vertex = 0; vertex < 3; ++vertex) {
    item.xs[vertex] = 2;
    item.ys[vertex] = 2;
  }
  CHECK(!rq_probe_item_pixel(gpu, item, 2, 2).covered);
}

void test_explicit_sampler_uses_page_clut_and_texture_window() {
  GpuState gpu;
  *gpu.vram(66, 23) = 0xA4C2;
  *gpu.vram(320 + 4, 41) = 0x9234;
  GpuTextureSample sample = gpu.sample_tex_at(10, 3, 64, 20, 0, 320, 41, 0, 0, 0, 0);
  CHECK_EQ(sample.u, 10);
  CHECK_EQ(sample.v, 3);
  CHECK_EQ(sample.source_word, 0xA4C2);
  CHECK_EQ(sample.palette_index, 4);
  CHECK_EQ(sample.texel, 0x9234);

  *gpu.vram(133, 24) = 0x7B19;
  *gpu.vram(400 + 0x7B, 42) = 0x4567;
  sample = gpu.sample_tex_at(7, 4, 130, 20, 1, 400, 42, 0, 0, 0, 0);
  CHECK_EQ(sample.source_word, 0x7B19);
  CHECK_EQ(sample.palette_index, 0x7B);
  CHECK_EQ(sample.texel, 0x4567);

  *gpu.vram(205, 36) = 0x1357;
  sample = gpu.sample_tex_at(5, 6, 200, 30, 2, 0, 0, 0, 0, 0, 0);
  CHECK_EQ(sample.source_word, 0x1357);
  CHECK_EQ(sample.texel, 0x1357);

  *gpu.vram(66, 28) = 0x0003;
  *gpu.vram(320 + 3, 41) = 0x8001;
  sample = gpu.sample_tex_at(0, 0, 64, 20, 0, 320, 41, 1, 1, 1, 1);
  CHECK_EQ(sample.u, 8);
  CHECK_EQ(sample.v, 8);
  CHECK_EQ(sample.palette_index, 3);
  CHECK_EQ(sample.texel, 0x8001);
}

void test_pixel_probe_reports_texture_transparency_and_stp_blend() {
  GpuState gpu;
  RqItem item = triangle();
  item.mode = 0;
  item.tp_x = 64;
  item.tp_y = 20;
  item.clut_x = 320;
  item.clut_y = 41;
  for (int vertex = 0; vertex < 3; ++vertex) {
    item.us[vertex] = 4;
    item.vs[vertex] = 3;
  }
  *gpu.vram(65, 23) = 0x0007;
  *gpu.vram(327, 41) = 0x8123;
  item.semi = 1;
  RqPixelSample sample = rq_probe_item_pixel(gpu, item, 1, 1);
  CHECK(sample.covered);
  CHECK(sample.writes);
  CHECK(sample.blends);
  CHECK_EQ(sample.source_word, 0x0007);
  CHECK_EQ(sample.palette_index, 7);
  CHECK_EQ(sample.texel, 0x8123);

  *gpu.vram(327, 41) = 0;
  sample = rq_probe_item_pixel(gpu, item, 1, 1);
  CHECK(sample.covered);
  CHECK(!sample.writes);
  CHECK(!sample.blends);
  CHECK_EQ(sample.texel, 0);
}

void test_shipping_depth_and_source_ot_choose_their_documented_direction() {
  const float depth = 0.4f;
  CHECK(std::strcmp(gpu_vk_world_depth_compare_name(), "GREATER_OR_EQUAL") == 0);
  CHECK(gpu_vk_world_depth_test_passes(0.5f, 0.5f));
  CHECK(gpu_vk_world_depth_test_passes(0.6f, 0.5f));
  CHECK(!gpu_vk_world_depth_test_passes(0.4f, 0.5f));
  CHECK(gpu_vk_map_ordered_3d_depth(depth, 2) > gpu_vk_map_ordered_3d_depth(depth, 1));
  CHECK(gpu_vk_map_ordered_3d_depth(0.6f, 0) > gpu_vk_map_ordered_3d_depth(0.4f, 0));

  RqPixelProbeWinner current{};
  RqItem first{};
  first.sort_key = 500;
  first.key_ord = 0.8f;
  first.seq = 7;
  CHECK(rq_source_ot_candidate_wins(first, current));
  current.valid = true;
  current.key_ord = first.key_ord;
  current.seq = first.seq;

  RqItem laterSameBucket = first;
  laterSameBucket.seq = 8;
  CHECK(!rq_source_ot_candidate_wins(laterSameBucket, current));
  RqItem earlierSameBucket = first;
  earlierSameBucket.seq = 6;
  CHECK(rq_source_ot_candidate_wins(earlierSameBucket, current));
  RqItem nearerBucket = first;
  nearerBucket.key_ord = 0.9f;
  CHECK(rq_source_ot_candidate_wins(nearerBucket, current));
  RqItem noSourceKey = first;
  noSourceKey.sort_key = -1;
  CHECK(!rq_source_ot_candidate_wins(noSourceKey, current));
}

void test_provenance_packet_reads_the_exact_tagged_guest_payload() {
  auto game = std::make_unique<Game>();
  constexpr uint32_t node = 0x80012340u;
  game->core.mem_w32(node, 0x03FFFFFFu);
  game->core.mem_w32(node + 4u, 0x30FF2952u);
  game->core.mem_w32(node + 8u, 0x01730023u);
  game->core.mem_w32(node + 12u, 0x00440041u);

  const GpuProvenancePacket packet = gpu_provenance_packet(game->core, node);
  CHECK_EQ(packet.node, node);
  CHECK_EQ(packet.word_count, 3);
  CHECK_EQ(packet.words[0], 0x30FF2952u);
  CHECK_EQ(packet.words[1], 0x01730023u);
  CHECK_EQ(packet.words[2], 0x00440041u);

  game->core.mem_w32(node, 0);
  CHECK_EQ(gpu_provenance_packet(game->core, node).word_count, 0);
  CHECK_EQ(gpu_provenance_packet(game->core, 0).word_count, 0);
}

} // namespace

int main() {
  RUN(coverage_rejects_degenerate_and_clipped_faces);
  RUN(explicit_sampler_uses_page_clut_and_texture_window);
  RUN(pixel_probe_reports_texture_transparency_and_stp_blend);
  RUN(shipping_depth_and_source_ot_choose_their_documented_direction);
  RUN(provenance_packet_reads_the_exact_tagged_guest_payload);
  return pt_summary();
}
