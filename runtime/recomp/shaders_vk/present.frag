#version 450
// Present pass: sample the R16_UINT VRAM image (PSX 1555) at the display region (push constant),
// unpack 1555 -> RGB. Integer texture, so texelFetch (nearest); upscale filtering is the swapchain's.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform usampler2D u_vram;
// PUSH-CONSTANT: disp=[x,y,w,h] at FRAGMENT offset 0. fade at FRAGMENT offset 48 — the [16,48) range is the
// VERTEX-stage pane transform (tri/tritex), a DIFFERENT stage, so the cross-stage byte overlap is allowed by
// validation. fade = {mode(0 none/1 additive-to-white/2 subtractive-to-black), r, g, b} (r/g/b are 0..255).
layout(push_constant) uniform PC { ivec4 disp; layout(offset = 48) ivec4 fade; } pc;   // x,y,w,h ; mode,r,g,b
void main() {
    ivec2 t = pc.disp.xy + ivec2(v_uv * vec2(pc.disp.zw));
    // Clamp to the sampled region (disp) itself — never a stale image-height literal. disp=[x,y,w,h]
    // is the exact FB/display rect (rows >=512 under hi-res) and is always within the image. The old
    // hardcoded 991 (from when the image was 992 tall) collapsed every row past it onto one scanline
    // once FB_MAXH grew to 720 (image now 1232 tall) -> the bottom-of-screen vertical-streak smear.
    t = clamp(t, pc.disp.xy, pc.disp.xy + pc.disp.zw - ivec2(1));
    uint p = texelFetch(u_vram, t, 0).r;
    vec3 rgb = vec3(float(p & 31u) / 31.0, float((p >> 5) & 31u) / 31.0, float((p >> 10) & 31u) / 31.0);
    // Engine-owned screen FADE (replaces the PSX full-screen semi OT rect): additive = fade to/from white,
    // subtractive = fade to/from black, clamped per channel. col = fade.rgb / 255.
    if (pc.fade.x == 1)      rgb = min(rgb + vec3(pc.fade.yzw) / 255.0, vec3(1.0));   // additive (white)
    else if (pc.fade.x == 2) rgb = max(rgb - vec3(pc.fade.yzw) / 255.0, vec3(0.0));   // subtractive (black)
    o_col = vec4(rgb, 1.0);
}
