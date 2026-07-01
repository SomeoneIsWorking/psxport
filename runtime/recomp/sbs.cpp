// sbs.cpp — LIVE side-by-side two-core divergence debugger (PSXPORT_SBS=1).
//
// Runs TWO native-boot cores in ONE process, in lockstep, with IDENTICAL input, differing ONLY by MODE:
//   render   (default): A = native gameplay + NATIVE render,  B = native gameplay + PSX render
//   gameplay:           A = native gameplay,  B = PSX gameplay (psx_fallback); render IDENTICAL (PSX) on both
//   both:               A = full native (native gp + native render),  B = full PSX (PSX gp + PSX render)
//   oracle:              A = full native (native gp + native render),  B = PURE ORACLE — the interpreter
//                        engine (use_interp, docs/oracle.md) + the software rasterizer (soft_gpu), NOT the
//                        recomp substrate (`psx_fallback` alone). B's `render|gameplay|both` psx_fallback
//                        pane shares the SAME shared native rasterizer as A (gpu_native.cpp/gpu_gpu.cpp) —
//                        it is a scene-walk A/B, not an independent pixel oracle (user finding, 2026-07-01:
//                        SBS=both still showed the render-crack on "both" panes). `oracle` mode is the actual
//                        ground truth: B never touches gpu_gpu.cpp/the VK batch at all; its picture comes
//                        100% from soft_gpu's own s_vram, so a native-only render bug (the silhouette crack)
//                        will NOT reproduce on B unless it is a genuine PSX/content characteristic.
// Select with PSXPORT_SBS_MODE=render|gameplay|both|oracle.
//
// TRUE side-by-side, CONCURRENT FROM BOOT (user, 2026-06-25): both cores boot IN LOCKSTEP — stepped one
// frame each per iteration and PRESENTED to two side-by-side panes every frame (pane 0 = A/left, pane 1 =
// B/right) — so the user watches both boot at once, never a frozen pane while the other loads. The old
// SEQUENTIAL navigate(A)-then-navigate(B) (which froze A's pane while B booted) is GONE. This is safe now
// only because EVERY per-machine subsystem is per-instance: GTE/SPU/MDEC (BindState) AND the CD layer —
// CD-controller registers (cdc_native.c CdcState/cdc_bind) + the XA streamer (xa_stream.c XaState/xa_bind),
// the remaining shared-singleton blocker the user called out; the read-only CHD/disc image stays shared by
// design. Each core's frame EMITS into its OWN VK geometry batch (step_core sets gpu_gpu_select_target) and
// uploads its OWN VRAM to its pane (gpu_gpu_present_sbs: a separate staging buffer per pane); ONE acquire/
// command-buffer/submit/present per window frame, so the two cores never collide on the single VK present.
// Each core's auto-skip stops injecting input once IT reaches free-roam ("pause at goal") but keeps running
// (rendering) so the user can drive it. The dual-pane present is via g_dualview's panel machinery, but with
// TWO DIFFERENT CORES instead of one core rendered twice (g_sbs forces 2 targets + skips the in-engine
// dualview second pass). Two cores feed the SAME host input each frame (mirrored).
//
// Diff/inspection: each lockstep frame, after presenting, DIFF the guest RAM region + scratchpad (legit
// render-only regions excluded). The FIRST divergence PAUSES the loop, held for inspection over the debug
// server (PSXPORT_DEBUG_SERVER, tools/dbgclient.py):
//   sbs                 status: mode, frame, selected core, divergence summary, watch state
//   sbs diff            the divergence detail (frame, first diverging addr/range, A bytes vs B bytes)
//   sbs bt              guest stack backtrace of BOTH cores at the divergence (frame-boundary)
//   sbs watch           arm a write-watchpoint on the diverging address; on `sbs resume`, the WRITE that
//                       diverges pauses mid-frame with the EXACT guest backtrace of each writing core
//   sbs show a|b        which core r/rw/ents/node/scene/etc target (BOTH panes always show; this only
//                       selects which core the debug-server inspection commands act on)
//   sbs resume          unpause;   sbs step [n]   advance n lockstep frames then re-pause
// Both panes are always live; drive BOTH with the host keyboard (each core reads the same host pad each
// frame). r/rw/ents/... operate on the selected core (use `sbs show`).
//
// Modes (PSXPORT_SBS_MODE=render|gameplay|both):
//   render   (default): A = native gameplay + NATIVE render,  B = native gameplay + PSX render
//   gameplay:           A = native gameplay,  B = PSX gameplay (psx_fallback); render IDENTICAL (PSX) on both
//   both:               A = full native (native gp + native render),  B = full PSX (PSX gp + PSX render)
//
// Diagnostic, not behavior (one PC-native game ships; this is a debugger). Like dualcore.cpp it owns its
// own Game instances and never returns (the process exits when the window closes).

