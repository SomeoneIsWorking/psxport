// test_vram_persistence.cpp — THE COMPOSITE IS A FRAMEBUFFER: what a double-buffered guest drew into
// the back buffer must still be there one frame later, when that buffer is the one being displayed.
//
// WHAT WENT WRONG, in one sentence: `upload_vram()` memcpy'd all 1024x512 of guest CPU VRAM over the
// composite on every present, and under vk_path() the guest's polygons never reach CPU VRAM — so every
// present erased every pixel the VK rasterizer had ever drawn.
//
// MEASURED (Spyro, WINDOWED, the user's own pad replay `replays/bugs/flicker-session.pad`, presents
// 2200..2219 captured with PSXPORT_PRESENT_SHOT_AT):
//   * 20/20 consecutive presents alternate 3169-ish distinct colours / EXACTLY 2 colours. "non-black %"
//     reads 93.33% on BOTH classes, which is why distinct-colour count is the instrument here.
//   * the 2-colour frames are a solid 0xffdead — the guest's own clear fill, uploaded from CPU VRAM,
//     with no geometry over it.
//   * PSXPORT_DEBUG=presentskip: all 20 are PRESENT_REBUILD_GEOM. The batch is NOT empty on the flat
//     frames, so the empty-batch/REUSE_LAST classification is not involved.
//   * PSXPORT_DEBUG=rqflush (queue y-range): the queues alternate between y=[-93..332] and y=[147..572]
//     — the two display buffers, 240 apart. The present that shows a scene is the one whose batch
//     happens to contain the PREVIOUS queue, re-emitted by RenderQueue::flush's deferred reset.
//   * REMOVING THAT RE-EMIT ALONE turned 20/20 presents FLAT (2 colours). That is the discriminating
//     experiment: the re-emit was the only thing putting geometry into the displayed buffer, i.e. the
//     composite has no persistence of its own.
//
// This test models that guest — two 512x240 buffers at VRAM y=0 and y=240, geometry that goes only to
// the rasterizer, fills that go only to CPU VRAM — and drives it with the two upload rules:
//   UPLOAD_WHOLE_CANVAS  the rule the renderer shipped: memcpy everything, every present.
//   UPLOAD_DIRTY_RECTS   the rule it ships now: upload exactly the regions VramDirty recorded.
// Both arms are asserted, in opposite directions. The WHOLE_CANVAS arm is the NEGATIVE CONTROL: it is
// asserted to LOSE the previous frame's geometry, so this file cannot pass while modelling nothing.
//
// HERMETIC: no GPU, no window, no disc. The unit under test is VramDirty (runtime/recomp/vram_dirty.h);
// the framebuffer, the fills and the rasterizer around it are a model.

#include "testutil.h"
#include "vram_dirty.h"
#include <stdlib.h>
#include <string.h>

// ---- the modelled console ---------------------------------------------------------------------
static const int W = 1024, H = 512;   // guest VRAM
static const int DW = 512, DH = 240;  // one display buffer
static const int BUF_Y[2] = {0, 240}; // the two buffers, exactly as Spyro's queue y-ranges show

static const uint16_t CLEAR_FILL = 0x6BFF; // whatever the guest fills the back buffer with
#define GEO_COLOUR(frame) ((uint16_t)(0x1000 + (frame)))

enum UploadRule { UPLOAD_WHOLE_CANVAS, UPLOAD_DIRTY_RECTS };

struct Console {
  uint16_t *cpu;  // guest CPU VRAM — GP0 uploads/fills/copies ONLY (vk_path: no polygons here)
  uint16_t *comp; // the composite the display scans out of (the VK image)
  VramDirty dirty;
  // the native geometry batch: a filled span, reset at every present (GpuVkState::frame_end)
  int batch_y0 = 0, batch_y1 = 0;
  uint16_t batch_colour = 0;
  bool batch_live = false;

  Console() {
    cpu = (uint16_t *)calloc((size_t)W * H, 2);
    comp = (uint16_t *)calloc((size_t)W * H, 2);
    dirty.setCanvas(W, H);
    dirty.clear(); // the composite starts in a known (blank) state for the model
  }
  ~Console() {
    free(cpu);
    free(comp);
  }

  // GP0 0x02 fill / 0xA0 upload — CPU VRAM, plus the dirty chokepoint every such path already calls.
  void guestFill(int x, int y, int w, int h, uint16_t v) {
    for (int r = y; r < y + h; r++) {
      for (int c = x; c < x + w; c++) {
        cpu[(size_t)r * W + c] = v;
      }
    }
    dirty.add(x, y, w, h);
  }
  // A native submit: goes to the rasterizer, NEVER to CPU VRAM.
  void submitGeometry(int y0, int y1, uint16_t colour) {
    batch_y0 = y0;
    batch_y1 = y1;
    batch_colour = colour;
    batch_live = true;
  }

