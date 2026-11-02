// Kajo Niri Shader: Vertical Split Curtain — Window Open
// Left and right halves slide inward and unfold from the vertical centerline.

vec4 open_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float offset = (1.0 - p) * 0.35;

    vec2 uv = coords_geo.xy;
    if (uv.x < 0.5) {
        uv.x += offset;
        if (uv.x > 0.5) {
            return vec4(0.0);
        }
    } else {
        uv.x -= offset;
        if (uv.x < 0.5) {
            return vec4(0.0);
        }
    }

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * vec3(uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    return color * p;
}
