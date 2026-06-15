// Authentic boot: run the disc's boot stub (SCUS_944.54) as the real entry point.
//
// The REAL PSX boot is: BIOS -> boot executable (the SCUS stub, NOT MAIN.EXE) -> stub draws the
// SCEA "Sony Computer Entertainment America Presents" screen itself, then BIOS-LoadExec's
// cdrom:\MAIN.EXE;1 and jumps to MAIN's entry (0x800896E0). The port previously skipped the stub
// (loading MAIN directly), so there was no SCEA; a fake native_fmv "intro" stood in for it. See
// docs/journal.md "later 34" + memory [[psxport-scea-boot-stub]].
//
// The stub is NOT recompiled (only MAIN.EXE is), so we INTERPRET it (rec_interp) straight from
// g_ram, using the same runtime (mem/gpu/hle) as recompiled code. The stub carries its own copy
// of the PSY-Q libs (libgpu/libcd/libetc) at its own addresses (0x80018xxx..) — distinct from
// MAIN.EXE's. The native GPU renders the stub's draws (it pokes the same GP0/GP1/DMA the renderer
// parses). The stub's blocking libcd/libetc waits (which spin on CD/VBlank IRQs we don't deliver)
// are converted to native non-stalls by overrides registered here, mirroring what cd_override.c /
// timing.c / sync_overrides.c do for the MAIN.EXE copies.
//
// Hand-off: the stub's LoadExec(cdrom:\MAIN.EXE;1, ...) is intercepted (hle.c g_loadexec_hook):
// we load MAIN.EXE into RAM and longjmp out of the stub interpreter, then enter the native MAIN
// boot (native_boot.c), which takes over for Whoopee/OP/menu (later 33).
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifdef PSXPORT_SDL
#include <SDL.h>   // SDL_GetTicks/SDL_Delay — pace the SCEA stub to real 60 Hz when windowed
#endif

// The boot stub is recompiled as its OWN module (emit.py --stub: STUB_NAMES) with distinct
// dispatch/override symbols, since it overlaps MAIN.EXE's address space. stub_dispatch runs a
// stub function by address (hybrid: misses fall to the interpreter on the stub's RAM bytes);
// stub_set_override installs a native body for a recompiled stub function (CD/etc.).
void stub_dispatch(R3000* c, uint32_t addr);
void stub_set_override(uint32_t addr, OverrideFn fn);
void native_boot_run(R3000* c);
void interp_trace_open(const char* path);
extern void (*g_loadexec_hook)(R3000*);

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }

// Load a PS-X EXE image into g_ram and set the initial register file (gp/sp/fp/ra), matching the
// BIOS loader. Returns the entry PC. (Same contract as boot.c's load_exe; duplicated here so the
// stub path is self-contained — the stub overwrites MAIN's low text, and the MAIN reload at
// hand-off restores it, exactly as the real boot does.)
static uint32_t load_exe_image(const char* path, R3000* c) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* buf = malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(1); }
  fclose(f);
  uint32_t entry = rd32(buf+0x10), gp = rd32(buf+0x14);
  uint32_t load = rd32(buf+0x18), tsize = rd32(buf+0x1C), sp = rd32(buf+0x30);
  memcpy(&g_ram[load & 0x1FFFFF], buf + 0x800, tsize);
  free(buf);
  c->r[28] = gp;
  c->r[29] = sp ? sp : 0x801FFFF0u;
  c->r[30] = c->r[29];
  c->r[31] = 0xDEAD0000u;               // top-level return sentinel
  fprintf(stderr, "[stub] loaded %s: entry 0x%08X load 0x%08X text 0x%X sp 0x%08X\n",
          path, entry, load, tsize, c->r[29]);
  return entry;
}

static jmp_buf      g_stub_exit;        // longjmp target = the setjmp in native_stub_run
static const char*  g_main_path;        // MAIN.EXE path (reloaded at LoadExec hand-off)
static R3000*       g_boot_ctx;         // the boot R3000 (so the hook can reload MAIN into it)