#include "game.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <unistd.h>

// --- runtime entry points reused from the normal boot path / dual-core harness ---
void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
void pad_repl_hold(Core* c, uint16_t active_low_mask);
void pad_repl_tap(Core* c, uint16_t active_low_mask, int n);
void pad_service_frame(Core* c);              // pad_input.cpp — pump host input (keeps the window responsive)
extern "C" int cfg_on(const char*);           // cfg.c — env flag (PSXPORT_DEBUG_SERVER presence check)
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
extern "C" int  g_sbs;                        // PSXPORT_SBS: forces 2 VK render targets + skips the in-engine dualview pass
// Per-core render+readback + front-buffer/display-region accessors (gpu_gpu.cpp / gpu_native.cpp). The SBS
// now renders each core into the shared VK target (no swapchain present) and read
// back to a CPU RGBA buffer (gpu_gpu_render_readback), then composites the two panes side by side (gpu_gpu_present_sbs2). This
// replaces the two-pane VK swapchain composite (gpu_gpu_present_sbs) that MoltenVK rendered black on macOS.
void gpu_gpu_render_readback(Core* core, const uint16_t* vram, int sx, int sy, int w, int h, uint8_t* rgba);
void gpu_gpu_select_target(int t);
void gpu_gpu_frame_end(Core* core, const uint16_t* svram, int frame);   // gpu_native.cpp -> resets the VK batch
const uint16_t* gpu_vram_ptr(Core* core);
void gpu_disp_region(Core* core, int* sx, int* sy, int* w, int* h);
void pad_set_buttons(Core* c, uint16_t mask);   // pad_input.cpp — feed the active-low pad mask (SDL_GPU window input)
// SBS presenter (sbs_present_sdl.cpp): draws the two CPU panes + polls the window keyboard/quit.
extern "C" {
  void sbs_rl_init(void);
  int  sbs_rl_should_close(void);
  unsigned short sbs_rl_poll_input(void);
  void sbs_rl_present(const unsigned char* rgbaA, int wA, int hA, const unsigned char* rgbaB, int wB, int hB);
  void sbs_rl_shutdown(void);
}

// PSXPORT_SBS marker, DEFINED here (sbs.cpp owns the harness). gpu_gpu.cpp forces 2 VK render targets when
// set, and native_boot.cpp skips the in-engine dualview second pass (core B fills target 1 instead). 0
// outside the SBS harness, so the single-core game and the dualview path are completely unaffected.
extern "C" { int g_sbs = 0; int g_sbs_rl = 0; }   // g_sbs_rl: SDL_GPU window present -> single VK target (gpu_gpu.cpp)

