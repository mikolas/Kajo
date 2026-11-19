#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex0;

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

float kajo_hash1(float n) {
    return fract(sin(n * 91.3458 + 17.413) * 47453.5453);
}

float kajo_hash2(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7)) + 29.7) * 43758.5453);
}

float kajo_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 s = f * f * (3.0 - 2.0 * f);
    float a = kajo_hash2(i + vec2(0.0, 0.0));
    float b = kajo_hash2(i + vec2(1.0, 0.0));
    float c = kajo_hash2(i + vec2(0.0, 1.0));
    float d = kajo_hash2(i + vec2(1.0, 1.0));
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

vec4 sample_clamped(vec2 p) {
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
        return vec4(0.0);
    }
    return texture(u_tex0, p * u.uv_scale0);
}

vec4 eval_open(vec2 coords, float p) {
    if (u.preset == 1u) {
        // metro-flip
        float angle = (1.0 - p) * -1.25;
        float cos_a = cos(angle);
        float sin_a = sin(angle);
        float pivot_x = 0.08;
        if (abs(cos_a) < 0.05) return vec4(0.0);
        float s_u = pivot_x + (coords.x - pivot_x) / cos_a;
        float z = 1.0 - (s_u - pivot_x) * sin_a * 0.55;
        if (z <= 0.1) return vec4(0.0);
        float s_v = 0.5 + (coords.y - 0.5) * z;
        vec4 col = sample_clamped(vec2(s_u, s_v));
        float shade = clamp(1.0 - (1.0 - p) * (s_u - pivot_x) * 0.45, 0.4, 1.0);
        col.rgb *= shade;
        return col * smoothstep(0.0, 0.25, p);
    } else if (u.preset == 2u) {
        // expanding-circle
        float aspect = max(u.curr_size.x / max(u.curr_size.y, 1.0), 0.1);
        vec2 d = (coords - vec2(0.5)) * vec2(aspect, 1.0);
        float r = length(d);
        float max_r = length(vec2(0.5 * aspect, 0.5));
        float mask = 1.0 - smoothstep(max_r * p - 0.03, max_r * p + 0.01, r);
        return sample_clamped(coords) * mask;
    } else if (u.preset == 3u) {
        // crt-on
        float h_scale = clamp(p / 0.35, 0.02, 1.0);
        float v_t = clamp((p - 0.25) / 0.75, 0.0, 1.0);
        float v_scale = mix(0.008, 1.0, v_t * v_t);
        vec2 suv = vec2((coords.x - 0.5) / h_scale + 0.5, (coords.y - 0.5) / v_scale + 0.5);
        vec4 col = sample_clamped(suv);
        float flash = (1.0 - v_t) * 0.85;
        col.rgb = mix(col.rgb, vec3(col.a), flash);
        return col * clamp(p * 3.0, 0.0, 1.0);
    } else if (u.preset == 4u) {
        // pixelate-in
        float inv_p = 1.0 - p;
        float block = max(1.0, floor(1.0 + inv_p * inv_p * 42.0));
        vec2 grid = max(u.curr_size / block, vec2(1.0));
        vec2 suv = (floor(coords * grid) + 0.5) / grid;
        return sample_clamped(suv) * smoothstep(0.0, 0.35, p);
    } else if (u.preset == 5u) {
        // glitch-in
        float intensity = (1.0 - p) * (1.0 - p);
        float band = floor(coords.y * 24.0);
        float rnd = kajo_hash1(band + floor(p * 12.0));
        float shift = (rnd - 0.5) * 0.18 * intensity;
        vec2 base_uv = vec2(coords.x + shift, coords.y);
        if (base_uv.x < 0.0 || base_uv.x > 1.0 || base_uv.y < 0.0 || base_uv.y > 1.0) return vec4(0.0);
        float chroma = 0.025 * intensity;
        vec4 col_r = texture(u_tex0, vec2(clamp(base_uv.x + chroma, 0.0, 1.0), base_uv.y) * u.uv_scale0);
        vec4 col_g = texture(u_tex0, base_uv * u.uv_scale0);
        vec4 col_b = texture(u_tex0, vec2(clamp(base_uv.x - chroma, 0.0, 1.0), base_uv.y) * u.uv_scale0);
        return vec4(col_r.r, col_g.g, col_b.b, col_g.a) * smoothstep(0.0, 0.35, p);
    } else if (u.preset == 6u) {
        // curtain-open
        float dist = abs(coords.x - 0.5);
        float half_w = 0.5 * p;
        if (dist > half_w) return vec4(0.0);
        return sample_clamped(coords) * smoothstep(0.0, 0.25, p);
    }

    // default: crisp Metro scale (0.92 -> 1.0) & fade
    float sc = mix(0.92, 1.0, p);
    vec2 suv = (coords - vec2(0.5)) / sc + vec2(0.5);
    return sample_clamped(suv) * p;
}

