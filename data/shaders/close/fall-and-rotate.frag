// Kajo Niri Shader: Gravity Fall & Rotate — Window Close
// Downward drop with random pivot tilt while remaining visible throughout the fall.

vec4 close_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);

    vec2 coords = (coords_geo.xy - vec2(0.5, 1.0)) * size_geo.xy;
    coords.y -= p * 320.0;

    float random = (niri_random_seed - 0.5) / 2.0;
    random = sign(random) - random;
    float angle = p * 0.14 * random;

    float c = cos(angle);
    float s = sin(angle);
    mat2 rotate = mat2(c, -s, s, c);
    coords = rotate * coords;

    vec2 uv = coords / max(size_geo.xy, vec2(1.0, 1.0)) + vec2(0.5, 1.0);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * vec3(uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    float alpha = 1.0 - smoothstep(0.45, 1.0, p);
    return color * alpha;
}