namespace {

enum Mode { M_RENDER, M_GAMEPLAY, M_BOTH, M_ORACLE };
constexpr uint32_t GAME_ENTRY  = 0x8010637Cu;  // task0 entry while the GAME stage runs (in the field)
constexpr uint32_t TASK0_ENTRY = 0x801fe00cu;  // task0 obj +0xc = current stage entry
constexpr uint32_t CUT_FLAG    = 0x1F800137u;  // cutscene-active byte (1 = intro cutscene, 0 = free-roam)
constexpr uint16_t BTN_CROSS = 0x4000, BTN_START = 0x0008, BTN_NONE = 0xFFFF;

Game*    g_a = nullptr;
Game*    g_b = nullptr;

// Per-pane CPU RGBA8 frames read back from each core's headless VK render (SDL_GPU window uploads + draws these).
// Sized to the full VRAM extent (1024x512) so any display region fits; the live region size is tracked.
uint8_t  s_rgba_a[1024 * 512 * 4];
uint8_t  s_rgba_b[1024 * 512 * 4];
int      s_wa = 0, s_ha = 0, s_wb = 0, s_hb = 0;
int      s_mode = M_RENDER;
int      s_sel  = 0;                            // 0 = A, 1 = B (window + debug-server target core)
uint32_t s_lo   = 0x800B0000u, s_hi = 0x80110000u;
uint32_t s_frame = 0;                           // lockstep frame counter (since gameplay-start barrier)

// ALL per-machine state is PER-INSTANCE now (bound per core frame-step in native_step_frame): GTE (gte_bind),
// SPU (spu_bind), MDEC (mdec_bind), and the CD layer — CD-controller registers (cdc_bind) + XA streamer
// (xa_bind). Two cores keep entirely separate machine state, so there is no save/restore shadow and
// concurrent boot through FMV/loads is safe. The CHD/disc image is read-only data, shared by design.

// --- divergence record (frame-boundary RAM/scratchpad diff) ---
bool     s_div_found = false;
// The frame-boundary divergence check is only meaningful in actual FREE-ROAM gameplay — during boot / logos /
// FMV / menu / the intro cutscene the native and PSX cores legitimately differ (different code owns those),
// so checking there just spuriously pauses the harness. ARM it only once BOTH cores reach free-roam, detected
// per core as "the cutscene flag went up (intro) and then back down" (mirrors nav_step's gameplay-start test).
bool     s_div_armed = false;
bool     s_seen_cut_a = false, s_seen_cut_b = false, s_fr_a = false, s_fr_b = false;
uint32_t s_div_frame = 0, s_div_addr = 0, s_div_end = 0;
char     s_bt_a[4096] = {0}, s_bt_b[4096] = {0};
bool     s_have_dbgsrv = false;   // PSXPORT_DEBUG_SERVER set? — only PAUSE-on-divergence when it is (so the
                                  // user can `sbs diff`). Without it, a divergence LOGS and CONTINUES so a
                                  // plain windowed PSXPORT_SBS=1 run keeps driving both panes (never hangs).

// --- write-watchpoint record (exact corrupting-write site) ---
bool     s_ww_armed = false;
uint32_t s_ww_addr  = 0;
int      s_ww_hit   = 0;                        // bit0 = A wrote, bit1 = B wrote
uint32_t s_ww_va = 0, s_ww_vb = 0;
char     s_ww_bt_a[4096] = {0}, s_ww_bt_b[4096] = {0};

const char* mode_name() { return s_mode == M_RENDER ? "render" : s_mode == M_GAMEPLAY ? "gameplay" :
                                 s_mode == M_ORACLE ? "oracle" : "both"; }

// Legit render-only guest regions in MAIN RAM: the native vs PSX render paths write GP0 packets / OT /
// pool pointers here (render + both mode). Divergence here is render noise, not the gameplay we hunt.
bool is_render_region(uint32_t a) {
  if (a >= 0x800BF4F0u && a < 0x800BF54Cu) return true;   // pool ptrs + dwell
  if (a >= 0x800BFE68u && a < 0x800EA200u) return true;   // packet pool (×2) + OT (×2) + env
  return false;
}

// Legit render-only SCRATCHPAD workspace. In render/both mode the two cores run IDENTICAL gameplay but
// DIFFERENT render layers (A=native render, B=PSX recomp render). The PSX render path writes GTE / render
// scratchpad the native render path does not (and vice-versa), so these regions differ BY DESIGN and must
// NOT trip the divergence detector. The render workspace addresses are documented in engine_submit.cpp /
// engine_project.cpp (#define SCR 0x1F800000) — grep of those + engine_render/camera shows two bands:
//   0x1F800000..0x1F800100  GTE-compose work matrices, camera/RotMatrix area, intermediate transforms,
//                           world-readout (0x1F8000D2/D6/DA)
//   0x1F800140..0x1F800160  per-frame render-list write-ptr (0x148) / count (0x150) / cap
// The GAP 0x1F800100..0x1F800140 is GAMEPLAY scratchpad (e.g. the cutscene-active flag 0x1F800137, sub-mode
// bytes 0x134-0x138) and is STILL diffed, so real gameplay corruption is caught. Excluded ONLY when the
// render paths actually differ (render/both); gameplay mode renders PSX on both, so nothing here diverges
// and we keep the FULL scratchpad diff there.
bool is_render_spad(uint32_t a) {
  if (s_mode == M_GAMEPLAY) return false;                 // identical PSX render on both → no render-noise to mask
  if (a >= 0x1F800000u && a < 0x1F800100u) return true;   // GTE-compose work matrices + camera/RotMatrix + readout
  if (a >= 0x1F800140u && a < 0x1F800160u) return true;   // per-frame render-list write-ptr/count/cap
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
    case M_ORACLE:   g_render_psx = 0;                break;   // A native; B never consults this (use_interp
                                                                // bypasses the native/PSX g_render_psx flip
                                                                // entirely — it's fully interpreted+soft-rasterized)
  }
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
    bool noise = spad ? is_render_spad(x) : is_render_region(x);
    if (g_a->core.mem_r8(x) != g_b->core.mem_r8(x) && !noise) { last = x; gap = 0; } else gap++;
  }
  s_div_found = true; s_div_frame = s_frame; s_div_addr = addr; s_div_end = last + 1;
  cap_bt(&g_a->core, s_bt_a, sizeof s_bt_a);
  cap_bt(&g_b->core, s_bt_b, sizeof s_bt_b);
  fprintf(stderr, "\n[sbs] *** DIVERGENCE at lockstep frame %u: 0x%08X..0x%08X (mode=%s) ***\n",
          s_frame, s_div_addr, s_div_end, mode_name());
  // PAUSE for inspection ONLY when the debug server is up (so the user can `sbs diff`/`sbs bt`). Otherwise
  // (plain windowed PSXPORT_SBS=1) LOG and CONTINUE — the run keeps driving both panes; never hang.
  if (s_have_dbgsrv) {
    fprintf(stderr, "[sbs] paused. Inspect over the debug server: `sbs diff`, `sbs bt`, `sbs watch`.\n");
    dbg_set_paused(1);
  } else {
    fprintf(stderr, "[sbs] (no debug server: logging and continuing; set PSXPORT_DEBUG_SERVER=1 to pause + `sbs diff`)\n");
  }
}