vec4 eval_close(vec2 coords, float p) {
    if (u.preset == 1u) {
        // fall-and-rotate
        float fall = p * p * 0.45;
        float angle = p * 0.28;
        float c = cos(angle);
        float s = sin(angle);
        vec2 centered = coords - vec2(0.5, 0.5 + fall);
        vec2 suv = mat2(c, -s, s, c) * centered + vec2(0.5);
        return sample_clamped(suv) * (1.0 - p);
    } else if (u.preset == 2u) {
        // crt-off
        float v_t = clamp(p / 0.55, 0.0, 1.0);
        float h_t = clamp((p - 0.50) / 0.50, 0.0, 1.0);
        float v_scale = max(0.008, 1.0 - v_t);
        float h_scale = max(0.008, 1.0 - h_t);
        vec2 suv = vec2((coords.x - 0.5) / h_scale + 0.5, (coords.y - 0.5) / v_scale + 0.5);
        vec4 col = sample_clamped(suv);
        float beam = clamp(v_t * 0.9, 0.0, 1.0);
        col.rgb = mix(col.rgb, vec3(col.a), beam);
        return col * (1.0 - smoothstep(0.88, 1.0, p));
    } else if (u.preset == 3u) {
        // ash-dissolve
        vec2 suv = coords + vec2(0.0, p * 0.07);
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) return vec4(0.0);
        vec2 nc = suv * (u.curr_size / 16.0);
        float raw_n = 0.65 * kajo_noise(nc) + 0.35 * kajo_noise(nc * 2.3);
        float n = clamp((raw_n - 0.18) / 0.64, 0.0, 1.0);
        float threshold = p * 1.25 - (1.0 - suv.y) * 0.25;
        if (n < threshold) return vec4(0.0);
        vec4 col = texture(u_tex0, suv * u.uv_scale0);
        float edge = 1.0 - smoothstep(0.0, 0.14, n - threshold);
        vec3 ember = vec3(1.0, 0.22, 0.015) * col.a;
        col.rgb = mix(col.rgb, ember, edge * 0.90);
        return col * (1.0 - smoothstep(0.92, 1.0, p));
    } else if (u.preset == 4u) {
        // vortex-implode
        float sc = max(0.04, 1.0 - p * 0.92);
        vec2 centered = (coords - vec2(0.5)) / sc;
        float r = length(centered);
        float swirl = (1.0 - clamp(r * 1.4, 0.0, 1.0)) * p * 4.5;
        float c = cos(swirl);
        float s = sin(swirl);
        vec2 suv = mat2(c, -s, s, c) * centered + vec2(0.5);
        return sample_clamped(suv) * (1.0 - smoothstep(0.55, 1.0, p));
    } else if (u.preset == 5u) {
        // pixelate-out
        float block = max(1.0, floor(1.0 + p * p * 48.0));
        vec2 grid = max(u.curr_size / block, vec2(1.0));
        vec2 suv = (floor(coords * grid) + 0.5) / grid;
        return sample_clamped(suv) * (1.0 - p);
    } else if (u.preset == 6u) {
        // slice-shatter
        float slice = floor(coords.y * 18.0);
        float dir = (mod(slice, 2.0) < 0.5) ? 1.0 : -1.0;
        float shift = dir * p * p * (0.35 + 0.35 * kajo_hash1(slice));
        vec2 suv = vec2(coords.x + shift, coords.y);
        return sample_clamped(suv) * (1.0 - p);
    }

    // default: crisp Metro scale-down (1.0 -> 0.92) & fade
    float sc = mix(1.0, 0.92, p);
    vec2 suv = (coords - vec2(0.5)) / sc + vec2(0.5);
    return sample_clamped(suv) * (1.0 - p);
}

void main() {
    float p = clamp(u.eased_progress, 0.0, 1.0);
    vec4 col = (u.kind == 0u) ? eval_open(uv, p) : eval_close(uv, p);
    if (col.a <= 0.0001) {
        discard;
    }
    out_color = col * clamp(u.common_hdr.opacity, 0.0, 1.0);
}
