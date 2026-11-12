#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../compositor/compositor.h"
#include "../../compositor/kajo_comp_ipc.h"
#include "../../hardware/hw_caps.h"
#include "../../hardware/kbd_backlight.h"
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    gint64 width;
    gint64 height;
    double refresh_hz;
} DisplayModeGeom;

typedef struct _SettingsPageDisplays {
    GtkBox parent_instance;

    /* Card 1: Monitor Output & Scaling */
    GtkWidget *card_mon;
    GtkWidget *lbl_mon_heading;
    GtkWidget *row_res;
    GtkWidget *combo_res;
    GtkWidget *seg_orient;
    GtkWidget *row_scale;
    GtkWidget *seg_scale;
    GtkWidget *row_vrr;
    GtkWidget *seg_vrr;

    gchar     *out_name;
    gchar     *make_model;
    GPtrArray *raw_modes;
    GArray    *mode_geoms;
    gboolean   vrr_avail;
    gboolean   saved_vrr_active;
    guint      saved_vrr_idx;
    guint      saved_mode_idx;
    guint      saved_orient_idx;
    guint      saved_scale_idx;

    /* Card 2: Idle Screensaver, DPMS & Session Lock */
    GtkWidget *seg_ss_mode;
    GtkWidget *row_ss_timeout;
    GtkWidget *combo_ss_timeout;
    GtkWidget *combo_dpms_timeout;
    GtkWidget *combo_lock_timeout;
    GtkWidget *sw_lock_suspend;

    /* Card 3: Keyboard Backlight & Timeout (hardware-gated) */
    GtkWidget *card_kbd;
    GtkWidget *kbd_level_ctl;
    GtkWidget *kbd_timeout_ctl;

    /* Card 4: 360° Foldable Posture & IIO Sensors (hardware-gated) */
    GtkWidget *card_posture;
    GtkWidget *sw_posture_autorotate;
    GtkWidget *sw_posture_disable_kbd;
    GtkWidget *sw_posture_disable_touch;

    gboolean   syncing_ui;
} SettingsPageDisplays;

typedef struct _SettingsPageDisplaysClass {
    GtkBoxClass parent_class;
} SettingsPageDisplaysClass;

G_DEFINE_TYPE(SettingsPageDisplays, settings_page_displays, GTK_TYPE_BOX)

static const gint   k_ss_timeout_vals[]   = {  60, 300, 600,  900, 1800, 0 };
static const gint   k_dpms_timeout_vals[] = { 300, 600, 900, 1800,    0 };
static const gint   k_lock_timeout_vals[] = { 300, 600, 900, 1800,    0 };
static const gint   k_kbd_timeout_vals[]  = {   0,  30, 180 };
static const double k_scale_vals[]        = { 1.00, 1.25, 1.50, 1.75, 2.00 };

static guint
timeout_sec_to_index(gint sec, const gint *values, guint count, guint fallback_idx)
{
    for (guint i = 0; i < count; i++) {
        if (values[i] == sec)
            return i;
    }
    return fallback_idx;
}

static void
on_niri_subproc_done(GObject *source, GAsyncResult *res, gpointer user_data G_GNUC_UNUSED)
{
    g_subprocess_wait_finish(G_SUBPROCESS(source), res, NULL);
}

static void
run_niri_output_cmd_async(const char *out_name, const char *action, const char *arg)
{
    if (!out_name || !action || !arg)
        return;
    const gchar *argv[] = { "niri", "msg", "output", out_name, action, arg, NULL };
    g_autoptr(GSubprocess) proc = g_subprocess_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL);
    if (proc)
        g_subprocess_wait_async(proc, NULL, on_niri_subproc_done, NULL);
}

static void
write_inhibit_paths_direct(GPtrArray *paths, gboolean inhibit)
{
    if (!paths)
        return;
    const gchar *val = inhibit ? "1\n" : "0\n";
    size_t len = strlen(val);
    for (guint i = 0; i < paths->len; i++) {
        const gchar *p = g_ptr_array_index(paths, i);
        if (!p)
            continue;
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            ssize_t unused G_GNUC_UNUSED = write(fd, val, len);
            close(fd);
        }
    }
}

static void
update_displays_dynamic_subtitles_and_telemetry(SettingsPageDisplays *self)
{
    if (!self)
        return;

    guint mode_sel = self->combo_res ? gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_res)) : 0;
    guint sc_sel   = self->seg_scale ? segmented_group_get_selected(self->seg_scale) : 0;
    double sc      = (sc_sel < G_N_ELEMENTS(k_scale_vals)) ? k_scale_vals[sc_sel] : 1.0;

    gint64 w = 0, h = 0;
    double rr = 0.0;
    if (self->mode_geoms && mode_sel < self->mode_geoms->len) {
        DisplayModeGeom g = g_array_index(self->mode_geoms, DisplayModeGeom, mode_sel);
        w = g.width;
        h = g.height;
        rr = g.refresh_hz;
    }

    if (self->row_scale) {
        gchar sc_buf[16];
        g_ascii_formatd(sc_buf, sizeof(sc_buf), "%.2f", sc);
        if (w > 0 && h > 0) {
            int lw = (int)llround((double)w / sc);
            int lh = (int)llround((double)h / sc);
            g_autofree gchar *sub = g_strdup_printf(
                "%.0f%% (%s) yields %d×%d logical workspace",
                sc * 100.0, sc_buf, lw, lh);
            boxed_row_set_subtitle(self->row_scale, sub);
        } else {
            g_autofree gchar *sub = g_strdup_printf(
                "%.0f%% (%sx) Wayland fractional output scale",
                sc * 100.0, sc_buf);
            boxed_row_set_subtitle(self->row_scale, sub);
        }
    }

    const gchar *out = (self->out_name && self->out_name[0]) ? self->out_name : "eDP-1";
    settings_page_set_counter(GTK_WIDGET(self), 0, "OUTPUT", out, "accent");
    settings_page_set_nav_badge(GTK_WIDGET(self), "displays", out, NULL);

    if (w > 0 && h > 0) {
        g_autofree gchar *m_val = g_strdup_printf("%" G_GINT64_FORMAT "×%" G_GINT64_FORMAT "@%.0f", w, h, rr);
        settings_page_set_counter(GTK_WIDGET(self), 1, "MODE", m_val, NULL);
    } else {
        settings_page_set_counter(GTK_WIDGET(self), 1, "MODE", "AUTO / NATIVE", NULL);
    }

    gchar sc_txt[24];
    g_ascii_formatd(sc_txt, sizeof(sc_txt), "%.2f", sc);
    g_autofree gchar *s_val = g_strdup_printf("%.0f%% (%s)", sc * 100.0, sc_txt);
    settings_page_set_counter(GTK_WIDGET(self), 2, "SCALE", s_val, NULL);

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    gdouble hinge_deg = 0.0;
    if (caps && shell_hw_caps_read_hinge_angle_deg(caps, &hinge_deg)) {
        const gchar *pm = (hinge_deg >= 180.0) ? "TABLET" : "LAPTOP";
        g_autofree gchar *p_val = g_strdup_printf("%s (%.0f°)", pm, hinge_deg);
        settings_page_set_counter(GTK_WIDGET(self), 3, "POSTURE", p_val, "success");
        if (self->card_posture) {
            g_autofree gchar *b_val = g_strdup_printf("%s • %.0f°", pm, hinge_deg);
            section_card_set_badge(self->card_posture, b_val, "success");
        }
    } else {
        guint ss_mode = self->seg_ss_mode ? segmented_group_get_selected(self->seg_ss_mode) : 0;
        const gchar *m_lbl = (ss_mode == 1) ? "DPMS" : (ss_mode == 2 ? "OFF" : "OVERLAY");
        settings_page_set_counter(GTK_WIDGET(self), 3, "IDLE", m_lbl, NULL);
    }
}

