// Kajo Niri Shader: Cyberpunk RGB Glitch — Window Open
// Horizontal slice jitter with chromatic aberration that locks into place.

float kajo_hash(float n) {
    return fract(sin(n * 91.3458 + niri_random_seed * 47.123) * 47453.5453);
}

vec4 open_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float intensity = (1.0 - p) * (1.0 - p);

    float band = floor(coords_geo.y * 24.0);
    float rnd = kajo_hash(band + floor(p * 12.0));
    float shift = (rnd - 0.5) * 0.18 * intensity;

    vec2 base_uv = vec2(coords_geo.x + shift, coords_geo.y);
    if (base_uv.x < 0.0 || base_uv.x > 1.0 || base_uv.y < 0.0 || base_uv.y > 1.0) {
        return vec4(0.0);
    }

    float chroma = 0.025 * intensity;
    vec2 uv_r = vec2(clamp(base_uv.x + chroma, 0.0, 1.0), base_uv.y);
    vec2 uv_b = vec2(clamp(base_uv.x - chroma, 0.0, 1.0), base_uv.y);

    vec4 col_r = texture2D(niri_tex, (niri_geo_to_tex * vec3(uv_r, 1.0)).st);
    vec4 col_g = texture2D(niri_tex, (niri_geo_to_tex * vec3(base_uv, 1.0)).st);
    vec4 col_b = texture2D(niri_tex, (niri_geo_to_tex * vec3(uv_b, 1.0)).st);

    vec4 color = vec4(col_r.r, col_g.g, col_b.b, col_g.a);
    return color * smoothstep(0.0, 0.35, p);
}