// Frame-boundary diff: report the first non-render-noise divergence in the RAM region, else the scratchpad.
void check_divergence() {
  const uint8_t* a = g_a->core.ram + (s_lo - 0x80000000u);
  const uint8_t* b = g_b->core.ram + (s_lo - 0x80000000u);
  uint32_t n = s_hi - s_lo;
  for (uint32_t i = 0; i < n; i++) if (a[i] != b[i] && !is_render_region(s_lo + i)) { record_divergence(s_lo + i); return; }
  for (uint32_t i = 0; i < 0x400; i++)
    if (g_a->core.scratch[i] != g_b->core.scratch[i] && !is_render_spad(0x1F800000u + i)) { record_divergence(0x1F800000u + i); return; }
}

// Step ONE core's frame for the SBS composite: diff_mode=1 suppresses its OWN per-core present/pace/audio
// (the SBS loop owns the single window present), sbs_render=1 re-enables the render-submit so it EMITS its
// geometry into VK batch `which` (gpu_gpu_select_target). Neither core presents; the SBS composite does.
void step_core(Game* g, int which) {
  g->core.game->diff_mode  = 1;
  g->core.game->sbs_render = 1;
  // PSXPORT_SBS intro-FMV: the OP.STR opening movie is OWNED by the native FMV player, which is skipped
  // in SBS (native_fmv.cpp: `if (g_sbs) return 0`). But the PSX core (B) runs the GUEST demo machine,
  // whose STR streamer strNext (FUN_8010755c) then waits for CD-streamed STR sectors that are NEVER fed
  // here — it busy-polls StGetNext (FUN_8008d030) ~2000x2000 times per attract cycle (~4M interpreted
  // polls, multi-second), a non-yielding spin that STALLS the lockstep = the "frozen" SBS + repeated
  // "time out in strNext()". Convert that async wait to a SYNC skip using the game's OWN skip-request
  // flag DAT_1f80019d: the demo machine's prologue (guest FUN_80106f80 / native demo_menu_machine
  // engine_demo.cpp:401) forces the teardown sub-state when it is set, so the FMV-streaming states are
  // never entered. Set it only while the DEMO stage (0x801062E4) is in its intro-FMV sub-state
  // (demo SM[0x48]==1) so the GAME-stage opening cutscene — the actual SBS comparison target — is never
  // skipped. The demo consumes/clears the flag at teardown; re-setting per frame keeps every attract
  // cycle from re-entering the spin.
  { Core* c = &g->core;
    if (c->mem_r32(0x801fe00cu) == 0x801062E4u) {            // DEMO stage (per-core memory)
      uint32_t sm = c->mem_r32(0x1f800138u);                 // demo state-machine block ptr
      if (sm && c->mem_r16(sm + 0x48u) == 1)                 // SM[0x48]==1 -> intro-FMV sub-state (s1)
        c->mem_w8(0x1f80019du, 1);                           // request skip (the game's Start-skip flag)
    } }
  apply_mode(which);
  gpu_gpu_select_target(which);        // single VK target under g_sbs_rl: both cores emit into batch 0
  dc_step_frame(&g->core, s_frame);   // native_step_frame binds THIS core's per-instance GTE/SPU/MDEC/CD
}

