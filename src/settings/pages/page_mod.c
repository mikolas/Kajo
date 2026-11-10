#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gio.h>
#include <string.h>

static const char *const MOD_FILTER_LABELS[] = { "A500", "A1200", "NEAR", "LIN", "SINC", NULL };
static const char *const MOD_LED_LABELS[]    = { "AUTO", "ON", "OFF", NULL };
static const char *const MOD_SID_LABELS[]    = { "AUTO", "6581", "8580", NULL };
static const char *const MOD_END_LABELS[]    = { "ADVANCE", "LOOP", "NEXT MOD", NULL };

typedef struct _SettingsPageMod {
    GtkBox parent_instance;

    GtkWidget    *seg_filter;
    GtkWidget    *seg_led;
    GtkWidget    *scale_stereo;
    GtkWidget    *lbl_stereo_badge;
    GtkWidget    *seg_end_action;

    GtkWidget    *seg_sid_model;
    GtkWidget    *scale_timeout;
    GtkWidget    *lbl_timeout_badge;
    GtkWidget    *sw_auto_extract;

    GtkWidget    *entry_hvsc;
    GtkWidget    *btn_browse_hvsc;
    GCancellable *browse_cancellable;

    /* Saved baseline for dirty-state tracking & REVERT */
    guint         saved_filter_idx;
    guint         saved_led_idx;
    int           saved_stereo_pct;
    guint         saved_end_idx;
    guint         saved_sid_idx;
    int           saved_timeout_sec;
    gboolean      saved_auto_extract;
    gchar        *saved_hvsc_path;

    gboolean      syncing_ui;
} SettingsPageMod;

typedef struct _SettingsPageModClass {
    GtkBoxClass parent_class;
} SettingsPageModClass;

G_DEFINE_TYPE(SettingsPageMod, settings_page_mod, GTK_TYPE_BOX)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SettingsPageMod, g_object_unref)

static gboolean
mod_page_compute_dirty(SettingsPageMod *self)
{
    if (!self)
        return FALSE;

    guint flt_idx = segmented_group_get_selected(self->seg_filter);
    guint led_idx = segmented_group_get_selected(self->seg_led);
    int   stereo  = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_stereo)) + 0.5), 0, 100);
    guint end_idx = segmented_group_get_selected(self->seg_end_action);
    guint sid_idx = segmented_group_get_selected(self->seg_sid_model);
    int   timeout = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_timeout)) + 0.5), 60, 600);
    gboolean ext  = gtk_switch_get_active(GTK_SWITCH(self->sw_auto_extract));
    const char *hvsc = gtk_editable_get_text(GTK_EDITABLE(self->entry_hvsc));

    if (flt_idx != self->saved_filter_idx) return TRUE;
    if (led_idx != self->saved_led_idx) return TRUE;
    if (stereo  != self->saved_stereo_pct) return TRUE;
    if (end_idx != self->saved_end_idx) return TRUE;
    if (sid_idx != self->saved_sid_idx) return TRUE;
    if (timeout != self->saved_timeout_sec) return TRUE;
    if (ext     != self->saved_auto_extract) return TRUE;
    if (g_strcmp0(hvsc ? hvsc : "", self->saved_hvsc_path ? self->saved_hvsc_path : "") != 0)
        return TRUE;

    return FALSE;
}