static void
mark_displays_changed(SettingsPageDisplays *self)
{
    if (!self || self->syncing_ui)
        return;
    update_displays_dynamic_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_any_dropdown_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    mark_displays_changed(user_data);
}

static void
on_any_segment_changed(GtkWidget *group G_GNUC_UNUSED, guint idx G_GNUC_UNUSED, gpointer user_data)
{
    mark_displays_changed(user_data);
}

static void
on_any_switch_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    mark_displays_changed(user_data);
}

static void
sync_vrr_row_subtitle(SettingsPageDisplays *self, guint vrr_idx)
{
    if (!self || !self->row_vrr)
        return;
    if (!self->vrr_avail) {
        boxed_row_set_subtitle(self->row_vrr, "Unsupported by this output");
        return;
    }
    const char *sub = "Adaptive Sync Disabled (Off)";
    if (vrr_idx == 1)
        sub = "Adaptive Sync Active (Always On)";
    else if (vrr_idx == 2)
        sub = "Adaptive Sync Active (Fullscreen On-Demand)";
    boxed_row_set_subtitle(self->row_vrr, sub);
}

static void
on_vrr_segment_changed(GtkWidget *group G_GNUC_UNUSED, guint idx, gpointer user_data)
{
    SettingsPageDisplays *self = user_data;
    if (!self || self->syncing_ui)
        return;
    sync_vrr_row_subtitle(self, idx);
    mark_displays_changed(self);
}

static void
on_lock_now_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    settings_send_ipc_command("lock");
}

static void
on_displays_preview_ss(gpointer user_data G_GNUC_UNUSED)
{
    settings_send_ipc_command("screensaver");
}

static void
save_outputs_toml_preserving_others(const char *out_name,
                                    const char *make_model,
                                    const char *mode_str,
                                    const char *scale_buf,
                                    const char *orient,
                                    const char *vrr_str)
{
    kajo_config_comp_save_output_rule(
        NULL, out_name, make_model, mode_str, scale_buf, orient, vrr_str, NULL);
}

