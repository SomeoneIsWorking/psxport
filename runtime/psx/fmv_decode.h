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
int bs_decode_frame(
    const uint8_t *payload, uint32_t payload_size, int width, int height, uint16_t *codes, int max_codes);

// Feed the MDEC (16bpp) with the code stream and extract/tile the RGB555 frame into `pixels`
// (width*height entries). Uploads the quant + IDCT tables first. Returns width*height, or
// negative on error.
int mdec_decode_to_rgb555(const uint16_t *codes, int ncodes, int width, int height, uint16_t *pixels);

// xa_decode_sector — decode one raw 2352B XA-ADPCM sector to interleaved S16 stereo.
// DECLARED IN c_subsys.h, which this includes. The declaration used to be copied here as well, with
// a comment saying so ("identical declaration") — a copy that is known about is still a copy, and
// the two would drift the first time either signature changed.
#include "c_subsys.h"

#ifdef __cplusplus
}
#endif
