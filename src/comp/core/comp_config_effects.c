/* comp_config_effects.c — [effects.*] and [animations.*] TOML section parsers (pure C). */
#include "comp_config_private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool
comp_config_parse_hex_rgba(const char *s, float out[4])
{
    if (!s || s[0] != '#')
        return false;
    size_t len = strlen(s + 1);
    if (len != 6 && len != 8)
        return false;
    unsigned int vals[4] = { 0, 0, 0, 255 };
    size_t nbytes = len / 2;
    for (size_t i = 0; i < nbytes; i++) {
        char hex[3] = { s[1 + i * 2], s[2 + i * 2], '\0' };
        if (!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1]))
            return false;
        vals[i] = (unsigned int)strtoul(hex, NULL, 16);
    }
    for (int i = 0; i < 4; i++)
        out[i] = (float)vals[i] / 255.0f;
    return true;
}

/* ───────────────────────── [animations.*] ───────────────────────── */

static void
merge_spring_subtable(CompSpringParams *sp, toml_datum_t sub, const char *name,
                      const char *src, CompDiagList *diag)
{
    if (sub.type != TOML_TABLE)
        return;
    CompSpringParams cand = *sp;
    double v = 0.0;
    toml_datum_t dr = toml_get(sub, COMP_KEY_DAMPING_RATIO);
    if (dr.type != TOML_UNKNOWN && comp_config_datum_as_double(dr, &v))
        cand.damping_ratio = v;
    toml_datum_t st = toml_get(sub, COMP_KEY_STIFFNESS);
    if (st.type != TOML_UNKNOWN && comp_config_datum_as_double(st, &v))
        cand.stiffness = v;
    toml_datum_t ep = toml_get(sub, COMP_KEY_EPSILON);
    if (ep.type != TOML_UNKNOWN && comp_config_datum_as_double(ep, &v))
        cand.epsilon = v;

    if (comp_spring_params_valid(&cand))
        *sp = cand;
    else
        comp_diag_add(diag, src, sub.lineno, "[animations.%s] parameters out of valid range; ignored", name);
}

static void
merge_overview_anim_subtable(CompOverviewAnimConfig *oa, toml_datum_t sub,
                             const char *src, CompDiagList *diag)
{
    if (sub.type != TOML_TABLE)
        return;

    CompOverviewAnimConfig cand = *oa;

    toml_datum_t pr = toml_get(sub, COMP_KEY_PRESET);
    if (pr.type != TOML_UNKNOWN) {
        CompOverviewPreset parsed = COMP_OVERVIEW_PRESET_SPRING_BOUNCE;
        if (pr.type == TOML_STRING && comp_overview_preset_from_str(pr.u.s, &parsed)) {
            if (parsed != COMP_OVERVIEW_PRESET_CUSTOM)
                comp_overview_anim_apply_preset(&cand, parsed);
            else
                cand.preset = COMP_OVERVIEW_PRESET_CUSTOM;
        } else {
            comp_diag_add(diag, src, pr.lineno,
                          "[animations.overview] invalid 'preset'; ignored");
        }
    }

    toml_datum_t md = toml_get(sub, COMP_KEY_MODE);
    if (md.type != TOML_UNKNOWN) {
        CompOverviewAnimMode parsed_mode = COMP_OVERVIEW_ANIM_SPRING;
        if (md.type == TOML_STRING && comp_overview_anim_mode_from_str(md.u.s, &parsed_mode)) {
            cand.mode = parsed_mode;
        } else {
            comp_diag_add(diag, src, md.lineno,
                          "[animations.overview] 'mode' must be \"spring\", \"curve\", or \"instant\"; ignored");
        }
    }

    double v = 0.0;
    toml_datum_t dr = toml_get(sub, COMP_KEY_DAMPING_RATIO);
    if (dr.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(dr, &v))
            cand.spring.damping_ratio = v;
        else
            comp_diag_add(diag, src, dr.lineno, "[animations.overview] invalid 'damping_ratio'; ignored");
    }
    toml_datum_t st = toml_get(sub, COMP_KEY_STIFFNESS);
    if (st.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(st, &v))
            cand.spring.stiffness = v;
        else
            comp_diag_add(diag, src, st.lineno, "[animations.overview] invalid 'stiffness'; ignored");
    }
    toml_datum_t ep = toml_get(sub, COMP_KEY_EPSILON);
    if (ep.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(ep, &v))
            cand.spring.epsilon = v;
        else
            comp_diag_add(diag, src, ep.lineno, "[animations.overview] invalid 'epsilon'; ignored");
    }

    toml_datum_t cv = toml_get(sub, COMP_KEY_CURVE);
    if (cv.type != TOML_UNKNOWN) {
        CompOverviewCurve parsed_curve = COMP_OVERVIEW_CURVE_EASE_OUT_BACK;
        if (cv.type == TOML_STRING && comp_overview_curve_from_str(cv.u.s, &parsed_curve))
            cand.curve = parsed_curve;
        else
            comp_diag_add(diag, src, cv.lineno, "[animations.overview] invalid 'curve'; ignored");
    }

    toml_datum_t dur = toml_get(sub, COMP_KEY_DURATION_MS);
    if (dur.type != TOML_UNKNOWN) {
        if (dur.type == TOML_INT64 && dur.u.int64 >= 0 && dur.u.int64 <= 2000)
            cand.duration_ms = (int32_t)dur.u.int64;
        else
            comp_diag_add(diag, src, dur.lineno, "[animations.overview] 'duration_ms' must be in 0..2000; ignored");
    }

    toml_datum_t cb = toml_get(sub, COMP_KEY_CLOSE_BOUNCE_GAIN);
    if (cb.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(cb, &v) && v >= 0.0 && v <= 1.0)
            cand.close_bounce_gain = v;
        else
            comp_diag_add(diag, src, cb.lineno, "[animations.overview] 'close_bounce_gain' must be in 0..1; ignored");
    }

    if (comp_overview_anim_config_valid(&cand))
        *oa = cand;
    else
        comp_diag_add(diag, src, sub.lineno, "[animations.overview] parameters out of valid range; ignored");
}