static void
load_niri_and_config_state(SettingsPageDisplays *self)
{
    if (!self)
        return;
    self->syncing_ui = TRUE;

    g_free(self->out_name);
    g_free(self->make_model);
    self->out_name   = g_strdup("eDP-1");
    self->make_model = g_strdup("Built-in Display");
    self->vrr_avail  = FALSE;
    gboolean vrr_active = FALSE;
    guint vrr_idx = 0;
    guint active_mode_idx = 0;
    double current_scale = 1.0;
    g_autofree gchar *current_transform = g_strdup("Normal");

    g_ptr_array_set_size(self->raw_modes, 0);
    g_array_set_size(self->mode_geoms, 0);
    GtkStringList *slist_res = gtk_string_list_new(NULL);

    gboolean use_kajo_comp = shell_compositor_is_kajo_comp();
    if (self->card_mon) {
        section_card_set_badge(self->card_mon, use_kajo_comp ? "outputs.toml" : "10-outputs.kdl", "accent");
    }

    if (use_kajo_comp) {
        g_autofree gchar *kc_resp = kajo_comp_ipc_send_request("{\"cmd\":\"get-outputs\"}");
        if (kc_resp) {
            g_autoptr(JsonParser) parser = json_parser_new();
            if (json_parser_load_from_data(parser, kc_resp, -1, NULL)) {
                JsonNode *root = json_parser_get_root(parser);
                if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject *root_obj = json_node_get_object(root);
                    JsonObject *ok_obj = (json_object_has_member(root_obj, "ok") &&
                                          !json_object_get_null_member(root_obj, "ok"))
                                             ? json_object_get_object_member(root_obj, "ok")
                                             : NULL;
                    JsonArray *outputs_arr = (ok_obj && json_object_has_member(ok_obj, "outputs"))
                                                 ? json_object_get_array_member(ok_obj, "outputs")
                                                 : NULL;
                    if (outputs_arr && json_array_get_length(outputs_arr) > 0) {
                        JsonObject *out_info = json_array_get_object_element(outputs_arr, 0);
                        for (guint oi = 0; oi < json_array_get_length(outputs_arr); oi++) {
                            JsonObject *cand = json_array_get_object_element(outputs_arr, oi);
                            if (cand && json_object_get_boolean_member_with_default(cand, "active", FALSE)) {
                                out_info = cand;
                                break;
                            }
                        }
                        if (out_info) {
                            const gchar *o_name = json_object_has_member(out_info, "name")
                                ? json_object_get_string_member(out_info, "name") : "eDP-1";
                            g_free(self->out_name);
                            self->out_name = g_strdup(o_name);

                            const gchar *m_make  = json_object_has_member(out_info, "make") ? json_object_get_string_member(out_info, "make") : "";
                            const gchar *m_model = json_object_has_member(out_info, "model") ? json_object_get_string_member(out_info, "model") : "";
                            g_free(self->make_model);
                            self->make_model = g_strstrip(g_strdup_printf("%s %s", m_make, m_model));
                            if (self->make_model[0] == '\0') {
                                g_free(self->make_model);
                                self->make_model = g_strdup("Display Output");
                            }

                            self->vrr_avail = TRUE;
                            const gchar *vrr_s = json_object_has_member(out_info, "vrr")
                                ? json_object_get_string_member(out_info, "vrr") : "off";
                            if (g_strcmp0(vrr_s, "on") == 0) {
                                vrr_idx = 1;
                                vrr_active = TRUE;
                            } else if (g_strcmp0(vrr_s, "on-demand") == 0) {
                                vrr_idx = 2;
                                vrr_active = TRUE;
                            }

                            if (json_object_has_member(out_info, "scale"))
                                current_scale = json_object_get_double_member(out_info, "scale");
                            if (json_object_has_member(out_info, "transform")) {
                                g_free(current_transform);
                                current_transform = g_strdup(json_object_get_string_member(out_info, "transform"));
                            }

                            gint64 cur_w = json_object_has_member(out_info, "width") ? json_object_get_int_member(out_info, "width") : 0;
                            gint64 cur_h = json_object_has_member(out_info, "height") ? json_object_get_int_member(out_info, "height") : 0;
                            gint64 cur_mhz = json_object_has_member(out_info, "refresh_mhz") ? json_object_get_int_member(out_info, "refresh_mhz") : 0;

                            if (json_object_has_member(out_info, "modes")) {
                                JsonArray *modes_arr = json_object_get_array_member(out_info, "modes");
                                guint n_modes = modes_arr ? json_array_get_length(modes_arr) : 0;
                                for (guint i = 0; i < n_modes; i++) {
                                    JsonObject *m = json_array_get_object_element(modes_arr, i);
                                    gint64 w = json_object_has_member(m, "width") ? json_object_get_int_member(m, "width") : 0;
                                    gint64 h = json_object_has_member(m, "height") ? json_object_get_int_member(m, "height") : 0;
                                    gint64 mhz = json_object_has_member(m, "refresh_mhz") ? json_object_get_int_member(m, "refresh_mhz") : 60000;
                                    double rr = (double)mhz / 1000.0;
                                    gboolean pref = json_object_get_boolean_member_with_default(m, "preferred", FALSE);

                                    if (w == cur_w && h == cur_h && llabs(mhz - cur_mhz) <= 50)
                                        active_mode_idx = i;

                                    gchar rr_raw_buf[32];
                                    g_ascii_formatd(rr_raw_buf, sizeof(rr_raw_buf), "%.3f", rr);
                                    g_ptr_array_add(self->raw_modes,
                                                    g_strdup_printf("%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT "@%s", w, h, rr_raw_buf));

                                    DisplayModeGeom geom = { w, h, rr };
                                    g_array_append_val(self->mode_geoms, geom);

                                    gchar rr_disp_buf[32];
                                    g_ascii_formatd(rr_disp_buf, sizeof(rr_disp_buf), "%.2f", rr);
                                    g_autofree gchar *disp_mode = g_strdup_printf(
                                        "%" G_GINT64_FORMAT "×%" G_GINT64_FORMAT " @ %sHz%s",
                                        w, h, rr_disp_buf, pref ? " (Preferred)" : "");
                                    gtk_string_list_append(slist_res, disp_mode);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    g_autofree gchar *ipc_resp = (!use_kajo_comp) ? niri_send_ipc_request("\"Outputs\"") : NULL;
    if (ipc_resp) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser, ipc_resp, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *root_obj = json_node_get_object(root);
                JsonObject *outputs_obj = NULL;

                if (json_object_has_member(root_obj, "Ok")) {
                    JsonNode *ok_node = json_object_get_member(root_obj, "Ok");
                    if (ok_node && JSON_NODE_HOLDS_OBJECT(ok_node)) {
                        JsonObject *ok_obj = json_node_get_object(ok_node);
                        if (ok_obj && json_object_has_member(ok_obj, "Outputs")) {
                            JsonNode *out_node = json_object_get_member(ok_obj, "Outputs");
                            if (out_node && JSON_NODE_HOLDS_OBJECT(out_node))
                                outputs_obj = json_node_get_object(out_node);
                        }
                    }
                } else {
                    outputs_obj = root_obj;
                }

                if (outputs_obj) {
                    GList *members = json_object_get_members(outputs_obj);
                    if (members && members->data) {
                        const gchar *out_name = (const gchar *)members->data;
                        JsonNode *out_node = json_object_get_member(outputs_obj, out_name);
                        if (out_node && JSON_NODE_HOLDS_OBJECT(out_node)) {
                            JsonObject *out_info = json_node_get_object(out_node);

                            g_free(self->out_name);
                            self->out_name = g_strdup(out_name);

                            const gchar *m_make  = json_object_has_member(out_info, "make") ? json_object_get_string_member(out_info, "make") : "";
                            const gchar *m_model = json_object_has_member(out_info, "model") ? json_object_get_string_member(out_info, "model") : "";
                            g_free(self->make_model);
                            self->make_model = g_strstrip(g_strdup_printf("%s %s", m_make, m_model));
                            if (self->make_model[0] == '\0') {
                                g_free(self->make_model);
                                self->make_model = g_strdup("Display Output");
                            }

                            if (json_object_has_member(out_info, "vrr_supported"))
                                self->vrr_avail = json_object_get_boolean_member(out_info, "vrr_supported");
                            if (json_object_has_member(out_info, "vrr_enabled"))
                                vrr_active = json_object_get_boolean_member(out_info, "vrr_enabled");
                            if (json_object_has_member(out_info, "current_mode")) {
                                JsonNode *cm_node = json_object_get_member(out_info, "current_mode");
                                if (cm_node && JSON_NODE_HOLDS_VALUE(cm_node))
                                    active_mode_idx = (guint)json_object_get_int_member(out_info, "current_mode");
                            }

                            if (json_object_has_member(out_info, "logical")) {
                                JsonNode *log_node = json_object_get_member(out_info, "logical");
                                if (log_node && JSON_NODE_HOLDS_OBJECT(log_node)) {
                                    JsonObject *log_obj = json_node_get_object(log_node);
                                    if (json_object_has_member(log_obj, "scale"))
                                        current_scale = json_object_get_double_member(log_obj, "scale");
                                    if (json_object_has_member(log_obj, "transform")) {
                                        g_free(current_transform);
                                        current_transform = g_strdup(json_object_get_string_member(log_obj, "transform"));
                                    }
                                }
                            }

                            if (json_object_has_member(out_info, "modes")) {
                                JsonNode *modes_node = json_object_get_member(out_info, "modes");
                                if (modes_node && JSON_NODE_HOLDS_ARRAY(modes_node)) {
                                    JsonArray *modes_arr = json_node_get_array(modes_node);
                                    guint n_modes = json_array_get_length(modes_arr);
                                    for (guint i = 0; i < n_modes; i++) {
                                        JsonObject *m = json_array_get_object_element(modes_arr, i);
                                        gint64 w = json_object_has_member(m, "width") ? json_object_get_int_member(m, "width") : 0;
                                        gint64 h = json_object_has_member(m, "height") ? json_object_get_int_member(m, "height") : 0;
                                        double rr = json_object_has_member(m, "refresh_rate") ? (json_object_get_int_member(m, "refresh_rate") / 1000.0) : 60.0;
                                        gboolean pref = json_object_has_member(m, "is_preferred") ? json_object_get_boolean_member(m, "is_preferred") : FALSE;

                                        gchar rr_raw_buf[32];
                                        g_ascii_formatd(rr_raw_buf, sizeof(rr_raw_buf), "%.3f", rr);
                                        g_ptr_array_add(self->raw_modes,
                                                        g_strdup_printf("%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT "@%s", w, h, rr_raw_buf));

                                        DisplayModeGeom geom = { w, h, rr };
                                        g_array_append_val(self->mode_geoms, geom);

                                        gchar rr_disp_buf[32];
                                        g_ascii_formatd(rr_disp_buf, sizeof(rr_disp_buf), "%.2f", rr);
                                        g_autofree gchar *disp_mode = g_strdup_printf(
                                            "%" G_GINT64_FORMAT "×%" G_GINT64_FORMAT " @ %sHz%s",
                                            w, h, rr_disp_buf, pref ? " (Preferred)" : "");
                                        gtk_string_list_append(slist_res, disp_mode);
                                    }
                                }
                            }
                        }
                        g_list_free(members);
                    }
                }
            }
        }
    }

    if (self->raw_modes->len == 0) {
        gtk_string_list_append(slist_res, "Automatic / Native Preferred Mode");
        g_ptr_array_add(self->raw_modes, g_strdup("auto"));
        DisplayModeGeom geom = { 0, 0, 60.0 };
        g_array_append_val(self->mode_geoms, geom);
    }

    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_res), G_LIST_MODEL(slist_res));
    g_object_unref(slist_res);
    if (active_mode_idx >= self->raw_modes->len)
        active_mode_idx = 0;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_res), active_mode_idx);

    if (self->lbl_mon_heading) {
        g_autofree gchar *h_txt = g_strdup_printf("1. MONITOR OUTPUT & SCALING (%s)", self->out_name);
        gtk_label_set_text(GTK_LABEL(self->lbl_mon_heading), h_txt);
    }
    if (self->row_res) {
        g_autofree gchar *sub = g_strdup_printf("%s • %s Primary Output", self->make_model, self->out_name);
        boxed_row_set_subtitle(self->row_res, sub);
    }

    guint orient_sel = 0;
    if (g_ascii_strcasecmp(current_transform, "90") == 0) orient_sel = 1;
    else if (g_ascii_strcasecmp(current_transform, "180") == 0) orient_sel = 2;
    else if (g_ascii_strcasecmp(current_transform, "270") == 0) orient_sel = 3;
    segmented_group_set_selected(self->seg_orient, orient_sel);

    guint scale_sel = 0;
    if (current_scale >= 1.9) scale_sel = 4;
    else if (current_scale >= 1.65) scale_sel = 3;
    else if (current_scale >= 1.4) scale_sel = 2;
    else if (current_scale >= 1.15) scale_sel = 1;
    segmented_group_set_selected(self->seg_scale, scale_sel);

    if (!use_kajo_comp && vrr_active)
        vrr_idx = 1;

    if (self->seg_vrr) {
        gtk_widget_set_sensitive(self->seg_vrr, self->vrr_avail);
        segmented_group_set_selected(self->seg_vrr, self->vrr_avail ? vrr_idx : 0);
    }
    sync_vrr_row_subtitle(self, self->vrr_avail ? vrr_idx : 0);

    self->saved_mode_idx   = active_mode_idx;
    self->saved_orient_idx = orient_sel;
    self->saved_scale_idx  = scale_sel;
    self->saved_vrr_active = self->vrr_avail && vrr_active;
    self->saved_vrr_idx    = self->vrr_avail ? vrr_idx : 0;

    /* Populate Idle / Screensaver / Backlight / Posture from KajoShellCfg */
    g_autoptr(KajoShellCfgRef) cfg = kajo_config_store_acquire_shell(NULL);

    guint ss_mode_idx = 0;
    if (cfg) {
        if (cfg->screensaver_mode == SCREENSAVER_MODE_DPMS) ss_mode_idx = 1;
        else if (cfg->screensaver_mode == SCREENSAVER_MODE_DISABLED) ss_mode_idx = 2;
    }
    segmented_group_set_selected(self->seg_ss_mode, ss_mode_idx);

    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(self->combo_ss_timeout),
        timeout_sec_to_index(cfg ? cfg->screensaver_timeout_sec : 300,
                             k_ss_timeout_vals, G_N_ELEMENTS(k_ss_timeout_vals), 1));
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(self->combo_dpms_timeout),
        timeout_sec_to_index(cfg ? cfg->dpms_timeout_sec : 600,
                             k_dpms_timeout_vals, G_N_ELEMENTS(k_dpms_timeout_vals), 1));
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(self->combo_lock_timeout),
        timeout_sec_to_index(cfg ? cfg->lock_timeout_sec : 900,
                             k_lock_timeout_vals, G_N_ELEMENTS(k_lock_timeout_vals), 2));
    gtk_switch_set_active(GTK_SWITCH(self->sw_lock_suspend), cfg ? cfg->lock_on_suspend : TRUE);

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (caps && caps->kbd_bl_backend != KBD_BL_NONE && self->kbd_level_ctl) {
        if (caps->kbd_bl_backend == KBD_BL_HP_EC) {
            gint saved_lvl = cfg ? cfg->kbd_backlight_level : 2;
            gtk_switch_set_active(GTK_SWITCH(self->kbd_level_ctl), saved_lvl > 0);
        } else {
            gint live_lvl = shell_kbd_backlight_get_level();
            if (live_lvl < 0)
                live_lvl = cfg ? cfg->kbd_backlight_level : 2;
            segmented_group_set_selected(self->kbd_level_ctl, (guint)CLAMP(live_lvl, 0, 2));
        }
        if (caps->has_kbd_timeout && self->kbd_timeout_ctl) {
            gint live_t = shell_kbd_backlight_get_timeout_sec();
            if (live_t < 0)
                live_t = cfg ? cfg->kbd_backlight_timeout_sec : 0;
            segmented_group_set_selected(
                self->kbd_timeout_ctl,
                timeout_sec_to_index(live_t, k_kbd_timeout_vals, G_N_ELEMENTS(k_kbd_timeout_vals), 0));
        }
    }

    if (caps && (caps->has_hinge_sensor || caps->has_accel_sensor)) {
        if (self->sw_posture_autorotate)
            gtk_switch_set_active(GTK_SWITCH(self->sw_posture_autorotate), cfg ? cfg->posture_auto_rotate : TRUE);
        if (self->sw_posture_disable_kbd)
            gtk_switch_set_active(GTK_SWITCH(self->sw_posture_disable_kbd), cfg ? cfg->posture_disable_kbd_in_tablet : TRUE);
        if (self->sw_posture_disable_touch)
            gtk_switch_set_active(GTK_SWITCH(self->sw_posture_disable_touch), cfg ? cfg->posture_disable_touch_in_laptop : FALSE);
    }

    self->syncing_ui = FALSE;
    update_displays_dynamic_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
on_displays_revert(gpointer user_data)
{
    load_niri_and_config_state(user_data);
}

typedef struct {
    ShellScreensaverMode screensaver_mode;
    gboolean             has_ss_timeout;
    gint                 screensaver_timeout_sec;
    gboolean             has_dpms_timeout;
    gint                 dpms_timeout_sec;
    gboolean             has_lock_timeout;
    gint                 lock_timeout_sec;
    gboolean             lock_on_suspend;
    gboolean             has_kbd_level;
    gint                 kbd_backlight_level;
    gboolean             has_kbd_timeout;
    gint                 kbd_backlight_timeout_sec;
    gboolean             has_posture;
    gboolean             posture_auto_rotate;
    gboolean             posture_disable_kbd_in_tablet;
    gboolean             posture_disable_touch_in_laptop;
} DisplaysSaveCtx;

static void
mutate_displays_shell_cfg(KajoShellCfg *cfg, gpointer user_data)
{
    const DisplaysSaveCtx *ctx = (const DisplaysSaveCtx *)user_data;

    cfg->screensaver_mode = ctx->screensaver_mode;
    if (ctx->has_ss_timeout)
        cfg->screensaver_timeout_sec = ctx->screensaver_timeout_sec;
    if (ctx->has_dpms_timeout)
        cfg->dpms_timeout_sec = ctx->dpms_timeout_sec;
    if (ctx->has_lock_timeout)
        cfg->lock_timeout_sec = ctx->lock_timeout_sec;
    cfg->lock_on_suspend = ctx->lock_on_suspend;

    if (ctx->has_kbd_level)
        cfg->kbd_backlight_level = ctx->kbd_backlight_level;
    if (ctx->has_kbd_timeout)
        cfg->kbd_backlight_timeout_sec = ctx->kbd_backlight_timeout_sec;

    if (ctx->has_posture) {
        cfg->posture_auto_rotate             = ctx->posture_auto_rotate;
        cfg->posture_disable_kbd_in_tablet   = ctx->posture_disable_kbd_in_tablet;
        cfg->posture_disable_touch_in_laptop = ctx->posture_disable_touch_in_laptop;
    }
}

static void
on_displays_save(gpointer user_data)
{
    SettingsPageDisplays *self = user_data;
    if (!self)
        return;

    /* 1. Save 10-outputs.kdl & apply via Niri IPC */
    const gchar *out_name = (self->out_name && self->out_name[0]) ? self->out_name : "eDP-1";
    guint mode_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_res));
    const gchar *res = (mode_sel < self->raw_modes->len)
        ? g_ptr_array_index(self->raw_modes, mode_sel)
        : "auto";

    guint orient_sel = segmented_group_get_selected(self->seg_orient);
    static const char *const k_orients[] = { "normal", "90", "180", "270" };
    const gchar *orient = (orient_sel < G_N_ELEMENTS(k_orients)) ? k_orients[orient_sel] : "normal";

    guint scale_sel = segmented_group_get_selected(self->seg_scale);
    double sc = (scale_sel < G_N_ELEMENTS(k_scale_vals)) ? k_scale_vals[scale_sel] : 1.00;
    guint vrr_sel = (self->vrr_avail && self->seg_vrr) ? segmented_group_get_selected(self->seg_vrr) : 0;
    static const char *const k_vrr_vals[] = { "off", "on", "on-demand" };
    const gchar *vrr_str = (vrr_sel < G_N_ELEMENTS(k_vrr_vals)) ? k_vrr_vals[vrr_sel] : "off";
    gboolean vrr = (vrr_sel > 0);

    gchar scale_buf[32];
    g_ascii_formatd(scale_buf, sizeof(scale_buf), "%.2f", sc);

    /* Save outputs.toml preserving unattached host output entries */
    save_outputs_toml_preserving_others(out_name, self->make_model, res, scale_buf, orient, vrr_str);

    /* Save 10-outputs.kdl for Niri session compatibility */
    g_autofree gchar *niri_dir = g_build_filename(g_get_user_config_dir(), "niri", "config.d", NULL);
    g_mkdir_with_parents(niri_dir, 0755);
    g_autofree gchar *kdl_path = g_build_filename(niri_dir, "10-outputs.kdl", NULL);

    g_autofree gchar *kdl_content = g_strdup_printf(
        "// Managed by kajo-settings — DO NOT EDIT DIRECTLY\n\n"
        "output \"%s\" {\n"
        "    mode \"%s\"\n"
        "    scale %s\n"
        "    transform \"%s\"\n"
        "    position x=0 y=0\n"
        "%s"
        "}\n",
        out_name, res, scale_buf, orient,
        vrr ? "    variable-refresh-rate\n" : "");
    g_file_set_contents(kdl_path, kdl_content, -1, NULL);

    if (shell_compositor_is_kajo_comp()) {
        const char *kc_mode = (g_strcmp0(res, "auto") == 0) ? "preferred" : res;
        g_autofree gchar *req = g_strdup_printf(
            "{\"cmd\":\"action\",\"name\":\"set-output-config\",\"output\":\"%s\","
            "\"mode\":\"%s\",\"scale\":%s,\"transform\":\"%s\",\"vrr\":\"%s\"}",
            out_name, kc_mode, scale_buf, orient, vrr_str);
        g_autofree gchar *resp = kajo_comp_ipc_send_request(req);
    } else {
        run_niri_output_cmd_async(out_name, "mode", res);
        run_niri_output_cmd_async(out_name, "scale", scale_buf);
        run_niri_output_cmd_async(out_name, "transform", orient);
        run_niri_output_cmd_async(out_name, "vrr", vrr ? "on" : "off");
        shell_compositor_reload_config();
    }

    /* 2. Snapshot GTK widget state, persist to ~/.config/kajo/shell.toml, then apply hardware sysfs */
    DisplaysSaveCtx ctx = { 0 };
    guint ss_mode = segmented_group_get_selected(self->seg_ss_mode);
    ctx.screensaver_mode = (ss_mode == 1)
        ? SCREENSAVER_MODE_DPMS
        : ((ss_mode == 2) ? SCREENSAVER_MODE_DISABLED : SCREENSAVER_MODE_OVERLAY);

    guint ss_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_ss_timeout));
    if (ss_sel < G_N_ELEMENTS(k_ss_timeout_vals)) {
        ctx.has_ss_timeout = TRUE;
        ctx.screensaver_timeout_sec = k_ss_timeout_vals[ss_sel];
    }

    guint dpms_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_dpms_timeout));
    if (dpms_sel < G_N_ELEMENTS(k_dpms_timeout_vals)) {
        ctx.has_dpms_timeout = TRUE;
        ctx.dpms_timeout_sec = k_dpms_timeout_vals[dpms_sel];
    }

    guint lock_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_lock_timeout));
    if (lock_sel < G_N_ELEMENTS(k_lock_timeout_vals)) {
        ctx.has_lock_timeout = TRUE;
        ctx.lock_timeout_sec = k_lock_timeout_vals[lock_sel];
    }

    ctx.lock_on_suspend = gtk_switch_get_active(GTK_SWITCH(self->sw_lock_suspend));

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    gboolean hp_ec_wake_on = FALSE;
    if (caps && caps->kbd_bl_backend != KBD_BL_NONE && self->kbd_level_ctl) {
        ctx.has_kbd_level = TRUE;
        if (caps->kbd_bl_backend == KBD_BL_HP_EC) {
            hp_ec_wake_on = gtk_switch_get_active(GTK_SWITCH(self->kbd_level_ctl));
            ctx.kbd_backlight_level = hp_ec_wake_on ? 2 : 0;
        } else {
            ctx.kbd_backlight_level = CLAMP((gint)segmented_group_get_selected(self->kbd_level_ctl), 0, 2);
        }
        if (shell_kbd_backlight_has_timeout() && self->kbd_timeout_ctl) {
            guint t_sel = segmented_group_get_selected(self->kbd_timeout_ctl);
            if (t_sel < G_N_ELEMENTS(k_kbd_timeout_vals)) {
                ctx.has_kbd_timeout = TRUE;
                ctx.kbd_backlight_timeout_sec = k_kbd_timeout_vals[t_sel];
            }
        }
    }

    if (caps && (caps->has_hinge_sensor || caps->has_accel_sensor)) {
        ctx.has_posture                     = TRUE;
        ctx.posture_auto_rotate             = gtk_switch_get_active(GTK_SWITCH(self->sw_posture_autorotate));
        ctx.posture_disable_kbd_in_tablet   = gtk_switch_get_active(GTK_SWITCH(self->sw_posture_disable_kbd));
        ctx.posture_disable_touch_in_laptop = gtk_switch_get_active(GTK_SWITCH(self->sw_posture_disable_touch));
    }

    kajo_config_store_update_shell(NULL, mutate_displays_shell_cfg, &ctx, NULL);

    /* Apply hardware sysfs changes outside the config mutator */
    if (ctx.has_kbd_level && caps) {
        if (caps->kbd_bl_backend == KBD_BL_HP_EC) {
            if (hp_ec_wake_on && shell_kbd_backlight_get_level() <= 0)
                shell_kbd_backlight_set_level(2);
        } else {
            shell_kbd_backlight_set_level(ctx.kbd_backlight_level);
        }
        if (ctx.has_kbd_timeout)
            shell_kbd_backlight_set_timeout_sec(ctx.kbd_backlight_timeout_sec);
    }

    if (ctx.has_posture && caps) {
        gdouble deg = 0.0;
        gboolean is_folded = (shell_hw_caps_read_hinge_angle_deg(caps, &deg) && deg >= 180.0);
        write_inhibit_paths_direct(caps->kbd_inhibit_paths, is_folded && ctx.posture_disable_kbd_in_tablet);
        write_inhibit_paths_direct(caps->touchpad_inhibit_paths, is_folded && ctx.posture_disable_kbd_in_tablet);
        write_inhibit_paths_direct(caps->touchscreen_inhibit_paths, (!is_folded) && ctx.posture_disable_touch_in_laptop);
    }

    settings_send_ipc_command("reload-config");
    update_displays_dynamic_subtitles_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
