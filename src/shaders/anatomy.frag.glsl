#version 450
layout(set = 1, binding = 0) uniform U {
    mat4 u_mvp;
    vec4 u_light;
} u;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 n = normalize(v_normal);
    float diff = max(dot(n, normalize(u.u_light.xyz)), 0.0);
    float light = 0.38 + 0.62 * diff;
    vec4 base = texture(u_tex, v_uv);
    o_color = vec4(base.rgb * light, 1.0);
}
