#include "queue_row_probe.h"

#include "cfg.h"
#include "gpu_native_internal.h"
#include "render_queue.h"

#include <cstdio>
#include <lucent/log.h>
#include <string>

namespace {
constexpr const char *kChannel = "qrow";
} // namespace

bool QueueRowProbe::configure() {
  if (mConfigured) {
    return mEnabled;
  }
  mConfigured = true;
  const char *setting = cfg_str("PSXPORT_QROW");
  if (!setting || !*setting) {
    return false;
  }
  int y = -1, x0 = -1, x1 = -1, frame = 0;
  const int fields = std::sscanf(setting, "%d,%d,%d,%d", &y, &x0, &x1, &frame);
  const bool wellFormed = fields >= 3 && y >= 0 && x0 >= 0 && x1 >= x0 && (x1 - x0) < kMaxSpan && frame >= 0;
  if (!wellFormed) {
    lucent::error(kChannel,
                  "REFUSED PSXPORT_QROW=\"{}\": expected y,x0,x1[,frame] with 0 <= x0 <= x1, span < {}. "
                  "NO ROW WAS PROBED.",
                  setting,
                  kMaxSpan);
    return false;
  }
  mY = y;
  mX0 = x0;
  mX1 = x1;
  mFromFrame = frame;
  mEnabled = true;
  lucent::info(kChannel, "row probe armed: y={} x={}..{} from frame {}", mY, mX0, mX1, mFromFrame);
  return true;
}

void QueueRowProbe::reportFrame() const {
  std::string strip;
  std::string owners;
  int covered = 0;
  for (int x = mX0; x <= mX1; ++x) {
    const Pixel &pixel = mPixels[(size_t)(x - mX0)];
    char cell[32];
    if (!pixel.covered) {
      // NOT "black": nothing in the queue covered this pixel at all. The two answers look identical
      // in a rendered image and must not look identical here.
      strip += "------ ";
      continue;
    }
    ++covered;
    std::snprintf(cell, sizeof cell, "%02X%02X%02X ", pixel.r & 0xff, pixel.g & 0xff, pixel.b & 0xff);
    strip += cell;
    std::snprintf(cell, sizeof cell, "%u@%08X ", pixel.seq, pixel.painter);
    owners += cell;
  }
  lucent::info(kChannel,
               "f{} y={} x={}..{} scanned {} prim(s), {} of {} pixel(s) covered\n  colours: {}\n  owners: {}",
               mFrame,
               mY,
               mX0,
               mX1,
               mScanned,
               covered,
               mX1 - mX0 + 1,
               strip,
               owners);
}

void QueueRowProbe::observe(GpuState &gpu, const RqItem &item) {
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
    mPixels.fill(Pixel{});
  }
  ++mScanned;
  const int absoluteY = gpu.s_disp_y + mY;
  for (int x = mX0; x <= mX1; ++x) {
    const RqPixelSample sample = rq_probe_item_pixel(gpu, item, gpu.s_disp_x + x, absoluteY);
    if (!sample.covered || !sample.writes) {
      continue;
    }
    Pixel &pixel = mPixels[(size_t)(x - mX0)];
    if (pixel.covered && sample.interpolated_depth < pixel.d32) {
      continue;
    }
    pixel.covered = true;
    pixel.d32 = sample.interpolated_depth;
    pixel.seq = item.seq;
    pixel.painter = (uint32_t)item.painter_object;
    pixel.r = sample.shaded_r;
    pixel.g = sample.shaded_g;
    pixel.b = sample.shaded_b;
  }
}
