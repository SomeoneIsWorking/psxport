// dbg_server.c — live, NON-BLOCKING TCP debug server for the native port.
//
// WHY: PSXPORT_REPL (drive.py FIFO) BLOCKS the game between commands, so it can't be used while the
// user PLAYS the game live (windowed) and points at a bug. This server runs in a background pthread
// listening on 127.0.0.1:<port> (default 5959, env PSXPORT_DEBUG_SERVER=1 or =<port>). The game keeps
// running at full speed; a command is marshalled to the MAIN thread and serviced once per frame at a
// safe point (dbg_server_service, called from the native frame loop), so all guest-RAM / GPU-state
// access happens on the main thread (no races). The textual result is sent back over the socket.
//
// Protocol (one command per line; reply is free text terminated by a line "---END---\n"):
//   help                  — list commands
//   r  <hex> [n]          — read n bytes  of guest RAM (default 16)
//   rw <hex> [n]          — read n words  of guest RAM (default 8)
//   stage                 — scene/stage latches (0x801fe00c / 0x801fe048 / scene-active 0x800BE258)
//   scene                 — classified display list of the CURRENT frame (gpu_scene_dump_now)
//   provat <x> <y>        — which prim drew each displayed pixel around (x,y) (gpu_provat_display)
//   shot [path]           — screenshot of the presented display region; default scratch/screenshots/dbg.png (PNG unless path ends .ppm)
//   frame                 — current present-frame counter
//
// Drive it from the repo with tools/dbgclient.py (or `nc 127.0.0.1 5959`).
#define _GNU_SOURCE
#include <stdio.h>
#include "cfg.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Portability: some platforms (e.g. macOS / clang under strict -std=c11, which sets __STRICT_ANSI__)
// hide the BSD constant INADDR_LOOPBACK from <netinet/in.h>. It's a fixed value (127.0.0.1), so define
// it ourselves when the header didn't — keeps the build working regardless of feature-test macros.
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001u
#endif

// --- guest RAM + GPU primitives provided by the rest of the port ---------------------------------
#include "core.h"                  // Core, mem_*, rec_dispatch — for the RE commands (call/ents/node)
#include "game.h"                  // Core::game->gpu (render state is per-instance now)
#include "dbg_server.h"            // class DbgServer — singleton state holder
#include "sbs.h"                   // class Sbs — Sbs::coreA()/coreB() for per-command core targeting
void gpu_scene_dump_now(Core* c, FILE* out);
void gpu_provat_display(Core* core, FILE* out, int qx, int qy);
void gpu_native_shot(Core* core, const char* path);
int  gpu_frame_no(Core* core);
int  gpu_vk_enabled(void);
void gpu_vk_shot(Core* core, const char* path);
void gpu_vk_stats(Core* core, int* tri, int* tex, int* semi);
void gpu_vk_vram_region(Core* core, const char* path, int x, int y, int w, int h);
// pad input (pad_input.c) — lets the debug server DRIVE the game (press/release/tap/hold)

