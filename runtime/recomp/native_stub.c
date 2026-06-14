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
// No VBlank IRQ is delivered, so the stub's VSync count DAT_800267B4 never advances and its
// vblank WAIT (0x80017FC4: spin until count >= a0, else "VSync: timeout") never completes. Mirror
// timing.c for MAIN: satisfy the wait by advancing the native frame clock to the requested count,
// then deliver the VBlank events and PRESENT one displayed frame (the stub draws + flips display
// per vblank; the native GPU shows it). This is the boot stub's frame heartbeat — where SCEA
// becomes visible. DAT_800267B4 is the libetc VSync counter the public VSync(mode) reads back.
#define STUB_VBLANK_COUNT 0x800267B4u
void gpu_present(void);
void hle_deliver_event(uint32_t ev_class, uint32_t spec);
static void ov_stub_vsyncwait(R3000* c) {
  uint32_t target = c->r[A0_];                       // wait until VSync count >= target
  uint32_t cur = mem_r32(STUB_VBLANK_COUNT);
  if ((int32_t)(cur - target) < 0) cur = target;     // reach the target (the IRQ would have)
  else cur += 1;                                      // already past: still advance one frame
  mem_w32(STUB_VBLANK_COUNT, cur);
  hle_deliver_event(0xF2000003u, 0xFFFFFFFFu);        // VBlank event classes (RCnt3 / libapi)
  hle_deliver_event(0xF0000001u, 0xFFFFFFFFu);
  gpu_present();                                      // show this frame (pets the watchdog)
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
  stub_override(0x80017FC4u, ov_stub_vsyncwait);  // libetc VSync vblank-count wait
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
