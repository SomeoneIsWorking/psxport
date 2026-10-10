// test_record_present - what the record rasterizer presents: the shown buffer, interlaced fields, widescreen
// canvases and 60 fps in-betweens, each checked against the GPU device's own drawing.

#include "record_raster_harness.h"

using namespace record_raster_harness;

// A present before the first record shows the presenter's empty record; the device's first record still lands.
static void test_the_first_record_follows_the_empty_one(void) {
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  RecordRasterizer rasterizer(gDevice);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  const psx::gpu::RecordView view{kBuffer, 0};
  rasterizer.update(cmd, psx::present::FrameRecord(), false, device.vram(), 1, view);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  rasterizer.update(cmd, device.sealRecord(), false, device.vram(), 1, view);
  rasterizer.download(cmd, {rasterizer.image(), kWholeVram});
  CHECK(submitAndWait(cmd));
  const std::vector<std::uint16_t> vram(device.vram().begin(), device.vram().end());
  CHECK_EQ(differsFromVram(rasterizer.downloaded(), 1024, 512, vram, 0, 0), 0);
}

// Frame k draws one buffer while the other, drawn by k-1, is displayed.
static void test_a_double_buffer_in_between_sits_between_the_shown_pictures(void) {
  const std::vector<Present> presents = presentFrames(6, alternating, previousAlternating);
  CHECK_EQ(presents.size(), 2u + 2u * 4u);
  checkPresents(presents, 1, alternating, previousAlternating);
}

// Spyro 2's cadence: each walk is followed by a record that drew nothing, which changes no pixel.
static void test_an_empty_record_after_the_shown_one_keeps_its_in_between(void) {
  const std::vector<Present> presents = presentFrames(6, alternating, previousAlternating, true);
  CHECK_EQ(presents.size(), 4u + 3u * 4u);
  checkPresents(presents, 1, alternating, previousAlternating);
}

// Every present is the render at its t: at t = 1 exactly the device's buffer.
static void test_a_rendered_object_draws_every_present_from_its_state(void) {
  psx::present::StateProducers renders;
  renders.install(kTriangleProducer, std::make_unique<TriangleRender>());
  const std::vector<Present> doubled = presentFrames(6, alternating, previousAlternating, false, &renders);
  CHECK_EQ(doubled.size(), 2u + 2u * 4u);
  for (std::size_t i = 1; i < doubled.size(); i++) {
    CHECK(doubled[i].composed);
  }
  checkPresents(doubled, 1, alternating, previousAlternating);
  const std::vector<Present> single = presentFrames(6, ::single, ::single, false, &renders);
  checkPresents(single, 0, ::single, ::single);
}

static void test_a_single_buffer_in_between_blends_n_minus_one_and_n(void) {
  const std::vector<Present> presents = presentFrames(6, single, single);
  CHECK_EQ(presents.size(), 1u + 2u * 5u);
  checkPresents(presents, 0, single, single);
}

static void test_four_three_presents_the_device_display(void) {
  Stream s = baseState();
  s.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  crossingPrimitives(s);
  const CanvasOutcome out = present({s}, {kBuffer, 0});
  CHECK(out.ran);
  CHECK(out.picture.texture == out.vramImage);
  CHECK(out.picture.rect == kBuffer);
  CHECK_EQ(differsFromVram(out.canvas, 320, 240, out.device, kBuffer.x0, kBuffer.y0), 0);
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);
}

// A device that rasterizes one field per frame (480-line interlace, E1 bit 10 clear) shows that field with each
// row doubled over its pair; the other field's rows are the previous frame's and never reach the picture.
static void test_an_interlaced_one_field_frame_presents_that_field(void) {
  Stream s = baseState();
  s.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  s.add({0xE1000000u | page(8, 0, 0, 0)});
  s.add({rgb(0x28, 200, 30, 30),
         xy(kBuffer.x0, kBuffer.y0),
         xy(kBuffer.x1, kBuffer.y0),
         xy(kBuffer.x0, kBuffer.y1),
         xy(kBuffer.x1, kBuffer.y1)});
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  device.gp1(0x08000000u | 0x24u | 0x01u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  RecordRasterizer rasterizer(gDevice);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gDevice);
  rasterizer.update(cmd, device.sealRecord(), false, device.vram(), 1, {kBuffer, 0});
  for (std::uint32_t word : s.words) {
    device.gp0(word, 0x80100000u);
  }
  const psx::present::FrameRecord record = device.sealRecord();
  rasterizer.update(cmd, record, false, device.vram(), 1, {kBuffer, 0});
  const psx::gpu::RecordPicture picture = rasterizer.presented();
  rasterizer.download(cmd, picture);
  CHECK(submitAndWait(cmd));
  const std::vector<std::uint16_t> shown = rasterizer.downloaded();
  const std::optional<int> undrawn = record.undrawnRowParity();
  CHECK(undrawn.has_value());
  CHECK(picture.texture != rasterizer.image());
  CHECK(rasterizer.woven().texture == rasterizer.image());
  const std::span<const std::uint16_t> vram = device.vram();
  const int width = kBuffer.x1 - kBuffer.x0;
  long wrong = 0;
  long stale = 0;
  for (int row = 0; row < kBuffer.y1 - kBuffer.y0; row++) {
    const int vramRow = kBuffer.y0 + row;
    const int drawnRow = (vramRow & 1) == *undrawn ? vramRow ^ 1 : vramRow;
    for (int column = 0; column < width; column++) {
      const std::size_t at =
          static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column);
      const std::uint16_t want = vram[static_cast<std::size_t>(drawnRow) * psx::gpu::kRecordVramWidth +
                                      static_cast<std::size_t>(kBuffer.x0 + column)];
      wrong += shown[at] != want ? 1 : 0;
      stale += vram[static_cast<std::size_t>(vramRow) * psx::gpu::kRecordVramWidth +
                    static_cast<std::size_t>(kBuffer.x0 + column)] != want
                   ? 1
                   : 0;
    }
  }
  CHECK_EQ(wrong, 0);
  CHECK(stale > 0);
}

