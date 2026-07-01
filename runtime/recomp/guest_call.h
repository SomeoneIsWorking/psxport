// guest_call.h — call a recompiled/overridden guest fn with up to 4 args, running it to its
// `jr ra` return via rec_dispatch. Shared by the native scheduler (native_boot.cpp) and the REPL
// (repl.cpp). static inline: each TU gets its own copy, no link conflict.
#pragma once
#include "core.h"
void rec_dispatch(Core*, uint32_t);
static inline void rc0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static inline void rc1(Core* c, uint32_t fn, uint32_t a0) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline void rc2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) { c->r[4]=a0; c->r[5]=a1; rec_dispatch(c, fn); }
static inline void rc3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) { c->r[4]=a0; c->r[5]=a1; c->r[6]=a2; rec_dispatch(c, fn); }
static inline void rc4(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { c->r[4]=a0; c->r[5]=a1; c->r[6]=a2; c->r[7]=a3; rec_dispatch(c, fn); }
