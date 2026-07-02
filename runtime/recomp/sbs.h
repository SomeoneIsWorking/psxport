// class Sbs — the LIVE two-core side-by-side divergence debugger (PSXPORT_SBS=1).
//
// One singleton per process. All harness state (mode, frame counter, both Game handles, per-pane
// RGBA buffers, divergence/write-watch record, scripted-input list, dbg-server pause state) lives on
// the instance — no file-scope globals. See sbs.cpp for the design commentary and the docs at the top
// of that file for the mode matrix (render / gameplay / full / oracle).
//
// PUBLIC API (call sites in other TUs):
//   Sbs::run(exePath)           process entry point from boot.cpp when PSXPORT_SBS=1
//   Sbs::active()               true while the harness is running (native_fmv / native_boot gate off it)
//   Sbs::coreId(c)              -1 if not active, 0 for A, 1 for B (overlay_router tag)
//   Sbs::frame()                lockstep frame counter (overlay_router tag)
//   Sbs::dbgCmd(out, line)      debug-server `sbs …` command dispatcher (from dbg_server.cpp)
//   Sbs::storeCb(c, addr, val)  write-watch callback (installed on mem via mem_set_store_watch_cb)
#pragma once
#include <cstdio>
#include <cstdint>
struct Core;

class Sbs {
public:
  // Process entry point — never returns (owns the process from PSXPORT_SBS=1 onward).
  static void run(const char* exePath);

  static bool     active();
  static int      coreId(Core* c);
  static uint32_t frame();
  static int      dbgCmd(FILE* out, const char* line);
  static void     storeCb(Core* c, uint32_t addr, uint32_t val);
};
