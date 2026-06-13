// Tomba! 2 (SCUS-94454) game module. RE notes live in patches/tomba2/.

#include "tomba2.h"

#include "../psxport_hooks.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint32_t s_render_hits = 0;

// Host "skip intro" input (Start), latched each frame by the frontend so the
// PC-native intro overrides below can see it from inside the interpreter.
bool s_skip_held = false;
// Latched skip-mode for the inter-FMV "Whoopee Camp logo" hold. A single Start
// *tap* during the hold is worthless if FmvDwellSkip only fires while Start is
// physically held (measured: -8f vs -121f for held Start; see
// docs/tomba2-fmv-skip.md). Latch on a Start press so one tap collapses the hold,
// and clear it when FMV#2's prebuffer is satisfied (FmvSkipClear) so the movie
// itself plays at native rate instead of being fast-forwarded.
bool s_fmv_skip_latch = false;
// Main RAM, stashed each frame from Tomba2_FrameTick so an override can read or
// write game state (e.g. mark a logo's "done" flag) without the emulator path.
uint8_t* s_ram = nullptr;
bool s_introskip = false; // intro skip installed (default on; PSXPORT_T2_NOINTROSKIP opts out)

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

// --- Native intro skip (PC-owned intro progression; RE: docs/tomba2-intro.md) -
// The intro is straight-line blocking code in the sequencer 0x800111B4 (verified:
// no data-driven stage var exists). Each logo is shown by a function that is
// entered once and BLOCKS in an internal per-VSync loop until its own end
// condition; the pad is never polled, so the stock game can't skip them (only
// the later FMV polls Start). These two overrides give the PC ownership of the
// *advance decision* for each logo: when Start is held, satisfy the logo's OWN
// exit path one step early. The emulated code still does all display/load — we
// only own "when does the hold end", which is why this is a true native skip and
// not emulator fast-forward, and why it can't race the asset loaders (the logo
// is already displayed by the time its hold loop runs).

// SCEA license (0x80010D54): a per-frame loop dispatching on phase $s5 (GPR[21]):
//   0 = fade-in, 1 = hold(180 frames), 2 = fade-out, 3 = done -> the function
// cleans up and returns (jr $ra @0x800111AC), and the sequencer advances. The
// dispatch beq chain starts at 0x80010ED0 ("beq $s5,$v0,..."), reached once per
// frame. On Start we set $s5=3 so the very next dispatch jumps straight to the
// done/return path -- skipping the hold AND the fade-out directly (not just
// ending the hold). The function's entry setup already ran, so done's cleanup is
// the legitimate terminal path.
int SceaSkip(uint32_t, uint32_t* gpr, uint32_t*)
{
  if (s_skip_held)
    gpr[21] = 3; // $s5 = done phase
  return PSXPORT_HOOK_CONTINUE;
}

// Whoopee Camp (0x8001138C): an animation player whose internal loop
// 0x80011414..0x80011528 runs one displayed frame per iteration and exits when
// *(0x800253EC)==1 (set by 0x800118C0 when the decode position passes frame
// 250). 0x80011414 ("lui $v0,0x8004") is the loop top, reached once per frame.
// On Start we take the function's own exit: mark *(0x800253EC)=1 (the natural
// "animation done" flag) and redirect to the epilogue 0x80011530, returning v0=1
// so the caller's poll loop (0x800112BC) advances to the opening FMV.
int WhoopeeSkip(uint32_t pc, uint32_t* gpr, uint32_t* redirect_pc)
{
  if (!s_skip_held)
    return PSXPORT_HOOK_CONTINUE;
  if (s_ram)
  {
    const uint32_t done = 1;
    memcpy(s_ram + 0x253EC, &done, 4); // the loop's own terminator flag
  }
  gpr[2] = 1; // v0 = "done" so the caller's `while(!v0)` poll loop exits
  // Resume at the function epilogue (restores ra/s0/s1 and returns), in the same
  // memory region the code runs in so the I-cache tag is unchanged.
  *redirect_pc = 0x11530 | (psxport_last_pc & 0xE0000000);
  return PSXPORT_HOOK_REDIRECT;
}

// Inter-stage timed dwell collapse. The load/transition state machine 0x80011a78
// (keyed on *(0x80025454)) blocks state 0xE for a PURE 200-frame dwell: handler
// 0x80012148 waits until frame_counter > *(0x80038498)+0xC8, displaying BLACK --
// dead time, not a load (instant-CD does not shrink it; verified black 0,0,0
// across the whole f321->f522 window). At 0x80012164 ("beqz $v1, stay") $v1 =
// (saved+200 < counter). We set $v1=1 so the branch is not taken and the handler
// runs its OWN advance path (sets 0x80025444=5, calls 0x80012584, advances to
// state 0xF) -- skipping only the wait.
//
// UNCONDITIONAL (not Start-gated): this is the "big wait from Whoopee to FMV"
// the user reported. It is pure black dead-time with no content, so on a PC port
// there is no reason to ever sit through it -- "instant on PC". Gating it on
// held-Start was wrong: a single Start tap at the Whoopee logo isn't still held
// ~3s later when this dwell runs, so the wait returned. The logos themselves
// (SceaSkip/WhoopeeSkip, which show actual content) stay Start-gated.
int DwellSkip(uint32_t, uint32_t* gpr, uint32_t*)
{
  gpr[3] = 1; // $v1 != 0 -> dwell treated as elapsed, take the advance path
  return PSXPORT_HOOK_CONTINUE;
}

