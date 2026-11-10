#include "../kajo_config_schema.h"
#include <string.h>

static const KajoEnumMap k_panel_pos_map[] = {
    { "top",    PANEL_POSITION_TOP },
    { "bottom", PANEL_POSITION_BOTTOM },
    { NULL, 0 }
};

static const KajoEnumMap k_panel_layer_map[] = {
    { "top",        PANEL_LAYER_TOP },
    { "overlay",    PANEL_LAYER_OVERLAY },
    { "bottom",     PANEL_LAYER_BOTTOM },
    { "background", PANEL_LAYER_BACKGROUND },
    { NULL, 0 }
};

static const KajoEnumMap k_toast_click_map[] = {
    { "focus",   TOAST_CLICK_FOCUS },
    { "dismiss", TOAST_CLICK_DISMISS },
    { NULL, 0 }
};

static const KajoEnumMap k_swipe_center_map[] = {
    { "launcher", SWIPE_CENTER_LAUNCHER },
    { "overview", SWIPE_CENTER_OVERVIEW },
    { "osk",      SWIPE_CENTER_OSK },
    { "none",     SWIPE_CENTER_NONE },
    { NULL, 0 }
};

static const KajoEnumMap k_swipe_right_map[] = {
    { "control-center", SWIPE_RIGHT_CONTROL_CENTER },
    { "notifications",  SWIPE_RIGHT_NOTIFICATIONS },
    { "osk",            SWIPE_RIGHT_OSK },
    { "none",           SWIPE_RIGHT_NONE },
    { NULL, 0 }
};

static const KajoEnumMap k_swipe_horiz_map[] = {
    { "column",    SWIPE_HORIZ_COLUMN },
    { "workspace", SWIPE_HORIZ_WORKSPACE },
    { "none",      SWIPE_HORIZ_NONE },
    { NULL, 0 }
};

static const KajoEnumMap k_osk_mode_map[] = {
    { "tablet", OSK_AUTO_SHOW_TABLET },
    { "always", OSK_AUTO_SHOW_ALWAYS },
    { "manual", OSK_AUTO_SHOW_MANUAL },
    { NULL, 0 }
};

static const KajoEnumMap k_ss_mode_map[] = {
    { "overlay",  SCREENSAVER_MODE_OVERLAY },
    { "dpms",     SCREENSAVER_MODE_DPMS },
    { "disabled", SCREENSAVER_MODE_DISABLED },
    { NULL, 0 }
};

