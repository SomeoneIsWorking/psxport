#version 450
#extension GL_GOOGLE_include_directive : require
// Record rasterizer: one target pixel per fragment, by gpu.c's rules; record_raster_setup.h sets up each op. A
// triangle's coverage and interpolants are the hardware's; a rect's pixels are worked out here.
// The target is a plane: the VRAM image or a display canvas, whose pixel p is VRAM-space pixel p + origin * scale.
// A crop canvas's UI quads (kMappingCropUi) map instead at the UI scale from the UI origin.
#include "record_op.glsl"
// RECORD_OPAQUE: only ops that never discard (see its main).
// RECORD_LEAN: only triangles, sprites, line pixels and fills, texturing from VRAM alone (no read-back or outline
// text); the smaller shader keeps more pixels in flight, and RecordRasterizer runs every other op through the full one.
// RECORD_TRIANGLE, RECORD_SPRITE (with RECORD_LEAN): a lean shader for that kind of op alone.
// RECORD_DEPTH_MARK: a depth group's or Model's mark, only the polygon's coverage and its moved-back depth (see main).
// Every other depth-tested draw tests and writes the 1/z record.vert interpolates, so it keeps early depth testing.

layout(location = 0) flat in uvec4 v_info;
layout(location = 1) noperspective in vec3 v_colour; // a triangle's colour interpolants, channel units
layout(location = 2) noperspective in vec2 v_uv;     // its texel interpolants
layout(location = 3) flat in uint v_texels;          // the texels it samples (RecordShape::texels)
layout(location = 4) flat in uvec4 v_op;     // its op's kOpFlags, kOpColour and kOpClut (record.vert)
layout(location = 5) flat in uvec4 v_window; // its op's texture window: kOpWindowXAnd, XAdd, YAnd, YAdd
layout(location = 6) flat in ivec4 v_sprite; // a sprite's texel base and place (record.vert)
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_target; // the target plane before this batch
#ifdef RECORD_FETCH
// The target pixel as the draws before this fragment left it, read in-pass (Metal framebuffer fetch): every earlier
// draw of the batch is already in it, so a draw over them needs neither a new batch nor the snapshot.
layout(input_attachment_index = 0, set = 2, binding = 8) uniform subpassInput u_targetNow;
#endif
layout(set = 2, binding = 1) uniform sampler2D u_vram;   // the VRAM image before this batch
layout(set = 2, binding = 2) uniform sampler2D u_readBack; // the read-back capture (RecordReadBack) or canvas
layout(set = 2, binding = 3) uniform sampler2D u_outlineFields;  // present::OutlineAtlas::fields, filtered
layout(set = 2, binding = 4) uniform sampler2D u_outlineIndices; // present::OutlineAtlas::indices
layout(std430, set = 2, binding = 5) readonly buffer Ops { uint ops[]; };
layout(std430, set = 2, binding = 6) readonly buffer Cluts { uint cluts[]; };
layout(std430, set = 2, binding = 7) readonly buffer Pixels { uint pixels[]; };
// plane: scale, origin x, origin y (native), the VRAM image's scale; ui: scale, origin x, origin y (UI pixels);
// outline: the atlas's field pixels a texel (0 for none); faces: each button slot's face colour, 0 for the CLUT's;
// fontPlaces: each outline font's first texel across VRAM and its atlas row; fontSizes: its width and height, then its
// lettering's first fill index and their count.
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
uint windowedU(uint u) { return (u & v_window.x) + v_window.y; }
uint windowedV(uint v) { return (v & v_window.z) + v_window.w; }

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
        t = clutEntry(v_op.z + ((t >> ((uext & 3u) * 4u)) & 0xFu));
    } else if (tm == 1u) {
        t = clutEntry(v_op.z + ((t >> ((uext & 1u) * 8u)) & 0xFFu));
    }
    return t;
}