void
comp_config_merge_animations_section(CompConfig *cfg, toml_datum_t tab,
                                     const char *src, CompDiagList *diag)
{
    if (tab.type != TOML_TABLE)
        return;
    toml_datum_t en = toml_get(tab, COMP_KEY_ENABLED);
    if (en.type == TOML_BOOLEAN)
        cfg->motion.enabled = en.u.boolean;

    double v = 0.0;
    toml_datum_t sd = toml_get(tab, COMP_KEY_SLOWDOWN);
    if (sd.type != TOML_UNKNOWN && comp_config_datum_as_double(sd, &v) && v >= 0.0 && v <= 100.0)
        cfg->motion.slowdown = v;

    merge_spring_subtable(&cfg->motion.workspace_switch,
                          toml_get(tab, COMP_SEC_ANIM_WORKSPACE_SWITCH),
                          COMP_SEC_ANIM_WORKSPACE_SWITCH, src, diag);
    merge_spring_subtable(&cfg->motion.horizontal_scroll,
                          toml_get(tab, COMP_SEC_ANIM_HORIZONTAL_SCROLL),
                          COMP_SEC_ANIM_HORIZONTAL_SCROLL, src, diag);
    merge_spring_subtable(&cfg->motion.window_movement,
                          toml_get(tab, COMP_SEC_ANIM_WINDOW_MOVEMENT),
                          COMP_SEC_ANIM_WINDOW_MOVEMENT, src, diag);
    merge_overview_anim_subtable(&cfg->overview_anim,
                                 toml_get(tab, COMP_SEC_ANIM_OVERVIEW),
                                 src, diag);
}

/* ───────────────────────── [effects.*] ───────────────────────── */

