#version 450
#extension GL_GOOGLE_include_directive : require
// Record rasterizer: one PSX pixel per fragment, by gpu.c's rules; record_raster_setup.h sets up each op.
// The target is a plane: the VRAM image or a display canvas, whose pixel p is VRAM-space pixel p + origin * scale.
// A crop canvas's UI quads (kMappingCropUi) map instead at the UI scale from the UI origin.
#include "record_op.glsl"
// A draw that leaves its depth moved back by the plane's kDepthBin (RecordRasterizer::drawQuads).
#ifdef RECORD_DEPTH_BACK
#define RECORD_DEPTH
#endif

layout(location = 0) flat in uvec4 v_info;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_target; // the target plane before this batch
layout(set = 2, binding = 1) uniform sampler2D u_vram;   // the VRAM image before this batch
layout(set = 2, binding = 2) uniform sampler2D u_readBack; // the read-back capture (RecordReadBack)
layout(set = 2, binding = 3) uniform sampler2D u_outlineFields;  // present::OutlineAtlas::fields, filtered
layout(set = 2, binding = 4) uniform sampler2D u_outlineIndices; // present::OutlineAtlas::indices
layout(std430, set = 2, binding = 5) readonly buffer Ops { uint ops[]; };
layout(std430, set = 2, binding = 6) readonly buffer Cluts { uint cluts[]; };
layout(std430, set = 2, binding = 7) readonly buffer Pixels { uint pixels[]; };
// plane: scale, origin x, origin y (native), the VRAM image's scale; ui: scale, origin x, origin y (UI pixels);
// outline: the atlas's field pixels a texel (0 for none); faces: each button slot's face colour, 0 for the CLUT's;
// fontPlaces: each outline font's first texel across VRAM and its atlas row; fontSizes: its width and height.
layout(set = 3, binding = 0) uniform Params {
    ivec4 plane; ivec4 ui; ivec4 outline; ivec4 faces; ivec4 fontPlaces[4]; ivec4 fontSizes[4];
} params;

uint op;
int S;  // target pixels per native pixel of this fragment's mapping
int Sv; // the VRAM image's

uint word(uint index) { return ops[op * kRecordOpWords + index]; }

uint halfword(vec2 rg) { return uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8); }
uint fetch(ivec2 p) { return halfword(texelFetch(u_vram, p, 0).rg); }

uint clutEntry(uint index) { return (cluts[index >> 1] >> ((index & 1u) * 16u)) & 0xFFFFu; }
uint poolPixel(uint index) { return (pixels[index >> 1] >> ((index & 1u) * 16u)) & 0xFFFFu; }

// Floor modulo; GLSL leaves % of a negative operand undefined.
int wrap(int value, int range) {
    int q = value >= 0 ? value / range : -((range - 1 - value) / range);
    return value - q * range;
}

// The texel column across VRAM at the op's mode and the VRAM row, after the texture window (gpu.c TWX/TWY).
uint windowedU(uint u) { return (u & word(kOpWindowXAnd)) + word(kOpWindowXAdd); }
uint windowedV(uint v) { return (v & word(kOpWindowYAnd)) + word(kOpWindowYAdd); }

// gpu_common.h GetTexel. `fu`/`fv` (1/256 texel) pick the target pixel inside the texel's scaled block, so a
// block drawn or copied at scale shows its detail; an uploaded block is uniform and reads as at 1x.
uint texel(uint u, uint v, uint fu, uint fv, uint flags) {
    uint tm = (flags >> kOpTexModeShift) & 3u;
    uint uext = windowedU(u);
    uint k = 2u - tm;
    int fx = int((uext >> k) & uint(kVramWidth - 1));
    int fy = int(windowedV(v) & uint(kVramHeight - 1));
    int sx = int(((((uext & ((1u << k) - 1u)) << 8) | fu) * uint(Sv)) >> (8u + k));
    int sy = int((fv * uint(Sv)) >> 8);
    uint t = fetch(ivec2(fx * Sv + sx, fy * Sv + sy));
    if (tm == 0u) {
        t = clutEntry(word(kOpClut) + ((t >> ((uext & 3u) * 4u)) & 0xFu));
    } else if (tm == 1u) {
        t = clutEntry(word(kOpClut) + ((t >> ((uext & 1u) * 8u)) & 0xFFu));
    }
    return t;
}

