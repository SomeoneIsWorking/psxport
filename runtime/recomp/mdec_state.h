// mdec_state.h — per-instance MDEC state handle (psxport, 2026-06-24). Mirror of spu_state.h: the Beetle
// MDEC (mednafen/psx/mdec.c) kept all mutable state file-scope; it now lives in a heap-allocated MdecState
// owned per Game, bound via MDEC_BindState (set per core frame-step) so two cores keep SEPARATE MDEC state.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void* MDEC_NewState(void);
void  MDEC_FreeState(void* p);
void  MDEC_BindState(void* p);   // make `p` the active MDEC instance (null -> the shared default)
void  MDEC_Power(void);          // reset the BOUND MDEC state to power-on
#ifdef __cplusplus
}
#endif
