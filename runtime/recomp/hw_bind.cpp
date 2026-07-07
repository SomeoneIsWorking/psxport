// hw_bind.cpp — per-instance HW-peripheral state binders (SPU/MDEC/CD-controller/XA). Split out of
// native_boot.cpp (2026-07 restructure): grouped here so a reader can find every per-core HW bind in one
// place instead of buried mid-file in the boot driver.
#include "core.h"
#include "game.h"
#include "hw_bind.h"

// Bind THIS core's per-instance SPU state (Beetle spu.c), lazily powering it on first use. Like gte_bind,
// called per core frame-step + at boot, from the explicit Core — two cores keep SEPARATE SPU state.
// Also binds this core's SPU write log so spu_write appends to A's or B's per-Core log during SBS.
void spu_bind(Core* c) { c->game->spu.bind(); }
// Same for MDEC (per-instance; lazy power on first bind — MDEC has no separate global init).
void mdec_bind(Core* c) { c->game->mdec.bind(); }
// Bind THIS core's per-instance CD-controller (cdc_native.c) and XA streamer (xa_stream.c) state, so two
// cores (native vs PSX-recomp) keep SEPARATE CD state — the recomp core busy-polls the CD registers /
// streams XA, the native core mostly bypasses them via cd_override. Plain-C BindState (cdc_state.h /
// xa_state.h), same per-frame-step contract as gte/spu/mdec.
void cdc_bind(Core* c) { cdc_bind_state(&c->game->cdc); }   // decls in cdc_state.h (via game.h, extern "C")
void xa_bind(Core* c)  { xa_bind_state(&c->game->xa); }     // decls in xa_state.h  (via game.h, extern "C")
