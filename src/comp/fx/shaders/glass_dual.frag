#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex0;
layout(set = 0, binding = 1) uniform sampler2D u_tex1;

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

vec3 sample_tex1(vec2 pos_tex) {
    vec2 p = clamp(pos_tex, u.aux_valid_bounds.xy, u.aux_valid_bounds.zw);
    vec4 c = texture(u_tex1, p * u.aux_inv_tex_size);
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

void main() {
    if (u.common_hdr.opacity <= 0.0001) {
        discard;
    }

    vec2 base0 = u.crop_rect.xy + uv * u.crop_rect.zw;
    vec2 base1 = u.aux_crop_rect.xy + uv * u.aux_crop_rect.zw;

    vec2 draw_log = u.common_hdr.dst_size / max(u.common_hdr.scale, 1e-4);
    vec2 p_surf = u.surface_offset + uv * draw_log;
    vec2 surf_sz = max(u.surface_size, vec2(1.0));
    vec2 tex0_per_log = u.crop_rect.zw / max(draw_log, vec2(1e-4));
    vec2 tex1_per_log = u.aux_crop_rect.zw / max(draw_log, vec2(1e-4));

    vec3 rgb;

    if (u.style == 1u) {
        /* Style 1: Liquid Glass — u_tex0 = sharp backdrop, u_tex1 = blurred interior. */
        vec3 nrm;
        float rim_t, d_edge;
        compute_bevel(p_surf, surf_sz, u.bevel_px, nrm, rim_t, d_edge);

        float k_rim = smoothstep(0.0, 0.85, rim_t);
        vec2 disp_log = -nrm.xy * (u.refraction * max(u.bevel_px, 2.0));
        vec2 d0 = disp_log * tex0_per_log;
        vec2 d1 = disp_log * tex1_per_log;

        vec3 sharp_c;
        vec3 blur_c;
        if (u.dispersion > 0.001 && k_rim > 0.001) {
            float sp = 0.18 * u.dispersion;
            sharp_c = vec3(
                sample_tex0(base0 + d0 * (1.0 - sp)).r,
                sample_tex0(base0 + d0).g,
                sample_tex0(base0 + d0 * (1.0 + sp)).b
            );
            blur_c = vec3(
                sample_tex1(base1 + d1 * (1.0 - sp)).r,
                sample_tex1(base1 + d1).g,
                sample_tex1(base1 + d1 * (1.0 + sp)).b
            );
        } else {
            sharp_c = sample_tex0(base0 + d0);
            blur_c = sample_tex1(base1 + d1);
        }

        rgb = mix(blur_c, sharp_c, 0.75 * k_rim);

        vec3 L = normalize(vec3(u.light_dir, 0.48));
        float ndl = dot(nrm, L);
        float omz = 1.0 - nrm.z;
        float fresnel = omz * omz * omz;
        float spec = u.specular * (0.65 * pow(max(ndl, 0.0), 6.0) + 0.35 * fresnel) * k_rim;
        float absorb = 1.0 - 0.18 * u.specular * max(-ndl, 0.0) * k_rim;
        rgb = max(rgb * absorb + vec3(spec) * vec3(0.95, 0.98, 1.0), vec3(0.0));
    } else {
        /* Style 2: Halation — u_tex0 = tight blur, u_tex1 = wide deep-pyramid scatter. */
        vec3 tight_c = sample_tex0(base0);
        vec3 wide_c = sample_tex1(base1);
        float y_wide = dot(wide_c, vec3(0.2126, 0.7152, 0.0722));
        float scatter = clamp(u.grain, 0.0, 1.0);
        float glow = smoothstep(0.08, 0.75, y_wide) * scatter;

        rgb = mix(tight_c, wide_c, 0.35 * scatter) + wide_c * (0.45 * glow);

        float d_edge = min(min(max(p_surf.x, 0.0), max(surf_sz.x - p_surf.x, 0.0)),
                           min(max(p_surf.y, 0.0), max(surf_sz.y - p_surf.y, 0.0)));
        float edge_trap = 1.0 - smoothstep(0.0, 2.0, d_edge);
        rgb += (0.08 * u.specular * edge_trap) * (vec3(0.5) + wide_c);
    }

    rgb *= u.brightness;

    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = max(mix(vec3(luma), rgb, u.saturation), vec3(0.0));

    float ta = clamp(u.tint_lin.a, 0.0, 1.0);
    rgb = mix(rgb, u.tint_lin.rgb, ta);

    float alpha = clamp(u.common_hdr.opacity, 0.0, 1.0);
    out_color = vec4(rgb * alpha, alpha);
}