static void
merge_effects_top_level(CompFxConfig *fx_cfg, toml_datum_t fx,
                        const char *src, CompDiagList *diag)
{
    toml_datum_t en = toml_get(fx, COMP_KEY_ENABLED);
    if (en.type == TOML_BOOLEAN)
        fx_cfg->enabled = en.u.boolean;
    toml_datum_t di = toml_get(fx, COMP_KEY_DITHER);
    if (di.type == TOML_BOOLEAN)
        fx_cfg->dither = di.u.boolean;

    toml_datum_t cf = toml_get(fx, COMP_KEY_COLOR_FILTER);
    if (cf.type != TOML_UNKNOWN) {
        CompFxColorFilter parsed_cf = COMP_FX_FILTER_NONE;
        if (cf.type == TOML_STRING && comp_fx_color_filter_from_str(cf.u.s, &parsed_cf))
            fx_cfg->color_filter = parsed_cf;
        else
            comp_diag_add(diag, src, cf.lineno,
                          "[effects] invalid 'color_filter' (expected none|grayscale|night-light|protanopia|deuteranopia|tritanopia); ignored");
    }

    double fx_dval = 0.0;
    toml_datum_t cfs = toml_get(fx, COMP_KEY_COLOR_FILTER_STRENGTH);
    if (cfs.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(cfs, &fx_dval) && fx_dval >= 0.0 && fx_dval <= 1.0)
            fx_cfg->color_filter_strength = fx_dval;
        else
            comp_diag_add(diag, src, cfs.lineno, "[effects] 'color_filter_strength' must be in 0..1; ignored");
    }

    toml_datum_t nlt = toml_get(fx, COMP_KEY_NIGHT_LIGHT_TEMPERATURE);
    if (nlt.type != TOML_UNKNOWN) {
        if (nlt.type == TOML_INT64 && nlt.u.int64 >= 1500 && nlt.u.int64 <= 6500)
            fx_cfg->night_light_temperature = (int32_t)nlt.u.int64;
        else
            comp_diag_add(diag, src, nlt.lineno, "[effects] 'night_light_temperature' must be an integer in 1500..6500; ignored");
    }

    toml_datum_t swn = toml_get(fx, COMP_KEY_SDR_WHITE_NITS);
    if (swn.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(swn, &fx_dval) && (fx_dval == 0.0 || (fx_dval >= 80.0 && fx_dval <= 1000.0)))
            fx_cfg->sdr_white_nits = fx_dval;
        else
            comp_diag_add(diag, src, swn.lineno, "[effects] 'sdr_white_nits' must be 0 or in 80..1000; ignored");
    }

    toml_datum_t tm = toml_get(fx, COMP_KEY_TONEMAP);
    if (tm.type == TOML_BOOLEAN)
        fx_cfg->tonemap = tm.u.boolean;

    toml_datum_t tmpk = toml_get(fx, COMP_KEY_TONEMAP_PEAK_NITS);
    if (tmpk.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(tmpk, &fx_dval) && (fx_dval == 0.0 || (fx_dval >= 100.0 && fx_dval <= 10000.0)))
            fx_cfg->tonemap_peak_nits = fx_dval;
        else
            comp_diag_add(diag, src, tmpk.lineno, "[effects] 'tonemap_peak_nits' must be 0 or in 100..10000; ignored");
    }
}

