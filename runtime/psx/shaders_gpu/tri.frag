#version 450
// SDL_GPU untextured opaque PSX triangle (fragment). The VRAM color target is R8G8_UNORM (R=low byte,
// G=high byte of the 1555 word — SDL_GPU forbids integer SAMPLER formats, so we can't render to/sample an
// R16_UINT; RG8 round-trips the 16 bits exactly and is sampler-legal everywhere incl. Metal). Pack the PSX
// 1555 word, then emit its two bytes. STP bit left 0 (opaque).
layout(location = 0) in vec3 v_col;
layout(location = 1) flat in float v_gouraud;
layout(location = 2) flat in float v_dither;
layout(location = 3) flat in ivec4 v_da;     // draw-area clip
// ires (internal-resolution) scale, exactly as tritex.frag uses it: v_da is in NATIVE VRAM pixel
// units while gl_FragCoord spans the scaled target, so it must be divided back down before the
// comparison. 1 at i==1, a no-op divide.
layout(set = 3, binding = 0) uniform PC { int scale; } pc;
layout(location = 0) out vec4 o_col;
const int dm[16] = int[16](-4,0,-3,1, 2,-2,3,-1, -3,1,-4,0, 3,-1,2,-2);
void main() {
    // THE DRAW-AREA CLIP, which this pipeline did not have. The PSX GPU clips EVERY primitive to the
    // drawing area; tritex.frag enforced it and this shader did not, so untextured polygons were
    // drawn unclipped. Measured on Spyro's title screen: the frame drawing into the back buffer
    // (draw area (0,248)-(511,471), offset (0,240)) put sky triangles as high as y=223 — 25 rows
    // above its own clip — straight into the FRONT buffer's bottom rows, which is the blue band
    // along the bottom edge of the picture. The same prims also ran past x=511 into the texture
    // atlas. Every spilling prim was tex=0; not one textured prim escaped, because that path had
    // this test.
    int px = int(gl_FragCoord.x) / pc.scale, py = int(gl_FragCoord.y) / pc.scale;
    if (px < v_da.x || px > v_da.z || py < v_da.y || py > v_da.w) discard;
    ivec3 c8 = ivec3(clamp(v_col, 0.0, 1.0) * 255.0 + 0.5);
    if (v_gouraud > 0.5 && v_dither > 0.5) {
        c8 = clamp(c8 + ivec3(dm[(py & 3) * 4 + (px & 3)]), ivec3(0), ivec3(255));
    }
    uvec3 c5 = uvec3(c8) >> 3;
    uint w = c5.r | (c5.g << 5) | (c5.b << 10);
    o_col = vec4(float(w & 0xFFu) / 255.0, float((w >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
