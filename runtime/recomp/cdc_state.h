// cdc_state.h — per-instance native CD-controller register state (cdc_native.cpp), so two Cores run with
// SEPARATE CD-controller state (one native, one PSX-recomp). The CXD1199-style register model
// (index/bank, param/response/data FIFOs, the pending-interrupt queue, drive/read state) used to live
// in file-scope statics in cdc_native.cpp; that mutable state now lives in this struct, one per Game
// (game.h embeds it). cdc_read/cdc_write take the instance EXPLICITLY (the MMIO dispatcher in
// mem.cpp passes &game->cdc) — no bound "current" pointer — so each core reads/writes only its own
// CD registers; nothing is shared (the disc image itself is a read-only data source, see disc.cpp).
//
// Plain-aggregate struct: the vendored Beetle backends (still C) embed sibling state structs, so this
// stays C-compatible even though cdc_native.cpp is now C++.
#pragma once
#include <stdint.h>

typedef struct CdcIrqEnt {
  uint8_t type;
  uint8_t resp[16];
  int len;
} CdcIrqEnt;

typedef uint64_t (*CdcTickNowFn)(void *context);

typedef struct CdcState {
  int index;                      // 0x1F801800 low 2 bits (register bank)       (was s_index)
  uint8_t param[16];              // param FIFO                                  (was s_param)
  int param_n;                    //                                            (was s_param_n)
  uint8_t data[2340];             // sector data FIFO                            (was s_data)
  int data_n, data_rd;            //                                            (was s_data_n/s_data_rd)
  uint8_t irq_en;                 // interrupt enable register                   (was s_irq_en)
  uint8_t stat;                   // drive status byte (bit1 = motor on)         (was s_stat, init 0x02)
  uint32_t loc_lba;               // Setloc target                              (was s_loc_lba)
  uint8_t mode;                   // Setmode                                     (was s_mode)
  int reading;                    // ReadN/ReadS active                          (was s_reading)
  uint8_t first_sector_pending;   // ReadN accepted; first data sector waits for its drive deadline
  uint8_t bfrd;                   // request-register BFRD latch (bit 7): 1 until explicitly deasserted
  uint8_t following_sector_ready; // drive-side next-sector availability, independent of data FIFO drain
  uint8_t drive_event_armed;      // one following-sector deadline is outstanding
  uint64_t drive_deadline_ticks;  // absolute timestamp in the injected guest-instruction domain
  void *tick_context;
  CdcTickNowFn tick_now;
  CdcIrqEnt q[8];              // pending-interrupt queue                     (was s_q)
  int q_head, q_tail, resp_rd; //                                     (was s_q_head/s_q_tail/s_resp_rd)
  uint8_t irq_edge;            // 1 = the controller just RAISED an interrupt and nothing has latched
                               // it yet. The MMIO dispatcher (mem.cpp) consumes this and sets I_STAT
                               // bit 2, which is edge-triggered on real hardware: acking the CD
                               // controller at 0x1F801803 does NOT clear I_STAT, and a queued second
                               // response raises a FRESH edge as it becomes current. Kept here rather
                               // than reaching for I_STAT directly because this model has no Game
                               // pointer.
  struct DiscState *disc;      // Game-owned disc backend (wired by Game())
} CdcState;

#ifdef __cplusplus
extern "C" {
#endif
// Initialize a fresh CdcState to power-on defaults (stat=0x02, everything else 0). Called by Game().
void cdc_state_init(CdcState *s);
// Inject the deterministic guest-instruction clock. Production binds Timing; hermetic tests bind a fake
// counter and call cdc_drive_service through the same controller path.
void cdc_bind_tick_source(CdcState *s, void *context, CdcTickNowFn now);
// Service one scheduled drive event on the guest thread. Returns 1 only when a due event emitted
// a data-ready INT1; an early wake leaves the existing deadline armed and returns 0.
int cdc_drive_service(CdcState *s);
// MMIO 0x1F801800-3 register model — the instance is explicit (mem.cpp passes &game->cdc).
uint32_t cdc_read(CdcState *s, uint32_t p);
void cdc_write(CdcState *s, uint32_t p, uint8_t v);
// DMA3 (CDROM -> RAM) controller read, called by the DMA3 CHCR model in mem.cpp. Writes every
// requested output word: FIFO data first, then the controller's zero value after depletion. Returns
// the FIFO-sourced word count so the caller can report the exact zero-filled denominator.
int cdc_dma_read(CdcState *s, uint32_t *out, int words);
// Position the controller at `lba` and load that sector into the data FIFO, so a guest driving the
// hardware directly (XA/streaming: spin on DRQSTS, then DMA3) sees real data even when the libcd
// file-read path is served natively. Both layers read the same disc image.
void cdc_begin_read(CdcState *s, uint32_t lba);
// Mirror a Setmode the native CD layer intercepted into the controller model. Bit 0x20 decides
// whether the data FIFO presents whole sectors (header + subheader + data) or user data only, and a
// streaming reader depends on that framing to identify sector types.
void cdc_set_mode(CdcState *s, uint8_t mode);
#ifdef __cplusplus
}
#endif
