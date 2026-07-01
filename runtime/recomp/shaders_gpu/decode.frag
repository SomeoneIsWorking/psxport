#version 450
// Unpack the PSX 1555 VRAM word (R8G8_UNORM: R=low byte, G=high byte) into float RGBA, for the semi-blend
// intermediate target (2026-07-01 dark-outline fix, gpu_gpu.cpp render_geom). Alpha is unused by the
// decode step itself (the semi shader's OWN alpha output drives the HW blend factor) — set to 0.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_vram;
void main() {
    ivec2 t = ivec2(v_uv * vec2(1024.0, 512.0));
    vec2 rg = texelFetch(u_vram, t, 0).rg;
    uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
    vec3 rgb = vec3(float(p & 31u) / 31.0, float((p >> 5) & 31u) / 31.0, float((p >> 10) & 31u) / 31.0);
    o_col = vec4(rgb, 0.0);
}
