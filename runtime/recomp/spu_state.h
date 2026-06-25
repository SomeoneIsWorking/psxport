// spu_state.h — per-instance SPU state handle (psxport, 2026-06-24). The Beetle SPU (mednafen/psx/spu.c)
// kept all its mutable state file-scope; it now lives in a heap-allocated SpuState owned per Game, bound
// to the SPU code via SPU_BindState (set per core frame-step) so two cores keep SEPARATE SPU state. The
// struct layout stays private to spu.c; the C++ side only holds the opaque pointer + binds it.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void* SPU_NewState(void);     // allocate+zero a fresh per-instance SPU state
void  SPU_FreeState(void* p);
void  SPU_BindState(void* p); // make `p` the active SPU instance (null -> the shared default)
void  SPU_Power(void);        // reset the BOUND state to power-on (after SPU_Init has run once globally)
#ifdef __cplusplus
}
#endif