// PSX pad: name -> active-HIGH bit (mirrors the REPL mapping in native_boot.c).
static unsigned dbg_btn(const char* n) {
  if (!strcmp(n,"start"))  return 0x0008; if (!strcmp(n,"select")) return 0x0001;
  if (!strcmp(n,"x")||!strcmp(n,"cross"))    return 0x4000;
  if (!strcmp(n,"o")||!strcmp(n,"circle"))   return 0x2000;
  if (!strcmp(n,"triangle")||!strcmp(n,"t")) return 0x1000;
  if (!strcmp(n,"square")||!strcmp(n,"sq"))  return 0x8000;
  if (!strcmp(n,"up")) return 0x0010; if (!strcmp(n,"down"))  return 0x0040;
  if (!strcmp(n,"left"))return 0x0080; if (!strcmp(n,"right")) return 0x0020;
  return (unsigned)strtoul(n, 0, 16);
}
// Held-input / pause / step / ctx / started state live on `class DbgServer` (dbg_server.h). Only
// one endpoint per process (one host TCP port); whichever Game's `dbg_server.start(c)` wins the
// bind claims the process-wide server. `sInstance` points at that DbgServer so the impl-TU
// dispatcher and thread body can reach it without a static instance() singleton.
class DbgServerInternals {
public:
  static DbgServer*      sInstance;
  static unsigned short& held()   { return sInstance->mHeld;   }
  static bool&           paused() { return sInstance->mPaused; }
  static int&            step()   { return sInstance->mStep;   }
  static Core*&          ctx()    { return sInstance->mCtx;    }
  static bool&           started(){ return sInstance->mStarted;}
  static pthread_mutex_t& mtx()   { return sInstance->mMtx;    }
  static pthread_cond_t&  done()  { return sInstance->mDone;   }
  static char           (&cmd())[512] { return sInstance->mCmd; }
  static int&    reqPending()     { return sInstance->mReqPending; }
  static int&    respReady()      { return sInstance->mRespReady;  }
  static char*&  respBuf()        { return sInstance->mRespBuf;    }
  static size_t& respLen()        { return sInstance->mRespLen;    }
};
DbgServer* DbgServerInternals::sInstance = nullptr;

// Impl-TU shorthand so the dispatcher below (a large switch) doesn't sprout accessor calls every
// three lines. Not exported — this block is impl-private.
#define s_held    (DbgServerInternals::held())
#define s_paused  (DbgServerInternals::paused())
#define s_step    (DbgServerInternals::step())
#define s_ctx     (DbgServerInternals::ctx())
#define s_started (DbgServerInternals::started())

// main<->server handoff (a single pending request, serviced on the main thread once per frame) lives
// on `class DbgServer` with the rest of the state; access via the DbgServerInternals aliases below.
#define s_mtx         (DbgServerInternals::mtx())
#define s_done        (DbgServerInternals::done())
#define s_cmd         (DbgServerInternals::cmd())
#define s_req_pending (DbgServerInternals::reqPending())
#define s_resp_ready  (DbgServerInternals::respReady())
#define s_resp_buf    (DbgServerInternals::respBuf())
#define s_resp_len    (DbgServerInternals::respLen())

// Per-command SBS core targeting. A leading "@a " / "@b " token (or "@A "/"@B ") swaps s_ctx to that
// SBS core for the duration of ONE command, then restores the frame-loop's context. Lets one debug
// endpoint drive BOTH SBS cores from a single connection without `sbs show` between every command.
// Silently ignored when the SBS harness isn't running (no-op passthrough with the leading token
// stripped) so scripts written for SBS work in standalone too.
static bool parse_core_target(const char*& line, Core*& saved, Core*& override_ctx) {
  saved = nullptr; override_ctx = nullptr;
  // Skip leading whitespace so `  @a  r 0x80000000` still targets A.
  while (*line == ' ' || *line == '\t') line++;
  if (line[0] != '@') return false;
  if (!line[1] || (line[2] != ' ' && line[2] != '\t')) return false;
  char which = line[1];
  Sbs* sbs = (s_ctx && s_ctx->game) ? s_ctx->game->sbs : nullptr;
  Core* target = sbs ? sbs->coreByLetter(which) : nullptr;
  if (!target) { line += 3; return false; }         // strip the token even if no-op
  saved = s_ctx; override_ctx = target;
  s_ctx = target;
  line += 3; while (*line == ' ' || *line == '\t') line++;
  return true;
}

