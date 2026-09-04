#version 450
// Unpack the PSX 1555 VRAM word (R8G8_UNORM: R=low byte, G=high byte) into float RGBA, for the semi-blend
// intermediate target (2026-07-01 dark-outline fix, gpu_vk.cpp render_geom). Alpha is unused by the
// decode step itself (the semi shader's OWN alpha output drives the HW blend factor) — set to 0.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_vram;
void main() {
    // textureSize(), not a hardcoded 1024x512: this pass also runs at ires (internal-resolution) scale,
    // targeting a VRAM_W*i x VRAM_H*i intermediate (gpu_vk.cpp render_geom) — querying the BOUND
    // texture's actual size keeps v_uv's [0,1] fullscreen-triangle span mapped 1:1 to whichever target is
    // live, at i==1 or scaled, with zero extra state.
    ivec2 sz = textureSize(u_vram, 0);
    ivec2 t = ivec2(v_uv * vec2(sz));
    vec2 rg = texelFetch(u_vram, t, 0).rg;
    uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
    vec3 rgb = vec3(float(p & 31u) / 31.0, float((p >> 5) & 31u) / 31.0, float((p >> 10) & 31u) / 31.0);
    o_col = vec4(rgb, 0.0);
}