// The captured pixel at fragment p moved by the op's offset, its 5-bit channels weighed into a CLUT index. A canvas
// pixel is the 15-bit texel itself, its buffer's columns taken from the VRAM image, which always holds them.
uint readBackTexel(ivec2 p, uint flags) {
    ivec2 at = p + ivec2(int(word(kOpReadBackX)), int(word(kOpReadBackY))) * S;
    if ((flags & kOpCanvasTexels) != 0u) {
        uint columns = word(kOpCanvasColumns);
        if (at.x >= int(columns & 0xFFFFu) * S && at.x < int(columns >> 16) * S) {
            uint origin = word(kOpCanvasOrigin);
            return fetch(at + ivec2(int(origin << 16) >> 16, int(origin >> 16)) * S);
        }
        return halfword(texelFetch(u_readBack, at, 0).rg);
    }
    uint t = halfword(texelFetch(u_readBack, at, 0).rg);
    uint w = word(kOpReadBackWeights);
    uint index = ((t & 0x1Fu) * (w & 0xFFu) + ((t >> 5) & 0x1Fu) * ((w >> 8) & 0xFFu) +
                  ((t >> 10) & 0x1Fu) * ((w >> 16) & 0xFFu)) >> (w >> 24);
    return clutEntry(v_op.z + (index & 0xFFu));
}

float median(vec3 d) { return max(min(d.r, d.g), min(max(d.r, d.g), d.b)); }

// outline_text.h kLetteringSlot.
const uint kLetteringSlot = 255u;

// A word glyph's fill `shade` of the way down its letter: the font's run of fill indices (OutlineLettering), blended.
uint letteringFill(float shade, uint font) {
    int first = params.fontSizes[font].z;
    int count = params.fontSizes[font].w;
    float k = clamp(shade, 0.0, 1.0) * float(max(count - 1, 0));
    int low = min(int(k), max(count - 2, 0));
    float f = clamp(k - float(low), 0.0, 1.0);
    uint a = clutEntry(v_op.z + uint(first + low));
    uint b = clutEntry(v_op.z + uint(first + min(low + 1, count - 1)));
    vec3 ca = vec3(uvec3(a, a >> 5, a >> 10) & 0x1Fu);
    vec3 cb = vec3(uvec3(b, b >> 5, b >> 10) & 0x1Fu);
    uvec3 c = uvec3(mix(ca, cb, f) + 0.5);
    return (a & 0x8000u) | c.r | (c.g << 5) | (c.b << 10);
}

// The glyph texel at (u, v) + (fu, fv) / 256 from the outline atlas: inside the glyph's field the CLUT index of the
// bitmap glyph's row, inside its shadow's the shadow's, else transparent (a word glyph: its shaded letters inside
// their rim); where the font names no glyph, VRAM's.
uint outlineTexel(uint u, uint v, uint fu, uint fv, uint flags) {
    uint font = (flags >> kOpOutlineFontShift) & 3u;
    ivec4 place = params.fontPlaces[font];
    ivec2 at = ivec2(int(windowedU(u)), int(windowedV(v))) - place.xy;
    ivec2 size = params.fontSizes[font].xy;
    if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) {
        return texel(u, v, fu, fv, flags);
    }
    at.y += place.z;
    vec4 indices = texelFetch(u_outlineIndices, ivec2(at.x * 2, at.y), 0);
    if (indices.a < 0.5) {
        return texel(u, v, fu, fv, flags);
    }
    // The texel's top-left in its glyph's packed tile.
    uvec4 tile = uvec4(texelFetch(u_outlineIndices, ivec2(at.x * 2 + 1, at.y), 0) * 255.0 + 0.5);
    vec2 field = vec2(tile.r | (tile.g << 8), tile.b | (tile.a << 8)) + vec2(fu, fv) / 256.0 * float(params.outline.x);
    vec4 distance = texture(u_outlineFields, field / vec2(textureSize(u_outlineFields, 0)));
    if (uint(indices.b * 255.0 + 0.5) == kLetteringSlot) {
        if (distance.r >= 0.5) {
            return letteringFill(distance.b, font);
        }
        return distance.g >= 0.5 ? clutEntry(v_op.z + uint(indices.g * 255.0 + 0.5)) : 0u;
    }
    uint index;
    if (median(distance.rgb) >= 0.5) {
        index = uint(indices.r * 255.0 + 0.5);
        uint slot = uint(indices.b * 255.0 + 0.5);
        if (slot > 0u && params.faces[slot - 1u] != 0) {
            return (clutEntry(v_op.z + index) & 0x8000u) | uint(params.faces[slot - 1u]);
        }
    } else if (distance.a >= 0.5) {
        index = uint(indices.g * 255.0 + 0.5);
    } else {
        return 0u;
    }
    return clutEntry(v_op.z + index);
}