// The captured pixel at fragment p moved by the op's offset, its 5-bit channels weighed into a CLUT index.
uint readBackTexel(ivec2 p) {
    ivec2 at = p + ivec2(int(word(kOpReadBackX)), int(word(kOpReadBackY))) * S;
    uint t = halfword(texelFetch(u_readBack, at, 0).rg);
    uint w = word(kOpReadBackWeights);
    uint index = ((t & 0x1Fu) * (w & 0xFFu) + ((t >> 5) & 0x1Fu) * ((w >> 8) & 0xFFu) +
                  ((t >> 10) & 0x1Fu) * ((w >> 16) & 0xFFu)) >> (w >> 24);
    return clutEntry(word(kOpClut) + (index & 0xFFu));
}

float median(vec3 d) { return max(min(d.r, d.g), min(max(d.r, d.g), d.b)); }

// The glyph texel at (u, v) + (fu, fv) / 256 from the outline atlas: inside the glyph's field the CLUT index of the
// bitmap glyph's row, inside its shadow's the shadow's, else transparent; where the font names no glyph, VRAM's.
uint outlineTexel(uint u, uint v, uint fu, uint fv, uint flags) {
    uint font = (flags >> kOpOutlineFontShift) & 3u;
    ivec4 place = params.fontPlaces[font];
    ivec2 at = ivec2(int(windowedU(u)), int(windowedV(v))) - place.xy;
    ivec2 size = params.fontSizes[font].xy;
    if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) {
        return texel(u, v, fu, fv, flags);
    }
    at.y += place.z;
    vec2 field = (vec2(at) + vec2(fu, fv) / 256.0) * float(params.outline.x);
    vec4 distance = texture(u_outlineFields, field / vec2(textureSize(u_outlineFields, 0)));
    vec4 indices = texelFetch(u_outlineIndices, at, 0);
    if (indices.a < 0.5) {
        return texel(u, v, fu, fv, flags);
    }
    uint index;
    if (median(distance.rgb) >= 0.5) {
        index = uint(indices.r * 255.0 + 0.5);
        uint slot = uint(indices.b * 255.0 + 0.5);
        if (slot > 0u && params.faces[slot - 1u] != 0) {
            return (clutEntry(word(kOpClut) + index) & 0x8000u) | uint(params.faces[slot - 1u]);
        }
    } else if (distance.a >= 0.5) {
        index = uint(indices.g * 255.0 + 0.5);
    } else {
        return 0u;
    }
    return clutEntry(word(kOpClut) + index);
}

uint pack(int r, int g, int b) { return 0x8000u | uint(r >> 3) | (uint(g >> 3) << 5) | (uint(b >> 3) << 10); }

uint channel(int value) { return uint(clamp(value >> 3, 0, 31)); }

uint modulate(uint t, int r, int g, int b) {
    return (t & 0x8000u) | channel(int(((t & 0x1Fu) * uint(r)) >> 4)) |
           (channel(int(((t & 0x3E0u) * uint(g)) >> 9)) << 5) | (channel(int(((t & 0x7C00u) * uint(b)) >> 14)) << 10);
}

// gpu_common.h PlotPixelBlend.
uint blend(uint f, uint bg, uint mode) {
    if (mode == 0u) {
        bg |= 0x8000u;
        return (((f + bg) - ((f ^ bg) & 0x0421u)) >> 1) & 0xFFFFu;
    }
    if (mode == 2u) {
        bg |= 0x8000u;
        f &= 0x7FFFu;
        uint diff = bg - f + 0x108420u;
        uint borrow = (diff - ((bg ^ f) & 0x108420u)) & 0x108420u;
        return ((diff - borrow) & (borrow - (borrow >> 5))) & 0xFFFFu;
    }
    bg &= 0x7FFFu;
    if (mode == 3u) {
        f = ((f >> 2) & 0x1CE7u) | 0x8000u;
    }
    uint sum = f + bg;
    uint carry = (sum - ((f ^ bg) & 0x8421u)) & 0x8420u;
    return ((sum - carry) | (carry - (carry >> 5))) & 0xFFFFu;
}

