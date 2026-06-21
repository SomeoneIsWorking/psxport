#version 450
// Untextured opaque PSX triangle. The color attachment is a BLENDABLE A1R5G5B5_UNORM_PACK16 view aliasing
// the R16_UINT VRAM image (same 16-bit compat class). A1R5G5B5 decodes the word as A=bit15, R=bits10-14,
// G=5-9, B=bits0-4 — PSX 1555 with R<->B SWAPPED. To keep the STORED BITS identical to PSX 1555 (PSX-red in
// the low 5), we emit PSX-blue in the view's R slot and PSX-red in its B slot (output swizzle). The stored
// word == the old `uint o_px`, so VRAM copy/upload/present/readback stay unchanged.
layout(location = 0) in vec3 v_col;
layout(location = 0) out vec4 o_col;
void main() {
    // Quantize to 5 bits to match the old integer pack exactly (round-to-nearest), then renormalize so the
    // UNORM attachment stores the identical 5-bit code. Output order = (B,G,R,STP) for the A1R5G5B5 view.
    float r = floor(clamp(v_col.r, 0.0, 1.0) * 31.0 + 0.5);
    float g = floor(clamp(v_col.g, 0.0, 1.0) * 31.0 + 0.5);
    float b = floor(clamp(v_col.b, 0.0, 1.0) * 31.0 + 0.5);
    o_col = vec4(b / 31.0, g / 31.0, r / 31.0, 0.0);
}
