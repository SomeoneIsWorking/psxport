#version 450
layout(location=0) in vec2 v_uv;
layout(set=2,binding=0) uniform sampler2D u_color;
layout(set=2,binding=1) uniform sampler2D u_depth;
layout(location=0) out vec4 o_col;
void main(){ float d=texture(u_depth,v_uv).r; if(d==0.0) discard; o_col=texture(u_color,v_uv); gl_FragDepth=d; }
