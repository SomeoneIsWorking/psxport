// wide60 reprojecting renderer — capture layer (see wide60.h).
//
// This first stage builds the per-frame "draw item" list the renderer needs:
// each polygon vertex joined to the GTE transform + local coordinates that
// produced it (via its screen-space SXY, which equals the GP0 packet's xy).
// It verifies the join is complete (coverage %) before any reprojection.

#include "wide60.h"
#include "psxport_hooks.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

bool s_verbose = false;

// A full GTE projection transform (one per object load: TR changes per object,
// the rotation matrix / screen params usually persist).
struct Xform
{
  int16_t R[9];
  int32_t TR[3];
  int32_t ofx, ofy;
  uint16_t H;
  uint8_t sf;
};

// A projected vertex's source: local coords + which transform produced it.
struct VertRef
{
  int16_t lx, ly, lz;
  int32_t xform_id;
};

// A captured polygon ready for reprojection.
struct Poly
{
  uint8_t cc;       // GP0 command (mode bits: gouraud/quad/textured/...)
  uint8_t nverts;   // 3 or 4
  int16_t x[4], y[4];
  uint8_t u[4], v[4];
  uint32_t color[4];
  uint16_t clut, tpage;
  int32_t xform_id[4]; // -1 if a vertex didn't join
};

uint32_t s_cr[32];          // live GTE control-register snapshot
int32_t s_cur_xform = -1;   // current transform group index

std::vector<Xform> s_xforms;
std::unordered_map<uint32_t, VertRef> s_sxy_map;  // this frame: (sx<<16|sy) -> src
std::unordered_map<uint32_t, VertRef> s_sxy_prev; // previous frame's map
std::vector<Poly> s_polys;

// per-frame stats
unsigned s_stat_polys = 0, s_stat_verts = 0, s_stat_joined = 0, s_stat_joined_prev = 0;

inline uint32_t SxyKey(int32_t sx, int32_t sy)
{
  return ((uint32_t)(sx & 0xFFFF) << 16) | (uint32_t)(sy & 0xFFFF);
}

void OnGteCr(unsigned which, uint32_t value)
{
  if (which < 32)
    s_cr[which] = value;
  if (which == 7) // TRZ written last => a transform is fully loaded
  {
    Xform x;
    x.R[0] = s_cr[0];
    x.R[1] = s_cr[0] >> 16;
    x.R[2] = s_cr[1];
    x.R[3] = s_cr[1] >> 16;
    x.R[4] = s_cr[2];
    x.R[5] = s_cr[2] >> 16;
    x.R[6] = s_cr[3];
    x.R[7] = s_cr[3] >> 16;
    x.R[8] = s_cr[4];
    x.TR[0] = (int32_t)s_cr[5];
    x.TR[1] = (int32_t)s_cr[6];
    x.TR[2] = (int32_t)s_cr[7];
    x.ofx = (int32_t)s_cr[24];
    x.ofy = (int32_t)s_cr[25];
    x.H = (uint16_t)s_cr[26];
    x.sf = 12; // RTPS uses sf per-instruction; recorded per-vertex too
    s_cur_xform = (int32_t)s_xforms.size();
    s_xforms.push_back(x);
  }
}

void OnRtpVertex(int32_t vx, int32_t vy, int32_t vz, int32_t sx, int32_t sy, uint32_t /*sf*/)
{
  VertRef ref;
  ref.lx = (int16_t)vx;
  ref.ly = (int16_t)vy;
  ref.lz = (int16_t)vz;
  ref.xform_id = s_cur_xform;
  s_sxy_map[SxyKey(sx, sy)] = ref; // last writer wins on SXY collision
}

// Decode a GP0 polygon packet (cb) into a Poly, joining each vertex to its
// transform via the SXY map. GP0 polygon layout:
//   word0 = (cc<<24) | color0
//   per vertex: [color word if gouraud and i>0] xy-word [uv-word if textured]
//   uv-word: u=lo8, v=next8; vertex0 high16 = CLUT, vertex1 high16 = texpage
void OnGpuPoly(uint32_t cc, const uint32_t* cb, int32_t /*off_x*/, int32_t /*off_y*/)
{
  const bool gouraud = cc & 0x10;
  const bool quad = cc & 0x08;
  const bool textured = cc & 0x04;
  const int nv = quad ? 4 : 3;

  Poly p;
  p.cc = (uint8_t)cc;
  p.nverts = (uint8_t)nv;
  p.clut = p.tpage = 0;

  unsigned w = 0;
  const uint32_t color0 = cb[w] & 0xFFFFFF;
  w++; // consume word0 (command|color0)
  for (int i = 0; i < nv; i++)
  {
    p.color[i] = (gouraud && i > 0) ? (cb[w++] & 0xFFFFFF) : color0;
    const uint32_t xy = cb[w++];
    p.x[i] = (int16_t)(xy & 0xFFFF);
    p.y[i] = (int16_t)(xy >> 16);
    if (textured)
    {
      const uint32_t uv = cb[w++];
      p.u[i] = uv & 0xFF;
      p.v[i] = (uv >> 8) & 0xFF;
      if (i == 0)
        p.clut = uv >> 16;
      else if (i == 1)
        p.tpage = uv >> 16;
    }
    else
    {
      p.u[i] = p.v[i] = 0;
    }
    // join to transform via screen coords (try this frame, then previous)
    const uint32_t key = SxyKey(p.x[i], p.y[i]);
    auto it = s_sxy_map.find(key);
    s_stat_verts++;
    if (it != s_sxy_map.end())
    {
      p.xform_id[i] = it->second.xform_id;
      s_stat_joined++;
    }
    else
    {
      p.xform_id[i] = -1;
      if (s_sxy_prev.find(key) != s_sxy_prev.end())
        s_stat_joined_prev++;
    }
  }
  s_stat_polys++;
  s_polys.push_back(p);
}

} // namespace

void Wide60_Install()
{
  s_verbose = std::getenv("PSXPORT_WIDE60_LOG") != nullptr;
  std::memset(s_cr, 0, sizeof(s_cr));
  psxport_set_gte_cr_hook(OnGteCr);
  psxport_set_rtp_hook(OnRtpVertex);
  psxport_set_gpu_poly_hook(OnGpuPoly);
}

void Wide60_FrameEnd(unsigned frame)
{
  if (s_verbose && s_stat_polys)
  {
    fprintf(stderr, "[%6u] wide60 polys=%u verts=%u cur=%u(%.0f%%) prev=%u(%.0f%%) xforms=%zu map=%zu\n", frame,
            s_stat_polys, s_stat_verts, s_stat_joined, s_stat_verts ? 100.0 * s_stat_joined / s_stat_verts : 0.0,
            s_stat_joined_prev, s_stat_verts ? 100.0 * s_stat_joined_prev / s_stat_verts : 0.0, s_xforms.size(),
            s_sxy_map.size());
  }
  // reset per-frame capture (keep this frame's map as 'previous' for latency
  // join; transform snapshot s_cr persists across frames)
  s_sxy_prev.swap(s_sxy_map);
  s_xforms.clear();
  s_sxy_map.clear();
  s_polys.clear();
  s_cur_xform = -1;
  s_stat_polys = s_stat_verts = s_stat_joined = s_stat_joined_prev = 0;
}
