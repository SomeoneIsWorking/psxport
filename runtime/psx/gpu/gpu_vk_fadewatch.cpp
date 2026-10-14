#include "gpu_vk_fadewatch.h"

#include "gpu_vk_device.h"
#include "legacy_game_hooks.h"

#include <lucent/log.h>

#include <cstdint>

void gpu_vk_fadewatch_present(const FadeState &fade, int sx, int sy, int w, int h) {
  static const lucent::Channel fadewatch_ch{"fadewatch"};
  if (!fadewatch_ch) { // guards the change-detector state below, not a logging call
    return;
  }
  GpuDevice &gd = *GpuDevice::sInstance;
  if (fade.mode == gd.s_fw_lastmode && fade.r == gd.s_fw_lr && fade.g == gd.s_fw_lg && fade.b == gd.s_fw_lb &&
      sx == gd.s_fw_lsx && sy == gd.s_fw_lsy && w == gd.s_fw_lw && h == gd.s_fw_lh) {
    return;
  }
  gd.s_fw_lastmode = fade.mode;
  gd.s_fw_lr = fade.r;
  gd.s_fw_lg = fade.g;
  gd.s_fw_lb = fade.b;
  gd.s_fw_lsx = sx;
  gd.s_fw_lsy = sy;
  gd.s_fw_lw = w;
  gd.s_fw_lh = h;
  lucent::debug("fadewatch",
                "present disp={},{} {}x{} fade mode={} rgb=({},{},{})",
                sx,
                sy,
                w,
                h,
                fade.mode,
                fade.r,
                fade.g,
                fade.b);
}
