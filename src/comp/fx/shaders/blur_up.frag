#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex0;

layout(std140, set = 1, binding = 0) uniform Params {
    FxCommon common_hdr;
    vec4 src_rect;
    vec4 valid_bounds;
    vec2 inv_tex_size;
    float offset;
    float _pad0;
} u;

vec4 sample_clamped(vec2 src_pos) {
    vec2 p = clamp(src_pos, u.valid_bounds.xy, u.valid_bounds.zw);
    return texture(u_tex0, p * u.inv_tex_size);
}

void main() {
    vec2 src_pos = u.src_rect.xy + uv * u.src_rect.zw;
    vec2 d = vec2(u.offset);
    vec4 c = vec4(0.0);
    c += sample_clamped(src_pos + vec2(-d.x * 2.0, 0.0)) * (1.0 / 12.0);
    c += sample_clamped(src_pos + vec2( d.x * 2.0, 0.0)) * (1.0 / 12.0);
    c += sample_clamped(src_pos + vec2(0.0, -d.y * 2.0)) * (1.0 / 12.0);
    c += sample_clamped(src_pos + vec2(0.0,  d.y * 2.0)) * (1.0 / 12.0);
    c += sample_clamped(src_pos + vec2(-d.x, -d.y))      * (2.0 / 12.0);
    c += sample_clamped(src_pos + vec2( d.x, -d.y))      * (2.0 / 12.0);
    c += sample_clamped(src_pos + vec2(-d.x,  d.y))      * (2.0 / 12.0);
    c += sample_clamped(src_pos + vec2( d.x,  d.y))      * (2.0 / 12.0);
    out_color = c;
}