// Render ONE core's just-emitted frame into the shared VK target HEADLESS and read it back to its CPU RGBA
// pane buffer (SDL_GPU window then draws it). Resets the VK geometry batch so the NEXT core renders into a clean
// target. Records the live display-region size for the SDL_GPU window upload. This is the per-core PROVEN path
// (single-mode headless render + readback, the same gpu_gpu_shot uses), which works on macOS/MoltenVK.
void grab_pane(Game* g, uint8_t* rgba, int* ow, int* oh) {
  int sx, sy, w, h; gpu_disp_region(&g->core, &sx, &sy, &w, &h);
  if (w < 1) w = 1; if (h < 1) h = 1;
  if (w > 1024) w = 1024; if (h > 512) h = 512;
  gpu_gpu_render_readback(&g->core, gpu_vram_ptr(&g->core), sx, sy, w, h, rgba);
  gpu_gpu_frame_end(&g->core, gpu_vram_ptr(&g->core), (int)s_frame);   // reset the VK geometry batch
  *ow = w; *oh = h;
}

// Draw the two most-recently grabbed panes (A left, B right) in one SDL_GPU window window frame.
void present_panes() { sbs_rl_present(s_rgba_a, s_wa, s_ha, s_rgba_b, s_wb, s_hb); }

// PSXPORT_SBS_KEYS — scripted timed input for HEADLESS repro (no window): a comma list of
// "FROM-TO:BTN" entries holding BTN (active-low) over frames [FROM,TO]. BTN is a libpad bit name:
// start(0x0008) cross(0x4000) circle(0x2000) triangle(0x1000) square(0x8000)
// up(0x0010) right(0x0020) down(0x0040) left(0x0080) — see pad. Example
// (reproduce the narration: tap Start ~every 70f): PSXPORT_SBS_KEYS="250-256:start,320-326:start,...".
// Lets a headless session DRIVE the exact path the windowed user reports, instead of needing the window.
struct SbsKey { uint32_t from, to; uint16_t btn; };
static std::vector<SbsKey> s_keys;
static bool s_keys_parsed = false;
static uint16_t sbs_btn_bit(const char* n) {
  if (!strcmp(n, "start")) return 0x0008; if (!strcmp(n, "select")) return 0x0001;
  if (!strcmp(n, "cross")) return 0x4000; if (!strcmp(n, "circle")) return 0x2000;
  if (!strcmp(n, "square")) return 0x8000; if (!strcmp(n, "triangle")) return 0x1000;
  // D-PAD bits (bug fix: these were aliased to the face-button values above, so up/down/left/right
  // scripted keys actually pressed triangle/cross/square/circle and never moved anything). Real PSX
  // digital-pad bit layout (active-low): UP=0x0010 RIGHT=0x0020 DOWN=0x0040 LEFT=0x0080 — matches
  // dbg_server.cpp's dbg_btn() (used by the live debug-server press/tap commands).
  if (!strcmp(n, "up")) return 0x0010; if (!strcmp(n, "down")) return 0x0040;
  if (!strcmp(n, "left")) return 0x0080; if (!strcmp(n, "right")) return 0x0020;
  return 0;
}
static void parse_sbs_keys() {
  s_keys_parsed = true;
  const char* e = getenv("PSXPORT_SBS_KEYS"); if (!e || !*e) return;
  char buf[2048]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
  for (char* p = strtok(buf, ","); p; p = strtok(0, ",")) {
    uint32_t from = 0, to = 0; char name[32] = {0};
    if (sscanf(p, "%u-%u:%31s", &from, &to, name) == 3) {
      uint16_t b = sbs_btn_bit(name);
      if (b) s_keys.push_back({from, to, b});
    }
  }
  fprintf(stderr, "[sbs] PSXPORT_SBS_KEYS: %zu scripted input ranges\n", s_keys.size());
}

// Feed the SAME host pad mask to BOTH cores (mirrored input). repl_on stays 0 in normal play, so
// pad_service_frame leaves this mask intact for the game to read. PSXPORT_SBS_KEYS injects timed input.
void feed_input() {
  if (!s_keys_parsed) parse_sbs_keys();
  uint16_t mask = (uint16_t)sbs_rl_poll_input();
  for (const SbsKey& k : s_keys)
    if (s_frame >= k.from && s_frame <= k.to) mask &= ~k.btn;   // active-low: pressed = bit cleared
  pad_set_buttons(&g_a->core, mask);
  pad_set_buttons(&g_b->core, mask);
}

