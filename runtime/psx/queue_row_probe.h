#pragma once

#include <array>
#include <cstdint>

struct RqItem;
class GpuState;

// A whole ROW of the render queue's own answer, so a capture can be diffed against it BY CODE.
//
// The single-pixel probe (PSXPORT_PRIMAT) explains one coordinate, which is only as trustworthy as
// the mapping between a captured image and the framebuffer. Two broad regions agreeing is not proof
// of that mapping: an error of a few pixels survives it and then silently misses any artefact small
// enough to matter (Spyro issue 0120, 2026-09-19 — a 2-pixel red artefact that the probe never found
// because the row it was on was never the row being explained). A strip of per-pixel colours pins the
// mapping and localises the disagreement in one run instead of one run per pixel.
//
// Knob: PSXPORT_QROW="y,x0,x1[,frame]" in DISPLAY coordinates. A malformed value is refused.
class QueueRowProbe {
public:
  static constexpr int kMaxSpan = 256;

  // Called for every prim as the render queue flushes it; detects the frame boundary itself.
  void observe(GpuState &gpu, const RqItem &item);

private:
  struct Pixel {
    bool covered = false;
    float d32 = 0.0f;
    uint32_t seq = 0;
    uint32_t painter = 0;
    int r = 0;
    int g = 0;
    int b = 0;
  };

  bool configure();
  void reportFrame() const;

  bool mConfigured = false;
  bool mEnabled = false;
  int mY = 0;
  int mX0 = 0;
  int mX1 = 0;
  int mFromFrame = 0;
  int mFrame = -1;
  long mScanned = 0;
  std::array<Pixel, kMaxSpan> mPixels{};
};