settings_page_displays_dispose(GObject *object)
{
    SettingsPageDisplays *self = (SettingsPageDisplays *)object;
    g_clear_pointer(&self->out_name, g_free);
    g_clear_pointer(&self->make_model, g_free);
    if (self->raw_modes) {
        g_ptr_array_free(self->raw_modes, TRUE);
        self->raw_modes = NULL;
    }
    if (self->mode_geoms) {
        g_array_free(self->mode_geoms, TRUE);
        self->mode_geoms = NULL;
    }
    G_OBJECT_CLASS(settings_page_displays_parent_class)->dispose(object);
}

static void
settings_page_displays_init(SettingsPageDisplays *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));

    self->raw_modes  = g_ptr_array_new_with_free_func(g_free);
    self->mode_geoms = g_array_new(FALSE, TRUE, sizeof(DisplayModeGeom));
}

static void
settings_page_displays_class_init(SettingsPageDisplaysClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_displays_dispose;
}

GtkWidget *
build_page_displays(void)
{
    SettingsPageDisplays *self = g_object_new(settings_page_displays_get_type(), NULL);
    self->syncing_ui = TRUE;

    /* ─── Card 1: Monitor Output & Scaling ─── */
    self->card_mon = create_section_card(
        SETTINGS_CARD_LEFT, "1. MONITOR OUTPUT & SCALING", "10-outputs.kdl", "accent", NULL);
    GtkWidget *hdr_row = gtk_widget_get_first_child(self->card_mon);
    self->lbl_mon_heading = hdr_row ? gtk_widget_get_first_child(hdr_row) : NULL;

    self->combo_res = gtk_drop_down_new(NULL, NULL);
    settings_make_dropdown_shrinkable(self->combo_res);
    g_signal_connect(self->combo_res, "notify::selected", G_CALLBACK(on_any_dropdown_changed), self);
    self->row_res = create_boxed_row("🖥", "Resolution & Refresh Rate", "Built-in Display", self->combo_res);
    gtk_box_append(GTK_BOX(self->card_mon), self->row_res);

    static const char *const orient_labels[] = { "0°", "90°", "180°", "270°", NULL };
    self->seg_orient = create_segmented_group(orient_labels, 0);
    segmented_group_connect(self->seg_orient, on_any_segment_changed, self);
    gtk_box_append(GTK_BOX(self->card_mon),
                   create_boxed_row("🔄", "Display Transform Orientation",
                                    "Wayland output rotation applied via Niri IPC",
                                    self->seg_orient));

    static const char *const scale_labels[] = { "100%", "125%", "150%", "175%", "200%", NULL };
    self->seg_scale = create_segmented_group(scale_labels, 0);
    segmented_group_connect(self->seg_scale, on_any_segment_changed, self);
    self->row_scale = create_boxed_row("🔍", "Fractional UI Scale",
                                       "100% (1.00x) logical workspace",
                                       self->seg_scale);
    gtk_box_append(GTK_BOX(self->card_mon), self->row_scale);

    static const char *const vrr_labels[] = { "OFF", "ON", "ON-DEMAND", NULL };
    self->seg_vrr = create_segmented_group(vrr_labels, 0);
    segmented_group_connect(self->seg_vrr, on_vrr_segment_changed, self);
    self->row_vrr = create_boxed_row("⚡", "Adaptive Sync Policy (VRR)",
                                     "Unsupported by this output",
                                     self->seg_vrr);
    gtk_box_append(GTK_BOX(self->card_mon), self->row_vrr);

    /* ─── Card 2: Idle Screensaver, DPMS & Session Lock ─── */
    GtkWidget *btn_lock_now = create_action_button("🔒 LOCK NOW", NULL, NULL, G_CALLBACK(on_lock_now_clicked), self);
    gtk_widget_add_css_class(btn_lock_now, "files-nav-btn");
    GtkWidget *card_idle = create_section_card(
        SETTINGS_CARD_CENTER, "2. IDLE SCREENSAVER, DPMS & SESSION LOCK",
        NULL, NULL, btn_lock_now);

    static const char *const ss_mode_labels[] = { "OVERLAY", "DPMS", "OFF", NULL };
    self->seg_ss_mode = create_segmented_group(ss_mode_labels, 0);
    segmented_group_connect(self->seg_ss_mode, on_any_segment_changed, self);
    gtk_box_append(GTK_BOX(card_idle),
                   create_boxed_row("🌙", "Idle Screensaver Mode",
                                    "OLED black clock overlay vs immediate DPMS off",
                                    self->seg_ss_mode));

    static const char *const ss_timeout_items[] = {
        "1 Minute", "5 Minutes", "10 Minutes", "15 Minutes", "30 Minutes", "Never", NULL
    };
    self->combo_ss_timeout = create_string_dropdown(ss_timeout_items);
    g_signal_connect(self->combo_ss_timeout, "notify::selected", G_CALLBACK(on_any_dropdown_changed), self);
    self->row_ss_timeout = create_boxed_row("⏱", "Screensaver Idle Timeout",
                                            "Show clock overlay after user inactivity",
                                            self->combo_ss_timeout);
    gtk_box_append(GTK_BOX(card_idle), self->row_ss_timeout);

    static const char *const dpms_timeout_items[] = {
        "5 Minutes", "10 Minutes", "15 Minutes", "30 Minutes", "Never", NULL
    };
    self->combo_dpms_timeout = create_string_dropdown(dpms_timeout_items);
    g_signal_connect(self->combo_dpms_timeout, "notify::selected", G_CALLBACK(on_any_dropdown_changed), self);
    gtk_box_append(GTK_BOX(card_idle),
                   create_boxed_row("🖥", "Display DPMS Power-Off",
                                    "Turn off display panel power when idle",
                                    self->combo_dpms_timeout));

    static const char *const lock_timeout_items[] = {
        "5 Minutes", "10 Minutes", "15 Minutes", "30 Minutes", "Never", NULL
    };
    self->combo_lock_timeout = create_string_dropdown(lock_timeout_items);
    g_signal_connect(self->combo_lock_timeout, "notify::selected", G_CALLBACK(on_any_dropdown_changed), self);
    gtk_box_append(GTK_BOX(card_idle),
                   create_boxed_row("🔒", "Automatic PAM Session Lock",
                                    "ext-session-lock-v1 hardware surface lock",
                                    self->combo_lock_timeout));

    self->sw_lock_suspend = gtk_switch_new();
    g_signal_connect(self->sw_lock_suspend, "notify::active", G_CALLBACK(on_any_switch_changed), self);
    gtk_box_append(GTK_BOX(card_idle),
                   create_boxed_row("💤", "Lock Session Before System Suspend",
                                    "Acquire logind sleep delay inhibitor before S3/s2idle",
                                    self->sw_lock_suspend));

    settings_bento_add_pair(GTK_WIDGET(self), self->card_mon, card_idle);

    /* ─── Card 3 (Hardware-Gated): Keyboard Backlight & Timeout ─── */
    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    self->card_kbd = create_section_card(
        SETTINGS_CARD_RIGHT, "3. KEYBOARD BACKLIGHT & TIMEOUT", "SYSFS LEDS", NULL, NULL);

    if (caps && caps->kbd_bl_backend != KBD_BL_NONE) {
        if (caps->kbd_bl_backend == KBD_BL_HP_EC) {
            self->kbd_level_ctl = gtk_switch_new();
            g_signal_connect(self->kbd_level_ctl, "notify::active", G_CALLBACK(on_any_switch_changed), self);
            gtk_box_append(GTK_BOX(self->card_kbd),
                           create_boxed_row("⌨", "Wake Keylights on Startup",
                                            "Hardware EC keylight enable (F4 cycles Low / High)",
                                            self->kbd_level_ctl));
        } else {
            static const char *const kbd_lvl_labels[] = { "OFF", "LOW", "HIGH", NULL };
            self->kbd_level_ctl = create_segmented_group(kbd_lvl_labels, 2);
            segmented_group_connect(self->kbd_level_ctl, on_any_segment_changed, self);
            gtk_box_append(GTK_BOX(self->card_kbd),
                           create_boxed_row("⌨", "Keyboard Backlight Illumination",
                                            "Hardware EC / sysfs LED brightness level",
                                            self->kbd_level_ctl));
        }

        if (caps->has_kbd_timeout) {
            static const char *const kbd_to_labels[] = { "NEVER", "30S", "3M", NULL };
            self->kbd_timeout_ctl = create_segmented_group(kbd_to_labels, 0);
            segmented_group_connect(self->kbd_timeout_ctl, on_any_segment_changed, self);
            gtk_box_append(GTK_BOX(self->card_kbd),
                           create_boxed_row("⏳", "Backlight Auto-Off Timeout",
                                            "Turn off keylights when typing stops",
                                            self->kbd_timeout_ctl));
        }
    } else {
        gtk_widget_set_visible(self->card_kbd, FALSE);
    }

    /* ─── Card 4 (Hardware-Gated): 360° Foldable Posture & IIO Sensors ─── */
    self->card_posture = create_section_card(
        SETTINGS_CARD_LEFT, "4. 360° FOLDABLE POSTURE & IIO SENSORS", "IIO ACTIVE", "success", NULL);

    if (caps && (caps->has_hinge_sensor || caps->has_accel_sensor)) {
        self->sw_posture_autorotate = gtk_switch_new();
        g_signal_connect(self->sw_posture_autorotate, "notify::active", G_CALLBACK(on_any_switch_changed), self);
        GtkWidget *r_rot = create_boxed_row("📐", "Auto-Rotate in Tablet / Tent Mode",
                                            "Use IIO accelerometer when hinge >= 180°",
                                            self->sw_posture_autorotate);
        gtk_widget_set_visible(r_rot, caps->has_accel_sensor);
        gtk_box_append(GTK_BOX(self->card_posture), r_rot);

        self->sw_posture_disable_kbd = gtk_switch_new();
        g_signal_connect(self->sw_posture_disable_kbd, "notify::active", G_CALLBACK(on_any_switch_changed), self);
        GtkWidget *r_kbd = create_boxed_row("🚫", "Inhibit Keyboard & Touchpad in Tablet Mode",
                                            "Suppress accidental rear key presses when folded",
                                            self->sw_posture_disable_kbd);
        gtk_widget_set_visible(r_kbd, caps->has_internal_kbd || caps->has_touchpad);
        gtk_box_append(GTK_BOX(self->card_posture), r_kbd);

        self->sw_posture_disable_touch = gtk_switch_new();
        g_signal_connect(self->sw_posture_disable_touch, "notify::active", G_CALLBACK(on_any_switch_changed), self);
        GtkWidget *r_tch = create_boxed_row("👆", "Disable Touchscreen in Laptop Mode",
                                            "Prevent palm/screen taps while hinge < 180°",
                                            self->sw_posture_disable_touch);
        gtk_widget_set_visible(r_tch, caps->has_touchscreen);
        gtk_box_append(GTK_BOX(self->card_posture), r_tch);
    } else {
        gtk_widget_set_visible(self->card_posture, FALSE);
    }

    if (gtk_widget_get_visible(self->card_kbd) || gtk_widget_get_visible(self->card_posture))
        settings_bento_add_pair(GTK_WIDGET(self), self->card_kbd, self->card_posture);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "▶ PREVIEW SCREENSAVER",
        on_displays_preview_ss,
        on_displays_revert,
        on_displays_save,
        self);

    load_niri_and_config_state(self);
    return GTK_WIDGET(self);
}
