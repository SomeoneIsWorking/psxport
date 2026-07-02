#ifndef HW_BIND_H
#define HW_BIND_H
struct Core;
// Bind THIS core's per-instance HW-peripheral state (so two cores — e.g. native vs PSX-recomp dual-view —
// keep SEPARATE peripheral state). Called per core frame-step + at boot, from the explicit Core.
void gte_bind(Core* c);   // gte_beetle.cpp — per-instance GTE register file
// (native-depth cache bind moved to `class ProjPrim::bind` — call `c->mRender->projprim.bind(c)`)
void spu_bind(Core* c);   // Beetle spu.c — per-instance SPU state (lazy-powers on first bind)
void mdec_bind(Core* c);  // Beetle mdec.c — per-instance MDEC state (lazy-powers on first bind)
void cdc_bind(Core* c);   // cdc_native.c — per-instance CD-controller registers
void xa_bind(Core* c);    // xa_stream.c — per-instance XA-ADPCM streamer state
#endif