static const KajoFieldSpec k_shell_fields[] = {
    /* [panel] */
    {
        .section  = KAJO_CFG_SEC_PANEL,
        .key      = KAJO_CFG_KEY_PANEL_POSITION,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, panel_position),
        .def.e    = PANEL_POSITION_BOTTOM,
        .enum_map = k_panel_pos_map,
    },
    {
        .section = KAJO_CFG_SEC_PANEL,
        .key     = KAJO_CFG_KEY_PANEL_HEIGHT,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, panel_height),
        .def.i   = 39,
        .min_int = 24,
        .max_int = 96,
    },
    {
        .section  = KAJO_CFG_SEC_PANEL,
        .key      = KAJO_CFG_KEY_PANEL_LAYER,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, panel_layer),
        .def.e    = PANEL_LAYER_TOP,
        .enum_map = k_panel_layer_map,
    },
    {
        .section = KAJO_CFG_SEC_PANEL,
        .key     = KAJO_CFG_KEY_PANEL_ICON_SIZE,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, icon_size),
        .def.i   = 24,
        .min_int = 12,
        .max_int = 64,
    },

    /* [autohide] */
    {
        .section = KAJO_CFG_SEC_AUTOHIDE,
        .key     = KAJO_CFG_KEY_AUTOHIDE_ENABLED,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, autohide_enabled),
        .def.b   = TRUE,
    },
    {
        .section = KAJO_CFG_SEC_AUTOHIDE,
        .key     = KAJO_CFG_KEY_AUTOHIDE_REV_DELAY,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, autohide_reveal_delay),
        .def.i   = 200,
        .min_int = 0,
        .max_int = 5000,
    },
    {
        .section = KAJO_CFG_SEC_AUTOHIDE,
        .key     = KAJO_CFG_KEY_AUTOHIDE_HIDE_DELAY,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, autohide_hide_delay),
        .def.i   = 1000,
        .min_int = 0,
        .max_int = 10000,
    },
    {
        .section = KAJO_CFG_SEC_AUTOHIDE,
        .key     = KAJO_CFG_KEY_AUTOHIDE_REV_DUR,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, autohide_reveal_duration),
        .def.i   = 150,
        .min_int = 0,
        .max_int = 2000,
    },
    {
        .section = KAJO_CFG_SEC_AUTOHIDE,
        .key     = KAJO_CFG_KEY_AUTOHIDE_HIDE_DUR,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, autohide_hide_duration),
        .def.i   = 200,
        .min_int = 0,
        .max_int = 2000,
    },
    {
        .section = KAJO_CFG_SEC_AUTOHIDE,
        .key     = KAJO_CFG_KEY_AUTOHIDE_EDGE_STRIP,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, autohide_edge_strip_px),
        .def.i   = 2,
        .min_int = 1,
        .max_int = 16,
    },

    /* [notifications] */
    {
        .section = KAJO_CFG_SEC_NOTIFICATIONS,
        .key     = KAJO_CFG_KEY_NOTIF_TIMEOUT_MS,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, toast_timeout_ms),
        .def.i   = 8000,
        .min_int = 1000,
        .max_int = 30000,
    },
    {
        .section = KAJO_CFG_SEC_NOTIFICATIONS,
        .key     = KAJO_CFG_KEY_NOTIF_MAX_HISTORY,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, max_notification_history),
        .def.i   = 50,
        .min_int = 10,
        .max_int = 200,
    },
    {
        .section  = KAJO_CFG_SEC_NOTIFICATIONS,
        .key      = KAJO_CFG_KEY_NOTIF_CLICK_ACTION,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, toast_click_action),
        .def.e    = TOAST_CLICK_FOCUS,
        .enum_map = k_toast_click_map,
    },
    {
        .section = KAJO_CFG_SEC_NOTIFICATIONS,
        .key     = KAJO_CFG_KEY_NOTIF_DND_ENABLED,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, dnd_enabled),
        .def.b   = FALSE,
    },

    /* [touch] */
    {
        .section = KAJO_CFG_SEC_TOUCH,
        .key     = KAJO_CFG_KEY_TOUCH_ENABLED,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, touch_gestures_enabled),
        .def.b   = TRUE,
    },
    {
        .section  = KAJO_CFG_SEC_TOUCH,
        .key      = KAJO_CFG_KEY_TOUCH_SWIPE_CENTER,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, touch_swipe_center),
        .def.e    = SWIPE_CENTER_LAUNCHER,
        .enum_map = k_swipe_center_map,
    },
    {
        .section  = KAJO_CFG_SEC_TOUCH,
        .key      = KAJO_CFG_KEY_TOUCH_SWIPE_RIGHT,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, touch_swipe_right),
        .def.e    = SWIPE_RIGHT_CONTROL_CENTER,
        .enum_map = k_swipe_right_map,
    },
    {
        .section  = KAJO_CFG_SEC_TOUCH,
        .key      = KAJO_CFG_KEY_TOUCH_SWIPE_HORIZ,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, touch_swipe_horizontal),
        .def.e    = SWIPE_HORIZ_COLUMN,
        .enum_map = k_swipe_horiz_map,
    },
    {
        .section = KAJO_CFG_SEC_TOUCH,
        .key     = KAJO_CFG_KEY_TOUCH_DWELL_MS,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, touch_dwell_ms),
        .def.i   = 4000,
        .min_int = 500,
        .max_int = 15000,
    },

    /* [osk] */
    {
        .section  = KAJO_CFG_SEC_OSK,
        .key      = KAJO_CFG_KEY_OSK_AUTO_SHOW,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, osk_auto_show_mode),
        .def.e    = OSK_AUTO_SHOW_TABLET,
        .enum_map = k_osk_mode_map,
    },
    {
        .section = KAJO_CFG_SEC_OSK,
        .key     = KAJO_CFG_KEY_OSK_PUSH_WINDOWS,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, osk_push_windows),
        .def.b   = TRUE,
    },
    {
        .section = KAJO_CFG_SEC_OSK,
        .key     = KAJO_CFG_KEY_OSK_HEIGHT_PX,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, osk_height_px),
        .def.i   = 260,
        .min_int = 180,
        .max_int = 400,
    },

    /* [lock] */
    {
        .section  = KAJO_CFG_SEC_LOCK,
        .key      = KAJO_CFG_KEY_LOCK_SS_MODE,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoShellCfg, screensaver_mode),
        .def.e    = SCREENSAVER_MODE_OVERLAY,
        .enum_map = k_ss_mode_map,
    },
    {
        .section = KAJO_CFG_SEC_LOCK,
        .key     = KAJO_CFG_KEY_LOCK_SS_TIMEOUT,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, screensaver_timeout_sec),
        .def.i   = 300,
        .min_int = 0,
        .max_int = 86400,
    },
    {
        .section = KAJO_CFG_SEC_LOCK,
        .key     = KAJO_CFG_KEY_LOCK_DPMS_TIMEOUT,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, dpms_timeout_sec),
        .def.i   = 600,
        .min_int = 0,
        .max_int = 86400,
    },
    {
        .section = KAJO_CFG_SEC_LOCK,
        .key     = KAJO_CFG_KEY_LOCK_TIMEOUT,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, lock_timeout_sec),
        .def.i   = 900,
        .min_int = 0,
        .max_int = 86400,
    },
    {
        .section = KAJO_CFG_SEC_LOCK,
        .key     = KAJO_CFG_KEY_LOCK_ON_SUSPEND,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, lock_on_suspend),
        .def.b   = TRUE,
    },

    /* [hardware] */
    {
        .section = KAJO_CFG_SEC_HARDWARE,
        .key     = KAJO_CFG_KEY_HW_KBD_BL_LEVEL,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, kbd_backlight_level),
        .def.i   = 2,
        .min_int = 0,
        .max_int = 2,
    },
    {
        .section = KAJO_CFG_SEC_HARDWARE,
        .key     = KAJO_CFG_KEY_HW_KBD_BL_TIMEOUT,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoShellCfg, kbd_backlight_timeout_sec),
        .def.i   = 0,
        .min_int = 0,
        .max_int = 86400,
    },
    {
        .section = KAJO_CFG_SEC_HARDWARE,
        .key     = KAJO_CFG_KEY_HW_POSTURE_ROTATE,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, posture_auto_rotate),
        .def.b   = TRUE,
    },
    {
        .section = KAJO_CFG_SEC_HARDWARE,
        .key     = KAJO_CFG_KEY_HW_DISABLE_KBD_TAB,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, posture_disable_kbd_in_tablet),
        .def.b   = TRUE,
    },
    {
        .section = KAJO_CFG_SEC_HARDWARE,
        .key     = KAJO_CFG_KEY_HW_DISABLE_TOUCH_LAP,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoShellCfg, posture_disable_touch_in_laptop),
        .def.b   = FALSE,
    },
};