static void
merge_effects_shadow_layers(CompFxShadowConfig *sh, toml_datum_t arr,
                            const char *src, CompDiagList *diag)
{
    if (arr.type == TOML_UNKNOWN)
        return;
    if (arr.type != TOML_ARRAY) {
        comp_diag_add(diag, src, arr.lineno, "[effects.shadow] 'layers' must be an array of tables; ignored");
        return;
    }

    size_t count = (size_t)arr.u.arr.size;
    if (count > COMP_FX_SHADOW_MAX_LAYERS) {
        comp_diag_add(diag, src, arr.lineno,
                      "[effects.shadow] at most %d layers supported; truncating extras",
                      COMP_FX_SHADOW_MAX_LAYERS);
        count = COMP_FX_SHADOW_MAX_LAYERS;
    }

    CompFxShadowLayer parsed[COMP_FX_SHADOW_MAX_LAYERS];
    memset(parsed, 0, sizeof(parsed));
    size_t valid_n = 0;

    for (size_t i = 0; i < count; i++) {
        toml_datum_t elem = arr.u.arr.elem[i];
        if (elem.type != TOML_TABLE) {
            comp_diag_add(diag, src, elem.lineno, "[effects.shadow] layer entry must be a table; ignored");
            continue;
        }
        CompFxShadowLayer l = { .sigma = 6.0, .offset_x = 0.0, .offset_y = 0.0, .opacity = 0.25 };
        double v = 0.0;
        toml_datum_t sg = toml_get(elem, COMP_KEY_SIGMA);
        if (sg.type != TOML_UNKNOWN) {
            if (comp_config_datum_as_double(sg, &v) && v > 0.0 && v <= 256.0)
                l.sigma = v;
            else
                comp_diag_add(diag, src, sg.lineno, "[effects.shadow] 'sigma' must be in (0..256]; ignored");
        }
        toml_datum_t ox = toml_get(elem, COMP_KEY_OFFSET_X);
        if (ox.type != TOML_UNKNOWN) {
            if (comp_config_datum_as_double(ox, &v) && v >= -256.0 && v <= 256.0)
                l.offset_x = v;
            else
                comp_diag_add(diag, src, ox.lineno, "[effects.shadow] 'offset_x' must be in -256..256; ignored");
        }
        toml_datum_t oy = toml_get(elem, COMP_KEY_OFFSET_Y);
        if (oy.type != TOML_UNKNOWN) {
            if (comp_config_datum_as_double(oy, &v) && v >= -256.0 && v <= 256.0)
                l.offset_y = v;
            else
                comp_diag_add(diag, src, oy.lineno, "[effects.shadow] 'offset_y' must be in -256..256; ignored");
        }
        toml_datum_t op = toml_get(elem, COMP_KEY_OPACITY);
        if (op.type != TOML_UNKNOWN) {
            if (comp_config_datum_as_double(op, &v) && v >= 0.0 && v <= 1.0)
                l.opacity = v;
            else
                comp_diag_add(diag, src, op.lineno, "[effects.shadow] 'opacity' must be in 0..1; ignored");
        }
        parsed[valid_n++] = l;
    }

    sh->n_layers = valid_n;
    if (valid_n > 0)
        memcpy(sh->layers, parsed, valid_n * sizeof(parsed[0]));
}

static void
merge_effects_shadow(CompFxShadowConfig *sh_cfg, toml_datum_t sh,
                     const char *src, CompDiagList *diag)
{
    if (sh.type != TOML_TABLE)
        return;

    toml_datum_t sen = toml_get(sh, COMP_KEY_ENABLED);
    if (sen.type == TOML_BOOLEAN)
        sh_cfg->enabled = sen.u.boolean;
    toml_datum_t scol = toml_get(sh, COMP_KEY_COLOR);
    if (scol.type == TOML_STRING && !comp_config_parse_hex_rgba(scol.u.s, sh_cfg->color_rgba))
        comp_diag_add(diag, src, scol.lineno, "[effects.shadow] invalid hex color '%s'; ignored", scol.u.s);

    double dval = 0.0;
    toml_datum_t us = toml_get(sh, COMP_KEY_UNFOCUSED_SCALE);
    if (us.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(us, &dval) && dval >= 0.0 && dval <= 2.0)
            sh_cfg->unfocused_scale = dval;
        else
            comp_diag_add(diag, src, us.lineno, "[effects.shadow] 'unfocused_scale' must be in 0..2; ignored");
    }
    toml_datum_t st = toml_get(sh, COMP_KEY_STRENGTH);
    if (st.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(st, &dval) && dval >= 0.0 && dval <= 2.0)
            sh_cfg->strength = dval;
        else
            comp_diag_add(diag, src, st.lineno, "[effects.shadow] 'strength' must be in 0..2; ignored");
    }
    toml_datum_t dur = toml_get(sh, COMP_KEY_DURATION_MS);
    if (dur.type == TOML_INT64 && dur.u.int64 >= 0 && dur.u.int64 <= 5000)
        sh_cfg->duration_ms = (int32_t)dur.u.int64;
    merge_effects_shadow_layers(sh_cfg, toml_get(sh, COMP_KEY_LAYERS), src, diag);
}

