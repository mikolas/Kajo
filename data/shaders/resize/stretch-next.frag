// Kajo Niri Shader: Elastic Stretch — Window Resize
// Stretches the next window texture cleanly across the interpolating geometry.

vec4 resize_color(vec3 coords_curr_geo, vec3 size_curr_geo) {
    if (coords_curr_geo.x < 0.0 || coords_curr_geo.x > 1.0
            || coords_curr_geo.y < 0.0 || coords_curr_geo.y > 1.0) {
        return vec4(0.0);
    }

    vec3 coords_tex_next = niri_geo_to_tex_next * coords_curr_geo;
    return texture2D(niri_tex_next, coords_tex_next.st);
}