typedef struct {
    const char     *id;
    ShellWidgetMode mode;
} DefaultWidgetSpec;

static const DefaultWidgetSpec k_default_left[] = {
    { "launcher",   WIDGET_MODE_ICON },
    { "workspaces", WIDGET_MODE_ICON },
    { "window",     WIDGET_MODE_FULL },
};

static const DefaultWidgetSpec k_default_center[] = {
    { "clock",      WIDGET_MODE_LABEL },
};

static const DefaultWidgetSpec k_default_right[] = {
    { "tray",            WIDGET_MODE_ICON },
    { "notifications",   WIDGET_MODE_ICON },
    { "media",           WIDGET_MODE_ICON },
    { "network",         WIDGET_MODE_ICON },
    { "bluetooth",       WIDGET_MODE_ICON },
    { "cpu-mem",         WIDGET_MODE_ICON },
    { "keyboard-layout", WIDGET_MODE_LABEL },
    { "osk",             WIDGET_MODE_ICON },
    { "volume",          WIDGET_MODE_ICON },
    { "battery",         WIDGET_MODE_ICON },
    { "privacy",         WIDGET_MODE_ICON },
    { "theme",           WIDGET_MODE_ICON },
    { "custom-script",   WIDGET_MODE_ICON },
    { "control-center",  WIDGET_MODE_ICON },
    { "power",           WIDGET_MODE_ICON },
};

