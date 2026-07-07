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
void  SPU_PeekRAM(uint8_t* dst);       // copy the BOUND instance's 512 KB SPU RAM (observable compares)
void  SPU_PokeRAM(const uint8_t* src); // restore it (SV_CHECK leg isolation)

// SBS SPU write log (spu_beetle.c). Per-Game log buffer of (addr, val) pairs — spu_write appends
// to the currently-bound log via spu_bind_log. SBS resets both cores' logs at frame start and
// compares them at frame end (Issue #29: catch audio-relevant divergences).
void*     spu_new_log(void);                       // allocate a fresh SpuWriteLog
void      spu_log_reset(void* log);                // clear count (call at frame start)
void      spu_bind_log(void* log);                 // set active log (null -> no logging)
uint32_t  spu_log_count(void* log);                // number of writes recorded
uint32_t  spu_log_entry(void* log, uint32_t i, int hi);   // entry i's addr (hi=0) or val (hi=1)
#ifdef __cplusplus
}
#endif
