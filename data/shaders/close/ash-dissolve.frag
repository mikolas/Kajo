// Kajo Niri Shader: Ash / Particle Dissolve — Window Close
// Procedural noise disintegration with upward ember drift and glowing burn edge.

float kajo_hash2(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7)) + niri_random_seed * 53.7) * 43758.5453);
}

float kajo_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    float a = kajo_hash2(i + vec2(0.0, 0.0));
    float b = kajo_hash2(i + vec2(1.0, 0.0));
    float c = kajo_hash2(i + vec2(0.0, 1.0));
    float d = kajo_hash2(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

vec4 close_color(vec3 coords_geo, vec3 size_geo) {
    float p = clamp(niri_clamped_progress, 0.0, 1.0);

    // Upward ash drift as the window dissolves
    vec2 uv = coords_geo.xy + vec2(0.0, p * 0.07);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    vec2 noise_coord = uv * (size_geo.xy / 16.0);
    float raw_n = 0.65 * kajo_noise(noise_coord) + 0.35 * kajo_noise(noise_coord * 2.3);
    // Normalize value noise from its natural [0.18, 0.82] bell curve to [0.0, 1.0]
    float n = clamp((raw_n - 0.18) / 0.64, 0.0, 1.0);

    // Sweep threshold smoothly from bottom (uv.y = 1) to top (uv.y = 0) across full [0, 1] progress
    float threshold = p * 1.25 - (1.0 - uv.y) * 0.25;
    if (n < threshold) {
        return vec4(0.0);
    }

    vec3 coords_tex = niri_geo_to_tex * vec3(uv, 1.0);
    vec4 color = texture2D(niri_tex, coords_tex.st);

    // Warm ember glow along the dissolving burn front
    float edge = 1.0 - smoothstep(0.0, 0.14, n - threshold);
    vec3 ember = vec3(1.0, 0.42, 0.08) * color.a;
    color.rgb = mix(color.rgb, ember, edge * 0.90);

    return color * (1.0 - smoothstep(0.92, 1.0, p));
}