// Execute one command, writing its textual result into `out` (a memstream). MAIN THREAD ONLY.
static void dbg_exec(FILE* out, const char* line) {
  char cmd[32] = {0}, arg[256] = {0};
  unsigned a = 0, b = 0;
  // Optional "@a "/"@b " prefix redirects this ONE command's Core context to the SBS core.
  const char* p = line;
  Core* saved_ctx = nullptr; Core* over_ctx = nullptr;
  bool  targeted  = parse_core_target(p, saved_ctx, over_ctx);
  struct RestoreCtx { Core*& ctx; Core* prev; bool on; ~RestoreCtx() { if (on) ctx = prev; } };
  RestoreCtx rc{ s_ctx, saved_ctx, targeted };
  line = p;                                  // dispatcher sees the command WITHOUT the target token
  if (sscanf(line, "%31s", cmd) != 1) { fprintf(out, "(empty)\n"); return; }
  if (s_ctx && s_ctx->game && s_ctx->game->sbs && s_ctx->game->sbs->dbgCmd(out, line)) return;   // SBS divergence-debugger commands

  if (!strcmp(cmd, "help")) {
    fprintf(out,
      "commands:\n"
      "  r <hex> [n]      read n bytes of guest RAM (default 16)\n"
      "  rw <hex> [n]     read n words of guest RAM (default 8)\n"
      "  w8/w16/w32 A V   write a byte/half/word V to guest RAM addr A (RE poke)\n"
      "  call A [a0..a3]  call guest fn at A (rec_dispatch) with up to 4 hex args; reports v0/v1\n"
      "  ents             walk the entity list (both heads): addr type pos handler rflag cmds geomblk\n"
      "  node A           decode entity node at A (type/state/pos/rot/handler/cmd-list)\n"
      "  geomblk G S      model-table lookup: T=*(0x800ECF58+G*4); geomblk=T+*(T+S*4+4)\n"
      "  stage            scene/stage latches\n"
      "  scene            classified display list of the current frame\n"
      "  provat <x> <y>   which prim drew each displayed pixel around (x,y)\n"
      "  shot [path]      screenshot of the presented output (VK readback if VK active, else SW)\n"
      "  vkshot [path]    force a VK-rendered readback to PPM\n"
      "  vkstats          last frame's VK batched vertex counts (flat/textured/semi)\n"
      "  @a <cmd> / @b <cmd>  route this command to SBS core A / B (default: whichever `sbs show` selected)\n"
      "  press/release B  hold/release a pad button (up/down/left/right/x/o/triangle/square/start/select)\n"
      "  tap B [n]        tap a button for n frames (default 4)\n"
      "  hold <hex>       set the raw active-low pad mask\n"
      "  padrec [save <path> [nframes]]  frames captured so far / cut the LIVE session into a .pad replay\n"
      "  sbs [0|1]        toggle/set Vulkan-vs-Software side-by-side view\n"
      "  pause            freeze the game (window holds last frame)\n"
      "  step [n]         advance exactly n frames then re-freeze (default 1)\n"
      "  play|resume      unfreeze\n"
      "  frame            current present-frame counter + paused state\n");
  } else if (!strcmp(cmd, "r") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
    if (!b) b = 16; if (b > 256) b = 256;
    fprintf(out, "%08X:", a);
    for (unsigned i = 0; i < b; i++) fprintf(out, " %02X", s_ctx->mem_r8(a + i));
    fprintf(out, "\n");
  } else if (!strcmp(cmd, "rw") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
    if (!b) b = 8; if (b > 64) b = 64;
    fprintf(out, "%08X:", a);
    for (unsigned i = 0; i < b; i++) fprintf(out, " %08X", s_ctx->mem_r32(a + i * 4));
    fprintf(out, "\n");
  } else if (!strcmp(cmd, "w32") && sscanf(line, "%*s %x %x", &a, &b) == 2) {
    s_ctx->mem_w32(a, b); fprintf(out, "[%08X] <- %08X\n", a, b);
  } else if (!strcmp(cmd, "w16") && sscanf(line, "%*s %x %x", &a, &b) == 2) {
    s_ctx->mem_w16(a, (uint16_t)b); fprintf(out, "[%08X] <- %04X\n", a, b & 0xffff);
  } else if (!strcmp(cmd, "w8") && sscanf(line, "%*s %x %x", &a, &b) == 2) {
    s_ctx->mem_w8(a, (uint8_t)b); fprintf(out, "[%08X] <- %02X\n", a, b & 0xff);
  } else if (!strcmp(cmd, "call") && sscanf(line, "%*s %x", &a) == 1) {
    // Call a guest function on the live CPU context. RE aid: probe a function's effect/return live
    // (e.g. the transform-build 0x80051C8C, the geomblk leaf 0x80051B04) without recompiling a probe.
    // Runs at a frame boundary on the main thread; SIDE EFFECTS ARE REAL (may perturb the game).
    if (!s_ctx) { fprintf(out, "call: no CPU context\n"); }
    else {
      uint32_t args[4] = {0,0,0,0};
      sscanf(line, "%*s %*x %x %x %x %x", &args[0], &args[1], &args[2], &args[3]);
      uint32_t sv[4]; for (int i = 0; i < 4; i++) { sv[i] = s_ctx->r[4+i]; s_ctx->r[4+i] = args[i]; }
      rec_dispatch(s_ctx, a);
      fprintf(out, "call %08X(a0=%08X,a1=%08X,a2=%08X,a3=%08X) -> v0=%08X v1=%08X\n",
              a, args[0], args[1], args[2], args[3], s_ctx->r[2], s_ctx->r[3]);
      for (int i = 0; i < 4; i++) s_ctx->r[4+i] = sv[i];   // restore a0-a3 (callee already used them)
    }
  } else if (!strcmp(cmd, "geomblk") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
    uint32_t T = s_ctx->mem_r32(0x800ECF58u + a*4);
    uint32_t gb = T + s_ctx->mem_r32(T + b*4 + 4);
    fprintf(out, "group=%u sub=%u  T=%08X  geomblk=%08X\n", a, b, T, gb);
  } else if (!strcmp(cmd, "node") && sscanf(line, "%*s %x", &a) == 1) {
    fprintf(out, "node %08X: type=%02X state=%04X rflag=%02X handler=%08X\n", a,
            s_ctx->mem_r8(a+0xc), s_ctx->mem_r16(a+0x28), s_ctx->mem_r8(a+1), s_ctx->mem_r32(a+0x1c));
    fprintf(out, "  pos=(%d,%d,%d) rot=(%d,%d,%d) model=%04X mdata=%08X\n",
            (int16_t)s_ctx->mem_r16(a+0x2e), (int16_t)s_ctx->mem_r16(a+0x32), (int16_t)s_ctx->mem_r16(a+0x36),
            (int16_t)s_ctx->mem_r16(a+0x54), (int16_t)s_ctx->mem_r16(a+0x56), (int16_t)s_ctx->mem_r16(a+0x58),
            s_ctx->mem_r16(a+0xe) & 0x3fff, s_ctx->mem_r32(a+0x38));
    unsigned cnt = s_ctx->mem_r8(a+8);
    fprintf(out, "  cmds(node+8)=%u  next=%08X prev=%08X\n", cnt, s_ctx->mem_r32(a+0x24), s_ctx->mem_r32(a+0x20));
    for (unsigned i = 0; i < cnt && i < 32; i++) {
      uint32_t cmdp = s_ctx->mem_r32(a + 0xc0 + i*4);
      fprintf(out, "    cmd[%u]=%08X geomblk=%08X\n", i, cmdp, cmdp ? s_ctx->mem_r32(cmdp+0x40) : 0);
    }
  } else if (!strcmp(cmd, "ents")) {
    const uint32_t heads[2] = { s_ctx->mem_r32(0x800fb168u), s_ctx->mem_r32(0x800f2624u) };
    int total = 0;
    for (int h = 0; h < 2; h++) {
      fprintf(out, "-- list %d head=%08X --\n", h, heads[h]);
      uint32_t n = heads[h];
      for (int guard = 0; n && guard < 300; guard++, n = s_ctx->mem_r32(n + 0x24)) {
        uint32_t cmd0 = s_ctx->mem_r8(n+8) ? s_ctx->mem_r32(n + 0xc0) : 0;
        fprintf(out, "  %08X t=%02X pos=(%6d,%6d,%6d) h=%08X rf=%u cmds=%u gb0=%08X\n",
                n, s_ctx->mem_r8(n+0xc),
                (int16_t)s_ctx->mem_r16(n+0x2e), (int16_t)s_ctx->mem_r16(n+0x32), (int16_t)s_ctx->mem_r16(n+0x36),
                s_ctx->mem_r32(n+0x1c), s_ctx->mem_r8(n+1), s_ctx->mem_r8(n+8), cmd0 ? s_ctx->mem_r32(cmd0+0x40) : 0);
        total++;
      }
    }
    fprintf(out, "(%d nodes)\n", total);
  } else if (!strcmp(cmd, "stage")) {
    fprintf(out, "stage(0x801fe00c)=%08X sm48(0x801fe048)=%d scene-active(0x800BE258)=%08X\n",
            s_ctx->mem_r32(0x801fe00c), (int)s_ctx->mem_r16(0x801fe048), s_ctx->mem_r32(0x800BE258));
  } else if (!strcmp(cmd, "scene")) {
    gpu_scene_dump_now(s_ctx, out);
  } else if (!strcmp(cmd, "provat") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
    gpu_provat_display(s_ctx, out, (int)a, (int)b);
  } else if (!strcmp(cmd, "shot")) {
    // Capture what is actually PRESENTED: VK readback when VK is the active renderer, else the SW
    // display region. (Under VK the SW s_vram has only uploads, not the rasterized geometry.)
    char path[256] = "scratch/screenshots/dbg.png";
    sscanf(line, "%*s %255s", path);
    if (gpu_vk_enabled()) { gpu_vk_shot(s_ctx, path); fprintf(out, "shot (VK) -> %s\n", path); }
    else                  { gpu_native_shot(s_ctx, path); fprintf(out, "shot (SW) -> %s\n", path); }
  } else if (!strcmp(cmd, "dumpram")) {
    // Full 2MB guest-RAM dump (+ .spad scratchpad sidecar), mirroring the REPL `dumpram`. Lets an
    // interactively-driven debug-server session capture before/after RAM for A/B diffing and feed
    // overlays to `disas.py --ram`. The main-RAM diff is BLIND to scratchpad, hence the sidecar.
    char path[256] = "scratch/bin/dbg_ram.bin";
    sscanf(line, "%*s %255s", path);
    FILE* fp = fopen(path, "wb");
    if (fp) { fwrite(s_ctx->ram, 1, 0x200000, fp); fclose(fp); fprintf(out, "dumpram -> %s\n", path); }
    else { fprintf(out, "dumpram: cannot open %s\n", path); }
    char spath[264]; snprintf(spath, sizeof spath, "%s.spad", path);
    FILE* sp = fopen(spath, "wb");
    if (sp) { fwrite(s_ctx->scratch, 1, sizeof s_ctx->scratch, sp); fclose(sp); fprintf(out, "dumpram scratchpad -> %s\n", spath); }
  } else if (!strcmp(cmd, "shotb")) {
    // SBS only: capture render TARGET 1 (core B / right pane). Plain `shot` reads target 0 (core A).
    char path[256] = "scratch/screenshots/dbg_b.png";
    sscanf(line, "%*s %255s", path);
    void gpu_vk_shot_b(Core*, const char*); gpu_vk_shot_b(s_ctx, path);
    fprintf(out, "shotb (VK target B) -> %s\n", path);
  } else if (!strcmp(cmd, "vkshot")) {
    char path[256] = "scratch/screenshots/dbg_vk.png";
    sscanf(line, "%*s %255s", path);
    gpu_vk_shot(s_ctx, path);
    fprintf(out, "vkshot -> %s\n", path);
  } else if (!strcmp(cmd, "vkstats")) {
    int tri = 0, tex = 0, semi = 0; gpu_vk_stats(s_ctx, &tri, &tex, &semi);
    fprintf(out, "vk last-frame verts: flat-tri=%d (%d tris) textured=%d (%d tris) semi=%d (%d tris)\n",
            tri, tri/3, tex, tex/3, semi, semi/3);
  } else if (!strcmp(cmd, "vkvram")) {
    unsigned x = 0, y = 0, w = 64, h = 64; char path[256] = "scratch/screenshots/dbg_vkvram.ppm";
    sscanf(line, "%*s %u %u %u %u %255s", &x, &y, &w, &h, path);
    gpu_vk_vram_region(s_ctx, path, (int)x, (int)y, (int)w, (int)h);
    fprintf(out, "vkvram (%u,%u %ux%u) -> %s\n", x, y, w, h, path);
  } else if (!strcmp(cmd, "vkpix")) {          // raw 16-bit VRAM word(s) at x,y (dark-outline STP diag)
    unsigned x = 0, y = 0, n = 1; sscanf(line, "%*s %u %u %u", &x, &y, &n);
    uint16_t words[64]; if (n > 64) n = 64;
    void gpu_vk_vram_words(Core*, int, int, int, uint16_t*);
    gpu_vk_vram_words(s_ctx, (int)x, (int)y, (int)n, words);
    fprintf(out, "vkpix (%u,%u x%u):", x, y, n);
    for (unsigned i = 0; i < n; i++) fprintf(out, " %04X", words[i]);
    fprintf(out, "\n");
  } else if (!strcmp(cmd, "debug")) {
    char ch[200] = {0}; sscanf(line, "%*s %199[^\n]", ch);
    void cfg_dbg_set(const char*); cfg_dbg_set(ch);
    fprintf(out, "debug channels = %s\n", ch[0] ? ch : "(none)");
  } else if (!strcmp(cmd, "press") && sscanf(line, "%*s %31s", arg) == 1) {
    s_held &= ~(unsigned short)dbg_btn(arg); s_ctx->game->pad.driveHold(s_held); fprintf(out, "held=%04X\n", s_held);
  } else if (!strcmp(cmd, "release") && sscanf(line, "%*s %31s", arg) == 1) {
    s_held |= (unsigned short)dbg_btn(arg); s_ctx->game->pad.driveHold(s_held); fprintf(out, "held=%04X\n", s_held);
  } else if (!strcmp(cmd, "tap") && sscanf(line, "%*s %31s %u", arg, &a) >= 1) {
    if (!a) a = 4; s_ctx->game->pad.driveTap((unsigned short)(0xFFFF & ~dbg_btn(arg)), (int)a);
    fprintf(out, "tap %s %u\n", arg, a);
  } else if (!strcmp(cmd, "hold") && sscanf(line, "%*s %x", &a) == 1) {
    s_held = (unsigned short)a; s_ctx->game->pad.driveHold(s_held); fprintf(out, "held=%04X\n", s_held);
  } else if (!strcmp(cmd, "padrec")) {
    // Cut a replay out of the LIVE session. Every frame's mask is kept in memory from frame 0, so a
    // session already in progress (user playing, repro on screen) can be captured on demand — no
    // restart into a chosen sink, no racing the incremental file writer.
    char sub[32] = {0}, path[256] = {0}; unsigned nf = 0;
    const size_t have = s_ctx->game->pad.recordedFrames();
    if (sscanf(line, "%*s %31s %255s %u", sub, path, &nf) >= 2 && !strcmp(sub, "save")) {
      const bool ok = s_ctx->game->pad.saveRecording(path, nf);
      fprintf(out, "padrec save -> %s (%zu of %zu frames)%s\n", path,
              (nf && nf < have) ? (size_t)nf : have, have, ok ? "" : " FAILED");
    } else {
      fprintf(out, "padrec: %zu frames captured; 'padrec save <path> [nframes]' to cut a replay\n", have);
    }
  } else if (!strcmp(cmd, "pause")) {
    s_paused = 1; s_step = 0; fprintf(out, "paused at frame %d\n", gpu_frame_no(s_ctx));
  } else if (!strcmp(cmd, "play") || !strcmp(cmd, "resume")) {
    s_paused = 0; s_step = 0; fprintf(out, "resumed\n");
  } else if (!strcmp(cmd, "step")) {
    a = 0; sscanf(line, "%*s %u", &a); if (!a) a = 1;
    s_paused = 1; s_step += (int)a; fprintf(out, "step +%u (frame %d)\n", a, gpu_frame_no(s_ctx));
  } else if (!strcmp(cmd, "frame")) {
    fprintf(out, "frame=%d paused=%d disp=(%d,%d)\n", gpu_frame_no(s_ctx), s_paused, s_ctx->game->gpu.s_disp_x, s_ctx->game->gpu.s_disp_y);
  } else {
    fprintf(out, "? %s  (try 'help')\n", line);
  }
}

