// native_gate.cpp — the PC-native-layer A/B GATE registry (REPL `native <name> on|off`). Each
// PC-native layer that REPLACES game behavior is gated behind a named flag (default ON);
// dropping it routes that behavior back to the recompiled PSX body so it can be A/B-compared
// against the native replacement. Diagnostic-only mechanism, not a shipped behavior toggle.
// Split out of native_boot.cpp (2026-07 restructure) — this has nothing to do with
// booting/frame-stepping.
#include "native_gate.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---- Native-layer GATE registry (user directive 2026-06-23) -------------------------------------
// Each PC-native layer that REPLACES game behavior is gated behind a named flag (DEFAULT ENABLED).
// `native <name> off` at the REPL drops that layer so the recompiled game code (the in-game ORACLE)
// runs in its place — letting us A/B a native layer against the faithful recomp to isolate breakage.
// Names auto-register on first query. Diagnostic-only mechanism (not a shipped behavior toggle).
struct NativeGate { const char* name; int on; };
static NativeGate s_gates[32];
static int s_ngates = 0;
extern "C" int native_gate(const char* name) {
  for (int i = 0; i < s_ngates; i++) if (!strcmp(s_gates[i].name, name)) return s_gates[i].on;
  if (s_ngates < 32) { s_gates[s_ngates] = { strdup(name), 1 }; return s_gates[s_ngates++].on; }  // copy name (REPL buf dangles); default ON
  return 1;
}
void native_gate_set(const char* name, int on) {
  (void)native_gate(name);   // ensure registered
  for (int i = 0; i < s_ngates; i++) if (!strcmp(s_gates[i].name, name)) { s_gates[i].on = on; return; }
}
void native_gate_list() {
  fprintf(stderr, "[native] gates (%d):\n", s_ngates);
  for (int i = 0; i < s_ngates; i++)
    fprintf(stderr, "  %-16s %s\n", s_gates[i].name, s_gates[i].on ? "on" : "off");
}
