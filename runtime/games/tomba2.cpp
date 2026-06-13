// Tomba! 2 (SCUS-94454) game module. RE notes live in patches/tomba2/.

#include "tomba2.h"

#include "../psxport_hooks.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint32_t s_render_hits = 0;

int RenderEntryHeartbeat(uint32_t, uint32_t*, uint32_t*)
{
  s_render_hits++;
  return PSXPORT_HOOK_CONTINUE;
}

// --- live object capture (RE: patches/tomba2/objects.md) ---------------------
// Every live drawable object funnels through the cull/LOD dispatcher
// 0x8007712C once per logic frame with a0 = its struct pointer. Hooking it
// enumerates the whole live object set; the pointer is the object identity.
constexpr unsigned kMaxObjs = 1024;
uint32_t s_obj_ptr[kMaxObjs];
unsigned s_obj_n = 0;
bool s_objlog = false;

int ObjectCull(uint32_t, uint32_t* gpr, uint32_t*)
{
  if (s_obj_n < kMaxObjs)
    s_obj_ptr[s_obj_n++] = gpr[4]; // a0 = object*
  return PSXPORT_HOOK_CONTINUE;
}

// --- GTE transform capture (camera/object transform fed to RTPS) -------------
// CR0-4 pack the 3x3 rotation matrix (s16, 1.0=0x1000); CR5-7 = translation
// TRX/TRY/TRZ (s32). The game loads a transform then projects; TRZ (which==7)
// is the natural "transform complete" marker. We assemble and count these.
uint32_t s_gte_cr[32]; // 0-7 = rot matrix + TR; 24/25/26 = OFX/OFY/H
unsigned s_gte_xforms = 0; // transforms (TRZ writes) this frame
bool s_gtelog = false;
FILE* s_rtpdump = nullptr; // PSXPORT_T2_RTPDUMP: (transform,V)->SXY tuples

void GteCrConsumer(unsigned which, uint32_t value)
{
  if (which < 32)
    s_gte_cr[which] = value;
  if (which == 7) // translation complete = one loaded transform
  {
    if (s_gtelog && s_gte_xforms < 6)
    {
      const int16_t r11 = s_gte_cr[0], r12 = s_gte_cr[0] >> 16, r13 = s_gte_cr[1], r21 = s_gte_cr[1] >> 16;
      const int16_t r22 = s_gte_cr[2], r23 = s_gte_cr[2] >> 16, r31 = s_gte_cr[3], r32 = s_gte_cr[3] >> 16;
      const int16_t r33 = s_gte_cr[4];
      fprintf(stderr, "[%6u] xform#%u TR=(%d,%d,%d) R=[%d %d %d; %d %d %d; %d %d %d]\n", psxport_frame, s_gte_xforms,
              (int32_t)s_gte_cr[5], (int32_t)s_gte_cr[6], (int32_t)s_gte_cr[7], r11, r12, r13, r21, r22, r23, r31, r32,
              r33);
    }
    s_gte_xforms++;
  }
}

// Dump (full transform, input local vertex) -> game output SXY, one row per
// projected vertex, for offline verification of our reprojection RTPS.
void RtpConsumer(int32_t vx, int32_t vy, int32_t vz, int32_t sx, int32_t sy, uint32_t sf)
{
  if (!s_rtpdump)
    return;
  const int16_t r11 = s_gte_cr[0], r12 = s_gte_cr[0] >> 16, r13 = s_gte_cr[1], r21 = s_gte_cr[1] >> 16;
  const int16_t r22 = s_gte_cr[2], r23 = s_gte_cr[2] >> 16, r31 = s_gte_cr[3], r32 = s_gte_cr[3] >> 16;
  const int16_t r33 = s_gte_cr[4];
  fprintf(s_rtpdump, "%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,%d,%d,%d\n", psxport_frame, r11, r12,
          r13, r21, r22, r23, r31, r32, r33, (int32_t)s_gte_cr[5], (int32_t)s_gte_cr[6], (int32_t)s_gte_cr[7],
          (int32_t)s_gte_cr[24], (int32_t)s_gte_cr[25], (uint16_t)s_gte_cr[26], sf, vx, vy, vz, sx, sy);
}

// --- Object cull-cone widening (override; RE: patches/tomba2/cull-widen.md) --
// The object enqueue-for-draw overlay tests v1 (cos-scaled dot/distance)
// against a per-LOD threshold with `slti v0, v1, IMM` (v1 < IMM => culled).
// The stock thresholds cull objects still partly visible at the screen edge,
// worse under the widescreen FOV. We override each slti site: compute the
// comparison against a widened (~0.72x) threshold natively, write v0, and
// redirect past the original instruction. RAM is never modified — the stock
// `slti` stays resident and diffable (this is an override, not the old poke),
// and the instruction-word signature ensures we only fire when this overlay
// (not some other code) is mapped at the address.
struct CullSite
{
  uint32_t pc;
  uint32_t instr;   // original `slti v0,v1,OLD` word (overlay signature)
  int32_t new_imm;  // widened threshold (~0.72 * OLD), signed
};
const CullSite kCullSites[] = {
  {0x800772D4, 0x28620370, 0x278}, // 0x370 -> 0x278
  {0x80077368, 0x28620358, 0x268}, // 0x358 -> 0x268
  {0x80077414, 0x28620358, 0x268}, // 0x358 -> 0x268
  {0x800774A8, 0x28620370, 0x278}, // 0x370 -> 0x278
  {0x8007753C, 0x28620350, 0x260}, // 0x350 -> 0x260
  {0x800775D0, 0x28620368, 0x270}, // 0x368 -> 0x270
};

