// fmv_decode.h — the pure .STR decode machinery, shared by the runtime FMV player
// (native_fmv.cpp, where Fmv::bsDecodeFrame / Fmv::mdecDecodeToRgb555 are thin wrappers
// around these) and the offline tools (tools/fmv_dump, tools/fmv_compare). Extracted so a
// standalone dump exercises the SAME code the runtime runs — a bug seen in a tool dump is a
// bug in the runtime, not in a forked copy.
//
// Deliberately free of game.h/core.h/SDL: the implementation needs only cfg.h (diagnostics +
// the PSXPORT_FMV_DCONLY / PSXPORT_FMV_ROWMAJOR knobs) and the mdec_beetle interface
// (c_subsys.h). C linkage so plain-C harnesses (fmv_compare.c, xa_stream.c) link unchanged.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode an entire BS frame (8-byte BS header + VLC bitstream) into the MDEC run-level code
// stream. Returns number of 16-bit codes written, or negative on error.
int bs_decode_frame(const uint8_t* payload, uint32_t payload_size,
                    int width, int height, uint16_t* codes, int max_codes);

// Feed the MDEC (16bpp) with the code stream and extract/tile the RGB555 frame into `pixels`
// (width*height entries). Uploads the quant + IDCT tables first. Returns width*height, or
// negative on error.
int mdec_decode_to_rgb555(const uint16_t* codes, int ncodes,
                          int width, int height, uint16_t* pixels);

// Decode one raw 2352B XA-ADPCM sector to interleaved S16 stereo (out[2*n]=L, out[2*n+1]=R;
// mono duplicated to both channels). Returns stereo frame count; `*freq` = sample rate.
// `hist[ch][0..1]` is the per-channel history that MUST persist across sectors.
// (Also declared in c_subsys.h for xa_stream.c; identical declaration.)
int xa_decode_sector(const uint8_t* raw, int16_t* out, int16_t hist[2][2], int* freq);

#ifdef __cplusplus
}
#endif
