// Kajo Niri Shader: Black Hole Vortex — Window Close
// Spirals and pinches the window toward its center before vanishing.

vec4 close_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);
    float scale = max(0.04, 1.0 - p * 0.92);

    vec2 centered = (coords_geo.xy - vec2(0.5, 0.5)) / scale;
    float r = length(centered);
    float swirl = (1.0 - clamp(r * 1.4, 0.0, 1.0)) * p * 4.5;

    float c = cos(swirl);
    float s = sin(swirl);
    mat2 rot = mat2(c, -s, s, c);
    vec2 uv = rot * centered + vec2(0.5, 0.5);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * vec3(uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    float alpha = 1.0 - smoothstep(0.55, 1.0, p);
    return color * alpha;
}
