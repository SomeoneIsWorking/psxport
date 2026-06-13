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
  int32_t xform_id[4];          // -1 if a vertex didn't join (CPU-projected/2D)
  int16_t lx[4], ly[4], lz[4];  // local (model-space) coords from the join
};

uint32_t s_cr[32];          // live GTE control-register snapshot
int32_t s_cur_xform = -1;   // current transform group index

std::vector<Xform> s_xforms;                      // this segment's transforms
std::vector<Xform> s_xforms_prev;                 // previous segment's transforms
std::unordered_map<uint32_t, VertRef> s_sxy_map;  // this frame: (sx<<16|sy) -> src
std::unordered_map<uint32_t, VertRef> s_sxy_prev; // previous frame's map
std::vector<Poly> s_polys;

// The synthesized in-between display list (frame A's polys with GTE-object
// vertices reprojected at the lerped transform; non-GTE verts left untouched =
// they show frame A's position = no flicker). Produced at each flip from the
// just-finished frame's geometry + the next logic frame's poses. This is what
// the present stage will rasterize between the two real frames.
std::vector<Poly> s_inbetween;
bool s_inbetween_ready = false;

// Nearest-TR object-identity match: object world translations (TR) are distinct
// per entity and continuous across logic frames, so the nearest TR in the next
// segment is the same object. Beyond this L1 gate => no match (pool-slot reuse /
// new object) => the vertex snaps to frame A rather than lerping to a wrong pose.
constexpr int64_t kMatchGateL1 = 8192; // TR is 1/4096 fixed; 8192 = 2.0 world units

// per-frame stats
unsigned s_stat_polys = 0, s_stat_verts = 0, s_stat_joined = 0, s_stat_joined_prev = 0;
unsigned s_unj_tex = 0, s_unj_flat = 0;
long long s_unj_tex_area = 0, s_unj_flat_area = 0;

inline uint32_t SxyKey(int32_t sx, int32_t sy)
{
  return ((uint32_t)(sx & 0xFFFF) << 16) | (uint32_t)(sy & 0xFFFF);
}

