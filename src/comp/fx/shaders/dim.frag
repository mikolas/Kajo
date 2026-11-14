#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(std140, set = 1, binding = 0) uniform Params {
    FxCommon common_hdr;
    vec4 color_lin;
    vec4 outer_rect;
} u;

void main() {
    vec2 p = u.common_hdr.dst_origin + uv * u.common_hdr.dst_size;
    if (p.x < u.outer_rect.x || p.x > u.outer_rect.z ||
        p.y < u.outer_rect.y || p.y > u.outer_rect.w) {
        discard;
    }

    float alpha = clamp(u.common_hdr.opacity * u.color_lin.a, 0.0, 1.0);
    if (alpha <= 0.0001) {
        discard;
    }

    out_color = vec4(u.color_lin.rgb * alpha, alpha);
}