// Post-Whoopee FMV-stage frame-pacing dwell. In a loaded overlay at 0x80050xxx
// the FMV/post-Whoopee loop frame-paces itself with a spin:
// `while (*(0x800E809C) < threshold@0x1F800235)` at 0x80050CE4
//   80050CE4 lhu  v0,-0x7f64(a0)   ; a0=0x800f0000 -> *(0x800E809C) vblank counter
//   80050CEC sltu v0,v0,v1         ; v1 = threshold
//   80050CF0 bnez v0,0x80050CE4    ; stay while counter < threshold
// Redirect to the loop exit 0x80050CF8 so the wait elapses immediately (same
// technique as DwellSkip; the loop's own work still runs each iteration). Escaping
// the per-frame pace dwell every frame lets the read/decode loop iterate faster,
// accelerating the prebuffer of FMV#2 (measured -121f to FMV#2 ReadS, glitch-free;
// docs/tomba2-fmv-skip.md). Fires on a LATCH (s_fmv_skip_latch, set on a Start
// press in Tomba2_SetSkipHeld) so a brief tap is enough, not a continuous hold; the
// latch is cleared at the prebuffer-satisfied branch (FmvSkipClear) so FMV#2 plays
// at native rate rather than being fast-forwarded.
int FmvDwellSkip(uint32_t pc, uint32_t* gpr, uint32_t* redirect_pc)
{
  if (s_skip_held)
    s_fmv_skip_latch = true; // a press latches skip-mode (survives a tap)
  if (!s_fmv_skip_latch)
    return PSXPORT_HOOK_CONTINUE;
  *redirect_pc = 0x50CF8 | (psxport_last_pc & 0xE0000000); // loop exit
  return PSXPORT_HOOK_REDIRECT;
}

// Prebuffer-satisfied branch target (0x8008A7B8, the `bnez $v1,0x8008a7b8` taken at
// 0x8008A784 when the buffered ring position passes the prebuffer target). This is
// the most direct "prebuffer done -> FMV#2 starting" signal, so we drop the
// dwell-skip latch here: the still-logo hold is over and the movie should now run at
// its native XA/MDEC rate (NOT fast-forwarded). Pure observer -- no register/PC
// change -- so it cannot perturb the player. Signature lui $a0,0x8002 gates it to
// the resident f839 player overlay.
int FmvSkipClear(uint32_t, uint32_t*, uint32_t*)
{
  s_fmv_skip_latch = false;
  return PSXPORT_HOOK_CONTINUE;
}
} // namespace

void Tomba2_SetSkipHeld(bool held)
{
  s_skip_held = held;
  if (held)
    s_fmv_skip_latch = true; // latch on any Start press so a brief tap collapses the FMV hold
}

void Tomba2_Install()
{
  // Heartbeat: per-object draw dispatch entry (RE'd from the live game;
  // overlay code, hence the instruction signature: addiu sp,sp,-0x20).
  // Nonzero hits per frame = the in-engine render loop is alive.
  psxport_add_hook(0x8003CCA4, 0x27BDFFE0, RenderEntryHeartbeat);

  // Cull-cone widening override (replaces the PSXPORT_POKE patch).
  // OFF by default: forcing draw of culled objects makes them render without
  // the engine ever setting up their collision/logic — they blink at the
  // widened boundary and can be walked through. The six slti sites are not all
  // confirmed to be the same cull test, so widening them corrupts gameplay.
  // Re-enable for RE with PSXPORT_T2_CULLWIDEN=1 once the sites are understood.
  if (std::getenv("PSXPORT_T2_CULLWIDEN") != nullptr)
  {
    for (const CullSite& s : kCullSites)
      psxport_add_hook(s.pc, s.instr, CullSlti);
  }

  // Native intro skip: PC owns the SCEA + Whoopee advance decision (Start).
  // Default ON (the hooks are inert unless Start is held); opt out with
  // PSXPORT_T2_NOINTROSKIP. Signature-gated to the resident intro code.
  s_introskip = std::getenv("PSXPORT_T2_NOINTROSKIP") == nullptr;
  if (s_introskip)
  {
    psxport_add_hook(0x80010ED0, 0x12A20019, SceaSkip);    // SCEA phase dispatch (beq $s5,$v0)
    psxport_add_hook(0x80011414, 0x3C028004, WhoopeeSkip); // Whoopee loop top (lui $v0,0x8004)
    psxport_add_hook(0x80012164, 0x10600008, DwellSkip);   // inter-stage 200f dwell (beqz $v1)
    psxport_add_hook(0x80050CE4, 0x9482809C, FmvDwellSkip); // post-Whoopee FMV-stage frame dwell
    psxport_add_hook(0x8008A7B8, 0x3C040280, FmvSkipClear); // prebuffer satisfied -> drop FMV dwell latch
  }

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
  s_ram = ram; // available to overrides that need to read/write game state
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

