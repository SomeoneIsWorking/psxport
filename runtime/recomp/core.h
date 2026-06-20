// Core — ONE emulator instance, as a C++ object.
//
// The runtime was historically a single global machine (g_ram, g_scratch, and ~hundreds of
// file-scope statics). To run TWO cores in one process side-by-side (e.g. overrides-ON vs
// overrides-OFF for visual A/B), all per-instance state lives in `class Core` and the code that
// operates on it are METHODS on that object. Two cores = two Core instances; you call
// a.run_frame() / b.run_frame() and each operates on its own members via `this`. There is NO
// global active-instance pointer and nothing is swapped per frame — the instance is reached
// explicitly (it is the `this` of a method, or an explicit Core* parameter at the boundaries).
//
// Core publicly inherits R3000 so the register file is reached as `c->r[..]`, `c->hi`, `c->lo`
// exactly as before (the interpreter already threads the CPU handle through every path); the
// handle's type just changes from R3000* to Core*. Memory is a set of methods (mem_r32, ...),
// so inside any Core method a bare `mem_r32(a)` resolves to `this->mem_r32(a)`.
#pragma once
#include <stdint.h>
#include "r3000.h"

#ifdef __cplusplus

class Game;   // the whole-machine owner (game.h); Core::game points back to it so any code holding a
              // Core* reaches migrated subsystem state via c->game->... (de-globalization, 2026-06-19).

class Core : public R3000 {
public:
  // ---- Memory (2 MB main RAM mirrored across KUSEG/KSEG0/KSEG1; 1 KB scratchpad) ----
  uint8_t ram[0x200000];
  uint8_t scratch[0x400];

  Game* game = nullptr;   // back-pointer to the owning Game (set by Game's constructor)

  uint32_t io_gpustat_toggle = 0;  // GPUSTAT (0x1F801814) even/odd line bit — per-instance HW state

  Core();

  // Memory access (delegates to host_ptr / the I/O map). PSX is little-endian == host.
  uint8_t  mem_r8 (uint32_t a);
  uint16_t mem_r16(uint32_t a);
  uint32_t mem_r32(uint32_t a);
  void     mem_w8 (uint32_t a, uint8_t  v);
  void     mem_w16(uint32_t a, uint16_t v);
  void     mem_w32(uint32_t a, uint32_t v);
  uint32_t mem_lwl(uint32_t cur, uint32_t a);
  uint32_t mem_lwr(uint32_t cur, uint32_t a);
  void     mem_swl(uint32_t a, uint32_t v);
  void     mem_swr(uint32_t a, uint32_t v);

  // Store watchpoints (REPL `watch` / PSXPORT_CW / PSXPORT_WWATCH).
  void mem_set_watch(uint32_t lo, uint32_t hi);
  int  mem_watch_hits();

private:
  uint8_t* host_ptr(uint32_t a, uint32_t bytes);
  uint32_t io_read (uint32_t a, uint32_t bytes);
  void     io_write(uint32_t a, uint32_t v, uint32_t bytes);
  void     cw_check(uint32_t a, uint32_t v, int width);
  void     wwatch_check(uint32_t a, uint32_t v);

  // DMA channel state (per-instance) — DMA0 MDEC-in, 1 MDEC-out, 2 GPU, 4 SPU, 6 OTC.
  uint32_t s_dma0_madr=0, s_dma0_bcr=0, s_dma0_chcr=0;
  uint32_t s_dma1_madr=0, s_dma1_bcr=0, s_dma1_chcr=0;
  uint32_t s_dma2_madr=0, s_dma2_bcr=0, s_dma2_chcr=0;
  uint32_t s_dma4_madr=0, s_dma4_bcr=0, s_dma4_chcr=0;
  uint32_t s_dma6_madr=0, s_dma6_bcr=0, s_dma6_chcr=0;
  uint32_t s_dma_buf[0x10000];

  // Watchpoint state.
  int      s_cw_init=0, s_cw_n=0;
  uint32_t s_cw_lo=0, s_cw_hi=0;
  int      s_ww_init=0;
  uint32_t s_ww_lo=0, s_ww_hi=0;
};

// A native override now receives its instance explicitly (was R3000*). c->r[] still works
// (Core : R3000); c->mem_r32() etc. reach this instance's memory — no global.
typedef void (*OverrideFn)(Core*);

extern "C" {

// The PC currently being interpreted (watchdog/diagnostics). Defined in interp.cpp.
extern volatile uint32_t g_interp_pc;

// ---- Dispatch & traps (interp.cpp / dispatch.cpp / hle.cpp) ----
void rec_dispatch(Core* c, uint32_t addr);
void rec_dispatch_miss(Core* c, uint32_t addr);
void rec_syscall(Core* c, uint32_t code);
void rec_break(Core* c, uint32_t code);
void rec_interp(Core* c, uint32_t pc);     // synchronous nested call (super-call / RAM-code)
void rec_coro_run(Core* c, uint32_t pc);   // cooperative task entry

// Native overrides (recomp-overrides): register a hand-written native fn keyed by entry address.
void rec_set_override(uint32_t addr, OverrideFn fn);
void rec_set_interp_override(uint32_t addr, OverrideFn fn);
void rec_set_interp_override_auto(uint32_t addr, OverrideFn fn);
void rec_set_overlay_load_hook(void (*fn)(Core* c, uint32_t base, uint32_t size));
void rec_overlay_loaded(Core* c, uint32_t base, uint32_t size);
int  rec_func_index(uint32_t addr);
OverrideFn rec_interp_override_for(uint32_t a);

// ---- COP0 (minimal) ----
uint32_t cop0_mfc(Core* c, uint32_t reg);
void     cop0_mtc(Core* c, uint32_t reg, uint32_t v);

// ---- COP2 / GTE ----
uint32_t gte_read_data (uint32_t reg);
void     gte_write_data(uint32_t reg, uint32_t v);
uint32_t gte_read_ctrl (uint32_t reg);
void     gte_write_ctrl(uint32_t reg, uint32_t v);
void     gte_op(Core* c, uint32_t insn);

// R3000 integer division semantics (no traps; defined /0 + overflow results).
void cpu_div (Core* c, uint32_t n, uint32_t d);
void cpu_divu(Core* c, uint32_t n, uint32_t d);

// ---- Subsystem entry points that read/write this instance's RAM (need the Core) ----
void gpu_dma2_linked_list(Core* c, uint32_t madr);
void gpu_dma2_block(Core* c, uint32_t madr, int count, int to_gpu);

} // extern "C"

// ---- Native renderer (gpu_native.cpp) — C++ linkage; take the Core for guest-RAM reads / DMA /
// per-frame present bookkeeping (no global). gpu_gp1 is display control (no RAM) but kept here. ----
void gpu_gp0(Core* core, uint32_t w);
void gpu_gp1(uint32_t w);
void gpu_present(Core* core);
void gpu_present_ex(Core* core, int do_blit);
// M3 provenance: an owned background drawer's override records the KSEG0 packet-pool span [lo,hi) it
// produced this frame, so the OT walk classifies those prims as RQ_BACKGROUND (engine_submit.cpp).
void gpu_bg_range_add(Core* core, uint32_t lo, uint32_t hi);
void gpu_pace_frame(Core* core);
void gpu_pace_subframe(Core* core, int parts);
void gpu_native_load_image(Core* core, int x, int y, int w, int h, uint32_t src);
int  native_fmv_play(Core* core, const char* path);
int  native_fmv_play_lba(Core* core, uint32_t lba, uint32_t size_bytes);
void fps60_frame_commit(Core* core);

#endif // __cplusplus