// Called once per frame from the native frame loop. Services at most one queued command.
// `c` is the live frame-loop CPU context, stored for the `call` command (runs guest fns at this
// frame boundary). Pass NULL from sites without a context (the call command then reports "no context").
void DbgServer::service(Core* c) {
  if (DbgServerInternals::sInstance != this) return;   // another Game owns the endpoint
  s_ctx = c;
  if (!s_started) return;
  pthread_mutex_lock(&s_mtx);
  if (s_req_pending) {
    char cmd[512]; memcpy(cmd, s_cmd, sizeof cmd);
    pthread_mutex_unlock(&s_mtx);            // run the (possibly slow) dump outside the lock
    char* buf = NULL; size_t len = 0;
    FILE* out = open_memstream(&buf, &len);
    if (out) { dbg_exec(out, cmd); fclose(out); }
    pthread_mutex_lock(&s_mtx);
    s_resp_buf = buf; s_resp_len = len;
    s_req_pending = 0; s_resp_ready = 1;
    pthread_cond_broadcast(&s_done);
  }
  pthread_mutex_unlock(&s_mtx);
}

// Submit one command to the main thread and block (this server thread only) for the result. Uses a
// timed wait so a slow/hung frame can never permanently wedge the server thread — on timeout it
// abandons the request and returns an error line, keeping the accept loop responsive.
static char* dbg_submit(const char* line, size_t* out_len) {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 4;
  char* buf = NULL;   // declared before any `goto timeout` so the jump doesn't cross its init (C++)
  pthread_mutex_lock(&s_mtx);
  while (s_req_pending || s_resp_ready)
    if (pthread_cond_timedwait(&s_done, &s_mtx, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&s_mtx); goto timeout; }
  snprintf(s_cmd, sizeof s_cmd, "%s", line);
  s_req_pending = 1; s_resp_ready = 0;
  while (!s_resp_ready)
    if (pthread_cond_timedwait(&s_done, &s_mtx, &ts) == ETIMEDOUT) {
      s_req_pending = 0;                       // abandon: main may service a stale slot harmlessly
      pthread_mutex_unlock(&s_mtx); goto timeout;
    }
  buf = s_resp_buf; *out_len = s_resp_len;
  s_resp_buf = NULL; s_resp_ready = 0;
  pthread_cond_broadcast(&s_done);           // wake any other server thread waiting to submit
  pthread_mutex_unlock(&s_mtx);
  return buf;
timeout:;
  const char* msg = "(debug server: command timed out — game frozen or busy)\n";
  char* b = (char*)malloc(strlen(msg) + 1); if (b) strcpy(b, msg); *out_len = b ? strlen(msg) : 0;
  return b;
}