// LoadExec(cdrom:\MAIN.EXE;1, stackbase, stackoffset) interceptor. The real LoadExec loads the
// named EXE and jumps to it (never returns). We instead reload MAIN.EXE (restoring the low text
// the stub overwrote) and bail out of the stub interpreter; native_stub_run then enters the
// native MAIN boot. a0 = filename ptr (logged for sanity).
static void ov_loadexec(R3000* c) {
  uint32_t name = c->r[4];
  char fn[64]; int i = 0;
  for (; i < 63; i++) { uint8_t ch = mem_r8(name + i); if (!ch) break; fn[i] = (char)ch; }
  fn[i] = 0;
  fprintf(stderr, "[stub] LoadExec(\"%s\") -> hand off to native MAIN boot\n", fn);
  load_exe_image(g_main_path, g_boot_ctx);     // reload MAIN.EXE image + registers
  longjmp(g_stub_exit, 1);
}

// --- Stub libcd overrides ------------------------------------------------------------------
// The boot stub carries its own PSY-Q libcd at 0x80019xxx-0x8001Axxx (distinct from MAIN.EXE's,
// which cd_override.c/sync_overrides.c handle). Its blocking primitives spin in a status poll
// with a timer-based timeout ("CD timeout: CD_cw(...)") waiting on a CD IRQ we never deliver.
// We replace them with native non-stalls, mirroring the MAIN.EXE overrides 1:1:
//   * CD_cw (0x8001A0C0, cmd,param,result,...) — the command-word engine. Real body sends the
//     command and waits for the controller ack/IRQ. Override: report success (v0=0), zero the
//     8 result/status bytes the caller copies (a2). Every libcd command (Init/Setloc/Setmode/
//     ReadN/Pause/Nop) funnels here, so this single override unblocks init + control. DATA is
//     served separately by the native read primitive (added once located).
//   * CdSync (0x80019B78, mode,result) — return 2 (status: complete), zero result(a1).
//   * CdDataSync (0x8001A944, mode) — DMA-done wait — return 0 (already done).
enum { V0_ = 2, A0_ = 4, A1_ = 5, A2_ = 6 };
static void zero_result8(uint32_t p) { if (p) for (int i = 0; i < 8; i++) mem_w8(p + i, 0); }
static void ov_stub_cd_cw(R3000* c)    { zero_result8(c->r[A2_]); c->r[V0_] = 0; }
static void ov_stub_cdsync(R3000* c)   { zero_result8(c->r[A1_]); c->r[V0_] = 2; }
static void ov_stub_cddatasync(R3000* c) { c->r[V0_] = 0; }