static void
merge_effects_dim(CompFxDimConfig *dm_cfg, toml_datum_t dm,
                  const char *src, CompDiagList *diag)
{
    if (dm.type != TOML_TABLE)
        return;

    toml_datum_t den = toml_get(dm, COMP_KEY_ENABLED);
    if (den.type == TOML_BOOLEAN)
        dm_cfg->enabled = den.u.boolean;
    toml_datum_t dcol = toml_get(dm, COMP_KEY_COLOR);
    if (dcol.type == TOML_STRING && !comp_config_parse_hex_rgba(dcol.u.s, dm_cfg->color_rgba))
        comp_diag_add(diag, src, dcol.lineno, "[effects.dim] invalid hex color '%s'; ignored", dcol.u.s);

    double dval = 0.0;
    toml_datum_t dop = toml_get(dm, COMP_KEY_OPACITY);
    if (dop.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(dop, &dval) && dval >= 0.0 && dval <= 1.0)
            dm_cfg->opacity = dval;
        else
            comp_diag_add(diag, src, dop.lineno, "[effects.dim] 'opacity' must be in 0..1; ignored");
    }
    toml_datum_t ddur = toml_get(dm, COMP_KEY_DURATION_MS);
    if (ddur.type == TOML_INT64 && ddur.u.int64 >= 0 && ddur.u.int64 <= 5000)
        dm_cfg->duration_ms = (int32_t)ddur.u.int64;
}

