// C-linkage declarations for the leaf subsystems still compiled as C (disc, mdec/spu Beetle
// adapters, spu_audio, watchdog, xa_stream) plus the vendored Beetle MDEC clock pump. The OOP
// refactor made most of the runtime C++; these few stay C, so the C++ callers must see their
// symbols with C linkage. Include this instead of scattering local (C++-mangled) forward decls.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// watchdog.c
void watchdog_init(void);
void watchdog_pet(void);
void watchdog_suspend(void);   // cancel the timeout during intentional idle (pause / REPL input wait)
void watchdog_disable(void);   // permanently disable (SBS debugger pauses indefinitely on a divergence)

// gpu_gpu.cpp — is a live on-screen window up (the single windowed/headless discriminator; replaces
// the old PSXPORT_GPU_WINDOW env gate). C-linkage so C and C++ subsystems share one source of truth.
int gpu_windowed(void);

// disc.c
// disc_* (disc.c) now take the Game-owned DiscState* explicitly — see disc.h.

// mdec_beetle.c + vendored mednafen mdec.c
void     mdec_init(void);
void     mdec_write(uint32_t addr, uint32_t val);
uint32_t mdec_read(uint32_t addr);
void     mdec_dma_in(const uint32_t* words, int count);
int      mdec_dma_out(uint32_t* buf, int count);
int      mdec_dma_out_rest(uint32_t* buf, int count);
int      mdec_dma_can_write(void);
void     MDEC_Run(int32_t clocks);

// (spu_audio has moved to `class SpuAudio` owned by Game — `c->game->spu_audio.method()`.
//  No C shims; callers use the class directly via spu_audio.h.)

// xa_stream.c — XA-ADPCM streaming (CD audio) + the raw-sector decoder. Every entry point takes
// the Game-owned XaState* explicitly (game->xa); see xa_state.h for the vendor-pull bind exception.
struct XaState;
void xa_stream_setmode(struct XaState* xs, uint8_t mode);
void xa_stream_setfilter(struct XaState* xs, uint8_t file, uint8_t chan);
void xa_stream_setloc(struct XaState* xs, uint8_t amm, uint8_t ass, uint8_t asect);
void xa_stream_start(struct XaState* xs);
void xa_stream_stop(struct XaState* xs);
int  xa_stream_play_lba(struct XaState* xs, uint32_t* lba);
void xa_stream_play(struct XaState* xs, uint8_t chan, uint32_t start, uint32_t end, int loop);
int  xa_stream_is_looping(struct XaState* xs);
int  xa_stream_is_active(struct XaState* xs);
int  xa_stream_owns_slot2(struct XaState* xs);
int  xa_stream_voice_busy(struct XaState* xs);
void xa_stream_voice_release(struct XaState* xs);
int  xa_decode_sector(const uint8_t* raw, int16_t* out, int16_t hist[2][2], int* freq);

#ifdef __cplusplus
}
#endif