// --- Stub libetc VSync ---------------------------------------------------------------------
// The stub's public VSync(mode) (0x80017E4C) measures frame timing off hardware we don't model
// (GPUSTAT 0x1F801814, Timer1 0x1F801110) and waits on the VBlank IRQ count — none of which
// advance, so its fades/holds spin and it polls GPUSTAT forever. Replace the whole function with
// the native frame model (mirrors timing.c's ov_vsync for MAIN.EXE):
//   mode > 0  -> wait `mode` frames;  mode == 0 -> wait one frame
//   mode < 0  -> "query current count" — but the stub's timed holds BUSY-POLL VSync(-1) expecting
//                the VBlank IRQ to advance the count in the background (e.g. SCEA state 9 loops
//                until VSync(-1) > start+3). We deliver no preemptive IRQ, so in this cooperative
//                model EVERY VSync call ticks one frame: a query-poll loop then makes progress and
//                keeps presenting. (A query advancing one frame is harmless to elapsed measurements
//                and is exactly what lets those wait loops terminate.)
// Each call advances the native frame clock, delivers the VBlank events, and PRESENTS one frame
// (the stub has drawn + flipped its display) — the heartbeat that makes SCEA visible and its timed
// hold/fade progress. DAT_800267B4 is the count SCEA also reads directly.
#define STUB_VBLANK_COUNT 0x800267B4u
void gpu_present(void);
void hle_deliver_event(uint32_t ev_class, uint32_t spec);
static uint32_t g_stub_vblank;
static void ov_stub_vsync(R3000* c) {
  int32_t mode = (int32_t)c->r[A0_];
  if (getenv("PSXPORT_VSYNCLOG")) fprintf(stderr, "[vsync] mode=%d count=%u\n", mode, g_stub_vblank);
  void watchdog_pet(void);
  hle_deliver_event(0xF2000003u, 0xFFFFFFFFu);         // VBlank event classes (RCnt3 / libapi)
  hle_deliver_event(0xF0000001u, 0xFFFFFFFFu);

  // PRESENT only on a real frame-wait (mode>=0), NOT on busy-poll queries (VSync(-1)). The SCEA
  // stub's per-frame body is: VSync(0) [frame boundary] -> clear framebuffer -> busy-poll VSync(-1)
  // (timing/CD) -> draw -> loop. Presenting on the polls flashes the just-cleared (black) buffer
  // between complete frames (the SCEA blink, journal later-46). Real HW refreshes the display only
  // at the VBlank frame boundary, never mid-draw.
  static int windowed = -1;
  if (windowed < 0) {
    const char* w = getenv("PSXPORT_GPU_WINDOW");      // run.sh sets "0" headless, "1" windowed
    windowed = (w && atoi(w) != 0 && !getenv("PSXPORT_NOPACE")) ? 1 : 0;
  }

  // SCEA skip: pressing Start jumps straight to the MAIN.EXE hand-off (skipping the fades), exactly
  // as ov_loadexec does. PSXPORT_SCEA_SKIP forces it (headless test / always-skip). Start is PSX
  // button bit 0x0008, active-low (0 == pressed) in the host pad mask.
  static int skip_dis = -1; if (skip_dis < 0) skip_dis = getenv("PSXPORT_NOSKIP") ? 1 : 0;
  if (!skip_dis) {
    uint16_t pad_buttons(void);
#ifdef PSXPORT_SDL
    if (windowed) { void pad_poll_sdl(void); pad_poll_sdl(); }   // refresh host input
#endif
    if (getenv("PSXPORT_SCEA_SKIP") || (pad_buttons() & 0x0008u) == 0) {
      fprintf(stderr, "[stub] SCEA skipped (Start) -> hand off to MAIN\n");
      load_exe_image(g_main_path, g_boot_ctx);
      longjmp(g_stub_exit, 1);
    }
  }
#ifdef PSXPORT_SDL
  if (windowed) {
    // Real-vblank model (matches hardware): the VSync count advances at 60 Hz of REAL time via the
    // VBlank IRQ, independent of how often the stub polls. Deriving the count from a per-call
    // increment (headless path below) inflates it by the busy-poll rate, so SCEA's count-timed fades
    // and holds run far too fast (journal later-48). Here the count tracks elapsed real time, and a
    // frame-wait sleeps to the next 60 Hz boundary before presenting — authentic fade/hold speed.
    static double t0 = -1, next = 0;
    double now = (double)SDL_GetTicks();
    if (t0 < 0) { t0 = now; next = now; }
    if (mode >= 0) {                                   // frame-wait: pace to the vblank, then present
      int frames = (mode > 0) ? mode : 1;
      next += frames * 1000.0 / 60.0;
      if (next > now) { SDL_Delay((unsigned)(next - now)); now = (double)SDL_GetTicks(); }
      else if (now - next > 1000.0) next = now;        // resync after a long stall (no debt)
      gpu_present();                                   // show the completed frame (pets watchdog)
    } else {
      watchdog_pet();                                  // poll: no present, no artificial count tick
    }
    g_stub_vblank = (uint32_t)((now - t0) * 60.0 / 1000.0);   // count == real vblanks elapsed
    mem_w32(STUB_VBLANK_COUNT, g_stub_vblank);
    c->r[V0_] = g_stub_vblank;
    return;
  }
#endif
  // Headless / unpaced: advance per call (fast) so tests run at full speed; present on frame-waits.
  g_stub_vblank += (mode > 0) ? (uint32_t)mode : 1u;
  if (mode >= 0) gpu_present();
  else watchdog_pet();
  mem_w32(STUB_VBLANK_COUNT, g_stub_vblank);
  c->r[V0_] = g_stub_vblank;
}