static void
merge_effects_glass(CompFxGlassConfig *gl_cfg, toml_datum_t gl,
                    const char *src, CompDiagList *diag)
{
    if (gl.type != TOML_TABLE)
        return;

    toml_datum_t gen = toml_get(gl, COMP_KEY_ENABLED);
    if (gen.type == TOML_BOOLEAN)
        gl_cfg->enabled = gen.u.boolean;

    toml_datum_t gpa = toml_get(gl, COMP_KEY_PASSES);
    if (gpa.type != TOML_UNKNOWN) {
        if (gpa.type == TOML_INT64 && gpa.u.int64 >= 1 && gpa.u.int64 <= COMP_FX_GLASS_MAX_PASSES)
            gl_cfg->passes = (int32_t)gpa.u.int64;
        else
            comp_diag_add(diag, src, gpa.lineno, "[effects.glass] 'passes' must be an integer in 1..%d; ignored",
                          COMP_FX_GLASS_MAX_PASSES);
    }

    double dval = 0.0;
    toml_datum_t gof = toml_get(gl, COMP_KEY_OFFSET);
    if (gof.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gof, &dval) && dval >= 0.0 && dval <= 16.0)
            gl_cfg->offset = dval;
        else
            comp_diag_add(diag, src, gof.lineno, "[effects.glass] 'offset' must be in 0..16; ignored");
    }

    toml_datum_t gsa = toml_get(gl, COMP_KEY_SATURATION);
    if (gsa.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gsa, &dval) && dval >= 0.0 && dval <= 4.0)
            gl_cfg->saturation = dval;
        else
            comp_diag_add(diag, src, gsa.lineno, "[effects.glass] 'saturation' must be in 0..4; ignored");
    }

    toml_datum_t gbr = toml_get(gl, COMP_KEY_BRIGHTNESS);
    if (gbr.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gbr, &dval) && dval >= 0.0 && dval <= 4.0)
            gl_cfg->brightness = dval;
        else
            comp_diag_add(diag, src, gbr.lineno, "[effects.glass] 'brightness' must be in 0..4; ignored");
    }

    toml_datum_t gti = toml_get(gl, COMP_KEY_TINT);
    if (gti.type == TOML_STRING && !comp_config_parse_hex_rgba(gti.u.s, gl_cfg->tint_rgba))
        comp_diag_add(diag, src, gti.lineno, "[effects.glass] invalid hex color '%s'; ignored", gti.u.s);

    toml_datum_t gta = toml_get(gl, COMP_KEY_TINT_ALPHA);
    if (gta.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gta, &dval) && dval >= 0.0 && dval <= 1.0)
            gl_cfg->tint_alpha = dval;
        else
            comp_diag_add(diag, src, gta.lineno, "[effects.glass] 'tint_alpha' must be in 0..1; ignored");
    }

    toml_datum_t mhz = toml_get(gl, COMP_KEY_MAX_RECOMPUTE_HZ);
    if (mhz.type != TOML_UNKNOWN) {
        if (mhz.type == TOML_INT64 && mhz.u.int64 >= 0 && mhz.u.int64 <= 1000)
            gl_cfg->max_recompute_hz = (int32_t)mhz.u.int64;
        else
            comp_diag_add(diag, src, mhz.lineno, "[effects.glass] 'max_recompute_hz' must be in 0..1000; ignored");
    }

    toml_datum_t gst = toml_get(gl, COMP_KEY_STYLE);
    if (gst.type != TOML_UNKNOWN) {
        CompFxGlassStyle st = COMP_FX_GLASS_FROSTED;
        if (gst.type == TOML_STRING && comp_fx_glass_style_from_str(gst.u.s, &st))
            gl_cfg->style = st;
        else
            comp_diag_add(diag, src, gst.lineno,
                          "[effects.glass] 'style' must be frosted|liquid|halation|fluted|acrylic|aero|dichroic; ignored");
    }

    toml_datum_t gref = toml_get(gl, COMP_KEY_REFRACTION);
    if (gref.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gref, &dval) && dval >= 0.0 && dval <= 2.0)
            gl_cfg->refraction = dval;
        else
            comp_diag_add(diag, src, gref.lineno, "[effects.glass] 'refraction' must be in 0..2; ignored");
    }

    toml_datum_t gdisp = toml_get(gl, COMP_KEY_DISPERSION);
    if (gdisp.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gdisp, &dval) && dval >= 0.0 && dval <= 1.0)
            gl_cfg->dispersion = dval;
        else
            comp_diag_add(diag, src, gdisp.lineno, "[effects.glass] 'dispersion' must be in 0..1; ignored");
    }

    toml_datum_t gbev = toml_get(gl, COMP_KEY_BEVEL_PX);
    if (gbev.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gbev, &dval) && dval >= 2.0 && dval <= 64.0)
            gl_cfg->bevel_px = dval;
        else
            comp_diag_add(diag, src, gbev.lineno, "[effects.glass] 'bevel_px' must be in 2..64; ignored");
    }

    toml_datum_t gspec = toml_get(gl, COMP_KEY_SPECULAR);
    if (gspec.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gspec, &dval) && dval >= 0.0 && dval <= 2.0)
            gl_cfg->specular = dval;
        else
            comp_diag_add(diag, src, gspec.lineno, "[effects.glass] 'specular' must be in 0..2; ignored");
    }

    toml_datum_t ggr = toml_get(gl, COMP_KEY_GRAIN);
    if (ggr.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(ggr, &dval) && dval >= 0.0 && dval <= 1.0)
            gl_cfg->grain = dval;
        else
            comp_diag_add(diag, src, ggr.lineno, "[effects.glass] 'grain' must be in 0..1; ignored");
    }

    toml_datum_t gfp = toml_get(gl, COMP_KEY_FLUTE_PX);
    if (gfp.type != TOML_UNKNOWN) {
        if (comp_config_datum_as_double(gfp, &dval) && dval >= 4.0 && dval <= 64.0)
            gl_cfg->flute_px = dval;
        else
            comp_diag_add(diag, src, gfp.lineno, "[effects.glass] 'flute_px' must be in 4..64; ignored");
    }

    toml_datum_t gfa = toml_get(gl, COMP_KEY_FLUTE_AXIS);
    if (gfa.type != TOML_UNKNOWN) {
        CompFxFluteAxis ax = COMP_FX_FLUTE_VERTICAL;
        if (gfa.type == TOML_STRING && comp_fx_flute_axis_from_str(gfa.u.s, &ax))
            gl_cfg->flute_axis = ax;
        else
            comp_diag_add(diag, src, gfa.lineno,
                          "[effects.glass] 'flute_axis' must be 'vertical' or 'horizontal'; ignored");
    }
}

