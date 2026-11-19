#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex0; // previous snapshot
layout(set = 0, binding = 1) uniform sampler2D u_tex1; // current snapshot

layout(std140, set = 1, binding = 0) uniform Params {
    FxCommon common_hdr;
    float progress;
    float eased_progress;
    uint preset;
    uint kind;
    vec2 prev_size;
    vec2 curr_size;
    vec2 uv_scale0;
    vec2 uv_scale1;
} u;

float kajo_resize_hash(float n) {
    return fract(sin(n * 113.57) * 43758.5453);
}

vec2 unrotate_uv(vec2 p, uint tf) {
    if (tf == 1u) return vec2(1.0 - p.y, p.x);
    if (tf == 2u) return vec2(1.0 - p.x, 1.0 - p.y);
    if (tf == 3u) return vec2(p.y, 1.0 - p.x);
    if (tf == 4u) return vec2(1.0 - p.x, p.y);
    if (tf == 5u) return vec2(p.y, p.x);
    if (tf == 6u) return vec2(p.x, 1.0 - p.y);
    if (tf == 7u) return vec2(1.0 - p.y, 1.0 - p.x);
    return p;
}

void main() {
    float p = clamp(u.eased_progress, 0.0, 1.0);
    vec2 luv = unrotate_uv(uv, u.common_hdr.flags);
    vec4 col = vec4(0.0);

    if (u.preset == 1u) {
        // stretch-next
        col = texture(u_tex1, luv * u.uv_scale1);
    } else if (u.preset == 2u) {
        // pixelate-morph
        float pulse = sin(p * 3.14159265);
        float block = max(1.0, floor(1.0 + pulse * pulse * 24.0));
        vec2 sz = mix(u.prev_size, u.curr_size, p);
        vec2 grid = max(sz / block, vec2(1.0));
        vec2 suv = (floor(luv * grid) + 0.5) / grid;
        col = mix(texture(u_tex0, suv * u.uv_scale0), texture(u_tex1, suv * u.uv_scale1), p);
    } else if (u.preset == 3u) {
        // glitch-morph
        float pulse = sin(p * 3.14159265);
        float band = floor(luv.y * 20.0);
        float jitter = (kajo_resize_hash(band + floor(p * 10.0)) - 0.5) * 0.06 * pulse;
        vec2 suv = vec2(clamp(luv.x + jitter, 0.0, 1.0), luv.y);
        col = mix(texture(u_tex0, suv * u.uv_scale0), texture(u_tex1, suv * u.uv_scale1), p);
    } else {
        // default: smooth crossfade
        col = mix(texture(u_tex0, luv * u.uv_scale0), texture(u_tex1, luv * u.uv_scale1), p);
    }

    if (col.a <= 0.0001) {
        discard;
    }
    out_color = col * clamp(u.common_hdr.opacity, 0.0, 1.0);
}
