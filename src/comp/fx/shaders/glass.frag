#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex0;

layout(std140, set = 1, binding = 0) uniform Params {
    FxCommon common_hdr;
    vec4 crop_rect;
    vec4 valid_bounds;
    vec4 tint_lin;
    vec2 inv_tex_size;
    float saturation;
    float brightness;
} u;

void main() {
    if (u.common_hdr.opacity <= 0.0001) {
        discard;
    }

    vec2 src_pos = u.crop_rect.xy + uv * u.crop_rect.zw;
    vec2 p = clamp(src_pos, u.valid_bounds.xy, u.valid_bounds.zw);
    vec4 backdrop = texture(u_tex0, p * u.inv_tex_size);

    vec3 rgb = backdrop.rgb / max(backdrop.a, 1e-5);
    rgb *= u.brightness;

    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = max(mix(vec3(luma), rgb, u.saturation), vec3(0.0));

    float ta = clamp(u.tint_lin.a, 0.0, 1.0);
    rgb = mix(rgb, u.tint_lin.rgb, ta);

    float alpha = clamp(u.common_hdr.opacity, 0.0, 1.0);
    out_color = vec4(rgb * alpha, alpha);
}