// MULTIPLE commands per connection. tools/dbgclient.py's interactive mode keeps ONE socket open and
// sends many newline-delimited commands, reading each reply up to the "---END---\n" terminator; its
// single-shot mode opens a fresh connection per command. Both work here: loop reading commands (handling
// partial / pipelined reads) and reply to each with "---END---". A generous IDLE recv timeout is the
// backstop so a client that connects then dies WITHOUT closing eventually frees the serial accept loop
// instead of wedging it. (Was one-command-per-connection, which broke interactive dbgclient: the 2nd
// command hit an already-closed socket -> BrokenPipeError.)
static void serve_conn(int fd) {
  struct timeval tv = { 120, 0 };                   // idle backstop: drop a stalled/dead client
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char in[1024]; size_t fill = 0;
  for (;;) {
    char* nl = (char*)memchr(in, '\n', fill);       // a pipelined next command may already be buffered
    while (!nl) {
      if (fill >= sizeof in - 1) { close(fd); return; }   // overlong line with no newline: give up
      ssize_t n = read(fd, in + fill, sizeof in - 1 - fill);
      if (n <= 0) { close(fd); return; }            // client closed / recv timeout / error
      fill += (size_t)n; in[fill] = 0;
      nl = (char*)memchr(in, '\n', fill);
    }
    *nl = 0;
    char line[512]; snprintf(line, sizeof line, "%s", in);
    size_t consumed = (size_t)(nl - in) + 1;        // drop this command + newline; keep any remainder
    memmove(in, in + consumed, fill - consumed); fill -= consumed; in[fill] = 0;
    size_t ll = strlen(line); if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
    if (!strcmp(line, "quit") || !strcmp(line, "q") || !strcmp(line, "exit")) { close(fd); return; }
    if (line[0]) {
      size_t rl = 0; char* resp = dbg_submit(line, &rl);
      if (resp && rl && write(fd, resp, rl) < 0) { free(resp); close(fd); return; }
      free(resp);
    }
    if (write(fd, "---END---\n", 10) < 0) { close(fd); return; }   // delimit every reply (incl. blank)
  }
}

