// gpu_trace.c — GP0-stream capture for the cross-renderer differ (carved out of gpu_native.c).
//
// Records, for ONE target frame, a snapshot of VRAM at frame start plus the EXACT GP0 word stream
// fed to gpu_gp0() during that frame. Replaying that file through BOTH our renderer and Beetle's
// (tools/gpu_differ) from the identical initial VRAM makes any output-VRAM difference a pure
// rasterizer-fidelity difference — no live game-state alignment needed (the whole point: our HLE
// port and the full-emulation oracle run at different timings, so we can't align by frame number;
// feeding the same primitive stream to both rasterizers sidesteps that entirely).
//
// File format ("GP0TRC01"): magic[8], u32 frame, u32 word_count, u32 vram_w(1024), u32 vram_h(512),
// then vram_w*vram_h u16 (initial VRAM), then word_count u32 (the GP0 stream, in feed order).
// PSXPORT_GPUTRACE="frame[,frame...][:path]". A single frame writes to `path` exactly (default
// scratch/bin/gp0trace.bin) — back-compat with the differ pipeline. A comma-separated LIST writes
// one file per frame at "<path>_f<N>.bin" (so a single deterministic run captures many scenes; the
// game is deterministic under PSXPORT_FORCE_BUTTONS, so frame N is reproducible). Targets are kept
// in feed order; we capture them one at a time as s_frame reaches each.
//
// trace_record() is called per GP0 word from gpu_gp0(); trace_flush() once per frame from
// gpu_present_ex(); gpu_gputrace_arm() arms an on-demand single-frame capture (live debug server).
#include "gpu_native_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_MAX 64
static int        s_trace_on = -1;       // -1 lazy, 0 off, 1 armed
static int        s_trace_frames[TRACE_MAX]; // target frames
static int        s_trace_count;         // number of targets
static int        s_trace_idx;           // index of the target currently being captured
static int        s_trace_multi;         // 1 if a list (per-frame filenames), 0 if single exact path
static const char* s_trace_path = "scratch/bin/gp0trace.bin";
static uint16_t*  s_trace_init;          // VRAM snapshot at frame start
static uint32_t*  s_trace_words;         // captured GP0 words (grown)
static size_t     s_trace_cap, s_trace_n;
static int        s_trace_inited;

static void trace_init_env(void) {
  const char* e = getenv("PSXPORT_GPUTRACE");
  s_trace_on = e ? 1 : 0;
  if (!e) return;
  const char* c = strchr(e, ':');
  if (c) s_trace_path = c + 1;
  s_trace_multi = (strchr(e, ',') != NULL);
  const char* p = e;
  while (*p && p != c && s_trace_count < TRACE_MAX) {
    s_trace_frames[s_trace_count++] = atoi(p);
    const char* nc = strchr(p, ',');
    if (!nc || (c && nc > c)) break;
    p = nc + 1;
  }
}

void trace_record(uint32_t w) {
  if (s_trace_on < 0) trace_init_env();
  if (s_trace_on <= 0 || s_trace_idx >= s_trace_count || s_frame != s_trace_frames[s_trace_idx]) return;
  if (!s_trace_inited) {
    if (!s_trace_init) s_trace_init = (uint16_t*)malloc(sizeof(uint16_t) * VRAM_W * VRAM_H);
    memcpy(s_trace_init, s_vram, sizeof(uint16_t) * VRAM_W * VRAM_H);
    s_trace_inited = 1;
  }
  if (s_trace_n >= s_trace_cap) {
    s_trace_cap = s_trace_cap ? s_trace_cap * 2 : 65536;
    s_trace_words = (uint32_t*)realloc(s_trace_words, s_trace_cap * sizeof(uint32_t));
  }
  s_trace_words[s_trace_n++] = w;
}

void trace_flush(void) {  // called from gpu_present while s_frame is still the target
  if (s_trace_on <= 0 || s_trace_idx >= s_trace_count || !s_trace_inited ||
      s_frame != s_trace_frames[s_trace_idx]) return;
  char namebuf[512];
  const char* path = s_trace_path;
  if (s_trace_multi) { snprintf(namebuf, sizeof namebuf, "%s_f%d.bin", s_trace_path, s_frame); path = namebuf; }
  FILE* f = fopen(path, "wb");
  if (f) {
    uint32_t meta[4] = { (uint32_t)s_frame, (uint32_t)s_trace_n, VRAM_W, VRAM_H };
    fwrite("GP0TRC01", 1, 8, f);
    fwrite(meta, 4, 4, f);
    fwrite(s_trace_init, 2, VRAM_W * VRAM_H, f);
    fwrite(s_trace_words, 4, s_trace_n, f);
    fclose(f);
    fprintf(stderr, "[gputrace] f%d -> %s (%zu words)\n", s_frame, path, s_trace_n);
  }
  s_trace_idx++;            // advance to the next target frame; reset the per-frame capture
  s_trace_inited = 0;
  s_trace_n = 0;
}

// On-demand GP0-trace arm for the live debug server (dbg_server.c): capture the NEXT present frame's
// GP0 stream + start-VRAM to `path` (single exact file, gpu_differ format). Mirrors the PSXPORT_GPUTRACE
// path but armed at runtime for s_frame+1 instead of from the env. Returns the target frame number.
static char s_trace_arm_path[512];
int gpu_gputrace_arm(const char* path) {
  snprintf(s_trace_arm_path, sizeof s_trace_arm_path, "%s", path && *path ? path : "scratch/bin/dbg_gp0.bin");
  s_trace_path = s_trace_arm_path;
  s_trace_multi = 0;
  s_trace_count = 1;
  s_trace_idx = 0;
  s_trace_frames[0] = s_frame + 1;   // next frame (this one's stream is already partly fed)
  s_trace_inited = 0;
  s_trace_n = 0;
  s_trace_on = 1;
  return s_frame + 1;
}