static void
update_mod_telemetry_and_dirty(SettingsPageMod *self)
{
    if (!self || self->syncing_ui)
        return;

    GtkWidget *page = GTK_WIDGET(self);
    guint flt_idx = MIN(segmented_group_get_selected(self->seg_filter), 4u);
    guint led_idx = MIN(segmented_group_get_selected(self->seg_led), 2u);
    int   stereo  = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_stereo)) + 0.5), 0, 100);
    guint sid_idx = MIN(segmented_group_get_selected(self->seg_sid_model), 2u);
    int   timeout = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_timeout)) + 0.5), 60, 600);

    g_autofree gchar *paula_str = g_strdup_printf("%s + LED %s",
                                                  MOD_FILTER_LABELS[flt_idx],
                                                  MOD_LED_LABELS[led_idx]);
    const gchar *sid_str = (sid_idx == 1) ? "MOS 6581" :
                           ((sid_idx == 2) ? "MOS 8580" : "AUTO (PSID)");

    g_autofree gchar *stereo_str = NULL;
    if (stereo == 0)
        stereo_str = g_strdup("0% MONO");
    else if (stereo == 70)
        stereo_str = g_strdup("70% AMIGA");
    else if (stereo == 100)
        stereo_str = g_strdup("100% HARD");
    else
        stereo_str = g_strdup_printf("%d%% SEP", stereo);

    g_autofree gchar *timeout_str = g_strdup_printf("%dS (%02d:%02d)",
                                                    timeout, timeout / 60, timeout % 60);

    settings_page_set_counter(page, 0, "PAULA", paula_str, "accent");
    settings_page_set_counter(page, 1, "SID CHIP", sid_str, (sid_idx == 0) ? NULL : "warn");
    settings_page_set_counter(page, 2, "STEREO", stereo_str, NULL);
    settings_page_set_counter(page, 3, "TIMEOUT", timeout_str, NULL);

    settings_page_set_nav_badge(page, "mod", MOD_FILTER_LABELS[flt_idx], NULL);
    settings_page_set_dirty(page, mod_page_compute_dirty(self));
}

static void
on_mod_segmented_changed(GtkWidget *group G_GNUC_UNUSED,
                         guint      selected G_GNUC_UNUSED,
                         gpointer   user_data)
{
    update_mod_telemetry_and_dirty((SettingsPageMod *)user_data);
}

static void
on_mod_scale_changed(GtkRange *range G_GNUC_UNUSED, gpointer user_data)
{
    update_mod_telemetry_and_dirty((SettingsPageMod *)user_data);
}

static void
on_mod_switch_changed(GObject    *obj G_GNUC_UNUSED,
                      GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer    user_data)
{
    update_mod_telemetry_and_dirty((SettingsPageMod *)user_data);
}

static void
on_mod_entry_changed(GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
    update_mod_telemetry_and_dirty((SettingsPageMod *)user_data);
}

static void
on_hvsc_folder_selected(GObject      *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
    GWeakRef *wr = (GWeakRef *)user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, &err);

    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_weak_ref_clear(wr);
        g_free(wr);
        return;
    }

    g_autoptr(SettingsPageMod) self = g_weak_ref_get(wr);
    g_weak_ref_clear(wr);
    g_free(wr);

    if (!self || !folder)
        return;

    g_autofree gchar *local_path = g_file_get_path(folder);
    if (local_path && local_path[0] != '\0') {
        gtk_editable_set_text(GTK_EDITABLE(self->entry_hvsc), local_path);
        update_mod_telemetry_and_dirty(self);
    }
}

static void
on_browse_hvsc_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageMod *self = (SettingsPageMod *)user_data;
    if (!self)
        return;

    if (self->browse_cancellable) {
        g_cancellable_cancel(self->browse_cancellable);
        g_clear_object(&self->browse_cancellable);
    }
    self->browse_cancellable = g_cancellable_new();

    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select HVSC (C64Music) Root Directory");
    gtk_file_dialog_set_modal(dlg, TRUE);

    const char *cur = gtk_editable_get_text(GTK_EDITABLE(self->entry_hvsc));
    if (cur && cur[0] != '\0' && g_file_test(cur, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GFile) init_dir = g_file_new_for_path(cur);
        gtk_file_dialog_set_initial_folder(dlg, init_dir);
    }

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
    GtkWindow *win = (root && GTK_IS_WINDOW(root)) ? GTK_WINDOW(root) : NULL;

    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, self);
    gtk_file_dialog_select_folder(dlg, win, self->browse_cancellable, on_hvsc_folder_selected, wr);
}

