// cdc_state.h — per-instance native CD-controller register state (cdc_native.c), so two Cores run with
// SEPARATE CD-controller state (one native, one PSX-recomp). The CXD1199-style register model
// (index/bank, param/response/data FIFOs, the pending-interrupt queue, drive/read state) used to live
// in file-scope statics in cdc_native.c; that mutable state now lives in this struct, one per Game
// (game.h embeds it). cdc_read/cdc_write take the instance EXPLICITLY (the MMIO dispatcher in
// mem.cpp passes &game->cdc) — no bound "current" pointer — so each core reads/writes only its own
// CD registers; nothing is shared (the disc image itself is a read-only data source, see disc.c).
//
// Plain-C struct (no C++) so cdc_native.c stays C, exactly like gte_state.h's GteRegs.
#pragma once
#include <stdint.h>

typedef struct CdcIrqEnt { uint8_t type; uint8_t resp[16]; int len; } CdcIrqEnt;

typedef struct CdcState {
  int      index;             // 0x1F801800 low 2 bits (register bank)       (was s_index)
  uint8_t  param[16];         // param FIFO                                  (was s_param)
  int      param_n;           //                                            (was s_param_n)
  uint8_t  data[2340];        // sector data FIFO                            (was s_data)
  int      data_n, data_rd;   //                                            (was s_data_n/s_data_rd)
  uint8_t  irq_en;            // interrupt enable register                   (was s_irq_en)
  uint8_t  stat;              // drive status byte (bit1 = motor on)         (was s_stat, init 0x02)
  uint32_t loc_lba;           // Setloc target                              (was s_loc_lba)
  uint8_t  mode;              // Setmode                                     (was s_mode)
  int      reading;           // ReadN/ReadS active                          (was s_reading)
  CdcIrqEnt q[8];             // pending-interrupt queue                     (was s_q)
  int      q_head, q_tail, resp_rd;  //                                     (was s_q_head/s_q_tail/s_resp_rd)
  int      verbose;           // `debug cdc` log gate (config, read at init)  (was s_verbose)
  struct DiscState* disc;     // Game-owned disc backend (wired by Game())
} CdcState;

#ifdef __cplusplus
extern "C" {
#endif
// Initialize a fresh CdcState to power-on defaults (stat=0x02, everything else 0). Called by Game().
void cdc_state_init(CdcState* s);
// MMIO 0x1F801800-3 register model — the instance is explicit (mem.cpp passes &game->cdc).
uint32_t cdc_read(CdcState* s, uint32_t p);
void     cdc_write(CdcState* s, uint32_t p, uint8_t v);
#ifdef __cplusplus
}
#endif
