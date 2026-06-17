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
//   shot [path]           — screenshot of the presented display region (PPM); default scratch/screenshots/dbg.ppm
//   gputrace [path]       — arm a gpu_differ GP0 capture of the NEXT frame; default scratch/bin/dbg_gp0.bin
//   sbs [0|1]             — toggle / set the Vulkan-vs-Software side-by-side present view
//   frame                 — current present-frame counter
//
// Drive it from the repo with tools/dbgclient.py (or `nc 127.0.0.1 5959`).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// --- guest RAM + GPU primitives provided by the rest of the port ---------------------------------
uint8_t  mem_r8(uint32_t);
uint16_t mem_r16(uint32_t);
uint32_t mem_r32(uint32_t);
void gpu_scene_dump_now(FILE* out);
void gpu_provat_display(FILE* out, int qx, int qy);
void gpu_provat_enable(void);
int  gpu_gputrace_arm(const char* path);
void gpu_native_shot(const char* path);
int  gpu_sbs_get(void);
void gpu_sbs_set(int on);
int  gpu_frame_no(void);
int  gpu_vk_enabled(void);
void gpu_vk_shot(const char* path);
void gpu_vk_stats(int* tri, int* tex, int* semi);
void gpu_vk_vram_region(const char* path, int x, int y, int w, int h);

// --- main<->server handoff: a single pending request, serviced on the main thread once per frame --
static pthread_mutex_t s_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_done = PTHREAD_COND_INITIALIZER;   // signalled by main when a result is ready
static char   s_cmd[512];        // command awaiting service (server -> main)
static int    s_req_pending;     // 1 while a command is queued for the main thread
static int    s_resp_ready;      // 1 once the main thread has produced a result
static char*  s_resp_buf;        // malloc'd result (main -> server); server frees after sending
static size_t s_resp_len;
static int    s_started;

// Execute one command, writing its textual result into `out` (a memstream). MAIN THREAD ONLY.
static void dbg_exec(FILE* out, const char* line) {
  char cmd[32] = {0}, arg[256] = {0};
  unsigned a = 0, b = 0;
  if (sscanf(line, "%31s", cmd) != 1) { fprintf(out, "(empty)\n"); return; }

  if (!strcmp(cmd, "help")) {
    fprintf(out,
      "commands:\n"
      "  r <hex> [n]      read n bytes of guest RAM (default 16)\n"
      "  rw <hex> [n]     read n words of guest RAM (default 8)\n"
      "  stage            scene/stage latches\n"
      "  scene            classified display list of the current frame\n"
      "  provat <x> <y>   which prim drew each displayed pixel around (x,y)\n"
      "  shot [path]      screenshot of the presented output (VK readback if VK active, else SW)\n"
      "  vkshot [path]    force a VK-rendered readback to PPM\n"
      "  vkstats          last frame's VK batched vertex counts (flat/textured/semi)\n"
      "  gputrace [path]  arm a gpu_differ GP0 capture of the next frame\n"
      "  sbs [0|1]        toggle/set Vulkan-vs-Software side-by-side view\n"
      "  frame            current present-frame counter\n");
  } else if (!strcmp(cmd, "r") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
    if (!b) b = 16; if (b > 256) b = 256;
    fprintf(out, "%08X:", a);
    for (unsigned i = 0; i < b; i++) fprintf(out, " %02X", mem_r8(a + i));
    fprintf(out, "\n");
  } else if (!strcmp(cmd, "rw") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
    if (!b) b = 8; if (b > 64) b = 64;
    fprintf(out, "%08X:", a);
    for (unsigned i = 0; i < b; i++) fprintf(out, " %08X", mem_r32(a + i * 4));
    fprintf(out, "\n");
  } else if (!strcmp(cmd, "stage")) {
    fprintf(out, "stage(0x801fe00c)=%08X sm48(0x801fe048)=%d scene-active(0x800BE258)=%08X\n",
            mem_r32(0x801fe00c), (int)mem_r16(0x801fe048), mem_r32(0x800BE258));
  } else if (!strcmp(cmd, "scene")) {
    gpu_scene_dump_now(out);
  } else if (!strcmp(cmd, "provat") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
    gpu_provat_display(out, (int)a, (int)b);
  } else if (!strcmp(cmd, "shot")) {
    // Capture what is actually PRESENTED: VK readback when VK is the active renderer, else the SW
    // display region. (Under VK the SW s_vram has only uploads, not the rasterized geometry.)
    char path[256] = "scratch/screenshots/dbg.ppm";
    sscanf(line, "%*s %255s", path);
    if (gpu_vk_enabled()) { gpu_vk_shot(path); fprintf(out, "shot (VK) -> %s\n", path); }
    else                  { gpu_native_shot(path); fprintf(out, "shot (SW) -> %s\n", path); }
  } else if (!strcmp(cmd, "vkshot")) {
    char path[256] = "scratch/screenshots/dbg_vk.ppm";
    sscanf(line, "%*s %255s", path);
    gpu_vk_shot(path);
    fprintf(out, "vkshot -> %s\n", path);
  } else if (!strcmp(cmd, "vkstats")) {
    int tri = 0, tex = 0, semi = 0; gpu_vk_stats(&tri, &tex, &semi);
    fprintf(out, "vk last-frame verts: flat-tri=%d (%d tris) textured=%d (%d tris) semi=%d (%d tris)\n",
            tri, tri/3, tex, tex/3, semi, semi/3);
  } else if (!strcmp(cmd, "vkvram")) {
    unsigned x = 0, y = 0, w = 64, h = 64; char path[256] = "scratch/screenshots/dbg_vkvram.ppm";
    sscanf(line, "%*s %u %u %u %u %255s", &x, &y, &w, &h, path);
    gpu_vk_vram_region(path, (int)x, (int)y, (int)w, (int)h);
    fprintf(out, "vkvram (%u,%u %ux%u) -> %s\n", x, y, w, h, path);
  } else if (!strcmp(cmd, "gputrace")) {
    char path[256] = "scratch/bin/dbg_gp0.bin";
    sscanf(line, "%*s %255s", path);
    int tf = gpu_gputrace_arm(path);
    fprintf(out, "gputrace armed for frame %d -> %s (appears after one frame)\n", tf, path);
  } else if (!strcmp(cmd, "sbs")) {
    if (sscanf(line, "%*s %u", &a) == 1) gpu_sbs_set((int)a);
    else gpu_sbs_set(!gpu_sbs_get());
    fprintf(out, "sbs=%d (Vulkan|Software side-by-side)\n", gpu_sbs_get());
  } else if (!strcmp(cmd, "frame")) {
    fprintf(out, "frame=%d\n", gpu_frame_no());
  } else {
    fprintf(out, "? %s  (try 'help')\n", line);
  }
}

