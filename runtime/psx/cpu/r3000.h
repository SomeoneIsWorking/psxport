// R3000A register file — the CPU state of one emulator instance.
//
// This header is the register portion of the runtime ABI. `Core` publicly inherits the register
// file and owns memory plus every subsystem as members; the rest of the ABI lives on Core in
// core.h.
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct R3000 {
  uint32_t r[32];  // GPRs; r[0] is hardwired 0
  uint32_t hi, lo; // mult/div result registers
  uint32_t pc;     // program counter — PER-CORE and synchronized at every runtime executor exit.
                   // Diagnostics therefore report the architectural PC for this Core; there is no
                   // process-global "current PC", and two Cores retain independent state.
} R3000;

#ifdef __cplusplus
}
#endif