static void
action_reset_mod_defaults(gpointer user_data)
{
    SettingsPageMod *self = (SettingsPageMod *)user_data;
    if (!self)
        return;

    self->syncing_ui = TRUE;
    segmented_group_set_selected(self->seg_filter, 0);     /* a500 */
    segmented_group_set_selected(self->seg_led, 0);        /* auto */
    gtk_range_set_value(GTK_RANGE(self->scale_stereo), 70.0);
    segmented_group_set_selected(self->seg_end_action, 0); /* next-sub */
    segmented_group_set_selected(self->seg_sid_model, 0);  /* auto */
    gtk_range_set_value(GTK_RANGE(self->scale_timeout), 180.0);
    gtk_switch_set_active(GTK_SWITCH(self->sw_auto_extract), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_hvsc), "");
    self->syncing_ui = FALSE;

    update_mod_telemetry_and_dirty(self);
}

static void
action_revert_mod(gpointer user_data)
{
    SettingsPageMod *self = (SettingsPageMod *)user_data;
    if (!self)
        return;

    self->syncing_ui = TRUE;
    segmented_group_set_selected(self->seg_filter, self->saved_filter_idx);
    segmented_group_set_selected(self->seg_led, self->saved_led_idx);
    gtk_range_set_value(GTK_RANGE(self->scale_stereo), (double)self->saved_stereo_pct);
    segmented_group_set_selected(self->seg_end_action, self->saved_end_idx);
    segmented_group_set_selected(self->seg_sid_model, self->saved_sid_idx);
    gtk_range_set_value(GTK_RANGE(self->scale_timeout), (double)self->saved_timeout_sec);
    gtk_switch_set_active(GTK_SWITCH(self->sw_auto_extract), self->saved_auto_extract);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_hvsc),
                          self->saved_hvsc_path ? self->saved_hvsc_path : "");
    self->syncing_ui = FALSE;

    update_mod_telemetry_and_dirty(self);
}

typedef struct {
    guint        flt_idx;
    guint        led_idx;
    int          stereo;
    guint        end_idx;
    guint        sid_idx;
    int          timeout;
    gboolean     ext;
    const gchar *hvsc;
} SaveModCtx;

static void
mutate_save_mod(KajoModCfg *cfg, gpointer user_data)
{
    SaveModCtx *ctx = (SaveModCtx *)user_data;
    cfg->filter_mode           = (KajoModFilterMode)ctx->flt_idx;
    cfg->led_mode              = (KajoModLedMode)ctx->led_idx;
    cfg->stereo_sep_pct        = ctx->stereo;
    cfg->end_action            = (KajoModEndAction)ctx->end_idx;
    cfg->sid_model             = (KajoModSidModel)ctx->sid_idx;
    cfg->uade_timeout_sec      = ctx->timeout;
    cfg->auto_extract_archives = ctx->ext;
    g_free(cfg->hvsc_path);
    cfg->hvsc_path = g_strdup(ctx->hvsc ? ctx->hvsc : "");
}

static void
action_save_mod(gpointer user_data)
{
    SettingsPageMod *self = (SettingsPageMod *)user_data;
    if (!self || self->syncing_ui)
        return;

    guint flt_idx = MIN(segmented_group_get_selected(self->seg_filter), 4u);
    guint led_idx = MIN(segmented_group_get_selected(self->seg_led), 2u);
    int   stereo  = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_stereo)) + 0.5), 0, 100);
    guint end_idx = MIN(segmented_group_get_selected(self->seg_end_action), 2u);
    guint sid_idx = MIN(segmented_group_get_selected(self->seg_sid_model), 2u);
    int   timeout = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_timeout)) + 0.5), 60, 600);
    gboolean ext  = gtk_switch_get_active(GTK_SWITCH(self->sw_auto_extract));

    g_autofree gchar *hvsc_trimmed = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_hvsc)));
    if (hvsc_trimmed)
        g_strstrip(hvsc_trimmed);

    SaveModCtx ctx = {
        flt_idx,
        led_idx,
        stereo,
        end_idx,
        sid_idx,
        timeout,
        ext,
        hvsc_trimmed ? hvsc_trimmed : "",
    };
    kajo_config_store_update_mod(NULL, mutate_save_mod, &ctx, NULL);

    self->saved_filter_idx   = flt_idx;
    self->saved_led_idx      = led_idx;
    self->saved_stereo_pct   = stereo;
    self->saved_end_idx      = end_idx;
    self->saved_sid_idx      = sid_idx;
    self->saved_timeout_sec  = timeout;
    self->saved_auto_extract = ext;
    g_free(self->saved_hvsc_path);
    self->saved_hvsc_path = g_strdup(hvsc_trimmed ? hvsc_trimmed : "");

    self->syncing_ui = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(self->entry_hvsc), self->saved_hvsc_path);
    self->syncing_ui = FALSE;

    settings_send_ipc_command("reload-config");
    update_mod_telemetry_and_dirty(self);
}

