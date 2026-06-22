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

// gpu_vk.cpp — is a live on-screen window up (the single windowed/headless discriminator; replaces
// the old PSXPORT_GPU_WINDOW env gate). C-linkage so C and C++ subsystems share one source of truth.
int gpu_windowed(void);

// disc.c
int  disc_read_raw(uint32_t lba, uint8_t* out, uint32_t n);   // raw 2352-byte sector
int  disc_read_sector(uint32_t lba, uint8_t* out);            // 2048-byte data sector

// mdec_beetle.c + vendored mednafen mdec.c
void     mdec_init(void);
void     mdec_write(uint32_t addr, uint32_t val);
uint32_t mdec_read(uint32_t addr);
void     mdec_dma_in(const uint32_t* words, int count);
int      mdec_dma_out(uint32_t* buf, int count);
int      mdec_dma_out_rest(uint32_t* buf, int count);
int      mdec_dma_can_write(void);
void     MDEC_Run(int32_t clocks);

// spu_audio.c
void spu_audio_frame(void);
void spu_wav_reopen(const char* path);

// xa_stream.c — XA-ADPCM streaming (CD audio) + the raw-sector decoder
void xa_stream_setmode(uint8_t mode);
void xa_stream_setfilter(uint8_t file, uint8_t chan);
void xa_stream_setloc(uint8_t amm, uint8_t ass, uint8_t asect);
void xa_stream_start(void);
void xa_stream_stop(void);
int  xa_stream_play_lba(uint32_t* lba);
void xa_stream_play(uint8_t chan, uint32_t start, uint32_t end, int loop);
int  xa_stream_is_looping(void);
int  xa_stream_is_active(void);
int  xa_stream_owns_slot2(void);
int  xa_stream_voice_busy(void);
void xa_stream_voice_release(void);
int  xa_decode_sector(const uint8_t* raw, int16_t* out, int16_t hist[2][2], int* freq);

#ifdef __cplusplus
}
#endif