static GPtrArray *
build_default_widget_array(const DefaultWidgetSpec *specs, size_t n)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)shell_widget_config_free);
    for (size_t i = 0; i < n; i++) {
        g_ptr_array_add(arr, shell_widget_config_new(specs[i].id, specs[i].mode));
    }
    return arr;
}

static GPtrArray *
clone_widget_array(const GPtrArray *src)
{
    GPtrArray *dst = g_ptr_array_new_with_free_func((GDestroyNotify)shell_widget_config_free);
    if (!src)
        return dst;
    for (guint i = 0; i < src->len; i++) {
        const ShellWidgetConfig *wc = (const ShellWidgetConfig *)g_ptr_array_index(src, i);
        if (wc)
            g_ptr_array_add(dst, shell_widget_config_copy(wc));
    }
    return dst;
}

static gboolean
widget_arrays_equal(const GPtrArray *a, const GPtrArray *b)
{
    if (a == b)
        return TRUE;
    guint la = a ? a->len : 0;
    guint lb = b ? b->len : 0;
    if (la != lb)
        return FALSE;
    for (guint i = 0; i < la; i++) {
        const ShellWidgetConfig *wa = (const ShellWidgetConfig *)g_ptr_array_index(a, i);
        const ShellWidgetConfig *wb = (const ShellWidgetConfig *)g_ptr_array_index(b, i);
        if (!shell_widget_config_equal(wa, wb))
            return FALSE;
    }
    return TRUE;
}

static void
shell_init_custom_defaults(void *struct_ptr)
{
    KajoShellCfg *cfg = (KajoShellCfg *)struct_ptr;
    cfg->layout_left   = build_default_widget_array(k_default_left, G_N_ELEMENTS(k_default_left));
    cfg->layout_center = build_default_widget_array(k_default_center, G_N_ELEMENTS(k_default_center));
    cfg->layout_right  = build_default_widget_array(k_default_right, G_N_ELEMENTS(k_default_right));
}