static GtkWidget *
create_routing_priority_row(const char *prio_glyph,
                            const char *title,
                            const char *subtitle,
                            const char *core_badge,
                            const char *core_badge_style,
                            gboolean    highlight_primary)
{
    GtkWidget *badges_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(badges_box, GTK_ALIGN_CENTER);

    GtkWidget *b_core = gtk_label_new(core_badge);
    gtk_widget_add_css_class(b_core, "shell-popover-badge");
    if (core_badge_style && core_badge_style[0] != '\0')
        gtk_widget_add_css_class(b_core, core_badge_style);
    gtk_widget_set_valign(b_core, GTK_ALIGN_CENTER);

    GtkWidget *b_ready = gtk_label_new("READY");
    gtk_widget_add_css_class(b_ready, "shell-popover-badge");
    gtk_widget_add_css_class(b_ready, "success");
    gtk_widget_set_valign(b_ready, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(badges_box), b_core);
    gtk_box_append(GTK_BOX(badges_box), b_ready);

    GtkWidget *row = create_boxed_row(prio_glyph, title, subtitle, badges_box);
    if (highlight_primary)
        gtk_widget_add_css_class(row, "active");
    return row;
}

static void
on_mod_page_mapped(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    update_mod_telemetry_and_dirty((SettingsPageMod *)widget);
}

static void
settings_page_mod_dispose(GObject *object)
{
    SettingsPageMod *self = (SettingsPageMod *)object;
    if (self->browse_cancellable) {
        g_cancellable_cancel(self->browse_cancellable);
        g_clear_object(&self->browse_cancellable);
    }
    g_clear_pointer(&self->saved_hvsc_path, g_free);
    G_OBJECT_CLASS(settings_page_mod_parent_class)->dispose(object);
}