// The integer part of a part edge's 32.32 x at interpolant row yi: edge + yi * step, mod 2^64.
int edgeX(uint edge, uint step, int yi) {
    uint yLow = uint(yi);
    uint yHigh = yi < 0 ? 0xFFFFFFFFu : 0u;
    uint productHigh;
    uint productLow;
    umulExtended(yLow, word(step), productHigh, productLow);
    productHigh += yLow * word(step + 1u) + yHigh * word(step);
    uint carry;
    uaddCarry(word(edge), productLow, carry);
    return int(word(edge + 1u) + productHigh + carry);
}

#ifdef RECORD_DEPTH
// The op's DepthPlane, the op after it.
float depthWord(uint field) { return uintBitsToFloat(ops[(op + 1u) * kRecordOpWords + field]); }

// A depth-tested triangle's view depth at native VRAM point `at`, as log2(z) / 16 over SZ's 16-bit range. The
// draw that writes a tested polygon's depth moves it back by the polygon's bin; the draw that tests it does not.
float fragmentDepth(vec2 at) {
    float inverse = depthWord(kDepthInverse) + depthWord(kDepthSlopeX) * (at.x - depthWord(kDepthOriginX)) +
                    depthWord(kDepthSlopeY) * (at.y - depthWord(kDepthOriginY));
    float z = 1.0 / max(inverse, 1.0e-6);
#ifdef RECORD_DEPTH_BACK
    z += depthWord(kDepthBin);
#endif
    return clamp(log2(max(z, 1.0)) / 16.0, 0.0, 1.0);
}
#endif

