#include "gpu_primitive_dump.h"

#include "cfg.h"
#include "core.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_native_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <lucent/log.h>

// A range is deliberate: 30 Hz games can emit primitives on only half of their 60 Hz presents, so
// arming one frame is a coin flip. The finish hook always reports either a written row denominator or
// an explicit zero; absence of a CSV is never allowed to masquerade as an empty scene measurement.
FILE *GpuState::primdump_open(int frame) {
  if (s_primdump_frame == -2) {
    const char *setting = cfg_str("PSXPORT_PRIMDUMP");
    s_primdump_frame = -1;
    s_primdump_end = -1;
    if (setting) {
      int begin = 0;
      int end = 0;
      if (std::sscanf(setting, "%d:%d", &begin, &end) == 2 || std::sscanf(setting, "%d-%d", &begin, &end) == 2) {
        s_primdump_frame = begin;
        s_primdump_end = end;
      } else {
        s_primdump_frame = std::atoi(setting);
        s_primdump_end = s_primdump_frame;
      }
    }
    lucent::info("primdump",
                 "armed frames {}..{} (PSXPORT_PRIMDUMP={})",
                 s_primdump_frame,
                 s_primdump_end,
                 setting ? setting : "<unset>");
  }
  if (s_primdump_frame < 0 || frame < s_primdump_frame || frame > s_primdump_end) {
    return nullptr;
  }

  ++s_primdump_seen;
  if (!s_primdump_f) {
    char path[128];
    std::snprintf(path, sizeof(path), "scratch/logs/prims_f%d.csv", s_primdump_frame);
    if (!Fs::ensureParentDirs(path)) {
      lucent::error("primdump", "cannot create the parent directory of {} — NOTHING written", path);
      return nullptr;
    }
    s_primdump_f = std::fopen(path, "w");
    if (!s_primdump_f) {
      lucent::error("primdump", "cannot open {} — NOTHING written", path);
      return nullptr;
    }
    std::fprintf(s_primdump_f,
                 "frame,id,kind,op,is3d,bg,x0,y0,x1,y1,r,g,b,tex,semi,"
                 "mode,raw,tpx,tpy,clutx,cluty,twmx,twmy,twox,twoy,u0,v0,umin,umax,vmin,vmax,"
                 "dax0,day0,dax1,day1,offx,offy\n");
  }
  return s_primdump_f;
}

namespace {

// These columns distinguish atlas addressing from clipping/draw-area faults. Untextured primitives
// emit -1 UVs because those slots were never authored and would otherwise expose stack garbage as data.
void write_texture_columns(FILE *file,
                           const GpuState &gpu,
                           int textured,
                           const int *textureU,
                           const int *textureV,
                           int count,
                           int rawTexture) {
  if (!textured) {
    std::fprintf(file,
                 ",3,%d,%d,%d,%d,%d,%d,%d,%d,%d,-1,-1,-1,-1,-1,-1,%d,%d,%d,%d,%d,%d\n",
                 rawTexture,
                 gpu.s_tp_x,
                 gpu.s_tp_y,
                 gpu.s_clut_x,
                 gpu.s_clut_y,
                 gpu.s_tw_mx,
                 gpu.s_tw_my,
                 gpu.s_tw_ox,
                 gpu.s_tw_oy,
                 gpu.s_da_x0,
                 gpu.s_da_y0,
                 gpu.s_da_x1,
                 gpu.s_da_y1,
                 gpu.s_off_x,
                 gpu.s_off_y);
    return;
  }

  const int u0 = count > 0 ? textureU[0] : 0;
  const int v0 = count > 0 ? textureV[0] : 0;
  int umin = u0;
  int umax = u0;
  int vmin = v0;
  int vmax = v0;
  for (int i = 1; i < count; ++i) {
    umin = std::min(umin, textureU[i]);
    umax = std::max(umax, textureU[i]);
    vmin = std::min(vmin, textureV[i]);
    vmax = std::max(vmax, textureV[i]);
  }
  std::fprintf(file,
               ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
               gpu.s_tp_mode,
               rawTexture,
               gpu.s_tp_x,
               gpu.s_tp_y,
               gpu.s_clut_x,
               gpu.s_clut_y,
               gpu.s_tw_mx,
               gpu.s_tw_my,
               gpu.s_tw_ox,
               gpu.s_tw_oy,
               u0,
               v0,
               umin,
               umax,
               vmin,
               vmax,
               gpu.s_da_x0,
               gpu.s_da_y0,
               gpu.s_da_x1,
               gpu.s_da_y1,
               gpu.s_off_x,
               gpu.s_off_y);
}

} // namespace

