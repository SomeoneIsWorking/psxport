// sbs.cpp — LIVE side-by-side two-core divergence debugger (PSXPORT_SBS=1).
//
// Runs TWO native-boot cores in ONE process, in lockstep, with IDENTICAL input, differing ONLY by MODE:
//   render   (default): A = native gameplay + NATIVE render,  B = native gameplay + PSX render
//   gameplay:           A = native gameplay,  B = PSX gameplay (psx_fallback); render IDENTICAL (PSX) on both
//   both:               A = full native (native gp + native render),  B = full PSX (PSX gp + PSX render)
// Select with PSXPORT_SBS_MODE=render|gameplay|both.
//
// Both cores navigate to the gameplay-START flag INDEPENDENTLY (their load timing differs — FMV/loads), a
// BARRIER (user: "whoever reaches the flag waits for the other"): only once BOTH are at gameplay-start does
// the lockstep begin, so the cores are compared in equal state, never tripping on boot/FMV/load desync.
//
// Each lockstep frame: feed both cores the SAME host input, step both, then DIFF the guest RAM region +
// scratchpad (legit render-only regions excluded). The FIRST divergence PAUSES the loop and is held for
// inspection over the debug server (PSXPORT_DEBUG_SERVER, tools/dbgclient.py):
//   sbs                 status: mode, frame, selected core, divergence summary, watch state
//   sbs diff            the divergence detail (frame, first diverging addr/range, A bytes vs B bytes)
//   sbs bt              guest stack backtrace of BOTH cores at the divergence (frame-boundary)
//   sbs watch           arm a write-watchpoint on the diverging address; on `sbs resume`, the WRITE that
//                       diverges pauses mid-frame with the EXACT guest backtrace of each writing core
//   sbs show a|b        which core the window displays AND which core r/rw/ents/node/scene/etc target
//   sbs resume          unpause;   sbs step [n]   advance n lockstep frames then re-pause
// The window shows the SELECTED core live; drive BOTH with the host keyboard (mirrored automatically since
// each core reads the same host pad each frame). r/rw/ents/... operate on the selected core (use `sbs show`).
//
// Diagnostic, not behavior (one PC-native game ships; this is a debugger). Like dualcore.cpp it owns its
// own Game instances and never returns (the process exits when the window closes).

#include "game.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

// --- runtime entry points reused from the normal boot path / dual-core harness ---
void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
void pad_repl_hold(Core* c, uint16_t active_low_mask);
void pad_repl_tap(Core* c, uint16_t active_low_mask, int n);
void dbg_server_start(Core* c);
void dbg_server_service(Core* c);
int  dbg_is_paused(void);
int  dbg_step_pending(void);
void dbg_consume_step(void);
void dbg_set_paused(int p);
extern "C" void watchdog_suspend(void);
extern "C" void watchdog_disable(void);   // SBS pauses indefinitely on a divergence — kill the frame watchdog
extern "C" void guest_backtrace_to(Core* c, FILE* out);
extern "C" int  g_render_psx;                 // engine_render.cpp — 0 = native render walks, 1 = PSX recomp render
extern void (*g_store_watch_cb)(Core*, uint32_t a, uint32_t v);   // mem.cpp — armed write-watch callback

