// Kajo Niri Shader: Horizontal Slice Shred — Window Close
// Splits the window into alternating horizontal bands that shear apart.

vec4 close_color(vec3 coords_geo, vec3 size_geo) {
    if (coords_geo.y < 0.0 || coords_geo.y > 1.0) {
        return vec4(0.0);
    }

    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float band = floor(coords_geo.y * 16.0);
    float dir = mod(band, 2.0) * 2.0 - 1.0;

    float shear = dir * p * 0.55;
    vec2 uv = vec2(coords_geo.x - shear, coords_geo.y);

    if (uv.x < 0.0 || uv.x > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * vec3(uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    float alpha = 1.0 - smoothstep(0.45, 1.0, p);
    return color * alpha;
}
