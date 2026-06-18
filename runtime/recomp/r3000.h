// R3000A register file — the CPU state of one emulator instance.
//
// Historically this header was the whole runtime ABI (memory, dispatch, HLE, GTE) shared with
// generated code. The static recompiler was dropped from the build (the runtime is interpreter-
// only, journal later-101), and the runtime is being made object-oriented: a `Core` (core.h)
// publicly inherits this R3000 register file and owns memory + every subsystem as members. So
// this header is now just the register struct; the rest of the ABI lives on Core in core.h.
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct R3000 {
  uint32_t r[32];   // GPRs; r[0] is hardwired 0
  uint32_t hi, lo;  // mult/div result registers
} R3000;

#ifdef __cplusplus
}
#endif
