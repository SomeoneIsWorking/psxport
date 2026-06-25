// xa_state.h — per-instance native XA-ADPCM CD-audio / voice-clip streamer state (xa_stream.c), so two
// Cores keep SEPARATE streaming state (active stream, position, decode ring, clip bookkeeping). This
// used to be file-scope statics in xa_stream.c; it now lives in this struct, one per Game (game.h
// embeds it). xa_stream.c is BOUND to one instance at a time via xa_bind(Core*) — set from the explicit
// Core before that core runs (native_step_frame), like gte_bind/spu_bind/mdec_bind/cdc_bind. The
// decoded PCM still feeds the per-instance SPU which mixes to the SHARED host audio sink (one physical
// speaker; a lockstep RAM/state diff is unaffected by the host device, shared by design — same policy
// as the FMV audio sink and SPU output).
//
// Plain-C struct (no C++) so xa_stream.c stays C, exactly like gte_state.h's GteRegs.
#pragma once
#include <stdint.h>

#define XA_RING_FRAMES 16384

typedef struct XaState {
  int      active;            // 1 while ReadS streaming with XA-ADPCM enabled  (was s_active)
  uint32_t lba;               // next CHD sector to read                        (was s_lba)
  uint8_t  mode;              // last Setmode                                   (was s_mode)
  int      filter_set;        // a Setfilter was issued                         (was s_filter_set)
  uint8_t  filter_file, filter_chan;  //                                       (was s_filter_*)
  int      owns_slot2;        // native owner of voice task slot 2              (was s_owns_slot2)
  uint32_t end_lba;           // clip end LBA (0 = open-ended)                  (was s_end_lba)
  uint32_t clip_start;        // clip start LBA (idempotency + loop restart)    (was s_clip_start)
  uint8_t  clip_chan;         // clip channel (idempotency)                     (was s_clip_chan)
  int      loop;              // loop the clip when the head passes the end     (was s_loop)
  int16_t  ring[XA_RING_FRAMES][2];   // decoded-sample ring (interleaved S16)  (was s_ring)
  uint32_t wr;                // total frames written (monotonic)               (was s_wr)
  double   rd;                // total frames read (monotonic, fractional)      (was s_rd)
  int16_t  hist[2][2];        // XA IIR history, persists across sectors        (was s_hist)
  int      src_freq;          //                                               (was s_src_freq, init 37800)
} XaState;

#ifdef __cplusplus
extern "C" {
#endif
// Initialize a fresh XaState to power-on defaults (src_freq=37800, everything else 0). Called by Game().
void xa_state_init(XaState* s);
// Make `s` the active XA streamer instance for subsequent xa_stream_* calls.
void xa_bind_state(XaState* s);
#ifdef __cplusplus
}
#endif