static void
parse_toml_widget_list(toml_datum_t arr_datum, GPtrArray **target_arr)
{
    if (arr_datum.type != TOML_ARRAY || !target_arr)
        return;

    GPtrArray *new_arr = g_ptr_array_new_with_free_func((GDestroyNotify)shell_widget_config_free);

    for (int i = 0; i < arr_datum.u.arr.size; i++) {
        toml_datum_t elem = arr_datum.u.arr.elem[i];
        if (elem.type == TOML_STRING) {
            g_autofree gchar *id = g_strndup(elem.u.str.ptr ? elem.u.str.ptr : "",
                                             (gsize)MAX(0, elem.u.str.len));
            if (*id)
                g_ptr_array_add(new_arr, shell_widget_config_new(id, WIDGET_MODE_ICON));
        } else if (elem.type == TOML_TABLE) {
            toml_datum_t id_d = toml_get(elem, KAJO_CFG_KEY_WIDGET_ID);
            if (id_d.type != TOML_STRING || id_d.u.str.len <= 0)
                continue;

            g_autofree gchar *id = g_strndup(id_d.u.str.ptr, (gsize)id_d.u.str.len);
            ShellWidgetMode mode = WIDGET_MODE_ICON;
            toml_datum_t mode_d = toml_get(elem, KAJO_CFG_KEY_WIDGET_MODE);
            if (mode_d.type == TOML_STRING) {
                g_autofree gchar *mstr = g_strndup(mode_d.u.str.ptr ? mode_d.u.str.ptr : "",
                                                   (gsize)MAX(0, mode_d.u.str.len));
                mode = shell_widget_mode_from_string(mstr);
            }

            ShellWidgetConfig *wc = shell_widget_config_new(id, mode);

            toml_datum_t int_d = toml_get(elem, KAJO_CFG_KEY_WIDGET_INTERVAL);
            if (int_d.type == TOML_INT64 && int_d.u.int64 > 0) {
                wc->interval_sec = (gint)CLAMP(int_d.u.int64, 1, 86400);
            }

            toml_datum_t exec_d = toml_get(elem, KAJO_CFG_KEY_WIDGET_EXEC);
            if (exec_d.type == TOML_STRING && exec_d.u.str.len > 0) {
                wc->exec_cmd = g_strndup(exec_d.u.str.ptr, (gsize)exec_d.u.str.len);
            }

            toml_datum_t cl_d = toml_get(elem, KAJO_CFG_KEY_WIDGET_CLICK_LEFT);
            if (cl_d.type == TOML_STRING && cl_d.u.str.len > 0) {
                wc->click_left_cmd = g_strndup(cl_d.u.str.ptr, (gsize)cl_d.u.str.len);
            }

            toml_datum_t cr_d = toml_get(elem, KAJO_CFG_KEY_WIDGET_CLICK_RIGHT);
            if (cr_d.type == TOML_STRING && cr_d.u.str.len > 0) {
                wc->click_right_cmd = g_strndup(cr_d.u.str.ptr, (gsize)cr_d.u.str.len);
            }

            g_ptr_array_add(new_arr, wc);
        }
    }

    if (*target_arr)
        g_ptr_array_free(*target_arr, TRUE);
    *target_arr = new_arr;
}

static void
shell_parse_custom_toml(void *struct_ptr, toml_datum_t root_table, gboolean is_overlay G_GNUC_UNUSED)
{
    KajoShellCfg *cfg = (KajoShellCfg *)struct_ptr;
    toml_datum_t layout_tab = toml_get(root_table, KAJO_CFG_SEC_LAYOUT);
    if (layout_tab.type != TOML_TABLE)
        return;

    toml_datum_t left_d   = toml_get(layout_tab, KAJO_CFG_KEY_LAYOUT_LEFT);
    toml_datum_t center_d = toml_get(layout_tab, KAJO_CFG_KEY_LAYOUT_CENTER);
    toml_datum_t right_d  = toml_get(layout_tab, KAJO_CFG_KEY_LAYOUT_RIGHT);

    if (left_d.type == TOML_ARRAY)
        parse_toml_widget_list(left_d, &cfg->layout_left);
    if (center_d.type == TOML_ARRAY)
        parse_toml_widget_list(center_d, &cfg->layout_center);
    if (right_d.type == TOML_ARRAY)
        parse_toml_widget_list(right_d, &cfg->layout_right);
}

