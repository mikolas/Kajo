// Kajo Niri Shader: Chromatic Glitch Resize — Window Resize
// Crossfades previous and next textures with scanline jitter at mid-transition.

float kajo_resize_hash(float n) {
    return fract(sin(n * 113.57) * 43758.5453);
}

vec4 resize_color(vec3 coords_curr_geo, vec3 size_curr_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float pulse = sin(p * 3.14159265);

    float band = floor(coords_curr_geo.y * 20.0);
    float jitter = (kajo_resize_hash(band + floor(p * 10.0)) - 0.5) * 0.06 * pulse;
    vec2 uv = vec2(coords_curr_geo.x + jitter, coords_curr_geo.y);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    vec3 sample_geo = vec3(uv, 1.0);
    vec4 col_prev = texture2D(niri_tex_prev, (niri_geo_to_tex_prev * sample_geo).st);
    vec4 col_next = texture2D(niri_tex_next, (niri_geo_to_tex_next * sample_geo).st);

    return mix(col_prev, col_next, p);
}