namespace {

enum Mode { M_RENDER, M_GAMEPLAY, M_BOTH };
constexpr uint32_t GAME_ENTRY  = 0x8010637Cu;  // task0 entry while the GAME stage runs (in the field)
constexpr uint32_t TASK0_ENTRY = 0x801fe00cu;  // task0 obj +0xc = current stage entry
constexpr uint32_t CUT_FLAG    = 0x1F800137u;  // cutscene-active byte (1 = intro cutscene, 0 = free-roam)
constexpr uint16_t BTN_CROSS = 0x4000, BTN_START = 0x0008, BTN_NONE = 0xFFFF;

Game*    g_a = nullptr;
Game*    g_b = nullptr;
int      s_mode = M_RENDER;
int      s_sel  = 0;                            // 0 = A, 1 = B (window + debug-server target core)
uint32_t s_lo   = 0x800B0000u, s_hi = 0x80110000u;
uint32_t s_frame = 0;                           // lockstep frame counter (since gameplay-start barrier)

// GTE state is now PER-INSTANCE (game.h GteRegs; bound per core frame-step in native_step_frame via
// gte_bind) — two cores keep separate GTE registers, so the old save/restore shadow is unnecessary.
// SPU/MDEC follow the same path as they de-globalize.

// --- divergence record (frame-boundary RAM/scratchpad diff) ---
bool     s_div_found = false;
uint32_t s_div_frame = 0, s_div_addr = 0, s_div_end = 0;
char     s_bt_a[4096] = {0}, s_bt_b[4096] = {0};

// --- write-watchpoint record (exact corrupting-write site) ---
bool     s_ww_armed = false;
uint32_t s_ww_addr  = 0;
int      s_ww_hit   = 0;                        // bit0 = A wrote, bit1 = B wrote
uint32_t s_ww_va = 0, s_ww_vb = 0;
char     s_ww_bt_a[4096] = {0}, s_ww_bt_b[4096] = {0};

const char* mode_name() { return s_mode == M_RENDER ? "render" : s_mode == M_GAMEPLAY ? "gameplay" : "both"; }

// Legit render-only guest regions: the native vs PSX render paths write GP0 packets / OT / pool pointers
// here (render mode + both mode). Divergence here is render noise, not the gameplay corruption we hunt.
bool is_render_region(uint32_t a) {
  if (a >= 0x800BF4F0u && a < 0x800BF54Cu) return true;   // pool ptrs + dwell
  if (a >= 0x800BFE68u && a < 0x800EA200u) return true;   // packet pool (×2) + OT (×2) + env
  return false;
}

void cap_bt(Core* c, char* buf, size_t n) {
  buf[0] = 0;
  FILE* f = fmemopen(buf, n, "w");
  if (f) { guest_backtrace_to(c, f); fclose(f); }
}

// 3-phase navigation to gameplay-start, IDENTICAL to native_boot AUTO_SKIP / dualcore: (0) tap Cross until
// the GAME stage, (1) wait for the intro cutscene to begin, (2) pulse Start while the cutscene flag is up,
// finishing once it has read 0 for 60 consecutive frames (the cutscene-end fade settles).
enum Phase { REACH_GAME, AWAIT_CUT, SKIP_CUT, DONE };
struct Nav { Phase phase = REACH_GAME; int idle = 0; };
bool nav_step(Core* c, Nav& nv, uint32_t f, const char* tag) {
  if ((f % 400u) == 0) fprintf(stderr, "[sbs-nav] %s f%u phase=%d stage=%08X cut=%u\n",
                               tag, f, (int)nv.phase, c->mem_r32(TASK0_ENTRY), c->mem_r8(CUT_FLAG));
  uint8_t cut = c->mem_r8(CUT_FLAG);
  switch (nv.phase) {
    case REACH_GAME:
      if (c->mem_r32(TASK0_ENTRY) == GAME_ENTRY) { fprintf(stderr, "[sbs] %s GAME @f%u\n", tag, f); nv.phase = AWAIT_CUT; }
      else if ((f % 12u) == 0) pad_repl_tap(c, (uint16_t)(BTN_NONE & ~BTN_CROSS), 6);
      break;
    case AWAIT_CUT:
      if (cut) { fprintf(stderr, "[sbs] %s cutscene up @f%u\n", tag, f); nv.phase = SKIP_CUT; nv.idle = 0; }
      break;
    case SKIP_CUT:
      if (cut) { nv.idle = 0; if ((f % 40u) == 0) pad_repl_tap(c, (uint16_t)(BTN_NONE & ~BTN_START), 6); }
      else if (++nv.idle >= 60) { fprintf(stderr, "[sbs] %s gameplay-start @f%u\n", tag, f); nv.phase = DONE; return true; }
      break;
    case DONE: return true;
  }
  return false;
}

// Per-core, per-step configuration of the SHARED render/gameplay switches (g_render_psx is a global, so it
// MUST be set right before each core's step). psx_fallback is per-Game, set once at boot.
void apply_mode(int which) {
  switch (s_mode) {
    case M_RENDER:   g_render_psx = which;            break;   // A native render (0), B PSX render (1)
    case M_GAMEPLAY: g_render_psx = 1;                break;   // PSX render on BOTH (isolate gameplay)
    case M_BOTH:     g_render_psx = which;            break;   // A native render, B PSX render
  }
}

void navigate(Game* g, const char* tag) {
  Nav nv;
  g->core.game->diff_mode = 0;                 // present this core's boot to the window
  apply_mode(g == g_a ? 0 : 1);
  for (uint32_t f = 0; f < 6000; f++) {
    dbg_server_service(&g->core);
    if (nav_step(&g->core, nv, f, tag)) return;
    dc_step_frame(&g->core, f);
  }
  fprintf(stderr, "[sbs] %s did NOT reach gameplay-start within 6000 frames\n", tag);
}

// Scratchpad is the 1 KB window 0x1F800000..0x1F8003FF — NOT "addr >= 0x1F800000" (KSEG0 RAM 0x80xxxxxx
// is numerically larger and would be misclassified, indexing the 1 KB scratch[] far out of bounds).
inline bool is_spad(uint32_t a) { return a >= 0x1F800000u && a < 0x1F800400u; }

void record_divergence(uint32_t addr) {
  // coalesce the contiguous differing span starting at addr (read via mem_r8 — safe address mapping)
  bool spad = is_spad(addr);
  uint32_t end_addr = spad ? 0x1F800400u : s_hi;
  uint32_t last = addr, gap = 0;
  for (uint32_t x = addr + 1; x < end_addr && gap < 64; x++) {
    if (g_a->core.mem_r8(x) != g_b->core.mem_r8(x) && (spad || !is_render_region(x))) { last = x; gap = 0; } else gap++;
  }
  s_div_found = true; s_div_frame = s_frame; s_div_addr = addr; s_div_end = last + 1;
  cap_bt(&g_a->core, s_bt_a, sizeof s_bt_a);
  cap_bt(&g_b->core, s_bt_b, sizeof s_bt_b);
  fprintf(stderr, "\n[sbs] *** DIVERGENCE at lockstep frame %u: 0x%08X..0x%08X (mode=%s) ***\n",
          s_frame, s_div_addr, s_div_end, mode_name());
  fprintf(stderr, "[sbs] paused. Inspect over the debug server: `sbs diff`, `sbs bt`, `sbs watch`.\n");
  dbg_set_paused(1);
}

// Frame-boundary diff: report the first non-render-noise divergence in the RAM region, else the scratchpad.
void check_divergence() {
  const uint8_t* a = g_a->core.ram + (s_lo - 0x80000000u);
  const uint8_t* b = g_b->core.ram + (s_lo - 0x80000000u);
  uint32_t n = s_hi - s_lo;
  for (uint32_t i = 0; i < n; i++) if (a[i] != b[i] && !is_render_region(s_lo + i)) { record_divergence(s_lo + i); return; }
  for (uint32_t i = 0; i < 0x400; i++) if (g_a->core.scratch[i] != g_b->core.scratch[i]) { record_divergence(0x1F800000u + i); return; }
}

void step_core(Game* g, int which) {
  g->core.game->diff_mode = (which == s_sel) ? 0 : 1;   // only the selected core presents to the window
  apply_mode(which);
  dc_step_frame(&g->core, s_frame);   // native_step_frame binds THIS core's per-instance GTE (gte_bind)
}

} // namespace

