// Offline texture-atlas reconstructor — proves the Tomba!2 asset pipeline (unpack + LZ decompress +
// VRAM placement) is FULLY PC-OWNED: it rebuilds the VRAM atlas from a raw compressed archive with the
// emulator OFF, running NONE of the PSX game. The logic mirrors engine/game_tomba2.cpp ov_unpack_group +
// lz_decompress exactly (the owned native codecs), operating on a flat byte buffer instead of guest RAM.
//
// Input  = a staging-buffer dump (PSXPORT_UNPACKDUMP/*.bin) — the bytes the loader CD-loads to 0x8018A000:
//          [count:u32][pad:u32] then `count` 12-byte descriptors {x:s16, y:s16, stride:s16, field:s16,
//          srclen:u32}, then the packed image data at +0x800. Each image decompresses to stride*field
//          16-bit VRAM words placed at VRAM (x,y). (CLUT/bit-depth decode-to-RGB is a separate view step;
//          the VRAM words themselves are what the upload writes, so a bit-identical VRAM IS the proof.)
// Output = a full 1024x512 16-bit VRAM image (raw u16) to compare against a live PSXPORT_VRAMDUMP.
//
// Build:  cc -O2 -o scratch/bin/tex_reconstruct tools/tex_reconstruct.c
// Run:    scratch/bin/tex_reconstruct <archive...> <out_vram.bin> [--cmp live_vram.bin]
//         archives are overlaid in load order; --cmp reports the bit-match % over the touched VRAM words.
//         Proof result (seaside, 5 archives vs live f455): 99.95% — residual is runtime-animated CLUTs only.
//
// The LZ back-reference offset table is MAIN.EXE .rodata @0x800153C8 (8 x {base:s32, factor:s32});
// offset[i] = base[i] + 2*factor[i]*stride. Baked in here as the proof must not touch the PSX.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512

// MAIN.EXE .rodata @0x800153C8, 8 x {base:s32, factor:s32}; offset[i] = base[i] + 2*factor[i]*stride.
// mode 1 = -1 byte (repeat-last RLE); modes 2..7 are vertical (row-relative, factor*stride).
static const int32_t OFF_BASE[8]   = { 0, -1,  0, -1, -2, -3,  1,  2 };
static const int32_t OFF_FACTOR[8] = { 0,  0, -1, -1, -1, -1, -1, -1 };

static int16_t rd16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); }

// Mirror of lz_decompress (game_tomba2.cpp) on flat buffers. Decompress one image: `src`/`srclen` packed
// input, `stride` descriptor stride, into `out` (capacity outcap bytes). Returns bytes written.
static uint32_t lz_decompress(uint8_t* out, uint32_t outcap, const uint8_t* src, uint32_t srclen, int32_t stride) {
    int32_t off[8];
    for (int i = 0; i < 8; i++) off[i] = OFF_BASE[i] + 2 * (OFF_FACTOR[i] * stride);
    uint32_t si = 0, oi = 0;
    while (si < srclen) {
        uint8_t ctrl = src[si++];
        uint32_t len = ctrl >> 3, mode = ctrl & 7u;
        if (mode != 0) {                                  // back-reference into the output so far
            int64_t bi = (int64_t)oi + off[mode];
            for (uint32_t k = 0; k < len; k++) {
                if (oi >= outcap || bi < 0 || bi >= (int64_t)outcap) return oi;
                out[oi++] = out[bi++];
            }
        } else {                                          // literal run from the source
            if (len == 0) break;                          // terminator
            for (uint32_t k = 0; k < len; k++) {
                if (oi >= outcap || si >= srclen) return oi;
                out[oi++] = src[si++];
            }
        }
    }
    return oi;
}

static uint16_t vram[VRAM_W * VRAM_H];   // zero-initialised; archives overlay in load order
static uint8_t  touched[VRAM_W * VRAM_H];

// Apply one archive (decompress + place every image). Returns 0 on success.
static int apply_archive(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    static uint8_t buf[0x80000];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n < 0x800) { fprintf(stderr, "%s: too small (%zu)\n", path, n); return 1; }
    uint32_t count = rd32(buf + 0);
    const uint8_t* entry = buf + 4;
    const uint8_t* src   = buf + 0x800;
    static uint8_t img[0x40000];
    for (uint32_t i = 0; i < count; i++, entry += 12) {
        int16_t  x = rd16(entry+0), y = rd16(entry+2), stride = rd16(entry+4), field = rd16(entry+6);
        uint32_t srclen = rd32(entry + 8);
        uint32_t got = lz_decompress(img, sizeof img, src, srclen, stride);
        if (got != (uint32_t)(2*stride*field))
            fprintf(stderr, "[recon] %s e%u (%d,%d) %dx%d: got %u != %u\n",
                    path, i, x, y, stride, field, got, (uint32_t)(2*stride*field));
        const uint16_t* px = (const uint16_t*)img;
        for (int r = 0; r < field; r++)
            for (int col = 0; col < stride; col++) {
                int vx = x + col, vy = y + r;
                if (vx >= 0 && vx < VRAM_W && vy >= 0 && vy < VRAM_H) {
                    vram[vy*VRAM_W + vx] = px[r*stride + col];
                    touched[vy*VRAM_W + vx] = 1;
                }
            }
        src += srclen;
    }
    fprintf(stderr, "[recon] applied %s (count=%u)\n", path, count);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <archive...> <out_vram.bin> [--cmp live_vram.bin]\n", argv[0]);
        return 2;
    }
    const char* cmp = NULL;
    int nargs = argc;
    if (argc >= 3 && !strcmp(argv[argc-2], "--cmp")) { cmp = argv[argc-1]; nargs = argc - 2; }
    const char* out = argv[nargs - 1];
    for (int a = 1; a < nargs - 1; a++) if (apply_archive(argv[a])) return 1;

    FILE* o = fopen(out, "wb");
    if (!o) { perror(out); return 1; }
    fwrite(vram, 2, VRAM_W * VRAM_H, o); fclose(o);
    fprintf(stderr, "[recon] wrote %s (1024x512x16)\n", out);

    if (cmp) {
        FILE* lf = fopen(cmp, "rb");
        if (!lf) { perror(cmp); return 1; }
        static uint16_t live[VRAM_W * VRAM_H];
        if (fread(live, 2, VRAM_W*VRAM_H, lf) != (size_t)(VRAM_W*VRAM_H)) { fprintf(stderr, "short live\n"); return 1; }
        fclose(lf);
        long tot = 0, mism = 0;
        for (int i = 0; i < VRAM_W*VRAM_H; i++) if (touched[i]) { tot++; if (vram[i] != live[i]) mism++; }
        fprintf(stderr, "[recon] CMP vs %s: %ld touched VRAM words, %ld mismatched (%.4f%% match)\n",
                cmp, tot, mism, tot ? 100.0*(tot-mism)/tot : 0.0);
        return mism ? 3 : 0;
    }
    return 0;
}
