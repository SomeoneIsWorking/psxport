#version 450
// Textured PSX triangle. Vertices in VRAM coords; UV is AFFINE (noperspective) to match PSX mapping.
// State (texpage base, color mode, CLUT) is constant per primitive -> flat-interpolated.
layout(location = 0) in vec2  i_pos;    // VRAM pixel coords (post draw-offset)
layout(location = 1) in vec2  i_uv;     // texel coords within the page (0..255)
layout(location = 2) in vec3  i_col;    // modulation color 0..1 (128/255 = neutral 1.0x)
layout(location = 3) in ivec4 i_tp;     // tpx, tpy (VRAM px base of page), mode(0=4bpp,1=8bpp,2=15bpp), raw
layout(location = 4) in ivec4 i_clut;   // clutx, cluty (VRAM px), -, -
layout(location = 5) in ivec4 i_tw;     // texture window: mask_x, mask_y, off_x, off_y (8px units)
layout(location = 6) in ivec4 i_da;     // draw-area clip: x0, y0, x1, y1 (VRAM px)
layout(location = 0) out vec3 v_col;
layout(location = 1) noperspective out vec2 v_uv;
layout(location = 2) flat out ivec4 v_tp;
layout(location = 3) flat out ivec4 v_clut;
layout(location = 4) flat out ivec4 v_tw;
layout(location = 5) flat out ivec4 v_da;
// PC-native widescreen / supersample transform (vertex push constant, offset 16 to clear the
// fragment present push at 0). wa=(enabled, fb_y0, ss, img_h); wb=(wide_off, fbw, fbh, fb_x0).
layout(push_constant) uniform VPC { layout(offset = 16) ivec4 wa; ivec4 wb; } w;
void main() {
    v_col = i_col; v_uv = i_uv; v_tp = i_tp; v_clut = i_clut; v_tw = i_tw;
    float ny = float(w.wa.w) * 0.5;   // image is IMG_H tall; map VRAM y -> NDC over the full image
    if (w.wa.x != 0) {
        // Relocate into the scratch FB at a wider FOV: keep native projection scale (no squish), just
        // re-center the framebuffer-local view into the wider FB. da.xy is the active framebuffer origin.
        float ss = float(w.wa.z);
        vec2 local = i_pos - vec2(i_da.xy);
        vec2 fb = vec2((local.x + float(w.wb.x)) * ss + float(w.wb.w),
                       float(w.wa.y) + local.y * ss);
        v_da = ivec4(w.wb.w, w.wa.y, w.wb.w + w.wb.y - 1, w.wa.y + w.wb.z - 1);   // clip = FB rect
        gl_Position = vec4(fb.x / 512.0 - 1.0, fb.y / ny - 1.0, 0.0, 1.0);
    } else {
        v_da = i_da;
        gl_Position = vec4(i_pos.x / 512.0 - 1.0, i_pos.y / ny - 1.0, 0.0, 1.0);
    }
}