// PSXPORT_SBS_DUMP=path: write the current two panes (A left | B right) as ONE side-by-side PPM. Lets a
// headless/remote session SEE the comparison without the window (the readback panes are the same pixels
// the window composites). Called once both cores reach free-roam (and on demand via `sbs dump`).
void dump_sbs_ppm(const char* path) {
  int H = s_ha > s_hb ? s_ha : s_hb; int W = s_wa + s_wb;
  if (W < 1 || H < 1) return;
  FILE* f = fopen(path, "wb"); if (!f) { fprintf(stderr, "[sbs] dump: cannot open %s\n", path); return; }
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const uint8_t* rgba; int px, pw, ph;
      if (x < s_wa) { rgba = s_rgba_a; px = x;        pw = s_wa; ph = s_ha; }
      else          { rgba = s_rgba_b; px = x - s_wa; pw = s_wb; ph = s_hb; }
      uint8_t rgb[3] = {0,0,0};
      if (px < pw && y < ph) { const uint8_t* p = rgba + ((size_t)y * pw + px) * 4; rgb[0]=p[0]; rgb[1]=p[1]; rgb[2]=p[2]; }
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
  fprintf(stderr, "[sbs] dumped side-by-side panes (A %dx%d | B %dx%d) -> %s\n", s_wa, s_ha, s_wb, s_hb, path);
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

// Diagnostics for cross-TU instrumentation (overlay_router): which SBS core is this, and the lockstep frame.
extern "C" int sbs_core_id(Core* c) { if (!g_a) return -1; return (g_b && c == &g_b->core) ? 1 : 0; }
extern "C" uint32_t sbs_frame_num() { return s_frame; }

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
  } else if (!strcmp(sub, "dump")) {
    char path[256] = {0}; if (sscanf(line, "%*s %*s %255s", path) != 1) snprintf(path, sizeof path, "scratch/screenshots/sbs.ppm");
    dump_sbs_ppm(path); fprintf(out, "dumped side-by-side panes -> %s\n", path);
  } else if (!strcmp(sub, "ramdiff")) {
    // ON-DEMAND full-region diff (ungated by free-roam arming): scan the whole gameplay RAM region +
    // scratchpad NOW and summarize EVERY diverging span (render-noise regions excluded). Use it to find
    // PSX(B)-vs-PC(A) divergences at whatever common state the two cores have reached. `sbs ramdiff N`
    // caps the listed spans (default 24); the totals always reflect the full scan.
    unsigned cap = 0; sscanf(line, "%*s %*s %u", &cap); if (!cap) cap = 24;
    const uint8_t* a = g_a->core.ram + (s_lo - 0x80000000u);
    const uint8_t* b = g_b->core.ram + (s_lo - 0x80000000u);
    uint32_t n = s_hi - s_lo, spans = 0, bytes = 0, listed = 0;
    for (uint32_t i = 0; i < n; ) {
      if (a[i] != b[i] && !is_render_region(s_lo + i)) {
        uint32_t start = s_lo + i;
        while (i < n && a[i] != b[i] && !is_render_region(s_lo + i)) { i++; bytes++; }
        spans++;
        if (listed++ < cap)
          fprintf(out, "  RAM  0x%08X..0x%08X (%u B)  A=%02X%02X%02X%02X B=%02X%02X%02X%02X\n",
                  start, s_lo + i, (s_lo + i) - start,
                  g_a->core.mem_r8(start), g_a->core.mem_r8(start+1), g_a->core.mem_r8(start+2), g_a->core.mem_r8(start+3),
                  g_b->core.mem_r8(start), g_b->core.mem_r8(start+1), g_b->core.mem_r8(start+2), g_b->core.mem_r8(start+3));
      } else i++;
    }
    uint32_t sspans = 0, sbytes = 0;
    for (uint32_t i = 0; i < 0x400; ) {
      if (g_a->core.scratch[i] != g_b->core.scratch[i] && !is_render_spad(0x1F800000u + i)) {
        uint32_t start = 0x1F800000u + i;
        while (i < 0x400 && g_a->core.scratch[i] != g_b->core.scratch[i] && !is_render_spad(0x1F800000u + i)) { i++; sbytes++; }
        sspans++;
        if (listed++ < cap)
          fprintf(out, "  SPAD 0x%08X..0x%08X (%u B)\n", start, 0x1F800000u + i, (0x1F800000u + i) - start);
      } else i++;
    }
    fprintf(out, "sbs ramdiff @frame %u: RAM %u spans / %u B diverge (region 0x%08X..0x%08X), "
                 "scratchpad %u spans / %u B. A=PC(native) B=PSX(full-recomp).\n",
            s_frame, spans, bytes, s_lo, s_hi, sspans, sbytes);
  } else {
    fprintf(out, "sbs subcommands: status | diff | bt | watch [hex] | show a|b | resume | step [n] | dump [path] | ramdiff [N]\n");
  }
  return 1;
}

