#version 450
// Present pass: sample the R16_UINT VRAM image (PSX 1555) at the display region (push constant),
// unpack 1555 -> RGB. Integer texture, so texelFetch (nearest); upscale filtering is the swapchain's.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform usampler2D u_vram;
layout(push_constant) uniform PC { ivec4 disp; } pc;   // x, y, w, h (VRAM texels)
void main() {
    ivec2 t = pc.disp.xy + ivec2(v_uv * vec2(pc.disp.zw));
    t = clamp(t, ivec2(0), ivec2(1023, 511));
    uint p = texelFetch(u_vram, t, 0).r;
    o_col = vec4(float(p & 31u) / 31.0, float((p >> 5) & 31u) / 31.0, float((p >> 10) & 31u) / 31.0, 1.0);
}
