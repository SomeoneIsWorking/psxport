#version 450
// The guest display region of the VRAM image, decoded to RGB with the screen fade applied.
// PRESENT_BOX: each pixel averages samples spread over the picture texels it covers, for a downscaled picture such
// as the bug report's clip, which one point sample would alias.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
// RG8 holds each halfword's low and high byte: SDL_GPU cannot sample an integer format.
layout(set = 2, binding = 0) uniform sampler2D u_vram;
layout(set = 3, binding = 0) uniform PC {
    ivec4 disp;   // x, y, w, h — VRAM display/FB region to sample; x is a byte at 24bpp
    ivec4 fade;   // mode (0 none / 1 additive-white / 2 subtractive-black), r, g, b (0..255)
    ivec4 fmt;    // x = display colour depth: 0 = 15-bit (1555), 1 = 24-bit (packed RGB888); y..z = rows scanned out
    ivec4 cols;   // x..y = columns holding picture; the rest is pillarbox
} pc;
// VRAM byte bx of row y, as a 0..1 channel.
float vram_byte(int bx, int y) {
    vec2 rg = texelFetch(u_vram, ivec2((bx >> 1) & 1023, y), 0).rg;
    return ((bx & 1) == 0) ? rg.r : rg.g;
}
// Picture texel l (column, row from the display's origin): pillarbox and unscanned rows black, edges clamped.
vec3 texel(ivec2 l) {
    ivec2 t = pc.disp.xy + l;
    if (t.y < pc.fmt.y || t.y >= pc.fmt.z || t.x < pc.cols.x || t.x >= pc.cols.y) {
        return vec3(0.0);
    }
    t = clamp(t, pc.disp.xy, pc.disp.xy + pc.disp.zw - ivec2(1));
    vec3 rgb;
    if (pc.fmt.x == 1) {
        // 24bpp column x starts at byte disp.x + x * 3.
        int lx = clamp(l.x, 0, pc.disp.z - 1);
        int bx = pc.disp.x + lx * 3;
        rgb = vec3(vram_byte(bx, t.y), vram_byte(bx + 1, t.y), vram_byte(bx + 2, t.y));
    } else {
    vec2 rg = texelFetch(u_vram, t, 0).rg;
    uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
    rgb = vec3(float(p & 31u) / 31.0, float((p >> 5) & 31u) / 31.0, float((p >> 10) & 31u) / 31.0);
    }
    if (pc.fade.x == 1)      rgb = min(rgb + vec3(pc.fade.yzw) / 255.0, vec3(1.0));   // additive (white)
    else if (pc.fade.x == 2) rgb = max(rgb - vec3(pc.fade.yzw) / 255.0, vec3(0.0));   // subtractive (black)
    return rgb;
}
void main() {
#ifdef PRESENT_BOX
    // This pixel's footprint in picture texels, averaged over an even grid of up to 4x4 samples across it.
    vec2 c = v_uv * vec2(pc.disp.zw);
    vec2 f = vec2(abs(dFdx(c.x)), abs(dFdy(c.y)));
    ivec2 n = clamp(ivec2(ceil(f)), ivec2(1), ivec2(4));
    vec2 step = f / vec2(n);
    vec2 lo = c - 0.5 * f + 0.5 * step;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < n.y; y++) {
        for (int x = 0; x < n.x; x++) {
            sum += texel(ivec2(floor(lo + vec2(x, y) * step)));
        }
    }
    o_col = vec4(sum / float(n.x * n.y), 1.0);
#else
    o_col = vec4(texel(ivec2(v_uv * vec2(pc.disp.zw))), 1.0);
#endif
}
