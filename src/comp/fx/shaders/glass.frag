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
    vec4 aux_crop_rect;
    vec4 aux_valid_bounds;
    vec2 aux_inv_tex_size;
    vec2 surface_size;
    vec2 light_dir;
    vec2 surface_offset;
    uint style;
    float refraction;
    float dispersion;
    float bevel_px;
    float specular;
    float grain;
    float flute_px;
    float _pad0;
} u;

vec3 sample_tex0(vec2 pos_tex) {
    vec2 p = clamp(pos_tex, u.valid_bounds.xy, u.valid_bounds.zw);
    vec4 c = texture(u_tex0, p * u.inv_tex_size);
    return c.rgb / max(c.a, 1e-5);
}

/* Mitered 0px-corner rectangular bevel normal field in surface-local logical px. */
void compute_bevel(vec2 p_surf, vec2 surf_sz, float bev,
                   out vec3 nrm, out float rim_t, out float d_edge) {
    float b = max(bev, 2.0);
    float dl = max(p_surf.x, 0.0);
    float dr = max(surf_sz.x - p_surf.x, 0.0);
    float dt = max(p_surf.y, 0.0);
    float db = max(surf_sz.y - p_surf.y, 0.0);

    d_edge = min(min(dl, dr), min(dt, db));
    rim_t = clamp(1.0 - d_edge / b, 0.0, 1.0);
    float slope = rim_t * rim_t * (3.0 - 2.0 * rim_t);

    float wl = clamp(1.0 - dl / b, 0.0, 1.0); wl *= wl;
    float wr = clamp(1.0 - dr / b, 0.0, 1.0); wr *= wr;
    float wt = clamp(1.0 - dt / b, 0.0, 1.0); wt *= wt;
    float wb = clamp(1.0 - db / b, 0.0, 1.0); wb *= wb;

    vec2 raw = vec2(wr - wl, wb - wt);
    float len = length(raw);
    vec2 n_xy = (len > 1e-5) ? (raw / len) * (slope * 0.92) : vec2(0.0);
    float n_z = sqrt(max(0.05, 1.0 - dot(n_xy, n_xy)));
    nrm = vec3(n_xy, n_z);
}

/* Deterministic sinless 2D hash in [-1, 1]^2 for surface-locked micro-facets. */
vec2 hash22_signed(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    vec2 h = fract((q.xx + q.yz) * q.zy);
    return h * 2.0 - 1.0;
}

