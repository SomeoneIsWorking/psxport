#version 450
// Shadow-map DEPTH pass vertex stage. Renders the OPAQUE world geometry — captured at submit time as
// VIEW-SPACE positions (the same metric view space the deferred pass reconstructs from the depth buffer:
// x = ir1, y = ir2, z = pz; see gte_beetle proj_native_xform / native_terrain wv[]) — from the
// DIRECTIONAL LIGHT's orthographic view. The only transform is light_view_proj (built CPU-side each frame
// from g_mods.light_dir + an ortho volume around the view origin), so this pass owns no PSX intricacy: it
// is a plain PC light-space raster of the engine's own scene geometry. gl_Position.z lands in [0,1] (the
// ortho's near/far map), written into a D32 shadow image the deferred pass then compares against.
layout(location = 0) in vec3 in_vpos;          // view-space position (x,y,z=pz) of this world vertex
layout(push_constant) uniform PC { mat4 light_view_proj; } pc;
void main() { gl_Position = pc.light_view_proj * vec4(in_vpos, 1.0); }
