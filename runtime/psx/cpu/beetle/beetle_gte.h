// beetle_gte.h — the Beetle GTE's C API (mednafen/psx/gte.c). Product code reaches the GTE through
// psx::cpu::Gte; only its adapter (gte_beetle.cpp), the oracle and the Beetle seam tests include this.
#pragma once

#include "gte_state.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void GTE_Init(void);
void GTE_Power(void);
int32_t GTE_Instruction(uint32_t instr);
void GTE_WriteCR(unsigned which, uint32_t value);
void GTE_WriteDR(unsigned which, uint32_t value);
uint32_t GTE_ReadCR(unsigned which);
uint32_t GTE_ReadDR(unsigned which);
// Makes `s` the register file every call above reads and writes; null selects Beetle's default file.
void GTE_BindState(GteRegs *s);
GteRegs *GTE_CurState(void);
// Runs one instruction against `s` and restores the caller's binding; -1 for a null state. Test reference only.
int32_t GTE_ExecuteIsolated(GteRegs *s, uint32_t instr);
#ifdef __cplusplus
}
#endif
