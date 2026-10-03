#include "prim_color_census.h"

#include "cfg.h"
#include "gpu_native_internal.h"
#include "render_queue.h"

#include <cstdio>
#include <cstdlib>
#include <lucent/log.h>

namespace {
constexpr const char *kChannel = "primrgb";
constexpr int kDefaultTolerance = 24;
} // namespace

bool PrimColorCensus::configure() {
  if (mConfigured) {
    return mEnabled;
  }
  mConfigured = true;
  const char *setting = cfg_str("PSXPORT_PRIMRGB");
  if (!setting || !*setting) {
    return false;
  }
  int red = -1, green = -1, blue = -1, frame = 0, tolerance = kDefaultTolerance;
  const int fields = std::sscanf(setting, "%d,%d,%d,%d,%d", &red, &green, &blue, &frame, &tolerance);
  const bool wellFormed = fields >= 3 && red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 &&
                          blue <= 255 && frame >= 0 && tolerance >= 0 && tolerance <= 255;
  if (!wellFormed) {
    // A tolerant parser here would leave the census running against whatever the partial scan left
    // behind and print a census nobody asked for, which is the failure this instrument exists to end.
    lucent::error(kChannel,
                  "REFUSED PSXPORT_PRIMRGB=\"{}\": expected R,G,B[,frame[,tolerance]] with 0-255 channels. "
                  "NOTHING was censused.",
                  setting);
    return false;
  }
  mRed = red;
  mGreen = green;
  mBlue = blue;
  mFromFrame = frame;
  mTolerance = tolerance;
  mEnabled = true;
  lucent::info(kChannel,
               "census armed: rgb=({},{},{}) tolerance=+-{} from frame {}",
               mRed,
               mGreen,
               mBlue,
               mTolerance,
               mFromFrame);
  return true;
}

int PrimColorCensus::matchingVertex(const RqItem &item) const {
  const int vertexCount = item.nv ? item.nv : 4;
  for (int i = 0; i < vertexCount; ++i) {
    if (std::abs((int)item.rs[i] - mRed) <= mTolerance && std::abs((int)item.gs[i] - mGreen) <= mTolerance &&
        std::abs((int)item.bs[i] - mBlue) <= mTolerance) {
      return i;
    }
  }
  return -1;
}

void PrimColorCensus::reportFrame() const {
  lucent::info(kChannel,
               "f{} scanned {} prim(s), {} carried rgb({},{},{}) +-{}",
               mFrame,
               mScanned,
               mMatched,
               mRed,
               mGreen,
               mBlue,
               mTolerance);
}

void PrimColorCensus::observe(GpuState &gpu, const RqItem &item) {
  if (!configure()) {
    return;
  }
  if (gpu.s_frame < mFromFrame) {
    return;
  }
  if (mFrame != gpu.s_frame) {
    if (mFrame >= 0) {
      reportFrame();
    }
    mFrame = gpu.s_frame;
    mScanned = 0;
    mMatched = 0;
  }
  ++mScanned;
  // The MATCHING vertex, not vertex 0: a Gouraud prim can carry the searched colour on any corner, and
  // reporting vertex 0 made every match look like a false positive against the colour that was asked for.
  const int vertex = matchingVertex(item);
  if (vertex < 0) {
    return;
  }
  ++mMatched;
  int minX = item.xs[0], minY = item.ys[0], maxX = item.xs[0], maxY = item.ys[0];
  const int vertexCount = item.nv ? item.nv : 4;
  for (int i = 1; i < vertexCount; ++i) {
    minX = item.xs[i] < minX ? item.xs[i] : minX;
    minY = item.ys[i] < minY ? item.ys[i] : minY;
    maxX = item.xs[i] > maxX ? item.xs[i] : maxX;
    maxY = item.ys[i] > maxY ? item.ys[i] : maxY;
  }
  lucent::info(kChannel,
               "f{} MATCH seq={} painter={:08X} node={:08X} layer={} om={} mode={} nv={} semi={} "
               "v{}=({},{},{}) bbox=({},{})-({},{}) display_bbox=({},{})-({},{}) depth0={:.6f}",
               mFrame,
               item.seq,
               (uint32_t)item.painter_object,
               item.dbg_node,
               item.layer,
               item.order_mode,
               item.mode,
               item.nv,
               item.semi,
               vertex,
               item.rs[vertex],
               item.gs[vertex],
               item.bs[vertex],
               minX,
               minY,
               maxX,
               maxY,
               minX - gpu.s_disp_x,
               minY - gpu.s_disp_y,
               maxX - gpu.s_disp_x,
               maxY - gpu.s_disp_y,
               item.depth[0]);
}
