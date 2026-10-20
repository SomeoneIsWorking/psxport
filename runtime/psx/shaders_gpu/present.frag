#version 450
// The guest display region of the VRAM image, decoded to RGB with the screen fade applied.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
// RG8 holds each halfword's low and high byte: SDL_GPU cannot sample an integer format.
layout(set = 2, binding = 0) uniform sampler2D u_vram;
layout(set = 3, binding = 0) uniform PC {
    ivec4 disp;   // x, y, w, h — VRAM display/FB region to sample
    ivec4 fade;   // mode (0 none / 1 additive-white / 2 subtractive-black), r, g, b (0..255)
    ivec4 fmt;    // x = display colour depth: 0 = 15-bit (1555), 1 = 24-bit (packed RGB888)
} pc;
// VRAM byte bx of row y, as a 0..1 channel.
float vram_byte(int bx, int y) {
    vec2 rg = texelFetch(u_vram, ivec2((bx >> 1) & 1023, y), 0).rg;
    return ((bx & 1) == 0) ? rg.r : rg.g;
}
void main() {
    ivec2 t = pc.disp.xy + ivec2(v_uv * vec2(pc.disp.zw));
    t = clamp(t, pc.disp.xy, pc.disp.xy + pc.disp.zw - ivec2(1));
    vec3 rgb;
    if (pc.fmt.x == 1) {
        // 24bpp column x starts at byte disp.x * 2 + x * 3.
        int lx = clamp(int(v_uv.x * float(pc.disp.z)), 0, pc.disp.z - 1);
        int bx = pc.disp.x * 2 + lx * 3;
        rgb = vec3(vram_byte(bx, t.y), vram_byte(bx + 1, t.y), vram_byte(bx + 2, t.y));
    } else {
    vec2 rg = texelFetch(u_vram, t, 0).rg;
    uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
    rgb = vec3(float(p & 31u) / 31.0, float((p >> 5) & 31u) / 31.0, float((p >> 10) & 31u) / 31.0);
    }
    if (pc.fade.x == 1)      rgb = min(rgb + vec3(pc.fade.yzw) / 255.0, vec3(1.0));   // additive (white)
    else if (pc.fade.x == 2) rgb = max(rgb - vec3(pc.fade.yzw) / 255.0, vec3(0.0));   // subtractive (black)
    o_col = vec4(rgb, 1.0);
}
