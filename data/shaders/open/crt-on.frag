// Kajo Niri Shader: CRT Power-On (Scanline Beam) — Window Open
// Ignites a horizontal phosphor scanline beam that expands vertically.

vec4 open_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);

    // Phase 1 (0.0 -> 0.35): horizontal beam expands from center
    // Phase 2 (0.35 -> 1.0): vertical deflection expands to full height
    float h_scale = clamp(p / 0.35, 0.02, 1.0);
    float v_t = clamp((p - 0.25) / 0.75, 0.0, 1.0);
    float v_scale = mix(0.008, 1.0, v_t * v_t);

    vec2 uv = vec2(
        (coords_geo.x - 0.5) / h_scale + 0.5,
        (coords_geo.y - 0.5) / v_scale + 0.5
    );

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * vec3(uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    // Phosphor beam brightness boost during early vertical expansion
    float flash = (1.0 - v_t) * 0.85;
    color.rgb = mix(color.rgb, vec3(color.a), flash);

    return color * clamp(p * 3.0, 0.0, 1.0);
}
