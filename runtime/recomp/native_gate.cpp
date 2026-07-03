// native_gate.cpp — the PC-native-layer A/B GATE registry (REPL `native <name> on|off`).
// See native_gate.h for the design commentary. One singleton per process, 32 entries max.
#include "native_gate.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

NativeGates& NativeGates::instance() {
  static NativeGates inst;
  return inst;
}

int NativeGates::get(const char* name) {
  for (int i = 0; i < mCount; i++) if (!strcmp(mGates[i].name, name)) return mGates[i].on;
  if (mCount < 32) { mGates[mCount] = { strdup(name), 1 }; return mGates[mCount++].on; }  // copy name (REPL buf dangles); default ON
  return 1;
}

void NativeGates::set(const char* name, int on) {
  (void)get(name);   // ensure registered
  for (int i = 0; i < mCount; i++) if (!strcmp(mGates[i].name, name)) { mGates[i].on = on; return; }
}

void NativeGates::list() const {
  fprintf(stderr, "[native] gates (%d):\n", mCount);
  for (int i = 0; i < mCount; i++)
    fprintf(stderr, "  %-16s %s\n", mGates[i].name, mGates[i].on ? "on" : "off");
}

// ---- Legacy free-function bridges — one-liners over the singleton -------------------------------
extern "C" {
int  native_gate(const char* name)                 { return NativeGates::instance().get(name); }
void native_gate_set(const char* name, int on)     { NativeGates::instance().set(name, on); }
void native_gate_list(void)                        { NativeGates::instance().list(); }
}