// The canvas equals the device drawing the same frame with its draw area widened by the margin.
static void test_a_display_draw_reaches_the_margins(void) {
  Stream wide = baseState();
  wide.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  wide.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0, kBuffer.y0), xy(320, 240)});
  crossingPrimitives(wide);
  const CanvasOutcome out = present({wide}, {kBuffer, kMargin});
  CHECK(out.ran);
  CHECK(out.picture.texture != out.vramImage);
  CHECK(out.picture.rect == (psx::gpu::RecordRect{0, 0, 416, 240}));
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);

  Stream reference = baseState();
  reference.area(kBuffer.x0 - kMargin, kBuffer.y0, kBuffer.x1 - 1 + kMargin, kBuffer.y1 - 1);
  reference.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0 - kMargin, kBuffer.y0), xy(416, 240)});
  crossingPrimitives(reference);
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  for (std::uint32_t word : reference.words) {
    device.gp0(word);
  }
  const std::vector<std::uint16_t> wideDevice(device.vram().begin(), device.vram().end());
  CHECK_EQ(differsFromVram(out.canvas, 416, 240, wideDevice, kBuffer.x0 - kMargin, kBuffer.y0), 0);
  // Something was drawn in each margin.
  CHECK(differsFromVram(out.canvas, 416, 240, out.device, kBuffer.x0 - kMargin, kBuffer.y0) > 0);
}

// A buffer at VRAM x = 0 puts its left margin left of VRAM; a flat quad drawn there lands in the canvas.
static void test_a_draw_left_of_vram_reaches_the_left_margin(void) {
  constexpr psx::gpu::RecordRect kFlush{0, 0, 320, 240};
  Stream s = baseState();
  s.area(kFlush.x0, kFlush.y0, kFlush.x1 - 1, kFlush.y1 - 1);
  s.add({rgb(0x28, 0, 248, 0), xy(-40, 20), xy(-5, 20), xy(-40, 60), xy(-5, 60)});
  const CanvasOutcome out = present({s}, {kFlush, kMargin});
  CHECK(out.ran);
  constexpr std::size_t width = 320 + 2 * kMargin;
  const std::uint16_t green = static_cast<std::uint16_t>(31u << 5);
  long drawn = 0;
  for (int row = 20; row < 60; row++) {
    for (int column = kMargin - 40; column < kMargin - 5; column++) {
      drawn += out.canvas[static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column)] == green ? 1 : 0;
    }
  }
  CHECK_EQ(drawn, 35 * 40);
}

// Mega Man X4 shows (0,0,320,240) and (0,240,320,479) alternately: neither canvas may retire the other.
static void test_a_double_buffer_of_differing_heights_keeps_both_canvases(void) {
  const psx::gpu::RecordCanvas top{{0, 0, 320, 240}, kMargin};
  const psx::gpu::RecordCanvas bottom{{0, 240, 320, 479}, kMargin};
  CHECK(psx::gpu::canvasSurvives(top, 320, kMargin));
  CHECK(psx::gpu::canvasSurvives(bottom, 320, kMargin));
  CHECK(!psx::gpu::canvasSurvives(top, 320, kMargin + 1));
  CHECK(!psx::gpu::canvasSurvives(top, 256, kMargin));
  CHECK(!psx::gpu::canvasSurvives(top, 320, 0));
}

