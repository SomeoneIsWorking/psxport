// guest_call.h — call a recompiled/overridden guest fn with up to 4 args, running it to its `jr ra`
// return via the native substrate or, for the SBS interpreter oracle, the interpreter dispatch seam.
// Shared by the native scheduler (native_boot.cpp) and the REPL (repl.cpp). static inline: each TU
// gets its own copy, no link conflict.
#pragma once
#include "core.h" // rec_dispatch lives here, inside its `extern "C"` — do NOT re-declare it
static inline void rc0(Core *c, uint32_t fn) {
  c->use_interp ? rec_interp(c, fn) : rec_dispatch(c, fn);
}
static inline void rc1(Core *c, uint32_t fn, uint32_t a0) {
  c->r[4] = a0;
  c->use_interp ? rec_interp(c, fn) : rec_dispatch(c, fn);
}
static inline void rc2(Core *c, uint32_t fn, uint32_t a0, uint32_t a1) {
  c->r[4] = a0;
  c->r[5] = a1;
  c->use_interp ? rec_interp(c, fn) : rec_dispatch(c, fn);
}
static inline void rc3(Core *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4] = a0;
  c->r[5] = a1;
  c->r[6] = a2;
  c->use_interp ? rec_interp(c, fn) : rec_dispatch(c, fn);
}
static inline void rc4(Core *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  c->r[4] = a0;
  c->r[5] = a1;
  c->r[6] = a2;
  c->r[7] = a3;
  c->use_interp ? rec_interp(c, fn) : rec_dispatch(c, fn);
}