  void present(UploadRule rule) {
    if (rule == UPLOAD_WHOLE_CANVAS) {
      // A hand transcription of the rule the renderer USED to ship. It no longer exists in the code,
      // so it has to be written out here; that is what makes this arm a negative control and not a
      // second copy of the shipped decision.
      memcpy(comp, cpu, (size_t)W * H * 2);
    } else {
      // THE SHIPPED DECISION, called directly — not modelled. gpu_vk.cpp's upload_vram() loops over
      // exactly this, so a regression that re-broadens the upload set fails here.
      VramDirtyRect rs[VramDirty::CAP + 1];
      const int nr = vram_upload_regions(dirty, rs, VramDirty::CAP + 1);
      for (int i = 0; i < nr; i++) {
        for (int y = rs[i].y; y < rs[i].y + rs[i].h; y++) {
          memcpy(&comp[(size_t)y * W + rs[i].x], &cpu[(size_t)y * W + rs[i].x], (size_t)rs[i].w * 2);
        }
      }
    }
    dirty.clear();
    if (batch_live) { // render_geom(): draw the batch over it
      for (int y = batch_y0; y < batch_y1; y++) {
        for (int x = 100; x < 200; x++) {
          comp[(size_t)y * W + x] = batch_colour;
        }
      }
      batch_live = false; // frame_end() resets the batch
    }
  }

  // How many pixels of the display band starting at `bufY` carry `colour`.
  int bandPixels(int bufY, uint16_t colour) const {
    int n = 0;
    for (int y = bufY; y < bufY + DH; y++) {
      for (int x = 0; x < DW; x++) {
        if (comp[(size_t)y * W + x] == colour) {
          n++;
        }
      }
    }
    return n;
  }
};

// Run `frames` guest frames of the modelled double-buffered game and return, for each present, how many
// pixels of the DISPLAYED band carried the PREVIOUS frame's geometry colour. That is the whole property.
static void run_double_buffered(UploadRule rule, int frames, int *prev_geo_px, int *cur_geo_px) {
  Console c;
  for (int n = 0; n < frames; n++) {
    const int back = BUF_Y[n & 1];
    const int front = BUF_Y[(n + 1) & 1];
    c.guestFill(0, back, DW, DH, CLEAR_FILL);              // the guest clears its back buffer
    c.submitGeometry(back + 10, back + 30, GEO_COLOUR(n)); // ...and draws frame n into it
    c.present(rule);
    // The display shows the buffer the guest finished LAST frame.
    prev_geo_px[n] = c.bandPixels(front, GEO_COLOUR(n - 1));
    cur_geo_px[n] = c.bandPixels(back, GEO_COLOUR(n));
  }
}

// ---- 1. the property, under the rule the renderer ships ---------------------------------------
static void test_displayed_buffer_keeps_last_frames_geometry(void) {
  const int N = 8;
  int prev[N], cur[N];
  run_double_buffered(UPLOAD_DIRTY_RECTS, N, prev, cur);
  // Denominator first: frames 1..N-1 are the ones with a previous frame to keep.
  CHECK_EQ(N - 1, 7);
  int kept = 0;
  for (int n = 1; n < N; n++) {
    CHECK_EQ(prev[n], 100 * 20); // the whole previous-frame span, still there
    if (prev[n] > 0) {
      kept++;
    }
  }
  CHECK_EQ(kept, N - 1);
  // ...and the frame just drawn is in the buffer it was drawn into, on every present.
  for (int n = 0; n < N; n++) {
    CHECK_EQ(cur[n], 100 * 20);
  }
}

// ---- 2. NEGATIVE CONTROL: the pre-fix rule must LOSE it ---------------------------------------
// Without this, a model that never draws anything would pass case 1 vacuously.
static void test_whole_canvas_upload_destroys_it(void) {
  const int N = 8;
  int prev[N], cur[N];
  run_double_buffered(UPLOAD_WHOLE_CANVAS, N, prev, cur);
  int lost = 0;
  for (int n = 1; n < N; n++) {
    CHECK_EQ(prev[n], 0); // erased by the blanket re-upload — the measured flat frame
    if (prev[n] == 0) {
      lost++;
    }
  }
  CHECK_EQ(lost, N - 1);
  // The current frame still shows, because its geometry is rasterized AFTER the upload. That is
  // exactly why the bug alternated instead of blacking the screen out entirely.
  for (int n = 0; n < N; n++) {
    CHECK_EQ(cur[n], 100 * 20);
  }
}

// ---- 3. an upload-only screen still repaints (issue 0043 / C149 must not regress) --------------
// A logo/loading screen writes VRAM and submits ZERO primitives. Under the dirty-rect rule that write
// is exactly the region uploaded, so the displayed band must change.
static void test_upload_only_screen_still_repaints(void) {
  Console c;
  c.guestFill(0, 0, DW, DH, 0x0001);
  c.present(UPLOAD_DIRTY_RECTS);
  CHECK_EQ(c.bandPixels(0, 0x0001), DW * DH);
  c.guestFill(0, 0, DW, DH, 0x7FFF); // the next logo frame, still zero primitives
  c.present(UPLOAD_DIRTY_RECTS);
  CHECK_EQ(c.bandPixels(0, 0x7FFF), DW * DH);
  CHECK_EQ(c.bandPixels(0, 0x0001), 0);
}

