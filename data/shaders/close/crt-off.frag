// Kajo Niri Shader: CRT TV Turn-Off — Window Close
// Collapses vertically into a bright scanline, then shrinks into a center dot.

vec4 close_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);

    // Phase 1 (0.0 -> 0.55): vertical collapse to thin line
    // Phase 2 (0.50 -> 1.0): horizontal collapse to dot
    float v_t = clamp(p / 0.55, 0.0, 1.0);
    float h_t = clamp((p - 0.50) / 0.50, 0.0, 1.0);

    float v_scale = max(0.008, 1.0 - v_t);
    float h_scale = max(0.008, 1.0 - h_t);

    vec2 uv = vec2(
        (coords_geo.x - 0.5) / h_scale + 0.5,
        (coords_geo.y - 0.5) / v_scale + 0.5
    );

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * vec3(uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    // White phosphor beam intensity as the image collapses
    float beam = clamp(v_t * 0.9, 0.0, 1.0);
    color.rgb = mix(color.rgb, vec3(color.a), beam);

    float fade = 1.0 - smoothstep(0.88, 1.0, p);
    return color * fade;
}