// Write-watch callback (mem.cpp). Fired mid-frame by whichever core writes the armed address; capture that
// core's EXACT guest backtrace + value. We DON'T pause here (mid-frame is unsafe) — the lockstep loop pauses
// after both cores finish the frame, with both write sites captured.
extern "C" void sbs_store_cb(Core* c, uint32_t a, uint32_t v) {
  if (!s_ww_armed || (a & ~3u) != (s_ww_addr & ~3u)) return;
  int which = (g_b && c == &g_b->core) ? 1 : 0;
  if (which) { s_ww_vb = v; cap_bt(c, s_ww_bt_b, sizeof s_ww_bt_b); }
  else       { s_ww_va = v; cap_bt(c, s_ww_bt_a, sizeof s_ww_bt_a); }
  s_ww_hit |= (1 << which);
}

// `sbs ...` debug-server commands. Returns 1 if it handled the line (so dbg_exec stops), 0 otherwise.
int sbs_dbg_cmd(FILE* out, const char* line) {
  char cmd[16] = {0}, sub[32] = {0};
  if (sscanf(line, "%15s", cmd) != 1 || strcmp(cmd, "sbs") != 0) return 0;
  if (!g_a) { fprintf(out, "sbs: harness not running (set PSXPORT_SBS=1)\n"); return 1; }
  sscanf(line, "%*s %31s", sub);

  if (!sub[0] || !strcmp(sub, "status")) {
    fprintf(out, "sbs mode=%s frame=%u selected=%c paused=%d\n", mode_name(), s_frame, s_sel ? 'B' : 'A', dbg_is_paused());
    if (s_div_found) fprintf(out, "  divergence: frame %u 0x%08X..0x%08X\n", s_div_frame, s_div_addr, s_div_end);
    else             fprintf(out, "  divergence: none yet\n");
    if (s_ww_armed)  fprintf(out, "  write-watch ARMED on 0x%08X (hit mask=%d)\n", s_ww_addr, s_ww_hit);
  } else if (!strcmp(sub, "diff")) {
    if (!s_div_found) { fprintf(out, "sbs: no divergence yet\n"); return 1; }
    fprintf(out, "divergence @lockstep-frame %u  0x%08X..0x%08X  in %s\n",
            s_div_frame, s_div_addr, s_div_end, is_spad(s_div_addr) ? "scratchpad" : "main RAM");
    uint32_t end = s_div_end; if (end > s_div_addr + 24) end = s_div_addr + 24;
    fprintf(out, "  A:");
    for (uint32_t x = s_div_addr; x < end; x++) fprintf(out, " %02X", g_a->core.mem_r8(x));
    fprintf(out, "\n  B:");
    for (uint32_t x = s_div_addr; x < end; x++) fprintf(out, " %02X", g_b->core.mem_r8(x));
    fprintf(out, "\n");
  } else if (!strcmp(sub, "bt")) {
    if (!s_div_found) { fprintf(out, "sbs: no divergence yet\n"); return 1; }
    fprintf(out, "== core A backtrace (frame-boundary, @divergence) ==\n%s", s_bt_a);
    fprintf(out, "== core B backtrace (frame-boundary, @divergence) ==\n%s", s_bt_b);
    if (s_ww_hit) {
      fprintf(out, "== WRITE SITE — core A wrote 0x%08X=%08X ==\n%s", s_ww_addr, s_ww_va, s_ww_bt_a);
      fprintf(out, "== WRITE SITE — core B wrote 0x%08X=%08X ==\n%s", s_ww_addr, s_ww_vb, s_ww_bt_b);
    }
  } else if (!strcmp(sub, "watch")) {
    unsigned addr = 0;
    if (sscanf(line, "%*s %*s %x", &addr) != 1) addr = s_div_addr;
    if (!addr) { fprintf(out, "sbs watch: no address (no divergence yet, give one: `sbs watch <hex>`)\n"); return 1; }
    s_ww_addr = addr; s_ww_armed = true; s_ww_hit = 0; s_ww_bt_a[0] = s_ww_bt_b[0] = 0;
    g_a->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    g_b->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    fprintf(out, "write-watch armed on 0x%08X — `sbs resume`; the diverging write will re-pause with the site.\n", addr);
  } else if (!strcmp(sub, "show")) {
    char w = 0; sscanf(line, "%*s %*s %c", &w);
    if (w == 'b' || w == 'B') s_sel = 1; else if (w == 'a' || w == 'A') s_sel = 0;
    fprintf(out, "selected core %c (window + r/rw/ents target)\n", s_sel ? 'B' : 'A');
  } else if (!strcmp(sub, "resume") || !strcmp(sub, "play")) {
    dbg_set_paused(0); fprintf(out, "resumed\n");
  } else if (!strcmp(sub, "step")) {
    unsigned n = 0; sscanf(line, "%*s %*s %u", &n); if (!n) n = 1;
    void dbg_add_step(int); dbg_add_step((int)n);
    fprintf(out, "step +%u\n", n);
  } else {
    fprintf(out, "sbs subcommands: status | diff | bt | watch [hex] | show a|b | resume | step [n]\n");
  }
  return 1;
}

