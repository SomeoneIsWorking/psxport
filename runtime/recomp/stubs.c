// S1 stubs for runtime hooks not yet implemented (GTE, COP0, syscalls). These let the
// recompiled core compile and link so the emitter can be verified now; each is replaced
// by a real implementation in later stages (S2 HLE, S3 GTE).
#include "r3000.h"
#include <stdio.h>

// GTE (COP2) is now the real Beetle implementation — see gte_beetle.c. COP0 stays minimal
// here (HLE BIOS handles exceptions; the game only touches a few COP0 regs).
uint32_t cop0_mfc(R3000* c, uint32_t reg)       { (void)c; (void)reg; return 0; }
void     cop0_mtc(R3000* c, uint32_t reg, uint32_t v) { (void)c; (void)reg; (void)v; }

// rec_syscall / rec_break live in hle.c (kernel concern).