static void
write_toml_widget_array(GString *out, const char *key, const GPtrArray *arr)
{
    g_string_append_printf(out, "%s = [\n", key);
    if (arr) {
        for (guint i = 0; i < arr->len; i++) {
            const ShellWidgetConfig *wc = (const ShellWidgetConfig *)g_ptr_array_index(arr, i);
            if (!wc || !wc->id || !*wc->id)
                continue;
            g_string_append(out, "    { id = ");
            kajo_toml_append_quoted_string(out, wc->id);
            g_string_append(out, ", mode = ");
            kajo_toml_append_quoted_string(out, shell_widget_mode_to_string(wc->mode));

            if (wc->interval_sec > 0) {
                g_string_append_printf(out, ", interval = %d", wc->interval_sec);
            }
            if (wc->exec_cmd && *wc->exec_cmd) {
                g_string_append(out, ", exec = ");
                kajo_toml_append_literal_or_quoted_string(out, wc->exec_cmd);
            }
            if (wc->click_left_cmd && *wc->click_left_cmd) {
                g_string_append(out, ", click_left = ");
                kajo_toml_append_literal_or_quoted_string(out, wc->click_left_cmd);
            }
            if (wc->click_right_cmd && *wc->click_right_cmd) {
                g_string_append(out, ", click_right = ");
                kajo_toml_append_literal_or_quoted_string(out, wc->click_right_cmd);
            }
            g_string_append(out, " },\n");
        }
    }
    g_string_append(out, "]\n");
}

static void
shell_write_custom_toml(const void *struct_ptr, GString *out)
{
    const KajoShellCfg *cfg = (const KajoShellCfg *)struct_ptr;
    g_string_append(out, "\n[" KAJO_CFG_SEC_LAYOUT "]\n");
    write_toml_widget_array(out, KAJO_CFG_KEY_LAYOUT_LEFT,   cfg->layout_left);
    g_string_append_c(out, '\n');
    write_toml_widget_array(out, KAJO_CFG_KEY_LAYOUT_CENTER, cfg->layout_center);
    g_string_append_c(out, '\n');
    write_toml_widget_array(out, KAJO_CFG_KEY_LAYOUT_RIGHT,  cfg->layout_right);
}

static void
shell_copy_custom_fields(void *dst_ptr, const void *src_ptr)
{
    KajoShellCfg *dst = (KajoShellCfg *)dst_ptr;
    const KajoShellCfg *src = (const KajoShellCfg *)src_ptr;
    dst->layout_left   = clone_widget_array(src->layout_left);
    dst->layout_center = clone_widget_array(src->layout_center);
    dst->layout_right  = clone_widget_array(src->layout_right);
}

static gboolean
shell_equal_custom_fields(const void *a_ptr, const void *b_ptr)
{
    const KajoShellCfg *a = (const KajoShellCfg *)a_ptr;
    const KajoShellCfg *b = (const KajoShellCfg *)b_ptr;
    return widget_arrays_equal(a->layout_left, b->layout_left) &&
           widget_arrays_equal(a->layout_center, b->layout_center) &&
           widget_arrays_equal(a->layout_right, b->layout_right);
}

static void
shell_free_custom_fields(void *struct_ptr)
{
    KajoShellCfg *cfg = (KajoShellCfg *)struct_ptr;
    if (cfg->layout_left) {
        g_ptr_array_free(cfg->layout_left, TRUE);
        cfg->layout_left = NULL;
    }
    if (cfg->layout_center) {
        g_ptr_array_free(cfg->layout_center, TRUE);
        cfg->layout_center = NULL;
    }
    if (cfg->layout_right) {
        g_ptr_array_free(cfg->layout_right, TRUE);
        cfg->layout_right = NULL;
    }
}

static const KajoDomainSchema k_shell_schema = {
    .domain               = KAJO_CONFIG_DOMAIN_SHELL,
    .filename             = KAJO_CFG_FILE_SHELL,
    .domain_table_name    = KAJO_CFG_DOMAIN_SHELL,
    .struct_size          = sizeof(KajoShellCfg),
    .fields               = k_shell_fields,
    .n_fields             = G_N_ELEMENTS(k_shell_fields),
    .init_custom_defaults = shell_init_custom_defaults,
    .parse_custom_toml    = shell_parse_custom_toml,
    .write_custom_toml    = shell_write_custom_toml,
    .copy_custom_fields   = shell_copy_custom_fields,
    .equal_custom_fields  = shell_equal_custom_fields,
    .free_custom_fields   = shell_free_custom_fields,
};

const KajoDomainSchema *
kajo_schema_get_shell(void)
{
    return &k_shell_schema;
}