int CullSlti(uint32_t pc, uint32_t* gpr, uint32_t* redirect_pc)
{
  // pc arrives region-masked (0x1FFFFFFF); the registry masks site PCs too.
  for (const CullSite& s : kCullSites)
  {
    if ((s.pc & 0x1FFFFFFF) == pc)
    {
      // slti v0, v1, new_imm  (signed; v0=GPR[2], v1=GPR[3])
      gpr[2] = (static_cast<int32_t>(gpr[3]) < s.new_imm) ? 1u : 0u;
      // Resume one instruction past the stock slti, in the SAME memory region
      // the code is executing in (KSEG0/KUSEG) so the I-cache tag is unchanged.
      *redirect_pc = (pc + 4) | (psxport_last_pc & 0xE0000000);
      return PSXPORT_HOOK_REDIRECT;
    }
  }
  return PSXPORT_HOOK_CONTINUE;
}
} // namespace

void Tomba2_Install()
{
  // Heartbeat: per-object draw dispatch entry (RE'd from the live game;
  // overlay code, hence the instruction signature: addiu sp,sp,-0x20).
  // Nonzero hits per frame = the in-engine render loop is alive.
  psxport_add_hook(0x8003CCA4, 0x27BDFFE0, RenderEntryHeartbeat);

  // Cull-cone widening override (replaces the PSXPORT_POKE patch).
  for (const CullSite& s : kCullSites)
    psxport_add_hook(s.pc, s.instr, CullSlti);

  // Live object enumeration: hook the universal per-object cull chokepoint.
  s_objlog = std::getenv("PSXPORT_T2_OBJLOG") != nullptr;
  psxport_add_hook(0x8007712C, 0x00051400, ObjectCull);

  // GTE transform capture (camera + object transforms used for projection).
  s_gtelog = std::getenv("PSXPORT_T2_GTELOG") != nullptr;
  if (const char* rtp = std::getenv("PSXPORT_T2_RTPDUMP"))
  {
    s_rtpdump = std::fopen(rtp, "w");
    if (s_rtpdump)
      std::fprintf(s_rtpdump, "frame,r11,r12,r13,r21,r22,r23,r31,r32,r33,trx,try,trz,ofx,ofy,h,sf,vx,vy,vz,sx,sy\n");
    psxport_set_rtp_hook(RtpConsumer);
  }
  if (s_gtelog || s_rtpdump || std::getenv("PSXPORT_T2_GTE") != nullptr)
    psxport_set_gte_cr_hook(GteCrConsumer);
}

uint32_t Tomba2_GetAndResetRenderHits()
{
  const uint32_t v = s_render_hits;
  s_render_hits = 0;
  return v;
}

uint16_t Tomba2_FrameTick(uint8_t* ram)
{
  // s_obj_ptr now holds the objects submitted during the previous retro_run.
  if (s_objlog && s_obj_n)
  {
    fprintf(stderr, "[%6u] objs n=%u:", psxport_frame, s_obj_n);
    for (unsigned i = 0; i < s_obj_n; i++)
    {
      const uint32_t p = s_obj_ptr[i] & 0x1FFFFF;
      int16_t x = 0, y = 0, z = 0;
      uint8_t type = 0;
      if (p + 0x38 < 0x200000)
      {
        memcpy(&x, ram + p + 0x2E, 2);
        memcpy(&y, ram + p + 0x32, 2);
        memcpy(&z, ram + p + 0x36, 2);
        type = ram[p + 0x0C];
      }
      fprintf(stderr, " %08X[t%u](%d,%d,%d)", s_obj_ptr[i], type, x, y, z);
    }
    fprintf(stderr, "\n");
  }
  if (s_gtelog && s_gte_xforms)
    fprintf(stderr, "[%6u] gte xforms=%u\n", psxport_frame, s_gte_xforms);
  s_obj_n = 0;
  s_gte_xforms = 0;
  return 0;
}

bool Tomba2_WantTurbo(const uint8_t* ram, unsigned frame)
{
  // The license text and Whoopee Camp logo are LOAD MASKS, not skippable
  // segments (verified: X has no effect; the segment-end code is absent from
  // RAM until the loader finishes, and the logo then runs out its jingle).
  // Turbo instead, while either holds:
  //  - the main game overlay is not yet resident (0x8005082C empty during
  //    BIOS/license/early logo, game code from ~frame 1989), or
  //  - the logo stream clock at 0x8011824C is still ticking (it advances
  //    every frame during the logo segment and freezes at its end; verified
  //    frozen during the in-engine intro cutscene).
  // Frame cap as a safety net against later overlay swaps / streams.
  if (frame > 6000)
    return false;
  uint32_t overlay, clock;
  memcpy(&overlay, ram + 0x5082C, 4);
  memcpy(&clock, ram + 0x11824C, 4);
  static uint32_t last_clock = 0;
  const bool ticking = (clock != last_clock);
  last_clock = clock;
  return overlay == 0 || ticking;
}