void sbs_run(const char* exe_path) {
  watchdog_disable();   // the SBS pauses indefinitely on a divergence for live inspection — not a hang
  const char* m = getenv("PSXPORT_SBS_MODE");
  if (m) { if (!strcmp(m, "gameplay")) s_mode = M_GAMEPLAY; else if (!strcmp(m, "both")) s_mode = M_BOTH;
            else if (!strcmp(m, "oracle")) s_mode = M_ORACLE; else s_mode = M_RENDER; }
  { const char* e = getenv("PSXPORT_SBS_LO"); if (e && *e) s_lo = (uint32_t)strtoul(e, 0, 0); }
  { const char* e = getenv("PSXPORT_SBS_HI"); if (e && *e) s_hi = (uint32_t)strtoul(e, 0, 0); }
  g_store_watch_cb = sbs_store_cb;
  g_sbs = 1;            // skip the in-engine dualview second pass (each core renders its OWN pane)
  g_sbs_rl = 1;         // sequential single-target render+readback per core (both cores emit into batch 0)
  // The SBS composite is now drawn by the SDL_GPU renderer's OWN window (gpu_gpu_present_sbs2), so each core
  // renders into the VRAM image WITHOUT presenting to the swapchain (gpu_gpu_render_readback), then both
  // panes are composited side by side in ONE window frame. No second window, so VK is NOT forced headless
  // (the old SDL_GPU window path opened its own GL window, which is why this used to set PSXPORT_VK_HEADLESS — that
  // two-window workaround is gone). The window opens lazily on the first render_readback.

  // psx_fallback per mode: gameplay/both run PSX gameplay on core B; render runs native gameplay on both;
  // oracle runs the PURE interpreter+soft-rasterizer oracle on B (docs/oracle.md) — NOT the recomp-substrate
  // psx_fallback pane, which shares A's native rasterizer and so cannot isolate a native-render-only bug.
  int fb_b = (s_mode == M_RENDER) ? 0 : 1;
  g_a = new Game(); g_a->psx_fallback = 0;
  g_b = new Game(); g_b->psx_fallback = fb_b;
  if (s_mode == M_ORACLE) { g_b->core.use_interp = 1; g_b->gpu.soft_gpu = 1; }
  load_exe(exe_path, &g_a->core); dc_boot_init(&g_a->core);
  load_exe(exe_path, &g_b->core); dc_boot_init(&g_b->core);

  sbs_rl_init();        // no-op: the SDL_GPU renderer owns the (single) window, created on first present

  fprintf(stderr, "[sbs] LIVE side-by-side: mode=%s  A=%s  B=%s  diff region 0x%08X..0x%08X + scratchpad\n",
          mode_name(),
          s_mode == M_RENDER ? "native-gp/native-render" : s_mode == M_GAMEPLAY ? "native-gp/PSX-render" : "FULL native",
          s_mode == M_RENDER ? "native-gp/PSX-render"    : s_mode == M_GAMEPLAY ? "PSX-gp/PSX-render"   :
          s_mode == M_ORACLE ? "PURE-ORACLE(interp+softGPU)" : "FULL PSX",
          s_lo, s_hi);

  s_have_dbgsrv = cfg_on("PSXPORT_DEBUG_SERVER") != 0;   // pause-on-divergence only when the server is up
  dbg_server_start(&g_a->core);   // PSXPORT_DEBUG_SERVER — inspect/control the harness live

  // CONCURRENT boot to gameplay-start: step BOTH cores in lockstep, presenting both panes every frame, so
  // the user watches both boot side by side and each PAUSES its auto-input at free-roam while the other
  // catches up (CD/GTE/SPU/MDEC are now all per-instance, so concurrent boot is safe — see *_bind). After
  // both reach free-roam the main lockstep loop below takes over (driven by host/server input).
  // You drive BOTH cores yourself from frame 0 (boot → logos → menu → game) with the window — no auto-skip,
  // so front-end / transition bugs are fully shown. The lockstep loop below is host-input-driven.
  // YOU DRIVE both cores from frame 0 by default (user 2026-06-30: "I want to control it myself") — the
  // SDL_GPU window keyboard feeds the SAME input to both panes (mirrored). Opt INTO the 3-phase Cross/Start
  // auto-nav (drive both to the GAME free-roam field, then hand off) with PSXPORT_SBS_AUTONAV=1.
  const char* sbs_autonav_env = getenv("PSXPORT_SBS_AUTONAV");
  const bool sbs_autonav = sbs_autonav_env && *sbs_autonav_env && strcmp(sbs_autonav_env, "0") != 0;
  const char* sbs_dump_path = getenv("PSXPORT_SBS_DUMP");   // write the side-by-side panes once at free-roam
  static Nav nav_a, nav_b; static bool s_dumped = false;
  fprintf(stderr, "[sbs] %s — then drive both panes with the window keyboard (WASD/arrows, K=Cross, "
                  "Enter=Start, …) or the debug server; inspect via `sbs` cmds.\n",
          sbs_autonav ? "AUTO-NAV to the field" : "LOCKSTEP from boot (no auto-nav)");

  for (;;) {
    if (sbs_rl_should_close()) { fprintf(stderr, "[sbs] window closed — exiting.\n"); break; }
    Core* sel = s_sel ? &g_b->core : &g_a->core;
    dbg_server_service(sel);                 // service one queued debug-server command on the selected core
    // Until both cores reach free-roam, auto-nav drives them (Cross/Start taps per core); after that, hand
    // off to interactive input so you can walk Tomba and compare PSX vs native at any spot.
    bool nav_done = !sbs_autonav || (nav_a.phase == DONE && nav_b.phase == DONE);
    if (!nav_done) { nav_step(&g_a->core, nav_a, s_frame, "A"); nav_step(&g_b->core, nav_b, s_frame, "B"); }
    else feed_input();                       // SDL_GPU window keyboard -> BOTH cores' pad (mirrored host input)
    // PAUSED (a divergence with the debug server up, or a manual `sbs` pause): keep the WINDOW LIVE — the
    // SDL_GPU window present re-draws the last grabbed panes and polls input, so the window stays responsive.
    if (dbg_is_paused() && !dbg_step_pending()) {
      present_panes();
      usleep(15000);
      continue;
    }
    if (dbg_step_pending()) dbg_consume_step();

    s_ww_hit = 0;
    step_core(g_a, 0); grab_pane(g_a, s_rgba_a, &s_wa, &s_ha);   // render A headless -> CPU readback (single VK target)
    step_core(g_b, 1); grab_pane(g_b, s_rgba_b, &s_wb, &s_hb);   // then render B headless -> CPU readback
    present_panes();                                             // SDL_GPU window window: pane A (left) | pane B (right)
    // PSXPORT_SBS_DUMP: once both cores are at the field, write the side-by-side composite once (so a
    // headless/remote session can confirm the panes aren't black and eyeball PSX-vs-native).
    if (sbs_dump_path && nav_done && !s_dumped && s_wa > 0 && s_wb > 0) { dump_sbs_ppm(sbs_dump_path); s_dumped = true; }

    // The frame-boundary diff NO LONGER auto-pauses (user 2026-06-30: drive both games freely with the same
    // input, don't freeze on a divergence). Inspect divergences ON DEMAND with `sbs ramdiff` instead. The
    // OPT-IN write-watch (`sbs watch <addr>`) still pauses on the exact diverging write, since you asked for it.
    if (s_ww_armed && s_ww_hit) {            // an EXPLICITLY-armed write-watch fired — pause with the site captured
      fprintf(stderr, "[sbs] write-watch caught 0x%08X (A=%08X B=%08X) at frame %u — paused; see `sbs bt`.\n",
              s_ww_addr, s_ww_va, s_ww_vb, s_frame);
      s_ww_armed = false;
      g_a->core.wwatch_arm(0, 0); g_b->core.wwatch_arm(0, 0);
      dbg_set_paused(1);
    }
    s_frame++;
  }
  sbs_rl_shutdown();
  exit(0);   // the SBS debugger owns the process; closing the window ends it (mirrors the old never-return)
}
