#version 450
// Record rasterizer: one PSX pixel per fragment, by gpu.c's rules. Coverage and interpolant bases
// come from record_raster_setup.cpp; op word layout is RecordOpField there. The target is a plane: the
// VRAM image or a display canvas, whose pixel p is VRAM-space pixel p + origin * scale.
layout(location = 0) flat in uvec4 v_info;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_target; // the target plane before this batch
layout(set = 2, binding = 1) uniform sampler2D u_vram;   // the VRAM image before this batch
layout(std430, set = 2, binding = 2) readonly buffer Ops { uint ops[]; };
layout(std430, set = 2, binding = 3) readonly buffer Cluts { uint cluts[]; };
layout(std430, set = 2, binding = 4) readonly buffer Pixels { uint pixels[]; };
layout(set = 3, binding = 0) uniform Params { ivec4 scaleOrigin; } params; // scale, origin x, origin y

const uint KIND_TRIANGLE = 0u;
const uint KIND_SPRITE = 1u;
const uint KIND_LINE = 2u;
const uint KIND_FILL = 3u;
const uint KIND_COPY = 4u;
const uint KIND_UPLOAD = 5u;
const uint F_TEXTURED = 1u << 8;
const uint F_MODULATE = 1u << 9;
const uint F_GOURAUD = 1u << 10;
const uint F_DITHER = 1u << 11;
const uint F_SEMI = 1u << 12;
const uint F_MASK_CHECK = 1u << 13;
const uint F_MASK_SET = 1u << 14;
const uint F_SKIP_ROWS = 1u << 20;
const uint F_SKIP_PARITY = 1u << 21;
const uint F_FLIP_X = 1u << 22;
const uint F_FLIP_Y = 1u << 23;

const int DITHER[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

uint op;
int S;

uint word(int index) { return ops[op * 32u + uint(index)]; }

uint halfword(vec2 rg) { return uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8); }
uint fetch(ivec2 p) { return halfword(texelFetch(u_vram, p, 0).rg); }

uint clutEntry(uint index) { return (cluts[index >> 1] >> ((index & 1u) * 16u)) & 0xFFFFu; }
uint poolPixel(uint index) { return (pixels[index >> 1] >> ((index & 1u) * 16u)) & 0xFFFFu; }

// gpu_common.h GetTexel.
uint texel(uint u, uint v, uint flags) {
    uint tm = (flags >> 18) & 3u;
    uint uext = (u & word(2)) + word(3);
    int fx = int((uext >> (2u - tm)) & 1023u);
    int fy = int(((v & word(4)) + word(5)) & 511u);
    uint t = fetch(ivec2(fx * S, fy * S));
    if (tm == 0u) {
        t = clutEntry(word(6) + ((t >> ((uext & 3u) * 4u)) & 0xFu));
    } else if (tm == 1u) {
        t = clutEntry(word(6) + ((t >> ((uext & 1u) * 8u)) & 0xFFu));
    }
    return t;
}

uint lut(int value, int d) { return uint(clamp((value + d) >> 3, 0, 31)); }

uint modulate(uint t, int r, int g, int b, int d) {
    return (t & 0x8000u) | lut(int(((t & 0x1Fu) * uint(r)) >> 4), d) |
           (lut(int(((t & 0x3E0u) * uint(g)) >> 9), d) << 5) | (lut(int(((t & 0x7C00u) * uint(b)) >> 14), d) << 10);
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

void write(uint value) {
    o_col = vec4(float(value & 0xFFu) / 255.0, float((value >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}

void main() {
    op = v_info.x;
    S = params.scaleOrigin.x;
    ivec2 target = ivec2(gl_FragCoord.xy);
    ivec2 p = target + params.scaleOrigin.yz * S;
    // Floor division: a canvas reaches left of VRAM.
    ivec2 n = ivec2(floor(vec2(p) / float(S)));
    uint flags = word(0);
    uint kind = flags & 0xFFu;
    if ((flags & F_SKIP_ROWS) != 0u && uint(n.y & 1) == ((flags & F_SKIP_PARITY) != 0u ? 1u : 0u)) {
        discard;
    }
    uint dst = halfword(texelFetch(u_target, target, 0).rg);
    bool maskCheck = (flags & F_MASK_CHECK) != 0u;
    uint maskSet = (flags & F_MASK_SET) != 0u ? 0x8000u : 0u;
    if (kind == KIND_FILL) {
        write(word(1));
        return;
    }
    if (kind == KIND_COPY) {
        int rx = (p.x - int(word(22)) * S + 1024 * S) % (1024 * S);
        int ry = (p.y - int(word(23)) * S + 512 * S) % (512 * S);
        ivec2 src = ivec2((int(word(24)) * S + rx) % (1024 * S), (int(word(25)) * S + ry) % (512 * S));
        if (maskCheck && (dst & 0x8000u) != 0u) {
            discard;
        }
        write(fetch(src) | maskSet);
        return;
    }
    if (kind == KIND_UPLOAD) {
        int rx = (n.x - int(word(22)) + 1024) % 1024;
        int ry = (n.y - int(word(23)) + 512) % 512;
        if (maskCheck && (dst & 0x8000u) != 0u) {
            discard;
        }
        write(poolPixel(word(26) + uint(ry) * word(24) + uint(rx)) | maskSet);
        return;
    }
    bool textured = (flags & F_TEXTURED) != 0u;
    bool dither = (flags & F_DITHER) != 0u;
    int d = DITHER[(n.y & 3) * 4 + (n.x & 3)];
    uint colour = word(1);
    int r = int(colour & 0xFFu);
    int g = int((colour >> 8) & 0xFFu);
    int b = int((colour >> 16) & 0xFFu);
    uint fore;
    if (kind == KIND_LINE) {
        fore = v_info.y;
    } else {
        uint u = 0u;
        uint v = 0u;
        if (kind == KIND_TRIANGLE) {
            uint xi = uint(p.x) + v_info.y;
            uint yi = uint(p.y) + v_info.z;
            uint value[5];
            for (int c = 0; c < 5; c++) {
                value[c] = (word(7 + c) + word(12 + c) * xi + word(17 + c) * yi) >> 24;
            }
            r = int(value[0]);
            g = int(value[1]);
            b = int(value[2]);
            u = value[3];
            v = value[4];
        } else {
            int du = (flags & F_FLIP_X) != 0u ? -1 : 1;
            int dv = (flags & F_FLIP_Y) != 0u ? -1 : 1;
            u = uint(int(word(24)) + (n.x - int(word(22))) * du) & 0xFFu;
            v = uint(int(word(25)) + (n.y - int(word(23))) * dv) & 0xFFu;
            dither = false;
        }
        if (textured) {
            fore = texel(u, v, flags);
            if (fore == 0u) {
                discard;
            }
            if ((flags & F_MODULATE) != 0u) {
                fore = modulate(fore, r, g, b, dither ? d : 0);
            }
        } else if (kind == KIND_TRIANGLE && (flags & F_GOURAUD) != 0u && dither) {
            fore = 0x8000u | lut(r, d) | (lut(g, d) << 5) | (lut(b, d) << 10);
        } else {
            fore = 0x8000u | uint(r >> 3) | (uint(g >> 3) << 5) | (uint(b >> 3) << 10);
        }
    }
    if ((flags & F_SEMI) != 0u && (fore & 0x8000u) != 0u) {
        fore = blend(fore, dst, (flags >> 16) & 3u);
    }
    if (maskCheck && (dst & 0x8000u) != 0u) {
        discard;
    }
    write(textured ? (fore | maskSet) : ((fore & 0x7FFFu) | maskSet));
}