void main() {
    if (u.common_hdr.opacity <= 0.0001) {
        discard;
    }

    vec2 base_pos = u.crop_rect.xy + uv * u.crop_rect.zw;
    vec3 rgb;

    if (u.style == 0u) {
        /* Style 0: Frosted (classic Dual-Kawase fast path). */
        rgb = sample_tex0(base_pos);
    } else {
        vec2 draw_log = u.common_hdr.dst_size / max(u.common_hdr.scale, 1e-4);
        vec2 p_surf = u.surface_offset + uv * draw_log;
        vec2 surf_sz = max(u.surface_size, vec2(1.0));
        vec2 tex0_per_log = u.crop_rect.zw / max(draw_log, vec2(1e-4));

        if (u.style == 3u) {
            /* Style 3: Fluted / architectural reeded cylindrical lenslets. */
            vec2 axis = u.light_dir;
            float ax_len = length(axis);
            axis = (ax_len > 1e-5) ? (axis / ax_len) : vec2(1.0, 0.0);

            float pitch = max(u.flute_px, 4.0);
            float coord = dot(p_surf, axis);
            float phi = fract(coord / pitch) * 2.0 - 1.0;
            float slope = phi * sqrt(max(0.1, 1.0 - 0.5 * phi * phi));

            vec2 disp_log = -axis * (slope * u.refraction * pitch * 0.65);
            vec2 disp_tex = disp_log * tex0_per_log;

            if (u.dispersion > 0.001) {
                float d = 0.18 * u.dispersion;
                float r = sample_tex0(base_pos + disp_tex * (1.0 - d)).r;
                float g = sample_tex0(base_pos + disp_tex).g;
                float b = sample_tex0(base_pos + disp_tex * (1.0 + d)).b;
                rgb = vec3(r, g, b);
            } else {
                rgb = sample_tex0(base_pos + disp_tex);
            }

            float crest = max(0.0, 1.0 - abs(phi + 0.35) * 2.4);
            float valley = max(0.0, 1.0 - abs(phi - 0.75) * 3.0);
            float ridge = u.specular * (0.28 * crest * crest * crest - 0.10 * valley * valley);
            rgb = max(rgb + vec3(ridge), vec3(0.0));
        } else if (u.style == 4u) {
            /* Style 4: Acrylic / etched micro-facet frost. */
            vec2 q = floor(p_surf * max(u.common_hdr.scale, 1.0));
            vec2 xi = hash22_signed(q);
            vec2 jitter_tex = (xi * (u.grain * 3.5)) * tex0_per_log;
            rgb = sample_tex0(base_pos + jitter_tex);

            float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
            float glint = (xi.x * 0.5) * 0.045 * u.grain * (0.35 + clamp(y, 0.0, 1.0));
            float top_edge = 1.0 - smoothstep(0.0, 1.5, p_surf.y);
            rgb = max(rgb + vec3(glint) + vec3(0.10 * u.specular * top_edge), vec3(0.0));
        } else if (u.style == 5u) {
            /* Style 5: Aero crystal sheen + 45-degree chamfer rim. */
            rgb = sample_tex0(base_pos);
            float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
            float luma_mod = 0.35 + 0.65 * clamp(y, 0.0, 1.0);

            float diag = (0.65 * p_surf.x + 0.35 * p_surf.y) / max(surf_sz.x, surf_sz.y);
            float b1 = exp(-pow((diag - 0.28) / 0.09, 2.0));
            float b2 = 0.55 * exp(-pow((diag - 0.52) / 0.045, 2.0));
            float sheen = u.specular * 0.22 * (b1 + b2) * luma_mod;

            vec3 nrm;
            float rim_t, d_edge;
            compute_bevel(p_surf, surf_sz, u.bevel_px, nrm, rim_t, d_edge);
            vec3 L = normalize(vec3(u.light_dir, 0.55));
            float ndl = dot(nrm, L);
            float hi = u.specular * 0.32 * max(ndl, 0.0) * rim_t;
            float sh = 1.0 - u.specular * 0.14 * max(-ndl, 0.0) * rim_t;

            rgb = max(rgb * sh + vec3(sheen + hi) * vec3(0.95, 0.98, 1.0), vec3(0.0));
        } else if (u.style == 6u) {
            /* Style 6: Dichroic thin-film spectral interference. */
            rgb = sample_tex0(base_pos);
            float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));

            vec3 nrm;
            float rim_t, d_edge;
            compute_bevel(p_surf, surf_sz, u.bevel_px, nrm, rim_t, d_edge);

            float k_rim = smoothstep(0.0, 0.85, rim_t);
            float fresnel = (1.0 - nrm.z) * (1.0 - nrm.z);
            vec2 norm_p = p_surf / surf_sz;
            float phase = 6.2831853 * (0.65 * (1.0 - nrm.z) +
                                       0.18 * (norm_p.x + 0.5 * norm_p.y) +
                                       0.12 * clamp(y, 0.0, 1.0));
            vec3 spectral = 0.5 + 0.5 * cos(phase + vec3(0.0, 2.0943951, 4.1887902));
            float weight = u.specular * max(u.dispersion, 0.15) *
                           (0.55 * k_rim * (0.30 + fresnel) + 0.06 * clamp(y, 0.0, 1.0));
            rgb = max(rgb + spectral * weight, vec3(0.0));
        } else {
            rgb = sample_tex0(base_pos);
        }
    }

    rgb *= u.brightness;

    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = max(mix(vec3(luma), rgb, u.saturation), vec3(0.0));

    float ta = clamp(u.tint_lin.a, 0.0, 1.0);
    rgb = mix(rgb, u.tint_lin.rgb, ta);

    float alpha = clamp(u.common_hdr.opacity, 0.0, 1.0);
    out_color = vec4(rgb * alpha, alpha);
}
