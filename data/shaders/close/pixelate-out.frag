// Kajo Niri Shader: Pixelate Dematerialize — Window Close
// Coarsens the window into expanding mosaic blocks before fading out.

vec4 close_color(vec3 coords_geo, vec3 size_geo) {
    if (coords_geo.x < 0.0 || coords_geo.x > 1.0 || coords_geo.y < 0.0 || coords_geo.y > 1.0) {
        return vec4(0.0);
    }

    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float block_px = mix(1.0, 72.0, p);
    vec2 grid = max(size_geo.xy / block_px, vec2(1.0, 1.0));

    vec2 quant_uv = (floor(coords_geo.xy * grid) + vec2(0.5, 0.5)) / grid;
    vec3 coords_tex = niri_geo_to_tex * vec3(quant_uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    // Keep blocks clearly visible during pixelation, then fade in the final half
    float alpha = 1.0 - smoothstep(0.45, 1.0, p);
    return color * alpha;
}
