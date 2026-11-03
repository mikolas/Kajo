// Kajo Niri Shader: Iris Circle Reveal — Window Open
// Reveals the window through an anti-aliased expanding circular aperture.

vec4 open_color(vec3 coords_geo, vec3 size_geo) {
    if (coords_geo.x < 0.0 || coords_geo.x > 1.0 || coords_geo.y < 0.0 || coords_geo.y > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * coords_geo;
    vec4 color = texture2D(niri_tex, coords_tex.st);

    vec2 centered = (coords_geo.xy - vec2(0.5, 0.5)) * size_geo.xy * 2.0;
    float max_r = max(length(size_geo.xy), 1.0);
    float dist = length(centered) / max_r;

    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float soft = 0.03;
    float mask = 1.0 - smoothstep(max(0.0, p - soft), p + soft, dist);

    return color * mask;
}