void gpu_primitive_dump_polygon(Core *core,
                                int frame,
                                unsigned id,
                                uint8_t op,
                                int vertexCount,
                                int is3d,
                                int background,
                                const int *screenX,
                                const int *screenY,
                                const int *textureU,
                                const int *textureV,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue,
                                int textured,
                                int semitransparent,
                                int rawTexture) {
  FILE *file = core->game->gpu.primdump_open(frame);
  if (!file) {
    return;
  }

  int x0 = screenX[0];
  int y0 = screenY[0];
  int x1 = screenX[0];
  int y1 = screenY[0];
  for (int i = 1; i < vertexCount; ++i) {
    x0 = std::min(x0, screenX[i]);
    x1 = std::max(x1, screenX[i]);
    y0 = std::min(y0, screenY[i]);
    y1 = std::max(y1, screenY[i]);
  }
  std::fprintf(file,
               "%d,%u,poly,%02X,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
               frame,
               id,
               op,
               is3d,
               background,
               x0,
               y0,
               x1,
               y1,
               red,
               green,
               blue,
               textured,
               semitransparent);
  write_texture_columns(file, core->game->gpu, textured, textureU, textureV, vertexCount, rawTexture);
}

void gpu_primitive_dump_sprite(Core *core,
                               int frame,
                               unsigned id,
                               uint8_t op,
                               int x,
                               int y,
                               int width,
                               int height,
                               int background,
                               uint8_t red,
                               uint8_t green,
                               uint8_t blue,
                               int textured,
                               int semitransparent,
                               int textureU,
                               int textureV,
                               int rawTexture) {
  FILE *file = core->game->gpu.primdump_open(frame);
  if (!file) {
    return;
  }
  std::fprintf(file,
               "%d,%u,sprite,%02X,0,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
               frame,
               id,
               op,
               background,
               x,
               y,
               x + width,
               y + height,
               red,
               green,
               blue,
               textured,
               semitransparent);
  const int textureUs[2] = {textureU, textureU + (width > 0 ? width - 1 : 0)};
  const int textureVs[2] = {textureV, textureV + (height > 0 ? height - 1 : 0)};
  write_texture_columns(file, core->game->gpu, textured, textureUs, textureVs, 2, rawTexture);
}

void gpu_primitive_dump_finish_frame(Core *core, int frame) {
  GpuState &gpu = core->game->gpu;
  if (gpu.s_primdump_frame < 0 || gpu.s_primdump_reported || frame <= gpu.s_primdump_end) {
    return;
  }
  gpu.s_primdump_reported = 1;
  if (gpu.s_primdump_f) {
    std::fclose(gpu.s_primdump_f);
    gpu.s_primdump_f = nullptr;
    lucent::info("primdump",
                 "wrote scratch/logs/prims_f{}.csv — {} prims over frames {}..{}",
                 gpu.s_primdump_frame,
                 gpu.s_primdump_seen,
                 gpu.s_primdump_frame,
                 gpu.s_primdump_end);
    return;
  }
  lucent::warn("primdump",
               "frames {}..{} passed with ZERO prims offered — no CSV written. The game "
               "draws at 30 Hz (half the presents emit nothing); widen the window (PSXPORT_PRIMDUMP=a:b).",
               gpu.s_primdump_frame,
               gpu.s_primdump_end);
}