void sbs_run(const char* exe_path) {
  watchdog_disable();   // the SBS pauses indefinitely on a divergence for live inspection — not a hang
  const char* m = getenv("PSXPORT_SBS_MODE");
  if (m) { if (!strcmp(m, "gameplay")) s_mode = M_GAMEPLAY; else if (!strcmp(m, "both")) s_mode = M_BOTH; else s_mode = M_RENDER; }
  { const char* e = getenv("PSXPORT_SBS_LO"); if (e && *e) s_lo = (uint32_t)strtoul(e, 0, 0); }
  { const char* e = getenv("PSXPORT_SBS_HI"); if (e && *e) s_hi = (uint32_t)strtoul(e, 0, 0); }
  g_store_watch_cb = sbs_store_cb;

  // psx_fallback per mode: gameplay/both run PSX gameplay on core B; render runs native gameplay on both.
  int fb_b = (s_mode == M_RENDER) ? 0 : 1;
  g_a = new Game(); g_a->psx_fallback = 0;
  g_b = new Game(); g_b->psx_fallback = fb_b;
  load_exe(exe_path, &g_a->core); dc_boot_init(&g_a->core);
  load_exe(exe_path, &g_b->core); dc_boot_init(&g_b->core);

  fprintf(stderr, "[sbs] LIVE side-by-side: mode=%s  A=%s  B=%s  diff region 0x%08X..0x%08X + scratchpad\n",
          mode_name(),
          s_mode == M_RENDER ? "native-gp/native-render" : s_mode == M_GAMEPLAY ? "native-gp/PSX-render" : "FULL native",
          s_mode == M_RENDER ? "native-gp/PSX-render"    : s_mode == M_GAMEPLAY ? "PSX-gp/PSX-render"   : "FULL PSX",
          s_lo, s_hi);

  dbg_server_start(&g_a->core);   // PSXPORT_DEBUG_SERVER — inspect/control the harness live

  // BARRIER: navigate each core to gameplay-start independently (their boot/FMV/load timing differs), then
  // compare in equal state. We run A's nav fully, then B's (the Beetle GTE/MDEC singletons make concurrent
  // boot unsafe; sequential nav is fine). After this, both are at the gameplay-start flag.
  navigate(g_a, "A(left)");
  navigate(g_b, "B(right)");
  fprintf(stderr, "[sbs] both cores at gameplay-start — LOCKSTEP begins. Drive with the window; inspect via the debug server.\n");

  for (;;) {
    Core* sel = s_sel ? &g_b->core : &g_a->core;
    dbg_server_service(sel);                 // service one queued debug-server command on the selected core
    if (dbg_is_paused() && !dbg_step_pending()) { usleep(8000); continue; }   // frozen: inspect via server
    if (dbg_step_pending()) dbg_consume_step();

    s_ww_hit = 0;
    step_core(g_a, 0);
    step_core(g_b, 1);

    if (s_ww_armed && s_ww_hit) {            // the diverging write fired — pause with the exact site captured
      fprintf(stderr, "[sbs] write-watch caught 0x%08X (A=%08X B=%08X) at frame %u — paused; see `sbs bt`.\n",
              s_ww_addr, s_ww_va, s_ww_vb, s_frame);
      s_ww_armed = false;
      g_a->core.wwatch_arm(0, 0); g_b->core.wwatch_arm(0, 0);
      dbg_set_paused(1);
    } else if (!s_div_found) {
      check_divergence();
    }
    s_frame++;
  }
}
