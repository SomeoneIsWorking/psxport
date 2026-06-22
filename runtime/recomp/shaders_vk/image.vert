#version 450
// Fullscreen triangle (no vertex buffer): 3 verts covering the viewport, UV 0..1. Used by the
// PC-native fullscreen IMAGE present (gpu_vk_present_image): a plain RGBA8 texture drawn
// letterboxed to 4:3 (the viewport does the letterbox; this just maps uv across the pane).
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
