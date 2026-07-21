// native_gate.cpp — the PC-native-layer A/B GATE registry. See native_gate.h.
#include "native_gate.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "cfg.h"

int NativeGates::get(const char* name) {
  for (int i = 0; i < mCount; i++) if (!strcmp(mGates[i].name, name)) return mGates[i].on;
  if (mCount < 32) { mGates[mCount] = { strdup(name), 1 }; return mGates[mCount++].on; }  // copy name; default ON
  return 1;
}

void NativeGates::set(const char* name, int on) {
  (void)get(name);   // ensure registered
  for (int i = 0; i < mCount; i++) if (!strcmp(mGates[i].name, name)) { mGates[i].on = on; return; }
}

void NativeGates::list() const {
  cfg_logi("native", "gates (%d):", mCount);
  for (int i = 0; i < mCount; i++)
    cfg_logi("native_gate", "  %-16s %s", mGates[i].name, mGates[i].on ? "on" : "off");
}
