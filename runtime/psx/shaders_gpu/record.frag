#version 450
#extension GL_GOOGLE_include_directive : require
// Record rasterizer: one PSX pixel per fragment, by gpu.c's rules; record_raster_setup.h sets up each op.
// The target is a plane: the VRAM image or a display canvas, whose pixel p is VRAM-space pixel p + origin * scale.
#include "record_op.glsl"

layout(location = 0) flat in uvec4 v_info;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_target; // the target plane before this batch
layout(set = 2, binding = 1) uniform sampler2D u_vram;   // the VRAM image before this batch
layout(std430, set = 2, binding = 2) readonly buffer Ops { uint ops[]; };
layout(std430, set = 2, binding = 3) readonly buffer Cluts { uint cluts[]; };
layout(std430, set = 2, binding = 4) readonly buffer Pixels { uint pixels[]; };
layout(set = 3, binding = 0) uniform Params { ivec4 scaleOrigin; } params; // scale, origin x, origin y

uint op;
int S;

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

// gpu_common.h GetTexel.
uint texel(uint u, uint v, uint flags) {
    uint tm = (flags >> kOpTexModeShift) & 3u;
    uint uext = (u & word(kOpWindowXAnd)) + word(kOpWindowXAdd);
    int fx = int((uext >> (2u - tm)) & uint(kVramWidth - 1));
    int fy = int(((v & word(kOpWindowYAnd)) + word(kOpWindowYAdd)) & uint(kVramHeight - 1));
    uint t = fetch(ivec2(fx * S, fy * S));
    if (tm == 0u) {
        t = clutEntry(word(kOpClut) + ((t >> ((uext & 3u) * 4u)) & 0xFu));
    } else if (tm == 1u) {
        t = clutEntry(word(kOpClut) + ((t >> ((uext & 1u) * 8u)) & 0xFFu));
    }
    return t;
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

void write(uint value) {
    o_col = vec4(float(value & 0xFFu) / 255.0, float((value >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}

void main() {
    op = v_info.x;
    S = params.scaleOrigin.x;
    ivec2 target = ivec2(gl_FragCoord.xy);
    ivec2 p = target + params.scaleOrigin.yz * S;
    // Integer floor division (a float divide is inexact at S = 3); biased because a canvas reaches left of VRAM.
    ivec2 n = (p + kVramWidth * S) / S - kVramWidth;
    uint flags = word(kOpFlags);
    uint kind = flags & kOpKindMask;
    if ((flags & kOpSkipRows) != 0u && uint(n.y & 1) == ((flags & kOpSkipParity) != 0u ? 1u : 0u)) {
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
        ivec2 src = ivec2((int(word(kOpSourceX)) * S + rx) % vramWidth, (int(word(kOpSourceY)) * S + ry) % vramHeight);
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
        if (kind == kRecordOpKindTriangle) {
            uint part = kOpParts + v_info.y * kPartWords;
            int yi = p.y - int(word(part + kPartRowShift));
            int left = edgeX(part + kPartLeft, part + kPartLeftStep, yi);
            int right = edgeX(part + kPartRight, part + kPartRightStep, yi);
            int x0 = wrap(left + vramWidth, 2 * vramWidth) - vramWidth;
            if (p.x < x0 || p.x >= x0 + (right - left)) {
                discard;
            }
            uint xi = uint(p.x + (left - x0));
            uint value[5];
            for (uint c = 0u; c < 5u; c++) {
                value[c] = (word(kOpBase + c) + word(kOpDx + c) * xi + word(kOpDy + c) * uint(yi)) >> 24;
            }
            r = int(value[0]);
            g = int(value[1]);
            b = int(value[2]);
            u = value[3];
            v = value[4];
        } else {
            int du = (flags & kOpFlipX) != 0u ? -1 : 1;
            int dv = (flags & kOpFlipY) != 0u ? -1 : 1;
            // The native pixel of the sprite this fragment shows, the sprite moved by its place.
            ivec2 shift = ivec2(int(word(kOpShiftX)), int(word(kOpShiftY)));
            ivec2 m = (p - shift + kVramWidth * S) / S - kVramWidth;
            u = uint(int(word(kOpSourceX)) + (m.x - int(word(kOpOriginX))) * du) & 0xFFu;
            v = uint(int(word(kOpSourceY)) + (m.y - int(word(kOpOriginY))) * dv) & 0xFFu;
        }
        if (textured) {
            fore = texel(u, v, flags);
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