static void
settings_page_mod_init(SettingsPageMod *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_mod_class_init(SettingsPageModClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_mod_dispose;
}

GtkWidget *
build_page_mod(void)
{
    SettingsPageMod *self = g_object_new(settings_page_mod_get_type(), NULL);
    self->syncing_ui = TRUE;

    g_autoptr(KajoModCfgRef) cfg = kajo_config_store_acquire_mod(NULL);
    guint flt_idx = cfg ? MIN((guint)cfg->filter_mode, 4u) : 0u;
    guint led_idx = cfg ? MIN((guint)cfg->led_mode,    2u) : 0u;
    int   stereo  = cfg ? CLAMP(cfg->stereo_sep_pct, 0, 100) : 70;
    guint end_idx = cfg ? MIN((guint)cfg->end_action,  2u) : 0u;
    guint sid_idx = cfg ? MIN((guint)cfg->sid_model,   2u) : 0u;
    int   timeout = cfg ? CLAMP(cfg->uade_timeout_sec, 60, 600) : 180;
    gboolean ext  = cfg ? cfg->auto_extract_archives : TRUE;
    gchar *hvsc   = g_strdup((cfg && cfg->hvsc_path) ? cfg->hvsc_path : "");

    self->saved_filter_idx   = flt_idx;
    self->saved_led_idx      = led_idx;
    self->saved_stereo_pct   = stereo;
    self->saved_end_idx      = end_idx;
    self->saved_sid_idx      = sid_idx;
    self->saved_timeout_sec  = timeout;
    self->saved_auto_extract = ext;
    self->saved_hvsc_path    = hvsc;

    /* ─── Card 1: Amiga Paula (libuade) & Tracker DSP Defaults ─── */
    GtkWidget *card_paula = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. AMIGA PAULA & TRACKER DSP",
        "PAULA 8364",
        "accent",
        NULL);

    self->seg_filter = create_segmented_group(MOD_FILTER_LABELS, flt_idx);
    segmented_group_connect(self->seg_filter, on_mod_segmented_changed, self);
    gtk_box_append(GTK_BOX(card_paula), create_boxed_row(
        "🎛",
        "Paula Filter",
        "Startup analog RC model (UADE) & interpolator (libopenmpt)",
        self->seg_filter));

    self->seg_led = create_segmented_group(MOD_LED_LABELS, led_idx);
    segmented_group_connect(self->seg_led, on_mod_segmented_changed, self);
    gtk_box_append(GTK_BOX(card_paula), create_boxed_row(
        "💡",
        "Amiga Power LED Low-Pass Filter (3.3 kHz)",
        "AUTO follows Eagleplayer $BFE001 bit 1; ON forces RC low-pass",
        self->seg_led));

    GtkWidget *stereo_wrap = create_scale_with_badge(
        0.0, 100.0, 5.0, (double)stereo, "%.0f%%",
        &self->scale_stereo, &self->lbl_stereo_badge);
    if (self->lbl_stereo_badge)
        gtk_widget_add_css_class(self->lbl_stereo_badge, "accent");
    g_signal_connect(self->scale_stereo, "value-changed", G_CALLBACK(on_mod_scale_changed), self);
    gtk_box_append(GTK_BOX(card_paula), create_boxed_row(
        "🎧",
        "Default Stereo Channel Separation",
        "0% = Mono, 70% = Classic Amiga L/R crossfeed, 100% = Hard pan",
        stereo_wrap));

    self->seg_end_action = create_segmented_group(MOD_END_LABELS, end_idx);
    segmented_group_connect(self->seg_end_action, on_mod_segmented_changed, self);
    gtk_box_append(GTK_BOX(card_paula), create_boxed_row(
        "🔁",
        "Default Track & Subsong End Action",
        "Behavior when a subsong or module reaches its end boundary",
        self->seg_end_action));

    /* ─── Card 2: C64 SID (reSIDfp), Timing & Module Archives ─── */
    GtkWidget *card_sid = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. C64 SID & ARCHIVES",
        "MOS 6581 / 8580",
        "peak-warn",
        NULL);

    self->seg_sid_model = create_segmented_group(MOD_SID_LABELS, sid_idx);
    segmented_group_connect(self->seg_sid_model, on_mod_segmented_changed, self);
    gtk_box_append(GTK_BOX(card_sid), create_boxed_row(
        "🕹",
        "Default MOS SID Chip Model",
        "AUTO uses PSID/RSID v2+ flags; 6581 warm curve vs 8580 crisp",
        self->seg_sid_model));

    GtkWidget *timeout_wrap = create_scale_with_badge(
        60.0, 600.0, 15.0, (double)timeout, "%.0fS",
        &self->scale_timeout, &self->lbl_timeout_badge);
    g_signal_connect(self->scale_timeout, "value-changed", G_CALLBACK(on_mod_scale_changed), self);
    gtk_box_append(GTK_BOX(card_sid), create_boxed_row(
        "⌛",
        "Endless Chiptune & Subsong Timeout",
        "Fallback length when HVSC Songlengths.md5 or tag is absent",
        timeout_wrap));

    self->sw_auto_extract = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(self->sw_auto_extract), ext);
    gtk_widget_set_valign(self->sw_auto_extract, GTK_ALIGN_CENTER);
    g_signal_connect(self->sw_auto_extract, "notify::active", G_CALLBACK(on_mod_switch_changed), self);
    gtk_box_append(GTK_BOX(card_sid), create_boxed_row(
        "📦",
        "Auto-Extract Module Archives",
        "Unpack .lha, .lzx, .zip, .7z with companion smpl.* & .NFO files",
        self->sw_auto_extract));

    settings_bento_add_pair(GTK_WIDGET(self), card_paula, card_sid);

    /* ─── Card 3: HVSC Root Directory & 4-Tier Engine Routing Matrix ─── */
    GtkWidget *card_matrix = create_section_card(
        SETTINGS_CARD_RIGHT,
        "3. HVSC (C64MUSIC) DATABASE PATH & 4-TIER ENGINE ROUTING MATRIX",
        "4 BACKENDS ACTIVE",
        "success",
        NULL);

    GtkWidget *hvsc_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(hvsc_box, GTK_ALIGN_CENTER);

    self->entry_hvsc = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_hvsc), 8);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_hvsc), 26);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_hvsc), "~/C64Music or $HVSC_BASE");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_hvsc), self->saved_hvsc_path);
    gtk_widget_set_hexpand(self->entry_hvsc, TRUE);
    gtk_widget_set_valign(self->entry_hvsc, GTK_ALIGN_CENTER);
    g_signal_connect(self->entry_hvsc, "changed", G_CALLBACK(on_mod_entry_changed), self);

    self->btn_browse_hvsc = gtk_button_new_with_label("📂 BROWSE");
    gtk_widget_add_css_class(self->btn_browse_hvsc, "files-nav-btn");
    gtk_widget_set_valign(self->btn_browse_hvsc, GTK_ALIGN_CENTER);
    g_signal_connect(self->btn_browse_hvsc, "clicked", G_CALLBACK(on_browse_hvsc_clicked), self);

    gtk_box_append(GTK_BOX(hvsc_box), self->entry_hvsc);
    gtk_box_append(GTK_BOX(hvsc_box), self->btn_browse_hvsc);

    gtk_box_append(GTK_BOX(card_matrix), create_boxed_row(
        "📚",
        "High Voltage SID Collection (HVSC) Root Directory",
        "Resolves DOCUMENTS/STIL.txt liner notes & Songlengths.md5 timings",
        hvsc_box));

    gtk_box_append(GTK_BOX(card_matrix), create_routing_priority_row(
        "1",
        "Priority 1 — Amiga 68000 CPU + Paula BLEP Emulator (libuade)",
        "4-ch Amiga .mod, TFMX (mdat/smpl), AHX/HVL, Hippel, OctaMED, Oktalyzer & 200+ Eagleplayers",
        "UADE CORE",
        "accent",
        TRUE));

    gtk_box_append(GTK_BOX(card_matrix), create_routing_priority_row(
        "2",
        "Priority 2 — C64 Cycle-Exact MOS 6581 / 8580 Filter (libsidplayfp + reSIDfp)",
        "PSID / RSID v1–v4 (.sid, .psid, .rsid, .mus) with CIA/VBI multi-subsong stepping",
        "RESIDFP",
        "peak-warn",
        FALSE));

    gtk_box_append(GTK_BOX(card_matrix), create_routing_priority_row(
        "3",
        "Priority 3 — Console Chiptune & FM/PSG Emulators (libgme)",
        "SNES SPC700 (.spc), Mega Drive (.vgm/.vgz), NES (.nsf/.nsfe), Game Boy (.gbs), ZX/CPC (.ay)",
        "LIBGME",
        NULL,
        FALSE));

    gtk_box_append(GTK_BOX(card_matrix), create_routing_priority_row(
        "4",
        "Priority 4 — PC Multi-Channel Tracker Fallback (libopenmpt + libxmp)",
        "FastTracker II (.xm), ScreamTracker 3 (.s3m), Impulse Tracker (.it), OpenMPT (.mptm) & 6–64ch .mod",
        "OPENMPT",
        NULL,
        FALSE));

    settings_bento_add_full(GTK_WIDGET(self), card_matrix);

    self->syncing_ui = FALSE;

    GtkWidget *root = GTK_WIDGET(self);
    settings_page_bind_actions(root,
                               "↺ RESET DSP DEFAULTS",
                               action_reset_mod_defaults,
                               action_revert_mod,
                               action_save_mod,
                               self);
    g_signal_connect(root, "map", G_CALLBACK(on_mod_page_mapped), NULL);
    update_mod_telemetry_and_dirty(self);

    return root;
}