// A draw area inset in rows (Tekken 3 draws rows 20..467 of 480) still reaches the margins.
static void test_a_row_inset_display_draw_reaches_the_margins(void) {
  constexpr int kInset = 20;
  Stream inset = baseState();
  inset.area(kBuffer.x0, kBuffer.y0 + kInset, kBuffer.x1 - 1, kBuffer.y1 - 1 - kInset);
  inset.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0, kBuffer.y0 + kInset), xy(320, 240 - 2 * kInset)});
  crossingPrimitives(inset);
  const CanvasOutcome out = present({inset}, {kBuffer, kMargin});
  CHECK(out.ran);
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);

  Stream reference = baseState();
  reference.area(kBuffer.x0 - kMargin, kBuffer.y0 + kInset, kBuffer.x1 - 1 + kMargin, kBuffer.y1 - 1 - kInset);
  reference.add({rgb(0x02, 8, 16, 24), xy(kBuffer.x0 - kMargin, kBuffer.y0 + kInset), xy(416, 240 - 2 * kInset)});
  crossingPrimitives(reference);
  GpuDevice device;
  device.gp1(0x00000000u, 0);
  Stream pattern;
  pattern.upload(0, 0, 1024, 512, 7);
  for (std::uint32_t word : pattern.words) {
    device.gp0(word);
  }
  for (std::uint32_t word : reference.words) {
    device.gp0(word);
  }
  const std::vector<std::uint16_t> wideDevice(device.vram().begin(), device.vram().end());
  // Rows outside the inset keep the seed, so compare the inset rows only.
  long wrong = 0;
  for (int row = kInset; row < 240 - kInset; row++) {
    for (int column = 0; column < 416; column++) {
      const std::uint16_t pixel = out.canvas[(static_cast<std::size_t>(row) * 416) + static_cast<std::size_t>(column)];
      const std::uint16_t want = wideDevice[static_cast<std::size_t>(kBuffer.y0 + row) * 1024 +
                                            static_cast<std::size_t>(kBuffer.x0 - kMargin + column)];
      wrong += pixel != want ? 1 : 0;
    }
  }
  CHECK_EQ(wrong, 0);
  CHECK(differsFromVram(out.canvas, 416, 240, out.device, kBuffer.x0 - kMargin, kBuffer.y0) > 0);
}

static void test_a_fill_of_the_display_clears_the_margins(void) {
  Stream first = baseState();
  first.area(kBuffer.x0, kBuffer.y0, kBuffer.x1 - 1, kBuffer.y1 - 1);
  first.add({rgb(0x20, 255, 0, 0), xy(-200, -10), xy(700, -10), xy(250, 600)});
  Stream second;
  second.add({rgb(0x02, 0, 0, 248), xy(kBuffer.x0, kBuffer.y0), xy(320, 240)});
  const CanvasOutcome out = present({first, second}, {kBuffer, kMargin});
  CHECK(out.ran);
  long notBlue = 0;
  for (std::uint16_t pixel : out.canvas) {
    notBlue += pixel != 0x7C00u ? 1 : 0;
  }
  CHECK_EQ(notBlue, 0);
}

static void test_an_offscreen_draw_leaves_the_canvas(void) {
  Stream s = baseState();
  s.area(512, 256, 767, 511);
  s.add({rgb(0x20, 255, 0, 0), xy(400, 200), xy(900, 260), xy(600, 600)});
  const CanvasOutcome out = present({s}, {kBuffer, kMargin});
  CHECK(out.ran);
  CHECK_EQ(differsFromVram(out.image, 1024, 512, out.device, 0, 0), 0);
  // The seed: the buffer's VRAM in the middle, empty margins.
  long wrong = 0;
  for (int row = 0; row < 240; row++) {
    for (int column = 0; column < 416; column++) {
      const std::uint16_t pixel = out.canvas[(static_cast<std::size_t>(row) * 416) + static_cast<std::size_t>(column)];
      const int x = column - kMargin;
      const std::uint16_t want =
          x < 0 || x >= 320 ? 0 : out.device[static_cast<std::size_t>(row) * 1024 + static_cast<std::size_t>(64 + x)];
      wrong += pixel != want ? 1 : 0;
    }
  }
  CHECK_EQ(wrong, 0);
}

int main(void) {
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
  if (!SDL_Init(SDL_INIT_VIDEO) ||
      (gDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr)) == nullptr) {
    fprintf(stderr, "    FAIL no headless Vulkan device: %s\n", SDL_GetError());
    return 1;
  }
  RUN(the_first_record_follows_the_empty_one);
  RUN(an_interlaced_one_field_frame_presents_that_field);
  RUN(four_three_presents_the_device_display);
  RUN(a_display_draw_reaches_the_margins);
  RUN(a_draw_left_of_vram_reaches_the_left_margin);
  RUN(a_row_inset_display_draw_reaches_the_margins);
  RUN(a_double_buffer_of_differing_heights_keeps_both_canvases);
  RUN(a_fill_of_the_display_clears_the_margins);
  RUN(an_offscreen_draw_leaves_the_canvas);
  RUN(a_double_buffer_in_between_sits_between_the_shown_pictures);
  RUN(an_empty_record_after_the_shown_one_keeps_its_in_between);
  RUN(a_single_buffer_in_between_blends_n_minus_one_and_n);
  RUN(a_rendered_object_draws_every_present_from_its_state);
  SDL_DestroyGPUDevice(gDevice);
  SDL_Quit();
  return pt_summary();
}
