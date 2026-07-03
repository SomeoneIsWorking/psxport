// native_gate.cpp — the PC-native-layer A/B GATE registry. See native_gate.h.
#include "native_gate.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

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
  fprintf(stderr, "[native] gates (%d):\n", mCount);
  for (int i = 0; i < mCount; i++)
    fprintf(stderr, "  %-16s %s\n", mGates[i].name, mGates[i].on ? "on" : "off");
}
