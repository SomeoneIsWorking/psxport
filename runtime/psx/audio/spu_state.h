// spu_state.h — Beetle spu.c's C API: per-instance state handles, register file, DMA and mixer.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *SPU_NewState(void); // allocate+zero a fresh per-instance SPU state
void SPU_FreeState(void *p);
void SPU_BindState(void *p); // make `p` the active SPU instance (null -> the shared default)
void SPU_Init(void);
void SPU_Power(void); // reset the BOUND state to power-on (after SPU_Init has run once globally)
void SPU_Write(int32_t timestamp, uint32_t A, uint16_t V);
uint16_t SPU_Read(int32_t timestamp, uint32_t A);
void SPU_WriteDMA(uint32_t V);
uint32_t SPU_ReadDMA(void);
int32_t SPU_UpdateFromCDC(int32_t clocks);
int SPU_Render(int16_t *out, int max_frames); // drains the BOUND state's output ring
#ifdef __cplusplus
}
#endif
