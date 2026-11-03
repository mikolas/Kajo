// Kajo Niri Shader: Pixelate Morph — Window Resize
// Crossfades previous and next window textures through a transient mosaic peak.

vec4 resize_color(vec3 coords_curr_geo, vec3 size_curr_geo) {
    if (coords_curr_geo.x < 0.0 || coords_curr_geo.x > 1.0
            || coords_curr_geo.y < 0.0 || coords_curr_geo.y > 1.0) {
        return vec4(0.0);
    }

    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float pulse = sin(p * 3.14159265);
    float block_px = mix(1.0, 28.0, pulse);
    vec2 grid = max(size_curr_geo.xy / block_px, vec2(1.0, 1.0));

    vec2 uv = (floor(coords_curr_geo.xy * grid) + vec2(0.5, 0.5)) / grid;
    uv = mix(coords_curr_geo.xy, uv, smoothstep(0.0, 0.15, pulse));

    vec3 sample_geo = vec3(uv, 1.0);
    vec3 coords_prev = niri_geo_to_tex_prev * sample_geo;
    vec3 coords_next = niri_geo_to_tex_next * sample_geo;

    vec4 col_prev = texture2D(niri_tex_prev, coords_prev.st);
    vec4 col_next = texture2D(niri_tex_next, coords_next.st);

    return mix(col_prev, col_next, p);
}
