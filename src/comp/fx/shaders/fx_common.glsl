// fx_common.glsl — Kajo effects common UBO header (std140, 32 bytes).
// Must match CompFxCommonParams in src/comp/core/comp_fx_core.h.
struct FxCommon {
    vec2 dst_origin;
    vec2 dst_size;
    float opacity;
    float scale;
    float hdr_headroom;
    uint flags;
};