static void* dbg_thread(void* arg) {
  int port = (int)(intptr_t)arg;
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) { perror("[dbgsrv] socket"); return NULL; }
  int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(ls, (struct sockaddr*)&sa, sizeof sa) < 0) { perror("[dbgsrv] bind"); close(ls); return NULL; }
  if (listen(ls, 4) < 0) { perror("[dbgsrv] listen"); close(ls); return NULL; }
  cfg_logi("dbgsrv", "listening on 127.0.0.1:%d (PSXPORT_DEBUG_SERVER)", port);
  for (;;) {
    int cs = accept(ls, NULL, NULL);
    if (cs < 0) continue;
    serve_conn(cs);
  }
  return NULL;
}

// Start the debug server thread if PSXPORT_DEBUG_SERVER is set (=1 -> default port, =<n> -> port n).
void DbgServer::start(Core* c) {
  const char* e = cfg_str("PSXPORT_DEBUG_SERVER");
  if (!e || !atoi(e)) return;
  int port = atoi(e); if (port == 1) port = 5959;
  // Claim the process-wide endpoint. In SBS the FIRST Game to reach start() wins the TCP port +
  // the impl-TU accessors below; the other Game's start() no-ops (its listener bind would fail).
  if (!DbgServerInternals::sInstance) DbgServerInternals::sInstance = this;
  if (DbgServerInternals::sInstance != this) return;
  // A debug client that disconnects mid-reply makes the server's write() raise SIGPIPE; with the default
  // disposition that TERMINATES THE WHOLE GAME (a dropped/timed-out dbgclient connection killed the live
  // port — looked like a crash on entering the New-Game cutscene, but the process was merely SIGPIPE'd).
  // Ignore it process-wide so the server just sees write()<0 and drops that one connection.
  signal(SIGPIPE, SIG_IGN);
  c->game->gpu.gpu_provat_enable();                // so `provat` works at any time (not gated on PSXPORT_PROVAT)
  pthread_t t;
  if (pthread_create(&t, NULL, dbg_thread, (void*)(intptr_t)port) != 0) {
    cfg_loge("dbgsrv", "pthread_create failed"); return;
  }
  pthread_detach(t);
  s_started = 1;
}