// Called once per frame from the native frame loop. Services at most one queued command.
void dbg_server_service(void) {
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

// Submit one command to the main thread and block (this server thread only) for the result.
static char* dbg_submit(const char* line, size_t* out_len) {
  pthread_mutex_lock(&s_mtx);
  while (s_req_pending || s_resp_ready) pthread_cond_wait(&s_done, &s_mtx);  // serialize
  snprintf(s_cmd, sizeof s_cmd, "%s", line);
  s_req_pending = 1; s_resp_ready = 0;
  while (!s_resp_ready) pthread_cond_wait(&s_done, &s_mtx);
  char* buf = s_resp_buf; *out_len = s_resp_len;
  s_resp_buf = NULL; s_resp_ready = 0;
  pthread_cond_broadcast(&s_done);           // wake any other server thread waiting to submit
  pthread_mutex_unlock(&s_mtx);
  return buf;
}

static void serve_conn(int fd) {
  char in[1024]; size_t fill = 0;
  for (;;) {
    ssize_t n = read(fd, in + fill, sizeof in - 1 - fill);
    if (n <= 0) break;
    fill += (size_t)n; in[fill] = 0;
    char* nl;
    while ((nl = memchr(in, '\n', fill)) != NULL) {
      *nl = 0;
      char line[512]; snprintf(line, sizeof line, "%s", in);
      // strip a trailing CR
      size_t ll = strlen(line); if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
      size_t rest = fill - (size_t)(nl + 1 - in);
      memmove(in, nl + 1, rest); fill = rest; in[fill] = 0;
      if (!line[0]) continue;
      size_t rl = 0; char* resp = dbg_submit(line, &rl);
      if (resp && rl) { (void)!write(fd, resp, rl); }
      free(resp);
      const char* end = "---END---\n";
      (void)!write(fd, end, strlen(end));
    }
    if (fill >= sizeof in - 1) fill = 0;   // overlong line with no newline: drop
  }
  close(fd);
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
  fprintf(stderr, "[dbgsrv] listening on 127.0.0.1:%d (PSXPORT_DEBUG_SERVER)\n", port);
  for (;;) {
    int cs = accept(ls, NULL, NULL);
    if (cs < 0) continue;
    serve_conn(cs);
  }
  return NULL;
}

// Start the debug server thread if PSXPORT_DEBUG_SERVER is set (=1 -> default port, =<n> -> port n).
void dbg_server_start(void) {
  const char* e = getenv("PSXPORT_DEBUG_SERVER");
  if (!e || !atoi(e)) return;
  int port = atoi(e); if (port == 1) port = 5959;
  gpu_provat_enable();                 // so `provat` works at any time (not gated on PSXPORT_PROVAT)
  pthread_t t;
  if (pthread_create(&t, NULL, dbg_thread, (void*)(intptr_t)port) != 0) {
    fprintf(stderr, "[dbgsrv] pthread_create failed\n"); return;
  }
  pthread_detach(t);
  s_started = 1;
}