// GP0 vertex coordinates are an 11-bit signed field (the GPU only reads 11
// bits, like beetle's sign_x_to_s32(11, ...)); the upper bits of the packet
// word are not part of the coordinate. Extract them the same way so they match
// the GTE SXY (also clamped to [-1024,1023]).
inline int16_t Coord11(uint32_t w)
{
  int32_t v = w & 0x7FF;
  if (v & 0x400)
    v -= 0x800;
  return (int16_t)v;
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

unsigned s_rtp_count = 0;    // projected vertices this segment
unsigned s_near_unjoined = 0; // unjoined verts with an RTP within +-4px (nudge?)
unsigned s_mvmva_count = 0;

// --- faithful GTE perspective divide + screen map (ported from gte.c, verified
// bit-exact in tools/reproject.py). Used to finish the terrain projection that
// the game splits MVMVA(GTE) + divide(CPU). -------------------------------
uint8_t s_divtable[0x101];
bool s_divtable_init = false;
void InitDivTable()
{
  for (uint32_t divisor = 0x8000; divisor < 0x10000; divisor += 0x80)
  {
    uint32_t xa = 512;
    for (int i = 1; i < 5; i++)
      xa = (xa * (1024 * 512 - ((divisor >> 7) * xa))) >> 18;
    s_divtable[(divisor >> 7) & 0xFF] = ((xa + 1) >> 1) - 0x101;
  }
  s_divtable[0x100] = s_divtable[0xFF];
  s_divtable_init = true;
}
int32_t CalcRecip(uint16_t divisor)
{
  int32_t x = 0x101 + s_divtable[((divisor & 0x7FFF) + 0x40) >> 7];
  int32_t tmp = (((int32_t)divisor * -x) + 0x80) >> 8;
  return ((x * (131072 + tmp)) + 0x80) >> 8;
}
uint32_t GteDivide(uint32_t H, uint32_t Z)
{
  if (Z * 2 > H)
  {
    const int shift = (Z & 0xFFFF) ? (__builtin_clz(Z & 0xFFFF) - 16) : 16;
    const uint32_t dividend = (H << shift) & 0xFFFFFFFF;
    const uint32_t divisor = (Z << shift) & 0xFFFFFFFF;
    uint32_t r = (uint32_t)(((uint64_t)dividend * CalcRecip(divisor | 0x8000) + 32768) >> 16);
    return r > 0x1FFFF ? 0x1FFFF : r;
  }
  return 0x1FFFF;
}
inline int32_t SatI(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

// Project a view-space point (MAC1/2/3 = R*V+TR) to screen, exactly as RTPS's
// divide + screen-map, using the given OFX/OFY/H.
bool ProjectFromMac(int64_t mac1, int64_t mac2, int64_t mac3, int32_t ofx, int32_t ofy, uint16_t H, int32_t& sx,
                    int32_t& sy)
{
  const int32_t sz = SatI((int32_t)mac3, 0, 65535);
  if (sz == 0)
    return false;
  const int32_t ir1 = SatI((int32_t)mac1, -32768, 32767);
  const int32_t ir2 = SatI((int32_t)mac2, -32768, 32767);
  const int64_t hds = GteDivide(H, sz);
  sx = SatI((int32_t)((ofx + ir1 * hds) >> 16), -1024, 1023);
  sy = SatI((int32_t)((ofy + ir2 * hds) >> 16), -1024, 1023);
  return true;
}

// Full RTPS reprojection of a local (model-space) vertex through transform xf:
// view = R*V + TR (>>12), then perspective divide + screen map. This is the core
// renderer op — at the captured transform it must reproduce the game's SXY; at an
// interpolated transform it yields the in-between position.
bool ReprojectRTPS(const Xform& xf, int32_t lx, int32_t ly, int32_t lz, int32_t& sx, int32_t& sy)
{
  const int64_t m1 = (((int64_t)xf.TR[0]) << 12) + (int64_t)xf.R[0] * lx + (int64_t)xf.R[1] * ly + (int64_t)xf.R[2] * lz;
  const int64_t m2 = (((int64_t)xf.TR[1]) << 12) + (int64_t)xf.R[3] * lx + (int64_t)xf.R[4] * ly + (int64_t)xf.R[5] * lz;
  const int64_t m3 = (((int64_t)xf.TR[2]) << 12) + (int64_t)xf.R[6] * lx + (int64_t)xf.R[7] * ly + (int64_t)xf.R[8] * lz;
  return ProjectFromMac(m1 >> 12, m2 >> 12, m3 >> 12, xf.ofx, xf.ofy, xf.H, sx, sy);
}

// Lerp one transform toward another at t in [0,1]. R is componentwise-lerped
// (over half a logic frame the rotation delta is tiny; the small loss of
// orthonormality is negligible and self-corrects at the next real frame). TR is
// linear. Screen params (OFX/OFY/H) are taken from A (they don't move per frame).
Xform LerpXform(const Xform& a, const Xform& b, double t)
{
  Xform o = a;
  for (int i = 0; i < 9; i++)
    o.R[i] = (int16_t)((double)a.R[i] + ((double)b.R[i] - (double)a.R[i]) * t);
  for (int i = 0; i < 3; i++)
    o.TR[i] = (int32_t)((double)a.TR[i] + ((double)b.TR[i] - (double)a.TR[i]) * t);
  return o;
}

// Match each previous-segment transform (the pose that produced the just-drawn
// frame) to its counterpart in the current segment (the next logic frame's pose)
// by nearest TR. out[prev_id] = cur_id, or -1 if nothing within the gate.
void MatchXforms(const std::vector<Xform>& prev, const std::vector<Xform>& cur, std::vector<int>& out)
{
  out.assign(prev.size(), -1);
  for (size_t i = 0; i < prev.size(); i++)
  {
    int64_t best = kMatchGateL1 + 1;
    int bestj = -1;
    for (size_t j = 0; j < cur.size(); j++)
    {
      const int64_t d = llabs((int64_t)prev[i].TR[0] - cur[j].TR[0]) + llabs((int64_t)prev[i].TR[1] - cur[j].TR[1]) +
                        llabs((int64_t)prev[i].TR[2] - cur[j].TR[2]);
      if (d < best)
      {
        best = d;
        bestj = (int)j;
      }
    }
    if (best <= kMatchGateL1)
      out[i] = bestj;
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
  s_rtp_count++;
}

// NEGATIVE RESULT (2026-06-13): Tomba 2's MVMVA ops are NOT terrain vertex
// projection — they are lighting/normal transforms. Reprojecting their results
// as positions yields garbage (mac1==0 -> sx==OFX center; off-screen Y), and
// adding them to the join map did not improve coverage. So the terrain is
// genuinely projected by pure-CPU MIPS math (transform AND divide), not via any
// GTE op we can reproject. We keep the tap for counting only; it does NOT feed
// the join. (ProjectViewSpace below is still used/verified for RTPS geometry.)
void OnMvmva(int32_t /*vx*/, int32_t /*vy*/, int32_t /*vz*/, int32_t /*mac1*/, int32_t /*mac2*/, int32_t /*mac3*/)
{
  s_mvmva_count++;
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
    p.x[i] = Coord11(xy);
    p.y[i] = Coord11(xy >> 16);
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
    // Join to transform by screen coords. The game projects one flip-segment
    // ahead of drawing, so this segment's draws correspond to the PREVIOUS
    // segment's projections (s_sxy_prev).
    const uint32_t key = SxyKey(p.x[i], p.y[i]);
    s_stat_verts++;
    auto it = s_sxy_prev.find(key);
    p.lx[i] = p.ly[i] = p.lz[i] = 0;
    if (it != s_sxy_prev.end())
    {
      p.xform_id[i] = it->second.xform_id; // NB: indexes the previous segment
      p.lx[i] = it->second.lx;
      p.ly[i] = it->second.ly;
      p.lz[i] = it->second.lz;
      s_stat_joined++;
    }
    else
    {
      p.xform_id[i] = -1;
      if (s_sxy_map.find(key) != s_sxy_map.end())
        s_stat_joined_prev++; // same-segment fallback count (diagnostic)
      // near-match scan: is there a projected vertex within +-4px (would mean
      // the game nudges projected coords, vs. the vertex never being projected)?
      for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
          if (s_sxy_prev.find(SxyKey(p.x[i] + dx, p.y[i] + dy)) != s_sxy_prev.end())
          {
            s_near_unjoined++;
            dx = dy = 100; // break out
          }
    }
  }
  s_stat_polys++;
  s_polys.push_back(p);

  // Diagnostic: classify unjoined polys (no vertex matched a projection). Is
  // the unjoined remainder 2D UI (small/untextured) or CPU-projected terrain
  // (large/textured)? Determines whether the RTPS-coverage cap matters.
  bool any_joined = false;
  for (int i = 0; i < nv; i++)
    any_joined |= (p.xform_id[i] >= 0);
  if (!any_joined)
  {
    int16_t minx = p.x[0], maxx = p.x[0], miny = p.y[0], maxy = p.y[0];
    for (int i = 1; i < nv; i++)
    {
      minx = p.x[i] < minx ? p.x[i] : minx;
      maxx = p.x[i] > maxx ? p.x[i] : maxx;
      miny = p.y[i] < miny ? p.y[i] : miny;
      maxy = p.y[i] > maxy ? p.y[i] : maxy;
    }
    const int area = (maxx - minx) * (maxy - miny);
    if (textured)
      s_unj_tex++, s_unj_tex_area += area;
    else
      s_unj_flat++, s_unj_flat_area += area;
  }
}

// Display flip = frame boundary. The just-finished segment's draws (s_polys,
// already joined to the previous segment's projections) form one rendered
// frame. Roll the projection window forward: previous <- current.
unsigned s_flip_count = 0;

void OnFlip(uint32_t /*value*/)
{
  if (s_verbose && s_stat_verts)
  {
    const unsigned unjoined = s_stat_verts - s_stat_joined;
    fprintf(stderr,
            "[flip %6u] verts=%u joined=%u (%.0f%%) rtp=%u unjoined=%u near4px=%u(%.0f%%) | unjTexPolys=%u\n",
            s_flip_count, s_stat_verts, s_stat_joined, s_stat_verts ? 100.0 * s_stat_joined / s_stat_verts : 0.0,
            s_rtp_count, unjoined, s_near_unjoined, unjoined ? 100.0 * s_near_unjoined / unjoined : 0.0, s_unj_tex);
  }
  // one-shot GTE opcode histogram on a 3D-content flip (RE: how is terrain
  // transformed? RTPS/RTPT vs MVMVA vs none).
  static bool op_dumped = false;
  if (s_verbose && !op_dumped && s_stat_verts > 3000)
  {
    op_dumped = true;
    fprintf(stderr, "  GTE op histogram (nonzero):");
    for (int op = 0; op < 64; op++)
      if (psxport_gte_op[op])
        fprintf(stderr, " %02X=%u", op, psxport_gte_op[op]);
    fprintf(stderr, "\n  (0x01=RTPS 0x30=RTPT 0x06=NCLIP 0x12/0x13=MVMVA/NCDS 0x2D/2E=AVSZ)\n");
  }

  // Verify the core renderer op: reproject each joined object vertex from its
  // (local coords, transform) and confirm it reproduces the game's captured SXY.
  // s_polys = this segment's draws; their xform_id indexes s_xforms_prev (the
  // segment whose projections they joined against).
  if (s_verbose && !s_xforms_prev.empty() && s_stat_joined > 1000)
  {
    unsigned checked = 0, exact = 0;
    for (const Poly& p : s_polys)
      for (int i = 0; i < p.nverts; i++)
        if (p.xform_id[i] >= 0 && p.xform_id[i] < (int)s_xforms_prev.size())
        {
          int32_t rx, ry;
          if (ReprojectRTPS(s_xforms_prev[p.xform_id[i]], p.lx[i], p.ly[i], p.lz[i], rx, ry))
          {
            checked++;
            if (rx == p.x[i] && ry == p.y[i])
              exact++;
          }
        }
    static bool once = false;
    if (!once && checked)
    {
      once = true;
      fprintf(stderr, "  REPROJECT-VERIFY: %u/%u object verts reproduced exactly (%.1f%%)\n", exact, checked,
              100.0 * exact / checked);
    }
  }

  // --- object-identity matching + in-between synthesis --------------------
  // The just-drawn frame (s_polys) was posed by s_xforms_prev; s_xforms is the
  // next logic frame's pose of the same objects. Match them, then synthesize
  // the t=0.5 frame: reproject each joined object vertex at the lerped pose;
  // leave unjoined verts (CPU terrain / 2D UI) at frame-A coords (no flicker).
  s_inbetween.clear();
  s_inbetween_ready = false;
  if (!s_xforms_prev.empty() && !s_xforms.empty() && !s_polys.empty())
  {
    std::vector<int> match;
    MatchXforms(s_xforms_prev, s_xforms, match);

    // verify the match endpoint: a joined vertex reprojected at its matched
    // NEXT-frame pose must land where the game itself projected that object
    // next frame (s_sxy_map = this segment's projections). High hit-rate =
    // correct identity match => the t=0.5 in-between is trustworthy.
    unsigned mchk = 0, mhit = 0;
    for (const Poly& p : s_polys)
    {
      Poly q = p;
      for (int i = 0; i < p.nverts; i++)
      {
        const int id = p.xform_id[i];
        const int mid = (id >= 0 && id < (int)match.size()) ? match[id] : -1;
        if (mid < 0)
          continue; // unmatched/2D/terrain: keep frame-A coords (snap, no lerp)

        int32_t bx, by;
        if (ReprojectRTPS(s_xforms[mid], p.lx[i], p.ly[i], p.lz[i], bx, by))
        {
          mchk++;
          // does the game's next-frame projection contain this position (±2px)?
          bool hit = false;
          for (int dy = -2; dy <= 2 && !hit; dy++)
            for (int dx = -2; dx <= 2 && !hit; dx++)
              hit = s_sxy_map.find(SxyKey(bx + dx, by + dy)) != s_sxy_map.end();
          mhit += hit;
        }
        int32_t hx, hy;
        if (ReprojectRTPS(LerpXform(s_xforms_prev[id], s_xforms[mid], 0.5), p.lx[i], p.ly[i], p.lz[i], hx, hy))
        {
          q.x[i] = (int16_t)hx;
          q.y[i] = (int16_t)hy;
        }
      }
      s_inbetween.push_back(q);
    }
    s_inbetween_ready = true;

    if (s_verbose)
    {
      static bool once_m = false;
      if (!once_m && mchk > 500)
      {
        once_m = true;
        unsigned matched = 0;
        for (int m : match)
          matched += (m >= 0);
        fprintf(stderr, "  MATCH-VERIFY: %u/%u xforms matched; %u/%u next-frame verts within 2px (%.1f%%)\n",
                matched, (unsigned)match.size(), mhit, mchk, mchk ? 100.0 * mhit / mchk : 0.0);
      }
    }
  }

  s_flip_count++;
  // roll the windows: this segment's projections + transforms become 'previous'
  s_sxy_prev.swap(s_sxy_map);
  s_xforms_prev.swap(s_xforms);
  s_sxy_map.clear();
  s_xforms.clear();
  s_polys.clear();
  s_cur_xform = -1;
  s_stat_polys = s_stat_verts = s_stat_joined = s_stat_joined_prev = 0;
  s_unj_tex = s_unj_flat = 0;
  s_unj_tex_area = s_unj_flat_area = 0;
  s_rtp_count = s_near_unjoined = s_mvmva_count = 0;
}

} // namespace

void Wide60_Install()
{
  s_verbose = std::getenv("PSXPORT_WIDE60_LOG") != nullptr;
  std::memset(s_cr, 0, sizeof(s_cr));
  if (!s_divtable_init)
    InitDivTable();
  psxport_set_gte_cr_hook(OnGteCr);
  psxport_set_rtp_hook(OnRtpVertex);
  psxport_set_mvmva_hook(OnMvmva);
  psxport_set_gpu_poly_hook(OnGpuPoly);
  psxport_set_gpu_flip_hook(OnFlip);
}

void Wide60_FrameEnd(unsigned /*frame*/)
{
  // Segmentation now happens on the GPU flip (OnFlip); nothing to do per host
  // frame yet. Reserved for presenting the synthesized in-between frame.
}