// ---- 4. a present with NO guest write leaves the composite alone ------------------------------
static void test_no_guest_write_uploads_nothing(void) {
  Console c;
  c.guestFill(0, 240, DW, DH, CLEAR_FILL);
  c.submitGeometry(250, 270, GEO_COLOUR(1));
  c.present(UPLOAD_DIRTY_RECTS);
  CHECK_EQ(c.bandPixels(240, GEO_COLOUR(1)), 100 * 20);
  CHECK(c.dirty.empty());                               // cleared by the present
  c.present(UPLOAD_DIRTY_RECTS);                        // an idle field: no fill, no geometry
  CHECK_EQ(c.bandPixels(240, GEO_COLOUR(1)), 100 * 20); // still there
}

// ---- 5. VramDirty itself ----------------------------------------------------------------------
static void test_dirty_starts_as_all(void) {
  VramDirty d;
  CHECK(d.all()); // no canvas, nothing uploaded: upload everything
  d.setCanvas(W, H);
  CHECK(d.all());
  d.clear();
  CHECK(!d.all());
  CHECK(d.empty());
  CHECK_EQ(d.count(), 0);
}

static void test_dirty_clips_and_counts_drops(void) {
  VramDirty d;
  d.setCanvas(W, H);
  d.clear();
  d.add(-50, -50, 100, 100); // straddles the origin
  CHECK_EQ(d.count(), 1);
  CHECK_EQ(d.at(0).x, 0);
  CHECK_EQ(d.at(0).y, 0);
  CHECK_EQ(d.at(0).w, 50);
  CHECK_EQ(d.at(0).h, 50);
  d.add(W + 10, 0, 8, 8); // entirely off-canvas
  d.add(0, 0, 0, 8);      // empty
  CHECK_EQ(d.count(), 1);
  CHECK_EQ((int)d.dropped(), 2); // dropped, and SAID SO — not silently ignored
  CHECK_EQ((int)d.adds(), 3);
}

static void test_dirty_dedupes_contained_writes(void) {
  VramDirty d;
  d.setCanvas(W, H);
  d.clear();
  d.add(0, 0, 512, 240);
  d.add(10, 10, 20, 20); // inside the first
  CHECK_EQ(d.count(), 1);
  CHECK_EQ((int)d.adds(), 2);
}

// Overflow must be CONSERVATIVE: never lose coverage. Feed CAP+8 disjoint writes and assert every one
// of them is still covered by the union of what is stored, with the count as the denominator.
static void test_dirty_overflow_never_loses_coverage(void) {
  VramDirty d;
  d.setCanvas(W, H);
  d.clear();
  const int N = VramDirty::CAP + 8;
  int xs[N], ys[N];
  for (int i = 0; i < N; i++) {
    xs[i] = (i % 32) * 32;
    ys[i] = (i / 32) * 32;
    d.add(xs[i], ys[i], 8, 8);
  }
  CHECK(d.count() <= VramDirty::CAP);
  CHECK_EQ((int)d.merges(), 8);
  int covered = 0;
  for (int i = 0; i < N; i++) {
    bool ok = false;
    for (int j = 0; j < d.count(); j++) {
      VramDirtyRect r = d.at(j);
      if (xs[i] >= r.x && ys[i] >= r.y && xs[i] + 8 <= r.x + r.w && ys[i] + 8 <= r.y + r.h) {
        ok = true;
        break;
      }
    }
    if (ok) {
      covered++;
    }
  }
  CHECK_EQ(covered, N); // scanned N writes, N covered — none dropped by the merge
}

static void test_dirty_canvas_change_rearms_full_upload(void) {
  VramDirty d;
  d.setCanvas(W, H);
  d.clear();
  d.add(0, 0, 8, 8);
  CHECK_EQ(d.count(), 1);
  d.setCanvas(W, 256); // the target was recreated: its content is unknown again
  CHECK(d.all());
  CHECK_EQ(d.whole().w, W);
  CHECK_EQ(d.whole().h, 256);
}

int main(void) {
  RUN(displayed_buffer_keeps_last_frames_geometry);
  RUN(whole_canvas_upload_destroys_it);
  RUN(upload_only_screen_still_repaints);
  RUN(no_guest_write_uploads_nothing);
  RUN(dirty_starts_as_all);
  RUN(dirty_clips_and_counts_drops);
  RUN(dirty_dedupes_contained_writes);
  RUN(dirty_overflow_never_loses_coverage);
  RUN(dirty_canvas_change_rearms_full_upload);
  return pt_summary();
}