// --- Stub CdRead: make SCEA do NO CD reads ----------------------------------------------------
// The SCEA stub's CdRead (0x8001BA64) preloads CD data the screen doesn't actually need — we play
// the intro FMVs natively and LoadExec MAIN from file, so no real CD stream reaches the stub. Its
// caller (0x8001B89C) retries while the CD-status word (CD struct @0x80026BF8, +20 = 0x80026C0C) is
// negative — producing the endless "CdRead: retry..." spam. Replace CdRead with a no-op that writes
// a SUCCESS status (a positive value, exactly as the original success path stores the VSync count)
// so the caller's `bgez` sees success and stops retrying; SCEA then just ends and cuts to the LOGO
// FMV. (User-requested: "make SCEA not do CD reads.")
#define STUB_CD_STATUS 0x80026C0Cu
static void ov_stub_cdread(R3000* c) {
  mem_w32(STUB_CD_STATUS, g_stub_vblank ? g_stub_vblank : 1u);
  c->r[V0_] = mem_r32(STUB_CD_STATUS);
}

// Register a stub override on BOTH the recompiled path (stub_set_override, function-index keyed —
// fires when the fn is reached via a direct call) and the interpreter path (rec_set_interp_override,
// raw-address keyed — fires when reached via a function pointer / non-recompiled caller). One of
// the two always applies regardless of how libcd is entered.
static void stub_override(uint32_t addr, OverrideFn fn) {
  stub_set_override(addr, fn);
  rec_set_interp_override(addr, fn);
}
static void stub_cd_overrides_init(void) {
  stub_override(0x8001A0C0u, ov_stub_cd_cw);      // CD_cw (command engine; entry precedes prologue)
  stub_override(0x80019B78u, ov_stub_cdsync);     // CdSync
  stub_override(0x8001A944u, ov_stub_cddatasync); // CdDataSync
  stub_override(0x80017E4Cu, ov_stub_vsync);      // libetc public VSync(mode)
  stub_override(0x8001BA64u, ov_stub_cdread);     // CdRead -> no-op success (no CD reads in SCEA)
}

void native_stub_run(R3000* c, const char* main_exe_path) {
  g_main_path = main_exe_path;
  g_boot_ctx  = c;
  g_loadexec_hook = ov_loadexec;
  stub_cd_overrides_init();
  interp_trace_open(getenv("PSXPORT_INTERP_TRACE"));

  // The stub (the disc's boot executable) lives next to MAIN.EXE — same directory, name
  // SCUS_944.54 (extracted by run.sh via `discdump get`). It loads to 0x80010000 (over MAIN's low
  // text), entry 0x80018B6C; the MAIN reload at LoadExec restores that text, as the real boot does.
  char stub[512];
  const char* slash = strrchr(main_exe_path, '/');
  int dirlen = slash ? (int)(slash - main_exe_path) + 1 : 0;
  snprintf(stub, sizeof stub, "%.*s%s", dirlen, main_exe_path, "SCUS_944.54");
  uint32_t entry = load_exe_image(stub, c);

  if (setjmp(g_stub_exit) == 0) {
    fprintf(stderr, "[stub] running boot stub from entry 0x%08X (draws SCEA, loads MAIN)\n", entry);
    stub_dispatch(c, entry);                     // runs SCEA + LoadExec; ov_loadexec longjmps out
    fprintf(stderr, "[stub] stub returned without LoadExec (unexpected) — booting MAIN directly\n");
    load_exe_image(g_main_path, c);
  }
  g_loadexec_hook = 0;
  fprintf(stderr, "[stub] entering native MAIN boot\n");
  native_boot_run(c);
}
