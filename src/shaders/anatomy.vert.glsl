#version 450
layout(set = 1, binding = 0) uniform U {
    mat4 u_mvp;
    vec4 u_light;
} u;
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
void main() {
    gl_Position = u.u_mvp * vec4(a_pos, 1.0);
    v_normal = a_normal;
    v_uv = a_uv;
}
