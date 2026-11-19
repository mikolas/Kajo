#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(std140, set = 1, binding = 0) uniform Params {
    FxCommon common_hdr;
    vec4 color_lin;
    vec4 outer_rect;
    vec4 layers[4];
} u;

// Abramowitz & Stegun 7.1.26 erf approximation (max abs error <= 1.5e-7)
vec2 approx_erf(vec2 x) {
    vec2 s = sign(x);
    vec2 ax = abs(x);
    vec2 t = 1.0 / (1.0 + 0.3275911 * ax);
    vec2 y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t
        - 0.284496736) * t + 0.254829592) * t * exp(-ax * ax);
    return s * y;
}

void main() {
    vec2 p = u.common_hdr.dst_origin + uv * u.common_hdr.dst_size;

    // Discard inside the surface's own outer box so translucent windows never
    // reveal their own shadow through themselves (spec §11.2).
    if (p.x >= u.outer_rect.x && p.x <= u.outer_rect.z &&
        p.y >= u.outer_rect.y && p.y <= u.outer_rect.w) {
        discard;
    }

    if (u.common_hdr.opacity <= 0.0) {
        discard;
    }

    float alpha = 0.0;
    uint n_layers = min(u.common_hdr.flags, 4u);
    for (uint i = 0u; i < n_layers; i++) {
        vec4 l = u.layers[i];
        float sigma = l.z;
        float layer_op = l.w;
        if (sigma <= 0.0 || layer_op <= 0.0) {
            continue;
        }
        float inv_s2 = 1.0 / (sigma * 1.4142135623730950488);
        vec2 x_edges = (p.x - vec2(u.outer_rect.x + l.x, u.outer_rect.z + l.x)) * inv_s2;
        vec2 y_edges = (p.y - vec2(u.outer_rect.y + l.y, u.outer_rect.w + l.y)) * inv_s2;
        vec2 ex = approx_erf(x_edges);
        vec2 ey = approx_erf(y_edges);
        float cov = clamp(0.25 * (ex.x - ex.y) * (ey.x - ey.y), 0.0, 1.0);
        float la = clamp(cov * layer_op * u.common_hdr.opacity, 0.0, 1.0);
        alpha = alpha + la * (1.0 - alpha);
    }

    alpha *= u.color_lin.a;
    if (alpha <= 0.0001) {
        discard;
    }

    out_color = vec4(u.color_lin.rgb * alpha, alpha);
}
