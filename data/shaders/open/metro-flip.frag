// Kajo Niri Shader: Metro Turnstile (3D Perspective Flip) — Window Open
// Swings the window open around its left edge in 3D perspective.

vec4 open_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float angle = (1.0 - p) * -1.25;
    float cos_a = cos(angle);
    float sin_a = sin(angle);

    // Pivot slightly inset from the left edge (x = 0.08) with perspective foreshortening
    float pivot_x = 0.08;
    float dx = coords_geo.x - pivot_x;
    if (abs(cos_a) < 0.05) {
        return vec4(0.0);
    }

    float u = pivot_x + dx / cos_a;
    float z = 1.0 - (u - pivot_x) * sin_a * 0.55;
    if (z <= 0.1) {
        return vec4(0.0);
    }

    float v = 0.5 + (coords_geo.y - 0.5) * z;
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
        return vec4(0.0);
    }

    vec3 sample_geo = vec3(u, v, 1.0);
    vec3 coords_tex = niri_geo_to_tex * sample_geo;
    vec4 color = texture2D(niri_tex, coords_tex.st);

    // Subtle depth shading on the swinging far edge
    float shade = clamp(1.0 - (1.0 - p) * (u - pivot_x) * 0.45, 0.4, 1.0);
    color.rgb *= shade;

    float alpha = smoothstep(0.0, 0.25, p);
    return color * alpha;
}