static void
merge_effects_transitions(CompFxTransitionsConfig *tr_cfg, toml_datum_t tr,
                          const char *src, CompDiagList *diag)
{
    if (tr.type != TOML_TABLE)
        return;

    toml_datum_t ten = toml_get(tr, COMP_KEY_ENABLED);
    if (ten.type == TOML_BOOLEAN)
        tr_cfg->enabled = ten.u.boolean;

    toml_datum_t op = toml_get(tr, COMP_KEY_OPEN_PRESET);
    if (op.type == TOML_STRING) {
        CompFxOpenPreset p = COMP_FX_OPEN_DEFAULT;
        if (comp_fx_open_preset_from_str(op.u.s, &p))
            tr_cfg->open_preset = p;
        else
            comp_diag_add(diag, src, op.lineno, "[effects.transitions] unknown open_preset '%s'; ignored", op.u.s);
    }
    toml_datum_t od = toml_get(tr, COMP_KEY_OPEN_DURATION_MS);
    if (od.type == TOML_INT64 && od.u.int64 >= 0 && od.u.int64 <= 2000)
        tr_cfg->open_duration_ms = (int32_t)od.u.int64;
    toml_datum_t oc = toml_get(tr, COMP_KEY_OPEN_CURVE);
    if (oc.type == TOML_STRING) {
        CompFxTransitionCurve c = COMP_FX_CURVE_EASE_OUT_EXPO;
        if (comp_fx_curve_from_str(oc.u.s, &c))
            tr_cfg->open_curve = c;
        else
            comp_diag_add(diag, src, oc.lineno, "[effects.transitions] unknown open_curve '%s'; ignored", oc.u.s);
    }

    toml_datum_t cp = toml_get(tr, COMP_KEY_CLOSE_PRESET);
    if (cp.type == TOML_STRING) {
        CompFxClosePreset p = COMP_FX_CLOSE_DEFAULT;
        if (comp_fx_close_preset_from_str(cp.u.s, &p))
            tr_cfg->close_preset = p;
        else
            comp_diag_add(diag, src, cp.lineno, "[effects.transitions] unknown close_preset '%s'; ignored", cp.u.s);
    }
    toml_datum_t cd = toml_get(tr, COMP_KEY_CLOSE_DURATION_MS);
    if (cd.type == TOML_INT64 && cd.u.int64 >= 0 && cd.u.int64 <= 2000)
        tr_cfg->close_duration_ms = (int32_t)cd.u.int64;
    toml_datum_t cc = toml_get(tr, COMP_KEY_CLOSE_CURVE);
    if (cc.type == TOML_STRING) {
        CompFxTransitionCurve c = COMP_FX_CURVE_EASE_OUT_QUAD;
        if (comp_fx_curve_from_str(cc.u.s, &c))
            tr_cfg->close_curve = c;
        else
            comp_diag_add(diag, src, cc.lineno, "[effects.transitions] unknown close_curve '%s'; ignored", cc.u.s);
    }

    toml_datum_t rp = toml_get(tr, COMP_KEY_RESIZE_PRESET);
    if (rp.type == TOML_STRING) {
        CompFxResizePreset p = COMP_FX_RESIZE_DEFAULT;
        if (comp_fx_resize_preset_from_str(rp.u.s, &p))
            tr_cfg->resize_preset = p;
        else
            comp_diag_add(diag, src, rp.lineno, "[effects.transitions] unknown resize_preset '%s'; ignored", rp.u.s);
    }
    toml_datum_t rd = toml_get(tr, COMP_KEY_RESIZE_DURATION_MS);
    if (rd.type == TOML_INT64 && rd.u.int64 >= 0 && rd.u.int64 <= 2000)
        tr_cfg->resize_duration_ms = (int32_t)rd.u.int64;
}

void
comp_config_merge_effects_section(CompConfig *cfg, toml_datum_t fx,
                                  const char *src, CompDiagList *diag)
{
    if (fx.type != TOML_TABLE)
        return;

    merge_effects_top_level(&cfg->fx, fx, src, diag);
    merge_effects_shadow(&cfg->fx.shadow, toml_get(fx, COMP_SEC_FX_SHADOW), src, diag);
    merge_effects_dim(&cfg->fx.dim, toml_get(fx, COMP_SEC_FX_DIM), src, diag);
    merge_effects_glass(&cfg->fx.glass, toml_get(fx, COMP_SEC_FX_GLASS), src, diag);
    merge_effects_transitions(&cfg->fx.transitions, toml_get(fx, COMP_SEC_FX_TRANSITIONS), src, diag);
}
