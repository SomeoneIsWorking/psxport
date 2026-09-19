#include "gpu_vk_fadewatch.h"

#include "core.h"
#include "game.h"
#include "game_hooks_opt.h"
#include "gpu_vk_device.h"

#include <lucent/log.h>

#include <cstdint>

namespace {

// The negative, written before the positive. A tap that reads two guest addresses the title never
// declared would print four plausible zero-ish fields and look like a measurement, so a title with
// no declared drivers says so ONCE and then keeps reporting transitions without the state dump.
void report_blind_once(const Core *core) {
  static bool reported = false; // one line per run, not channel filtering
  if (reported) {
    return;
  }
  reported = true;
  lucent::warn("fadewatch",
               "state tap BLIND — this title declares no fade-driver addresses "
               "(GameConfig::fadewatchDriverStruct/fadewatchFieldModeSmPtr are 0){}",
               core->cfg ? "" : "; it declares no GameConfig at all");
}

} // namespace

void gpu_vk_fadewatch_tap(Core *core, int sx, int sy, int w, int h) {
  static const lucent::Channel fadewatch_state_ch{"fadewatch"};
  if (!fadewatch_state_ch) { // guards the guest reads below, not a logging call
    return;
  }
  GpuDevice &gd = *GpuDevice::sInstance;
  int &lm = gd.s_fws_lastmode;
  uint8_t &lr = gd.s_fws_lr;
  uint8_t &lg = gd.s_fws_lg;
  uint8_t &lb = gd.s_fws_lb;
  int &lsx = gd.s_fws_lsx;
  int &lsy = gd.s_fws_lsy;
  int &lw = gd.s_fws_lw;
  int &lh = gd.s_fws_lh;
  const FadeState f = game_render_fade_state(core, core ? core->hooks : nullptr);
  const int m = f.mode;
  const uint8_t r = f.r;
  const uint8_t g = f.g;
  const uint8_t b = f.b;
  if (m == lm && r == lr && g == lg && b == lb && sx == lsx && sy == lsy && w == lw && h == lh) {
    return;
  }
  lm = m;
  lr = r;
  lg = g;
  lb = b;
  lsx = sx;
  lsy = sy;
  lw = w;
  lh = h;
  const uint32_t driver = core->cfg ? core->cfg->fadewatchDriverStruct : 0u;
  const uint32_t fieldModePtr = core->cfg ? core->cfg->fadewatchFieldModeSmPtr : 0u;
  if (!driver || !fieldModePtr) {
    report_blind_once(core);
    return;
  }
  const uint32_t sm = core->mem_r32(fieldModePtr);
  lucent::debug("fadewatch",
                "[fadewatch-state] P.st={} P8={} P0xA={} P3={} | sm50={} sm6c={}",
                core->mem_r8(driver + 4),
                core->mem_r16(driver + 8),
                core->mem_r16(driver + 0xa),
                core->mem_r8(driver + 3),
                core->mem_r16(sm + 0x50),
                core->mem_r8(sm + 0x6c));
}