void write(uint value) {
    o_col = vec4(float(value & 0xFFu) / 255.0, float((value >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}

void main() {
    op = v_info.x;
    bool ui = v_info.z == kMappingCropUi;
    S = ui ? params.ui.x : params.plane.x;
    Sv = params.plane.w;
    ivec2 target = ivec2(gl_FragCoord.xy);
    ivec2 p = target + (ui ? params.ui.yz : params.plane.yz * S);
    // Integer floor division (a float divide is inexact at S = 3); biased because a canvas reaches left of VRAM.
    ivec2 n = (p + kVramWidth * S) / S - kVramWidth;
    uint flags = word(kOpFlags);
    uint kind = flags & kOpKindMask;
#ifdef RECORD_DEPTH
    // Only depth-tested ops reach this variant.
    gl_FragDepth = fragmentDepth((vec2(p) + 0.5) / float(S));
#endif
    // A crop canvas shows every row; a transfer's op carries the image's field weave.
    if ((flags & kOpSkipRows) != 0u && v_info.z == kMappingPlane &&
        uint(n.y & 1) == ((flags & kOpSkipParity) != 0u ? 1u : 0u)) {
        discard;
    }
    uint dst = halfword(texelFetch(u_target, target, 0).rg);
    bool maskCheck = (flags & kOpMaskCheck) != 0u;
    uint maskSet = (flags & kOpMaskSet) != 0u ? 0x8000u : 0u;
    int vramWidth = kVramWidth * S;
    int vramHeight = kVramHeight * S;
    if (kind == kRecordOpKindFill) {
        write(word(kOpColour));
        return;
    }
    if (kind == kRecordOpKindCopy) {
        int rx = wrap(p.x - int(word(kOpOriginX)) * S, vramWidth);
        int ry = wrap(p.y - int(word(kOpOriginY)) * S, vramHeight);
        // A placed copy reads its source shifted by the target pixels its place moved against its destination; a crop
        // canvas takes its whole pixels, at the image's scale.
        ivec2 shift = v_info.z == kMappingPlane ? ivec2(int(word(kOpShiftX)), int(word(kOpShiftY))) : ivec2(0);
        ivec2 src = ivec2(wrap(int(word(kOpSourceX)) * Sv + rx * Sv / S + shift.x, kVramWidth * Sv),
                          wrap(int(word(kOpSourceY)) * Sv + ry * Sv / S + shift.y, kVramHeight * Sv));
        if (maskCheck && (dst & 0x8000u) != 0u) {
            discard;
        }
        write(fetch(src) | maskSet);
        return;
    }
    if (kind == kRecordOpKindUpload) {
        int rx = wrap(n.x - int(word(kOpOriginX)), kVramWidth);
        int ry = wrap(n.y - int(word(kOpOriginY)), kVramHeight);
        if (maskCheck && (dst & 0x8000u) != 0u) {
            discard;
        }
        write(poolPixel(word(kOpPool) + uint(ry) * word(kOpSourceX) + uint(rx)) | maskSet);
        return;
    }
    bool textured = (flags & kOpTextured) != 0u;
    uint colour = word(kOpColour);
    int r = int(colour & 0xFFu);
    int g = int((colour >> 8) & 0xFFu);
    int b = int((colour >> 16) & 0xFFu);
    uint fore;
    if (kind == kRecordOpKindLinePixel) {
        fore = pack(int(v_info.y & 0xFFu), int((v_info.y >> 8) & 0xFFu), int((v_info.y >> 16) & 0xFFu));
    } else {
        uint u = 0u;
        uint v = 0u;
        uint fu = 0u;
        uint fv = 0u;
        if (kind == kRecordOpKindTriangle) {
            uint part = kOpParts + v_info.y * kPartWords;
            int yi = p.y - int(word(part + kPartRowShift));
            int left = edgeX(part + kPartLeft, part + kPartLeftStep, yi);
            int right = edgeX(part + kPartRight, part + kPartRightStep, yi);
            // A canvas continues the picture past VRAM's columns, where the 11-bit position would wrap.
            int x0 = p.x < 0 || p.x >= vramWidth ? left : wrap(left + vramWidth, 2 * vramWidth) - vramWidth;
            if (p.x < x0 || p.x >= x0 + (right - left)) {
                discard;
            }
            uint xi = uint(p.x + (left - x0));
            uint value[5];
            for (uint c = 0u; c < 5u; c++) {
                value[c] = word(kOpBase + c) + word(kOpDx + c) * xi + word(kOpDy + c) * uint(yi);
            }
            r = int(value[0] >> 24);
            g = int(value[1] >> 24);
            b = int(value[2] >> 24);
            u = value[3] >> 24;
            v = value[4] >> 24;
            fu = (value[3] >> 16) & 0xFFu;
            fv = (value[4] >> 16) & 0xFFu;
        } else {
            int du = (flags & kOpFlipX) != 0u ? -1 : 1;
            int dv = (flags & kOpFlipY) != 0u ? -1 : 1;
            // The native pixel of the sprite this fragment shows, the sprite moved by its place.
            ivec2 shift = ivec2(int(word(kOpShiftX)), int(word(kOpShiftY)));
            ivec2 m = (p - shift + kVramWidth * S) / S - kVramWidth;
            u = uint(int(word(kOpSourceX)) + (m.x - int(word(kOpOriginX))) * du) & 0xFFu;
            v = uint(int(word(kOpSourceY)) + (m.y - int(word(kOpOriginY))) * dv) & 0xFFu;
            // A sprite's texel spans its native pixel: the target pixel inside it, mirrored when flipped.
            ivec2 inside = p - shift - m * S;
            inside = ivec2(du < 0 ? S - 1 - inside.x : inside.x, dv < 0 ? S - 1 - inside.y : inside.y);
            fu = uint((inside.x * 256 + 128) / S);
            fv = uint((inside.y * 256 + 128) / S);
        }
        if (textured) {
            if ((flags & kOpReadBack) != 0u) {
                fore = readBackTexel(p);
            } else if ((flags & kOpOutline) != 0u && params.outline.x > 0) {
                fore = outlineTexel(u, v, fu, fv, flags);
            } else {
                fore = texel(u, v, fu, fv, flags);
            }
            if (fore == 0u) {
                discard;
            }
            if ((flags & kOpModulate) != 0u) {
                fore = modulate(fore, r, g, b);
            }
        } else {
            fore = pack(r, g, b);
        }
    }
    if ((flags & kOpSemi) != 0u && (fore & 0x8000u) != 0u) {
        fore = blend(fore, dst, (flags >> kOpBlendShift) & 3u);
    }
    if (maskCheck && (dst & 0x8000u) != 0u) {
        discard;
    }
    write(textured ? (fore | maskSet) : ((fore & 0x7FFFu) | maskSet));
}
