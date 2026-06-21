#version 450
// Shadow-map DEPTH pass fragment stage. Depth-only render (no color attachment): this writes nothing —
// gl_FragDepth is the rasterized depth, captured by the D32 shadow image. A fragment stage is required by
// Vulkan even for a depth-only pipeline.
void main() {}
