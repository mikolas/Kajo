// Kajo Niri Shader: Pixelate Materialize — Window Open
// Coarsely pixelated mosaic blocks sharpen into the full window texture.

vec4 open_color(vec3 coords_geo, vec3 size_geo) {
    if (coords_geo.x < 0.0 || coords_geo.x > 1.0 || coords_geo.y < 0.0 || coords_geo.y > 1.0) {
        return vec4(0.0);
    }

    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float block_px = mix(64.0, 1.0, p);
    vec2 grid = max(size_geo.xy / block_px, vec2(1.0, 1.0));

    vec2 quant_uv = (floor(coords_geo.xy * grid) + vec2(0.5, 0.5)) / grid;
    quant_uv = mix(quant_uv, coords_geo.xy, smoothstep(0.85, 1.0, p));

    vec3 coords_tex = niri_geo_to_tex * vec3(quant_uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    float alpha = smoothstep(0.0, 0.30, p);
    return color * alpha;
}