// Colour as whole channel values 0..255 in floats. Each step below is exact: the products stay under 2^24 and every
// scale is a power of two, so these are gpu.c's integer shifts and clamps bit for bit.
uint packChannels(uvec3 c) { return c.r | (c.g << 5) | (c.b << 10); }

uint pack(vec3 rgb) { return 0x8000u | packChannels(uvec3(rgb * 0.125)); }

// The gouraud colour at this pixel: gpu.c's whole part of each interpolant, clamped.
vec3 gouraud() { return clamp(floor(v_colour), vec3(0.0), vec3(255.0)); }

vec3 flatColour(uint colour) { return vec3(uvec3(colour, colour >> 8, colour >> 16) & 0xFFu); }

// gpu.c: each 5-bit texel channel times the colour, >> 7, at most 31.
uint modulate(uint t, vec3 rgb) {
    vec3 t5 = vec3((uvec3(t) >> uvec3(0u, 5u, 10u)) & 0x1Fu);
    return (t & 0x8000u) | packChannels(uvec3(min(t5 * rgb * (1.0 / 128.0), vec3(31.0))));
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

#ifdef RECORD_DEPTH_MARK
// The op's DepthPlane, the op after it.
float depthWord(uint field) { return uintBitsToFloat(ops[(op + 1u) * kRecordOpWords + field]); }
#endif

// The native pixel of VRAM-space target pixel `p`: integer floor division (a float divide is inexact at S = 3),
// biased because a canvas reaches left of VRAM.
ivec2 native(ivec2 p) { return (p + kVramWidth * S) / S - kVramWidth; }

void write(uint value) {
    o_col = vec4(float(value & 0xFFu) / 255.0, float((value >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}

#ifdef RECORD_OPAQUE
// An op that can never discard (RecordRasterizer, needsDiscard): a fill, or an untextured triangle, sprite or line
// pixel with no mask test and no skipped rows. A shader with no discard keeps Apple GPUs' hidden-surface removal, so
// the pixels a later opaque draw covers are never shaded. Each path is main's below for the same op.
void main() {
    uint flags = v_op.x;
    uint kind = flags & kOpKindMask;
    if (kind == kRecordOpKindFill) {
        write(v_op.y);
        return;
    }
    uint fore;
    if (kind == kRecordOpKindLinePixel) {
        fore = pack(flatColour(v_info.y));
    } else if (kind == kRecordOpKindTriangle) {
        fore = pack(gouraud());
    } else {
        fore = pack(flatColour(v_op.y));
    }
    if ((flags & kOpSemi) != 0u) {
#ifdef RECORD_FETCH
        uint dst = halfword(subpassLoad(u_targetNow).rg);
#else
        uint dst = halfword(texelFetch(u_target, ivec2(gl_FragCoord.xy), 0).rg);
#endif
        fore = blend(fore, dst, (flags >> kOpBlendShift) & 3u);
    }
    write((fore & 0x7FFFu) | ((flags & kOpMaskSet) != 0u ? 0x8000u : 0u));
}
#else
void main() {
    op = v_info.x;
    bool ui = v_info.z == kMappingCropUi;
    S = ui ? params.ui.x : params.plane.x;
    Sv = params.plane.w;
    ivec2 target = ivec2(gl_FragCoord.xy);
    ivec2 p = target + (ui ? params.ui.yz : params.plane.yz * S);
    uint flags = v_op.x;
    uint kind = flags & kOpKindMask;
#ifdef RECORD_DEPTH_MARK
    // The polygon's 1/z here, left moved back by its bin, within SZ's 16-bit range as the test clamps it.
    gl_FragDepth = 1.0 / clamp(1.0 / max(gl_FragCoord.z, 1.0e-6) + depthWord(kDepthBin), 1.0, 65536.0);
    // The group's test draw marked in stencil exactly the pixels it drew, and a group's polygons cover no pixel twice,
    // so a marked pixel this shape covers is its own: the texel, blend and mask work that decided it is not repeated.
    return;
#endif
    // A crop canvas shows every row; a transfer's op carries the image's field weave.
    if ((flags & kOpSkipRows) != 0u && v_info.z == kMappingPlane &&
        uint(native(p).y & 1) == ((flags & kOpSkipParity) != 0u ? 1u : 0u)) {
        discard;
    }
    bool maskCheck = (flags & kOpMaskCheck) != 0u;
    // Only a blend or a mask test reads the target (RecordRasterizer runs every other op through pipelines that never
    // fetch it, so on Metal they keep the GPU's free ordering and hidden-surface removal).
    uint dst = 0u;
    if ((flags & (kOpSemi | kOpMaskCheck)) != 0u) {
#ifdef RECORD_FETCH
        dst = halfword(subpassLoad(u_targetNow).rg);
#else
        dst = halfword(texelFetch(u_target, target, 0).rg);
#endif
    }
    uint maskSet = (flags & kOpMaskSet) != 0u ? 0x8000u : 0u;
    int vramWidth = kVramWidth * S;
    int vramHeight = kVramHeight * S;
#if !defined(RECORD_TRIANGLE) && !defined(RECORD_SPRITE)
    if (kind == kRecordOpKindFill) {
        write(v_op.y);
        return;
    }
#endif
#ifndef RECORD_LEAN
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
        ivec2 n = native(p);
        int rx = wrap(n.x - int(word(kOpOriginX)), kVramWidth);
        int ry = wrap(n.y - int(word(kOpOriginY)), kVramHeight);
        if (maskCheck && (dst & 0x8000u) != 0u) {
            discard;
        }
        write(poolPixel(word(kOpPool) + uint(ry) * word(kOpSourceX) + uint(rx)) | maskSet);
        return;
    }
#endif
    bool textured = (flags & kOpTextured) != 0u;
    uint fore;
#if !defined(RECORD_TRIANGLE) && !defined(RECORD_SPRITE)
    if (kind == kRecordOpKindLinePixel) {
        fore = pack(flatColour(v_info.y));
    } else
#endif
    {
        vec3 rgb;
        uint u = 0u;
        uint v = 0u;
        uint fu = 0u;
        uint fv = 0u;
#if defined(RECORD_TRIANGLE)
        {
#elif defined(RECORD_SPRITE)
        if (false) {
#else
        if (kind == kRecordOpKindTriangle) {
#endif
            // gpu.c takes the whole part of each 8.24 interpolant, u and v mod 256, and the fraction's top byte picks
            // the target pixel inside a texel's scaled block.
            rgb = gouraud();
            uvec4 limits = (uvec4(v_texels) >> uvec4(0u, 8u, 16u, 24u)) & 0xFFu;
            vec2 uv = clamp(v_uv, vec2(limits.xz), vec2(limits.yw) + 255.0 / 256.0);
            vec2 whole = floor(uv);
            u = uint(int(whole.x)) & 0xFFu;
            v = uint(int(whole.y)) & 0xFFu;
            fu = min(uint((uv.x - whole.x) * 256.0), 255u);
            fv = min(uint((uv.y - whole.y) * 256.0), 255u);
        }
#if !defined(RECORD_TRIANGLE)
        else {
            rgb = flatColour(v_op.y);
            int du = (flags & kOpFlipX) != 0u ? -1 : 1;
            int dv = (flags & kOpFlipY) != 0u ? -1 : 1;
            // The native pixel of the sprite this fragment shows, the sprite moved by its place.
            ivec2 shift = v_sprite.zw;
            ivec2 m = (p - shift + kVramWidth * S) / S - kVramWidth;
            u = uint(v_sprite.x + m.x * du) & 0xFFu;
            v = uint(v_sprite.y + m.y * dv) & 0xFFu;
            // A sprite's texel spans its native pixel: the target pixel inside it, mirrored when flipped.
            ivec2 inside = p - shift - m * S;
            inside = ivec2(du < 0 ? S - 1 - inside.x : inside.x, dv < 0 ? S - 1 - inside.y : inside.y);
            fu = uint((inside.x * 256 + 128) / S);
            fv = uint((inside.y * 256 + 128) / S);
        }
#endif
        if (textured) {
#ifdef RECORD_LEAN
            fore = texel(u, v, fu, fv, flags);
#else
            if ((flags & kOpReadBack) != 0u) {
                fore = readBackTexel(p, flags);
            } else if ((flags & kOpOutline) != 0u && params.outline.x > 0) {
                fore = outlineTexel(u, v, fu, fv, flags);
            } else {
                fore = texel(u, v, fu, fv, flags);
            }
#endif
            if (fore == 0u) {
                discard;
            }
            if ((flags & kOpModulate) != 0u) {
                fore = modulate(fore, rgb);
            }
        } else {
            fore = pack(rgb);
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
#endif
