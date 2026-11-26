#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../amiga/amiga_project.h"
#include <gio/gio.h>
#include <string.h>

static const char *const AMIGA_PROFILES[]     = { "a500_ocs", "a500p_ecs", "a1200_aga", "a1200_fast", NULL };
static const char *const AMIGA_VIDEO_LABELS[] = { "PAL", "NTSC", NULL };
static const char *const AMIGA_BOOT_LABELS[]  = { "DH0:", "ADF", NULL };
static const char *const AMIGA_TC_LABELS[]    = { "ELF2HUNK", "VLINK", "SINGLE ASM", NULL };
static const char *const AMIGA_ENTRY_LABELS[] = { "ENTRY BREAK", "RUN TO BP", NULL };
static const char *const AMIGA_HEX_LABELS[]   = { "8B", "16B", "32B", NULL };

typedef struct _SettingsPageAmiga {
    GtkBox parent_instance;

    /* Card 1: Emulator, Profiles & ROMs */
    GtkWidget *entry_amiberry;
    GtkWidget *dd_profile;
    GtkWidget *seg_video;
    GtkWidget *seg_boot;
    GtkWidget *entry_kick500;
    GtkWidget *entry_kick1200;
    GtkWidget *entry_gdb_host;
    GtkWidget *spin_gdb_port;
    GtkWidget *sw_adf_uaeboard;

    /* Card 2: Toolchain, adflib & HRM Docs */
    GtkWidget *seg_toolchain;
    GtkWidget *entry_extra_paths;
    GtkWidget *entry_hrm_dir;
    GtkWidget *probe_badges[7];

    /* Card 3: Editor & Disassembly */
    GtkWidget *card_inherit_ide;
    GtkWidget *sw_cycle_gutter;
    GtkWidget *sw_sel_cycles;
    GtkWidget *sw_hover_decoders;
    GtkWidget *sw_m68k_lsp;
    GtkWidget *sw_clangd;

    /* Card 4: Debugger & Inspector Defaults */
    GtkWidget *seg_entry_break;
    GtkWidget *seg_hex_group;
    GtkWidget *card_trace_blits;
    GtkWidget *sw_guest_output;
    GtkWidget *sw_trace_history;
    GtkWidget *sw_bpl_grid;
    GtkWidget *sw_insp_visible;

    GCancellable *browse_cancellable;
    KajoAmigaCfg *saved;
    gboolean      syncing_ui;
} SettingsPageAmiga;

typedef struct _SettingsPageAmigaClass {
    GtkBoxClass parent_class;
} SettingsPageAmigaClass;

G_DEFINE_TYPE(SettingsPageAmiga, settings_page_amiga, GTK_TYPE_BOX)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SettingsPageAmiga, g_object_unref)

static guint
find_profile_idx(const char *id)
{
    for (guint i = 0; AMIGA_PROFILES[i] != NULL; i++) {
        if (g_strcmp0(AMIGA_PROFILES[i], id) == 0)
            return i;
    }
    return 0;
}

static void
read_ui_into_cfg(SettingsPageAmiga *self, KajoAmigaCfg *out)
{
    g_free(out->amiberry_bin);
    out->amiberry_bin = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_amiberry)));
    guint pidx = MIN(gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_profile)), 3u);
    g_free(out->default_profile);
    out->default_profile = g_strdup(AMIGA_PROFILES[pidx]);
    out->default_video = (KajoAmigaVideoMode)MIN(segmented_group_get_selected(self->seg_video), 1u);
    out->default_boot_mode = (KajoAmigaBootMode)MIN(segmented_group_get_selected(self->seg_boot), 1u);
    g_free(out->kickstart_a500);
    out->kickstart_a500 = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_kick500)));
    g_free(out->kickstart_a1200);
    out->kickstart_a1200 = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_kick1200)));
    g_free(out->gdb_host);
    out->gdb_host = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_gdb_host)));
    out->gdb_port = CLAMP(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->spin_gdb_port)), 1024, 65535);
    out->adf_uaeboard_guest_output = gtk_switch_get_active(GTK_SWITCH(self->sw_adf_uaeboard));

    out->default_toolchain = (KajoAmigaToolchain)MIN(segmented_group_get_selected(self->seg_toolchain), 2u);
    g_free(out->extra_bin_paths);
    out->extra_bin_paths = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_extra_paths)));
    g_free(out->hrm_dir);
    out->hrm_dir = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_hrm_dir)));

    out->inherit_ide_editor_prefs = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->card_inherit_ide));
    out->show_cycle_gutter = gtk_switch_get_active(GTK_SWITCH(self->sw_cycle_gutter));
    out->show_selection_cycle_sum = gtk_switch_get_active(GTK_SWITCH(self->sw_sel_cycles));
    out->enable_hover_decoders = gtk_switch_get_active(GTK_SWITCH(self->sw_hover_decoders));
    out->enable_m68k_lsp = gtk_switch_get_active(GTK_SWITCH(self->sw_m68k_lsp));
    out->enable_clangd = gtk_switch_get_active(GTK_SWITCH(self->sw_clangd));

    out->entry_break_mode = (KajoAmigaEntryBreak)MIN(segmented_group_get_selected(self->seg_entry_break), 1u);
    out->default_hex_grouping = (KajoAmigaHexGroup)MIN(segmented_group_get_selected(self->seg_hex_group), 2u);
    out->trace_blitter_writes = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->card_trace_blits));
    out->capture_guest_output = gtk_switch_get_active(GTK_SWITCH(self->sw_guest_output));
    out->enable_trace_history = gtk_switch_get_active(GTK_SWITCH(self->sw_trace_history));
    out->bitplane_grid_16px = gtk_switch_get_active(GTK_SWITCH(self->sw_bpl_grid));
    out->inspector_visible = gtk_switch_get_active(GTK_SWITCH(self->sw_insp_visible));
}

static void
refresh_toolchain_probes(SettingsPageAmiga *self, const char *extra_paths_csv)
{
    g_auto(GStrv) paths = (extra_paths_csv && extra_paths_csv[0])
                              ? g_strsplit(extra_paths_csv, ":", -1)
                              : NULL;
    AmigaToolchainStatus *tc = amiga_project_probe_toolchain(
        paths ? (const char *const *)paths : NULL);

    const char *vals[7] = {
        tc->amiberry,
        tc->adflib_version,
        tc->vasmm68k_mot,
        tc->m68k_elf_gcc,
        tc->elf2hunk ? tc->elf2hunk : tc->vlink,
        tc->m68k_lsp_server,
        tc->clangd,
    };
    for (int i = 0; i < 7; i++) {
        GtkWidget *lbl = self->probe_badges[i];
        if (!lbl) continue;
        gtk_widget_remove_css_class(lbl, "success");
        gtk_widget_remove_css_class(lbl, "warn");
        if (vals[i] && vals[i][0]) {
            gtk_label_set_text(GTK_LABEL(lbl), vals[i]);
            gtk_widget_add_css_class(lbl, "success");
        } else {
            gtk_label_set_text(GTK_LABEL(lbl), "MISSING");
            gtk_widget_add_css_class(lbl, "warn");
        }
    }

    settings_page_set_counter(
        GTK_WIDGET(self), 2, "ADFLIB",
        (tc->adflib_version && tc->adflib_version[0]) ? tc->adflib_version : "MISSING",
        (tc->adflib_version && tc->adflib_version[0]) ? "success" : "warn");

    amiga_toolchain_status_free(tc);
}

static void
update_amiga_telemetry_and_dirty(SettingsPageAmiga *self)
{
    if (!self || self->syncing_ui)
        return;

    g_autoptr(KajoAmigaCfg) cur = kajo_amiga_cfg_new_default();
    read_ui_into_cfg(self, cur);

    const KajoDomainSchema *s = kajo_schema_get_amiga();
    gboolean dirty = self->saved ? !kajo_cfg_domain_equal(s, cur, self->saved) : FALSE;

    g_autofree char *prof_up = g_ascii_strup(cur->default_profile ? cur->default_profile : "a500_ocs", -1);
    g_autofree char *tc_up = g_ascii_strup(kajo_amiga_toolchain_to_string(cur->default_toolchain), -1);
    g_autofree char *gdb_str = g_strdup_printf("%s:%d",
                                               cur->gdb_host ? cur->gdb_host : "127.0.0.1",
                                               cur->gdb_port);

    GtkWidget *page = GTK_WIDGET(self);
    settings_page_set_counter(page, 0, "PROFILE", prof_up, "accent");
    settings_page_set_counter(page, 1, "TOOLCHAIN", tc_up, NULL);
    settings_page_set_counter(page, 3, "GDB RSP", gdb_str, NULL);
    settings_page_set_nav_badge(page, "amiga", "68K", NULL);
    settings_page_set_dirty(page, dirty);
}

static void
apply_cfg_to_ui(SettingsPageAmiga *self, const KajoAmigaCfg *cfg)
{
    self->syncing_ui = TRUE;

    gtk_editable_set_text(GTK_EDITABLE(self->entry_amiberry), cfg->amiberry_bin ? cfg->amiberry_bin : "amiberry");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_profile), find_profile_idx(cfg->default_profile));
    segmented_group_set_selected(self->seg_video, (guint)cfg->default_video);
    segmented_group_set_selected(self->seg_boot, (guint)cfg->default_boot_mode);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_kick500), cfg->kickstart_a500 ? cfg->kickstart_a500 : "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_kick1200), cfg->kickstart_a1200 ? cfg->kickstart_a1200 : "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_gdb_host), cfg->gdb_host ? cfg->gdb_host : "127.0.0.1");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->spin_gdb_port), (double)cfg->gdb_port);
    gtk_switch_set_active(GTK_SWITCH(self->sw_adf_uaeboard), cfg->adf_uaeboard_guest_output);

    segmented_group_set_selected(self->seg_toolchain, (guint)cfg->default_toolchain);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_extra_paths), cfg->extra_bin_paths ? cfg->extra_bin_paths : "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_hrm_dir), cfg->hrm_dir ? cfg->hrm_dir : "");

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->card_inherit_ide), cfg->inherit_ide_editor_prefs);
    gtk_switch_set_active(GTK_SWITCH(self->sw_cycle_gutter), cfg->show_cycle_gutter);
    gtk_switch_set_active(GTK_SWITCH(self->sw_sel_cycles), cfg->show_selection_cycle_sum);
    gtk_switch_set_active(GTK_SWITCH(self->sw_hover_decoders), cfg->enable_hover_decoders);
    gtk_switch_set_active(GTK_SWITCH(self->sw_m68k_lsp), cfg->enable_m68k_lsp);
    gtk_switch_set_active(GTK_SWITCH(self->sw_clangd), cfg->enable_clangd);

    segmented_group_set_selected(self->seg_entry_break, (guint)cfg->entry_break_mode);
    segmented_group_set_selected(self->seg_hex_group, (guint)cfg->default_hex_grouping);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->card_trace_blits), cfg->trace_blitter_writes);
    gtk_switch_set_active(GTK_SWITCH(self->sw_guest_output), cfg->capture_guest_output);
    gtk_switch_set_active(GTK_SWITCH(self->sw_trace_history), cfg->enable_trace_history);
    gtk_switch_set_active(GTK_SWITCH(self->sw_bpl_grid), cfg->bitplane_grid_16px);
    gtk_switch_set_active(GTK_SWITCH(self->sw_insp_visible), cfg->inspector_visible);

    self->syncing_ui = FALSE;
    refresh_toolchain_probes(self, cfg->extra_bin_paths);
    update_amiga_telemetry_and_dirty(self);
}

static void on_seg_changed(GtkWidget *g G_GNUC_UNUSED, guint s G_GNUC_UNUSED, gpointer ud) {
    update_amiga_telemetry_and_dirty((SettingsPageAmiga *)ud);
}
static void on_dd_changed(GObject *o G_GNUC_UNUSED, GParamSpec *p G_GNUC_UNUSED, gpointer ud) {
    update_amiga_telemetry_and_dirty((SettingsPageAmiga *)ud);
}
static void on_sw_changed(GObject *o G_GNUC_UNUSED, GParamSpec *p G_GNUC_UNUSED, gpointer ud) {
    update_amiga_telemetry_and_dirty((SettingsPageAmiga *)ud);
}
static void on_toggle_card(GtkToggleButton *tb G_GNUC_UNUSED, gpointer ud) {
    update_amiga_telemetry_and_dirty((SettingsPageAmiga *)ud);
}
static void on_entry_changed(GtkEditable *ed G_GNUC_UNUSED, gpointer ud) {
    update_amiga_telemetry_and_dirty((SettingsPageAmiga *)ud);
}
static void on_spin_changed(GtkSpinButton *sp G_GNUC_UNUSED, gpointer ud) {
    update_amiga_telemetry_and_dirty((SettingsPageAmiga *)ud);
}

typedef struct {
    GWeakRef   weak;
    GtkWidget *target_entry;
} BrowseCtx;

static void
on_browse_folder_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    BrowseCtx *ctx = ud;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    g_autoptr(SettingsPageAmiga) self = g_weak_ref_get(&ctx->weak);
    GtkWidget *entry = ctx->target_entry;
    g_weak_ref_clear(&ctx->weak);
    g_free(ctx);
    if (!self || !f || (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
        return;
    g_autofree char *p = g_file_get_path(f);
    if (p) gtk_editable_set_text(GTK_EDITABLE(entry), p);
}

static void
on_browse_file_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    BrowseCtx *ctx = ud;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
    g_autoptr(SettingsPageAmiga) self = g_weak_ref_get(&ctx->weak);
    GtkWidget *entry = ctx->target_entry;
    g_weak_ref_clear(&ctx->weak);
    g_free(ctx);
    if (!self || !f || (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
        return;
    g_autofree char *p = g_file_get_path(f);
    if (p) gtk_editable_set_text(GTK_EDITABLE(entry), p);
}

static void
start_browse(SettingsPageAmiga *self, GtkWidget *entry, const char *title, gboolean folder)
{
    if (self->browse_cancellable) {
        g_cancellable_cancel(self->browse_cancellable);
        g_clear_object(&self->browse_cancellable);
    }
    self->browse_cancellable = g_cancellable_new();
    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_modal(dlg, TRUE);
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkWindow *win = (root && GTK_IS_WINDOW(root)) ? GTK_WINDOW(root) : NULL;
    BrowseCtx *ctx = g_new0(BrowseCtx, 1);
    g_weak_ref_init(&ctx->weak, self);
    ctx->target_entry = entry;
    if (folder)
        gtk_file_dialog_select_folder(dlg, win, self->browse_cancellable, on_browse_folder_done, ctx);
    else
        gtk_file_dialog_open(dlg, win, self->browse_cancellable, on_browse_file_done, ctx);
}

static void on_browse_k500(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
    SettingsPageAmiga *self = (SettingsPageAmiga *)ud;
    start_browse(self, self->entry_kick500, "Select Kickstart 1.3 ROM (A500)", FALSE);
}
static void on_browse_k1200(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
    SettingsPageAmiga *self = (SettingsPageAmiga *)ud;
    start_browse(self, self->entry_kick1200, "Select Kickstart 3.1 ROM (A1200)", FALSE);
}
static void on_browse_hrm(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
    SettingsPageAmiga *self = (SettingsPageAmiga *)ud;
    start_browse(self, self->entry_hrm_dir, "Select Local Amiga HRM Markdown Directory", TRUE);
}

static void
mutate_save_amiga(KajoAmigaCfg *draft, gpointer ud)
{
    kajo_cfg_domain_copy_into(kajo_schema_get_amiga(), draft, (const KajoAmigaCfg *)ud);
}

static void
action_save_amiga(gpointer ud)
{
    SettingsPageAmiga *self = (SettingsPageAmiga *)ud;
    if (!self || self->syncing_ui) return;
    g_autoptr(KajoAmigaCfg) draft = kajo_amiga_cfg_new_default();
    read_ui_into_cfg(self, draft);
    kajo_config_store_update_amiga(NULL, mutate_save_amiga, draft, NULL);

    g_clear_pointer(&self->saved, kajo_amiga_cfg_free);
    self->saved = kajo_amiga_cfg_copy(draft);
    refresh_toolchain_probes(self, draft->extra_bin_paths);
    update_amiga_telemetry_and_dirty(self);
}

static void
action_revert_amiga(gpointer ud)
{
    SettingsPageAmiga *self = (SettingsPageAmiga *)ud;
    if (!self || !self->saved) return;
    apply_cfg_to_ui(self, self->saved);
}

static void
action_reset_amiga_defaults(gpointer ud)
{
    SettingsPageAmiga *self = (SettingsPageAmiga *)ud;
    g_autoptr(KajoAmigaCfg) def = kajo_amiga_cfg_new_default();
    apply_cfg_to_ui(self, def);
}

static GtkWidget *
make_path_row(SettingsPageAmiga *self G_GNUC_UNUSED,
              const char        *glyph,
              const char        *title,
              const char        *subtitle,
              const char        *placeholder,
              GtkWidget        **out_entry,
              GCallback          browse_cb,
              gpointer           ud)
{
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
    GtkWidget *ed = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(ed), 10);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(ed), 24);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed), placeholder);
    gtk_widget_set_hexpand(ed, TRUE);
    gtk_widget_set_valign(ed, GTK_ALIGN_CENTER);
    g_signal_connect(ed, "changed", G_CALLBACK(on_entry_changed), ud);
    gtk_box_append(GTK_BOX(hb), ed);
    if (browse_cb) {
        GtkWidget *btn = gtk_button_new_with_label("📂");
        gtk_widget_add_css_class(btn, "files-nav-btn");
        gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
        g_signal_connect(btn, "clicked", browse_cb, ud);
        gtk_box_append(GTK_BOX(hb), btn);
    }
    *out_entry = ed;
    return create_boxed_row(glyph, title, subtitle, hb);
}

static GtkWidget *
make_switch_row(const char *glyph, const char *title, const char *sub, GtkWidget **out_sw, gpointer ud)
{
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_sw_changed), ud);
    *out_sw = sw;
    return create_boxed_row(glyph, title, sub, sw);
}

static void
on_amiga_page_mapped(GtkWidget *w, gpointer ud G_GNUC_UNUSED)
{
    update_amiga_telemetry_and_dirty((SettingsPageAmiga *)w);
}

static void
settings_page_amiga_dispose(GObject *obj)
{
    SettingsPageAmiga *self = (SettingsPageAmiga *)obj;
    if (self->browse_cancellable) {
        g_cancellable_cancel(self->browse_cancellable);
        g_clear_object(&self->browse_cancellable);
    }
    g_clear_pointer(&self->saved, kajo_amiga_cfg_free);
    G_OBJECT_CLASS(settings_page_amiga_parent_class)->dispose(obj);
}

static void
settings_page_amiga_class_init(SettingsPageAmigaClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = settings_page_amiga_dispose;
}

static void
settings_page_amiga_init(SettingsPageAmiga *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));

    g_autoptr(KajoAmigaCfgRef) live = kajo_config_store_acquire_amiga(NULL);
    self->saved = live ? kajo_amiga_cfg_copy(live) : kajo_amiga_cfg_new_default();

    /* Card 1: Emulator, Profiles & ROMs */
    GtkWidget *c_emu = create_section_card(
        SETTINGS_CARD_LEFT, "1. AMIBERRY, PROFILES & KICKSTART ROMS", "AMIGA.TOML", "accent", NULL);

    gtk_box_append(GTK_BOX(c_emu), make_path_row(
        self, "🕹", "Amiberry Executable (>= 8.5.0)",
        "Binary supporting -s debugging_features=segtracker,gdbserver",
        "amiberry", &self->entry_amiberry, NULL, self));

    self->dd_profile = create_string_dropdown(AMIGA_PROFILES);
    g_signal_connect(self->dd_profile, "notify::selected", G_CALLBACK(on_dd_changed), self);
    gtk_box_append(GTK_BOX(c_emu), create_boxed_row(
        "🖥", "Default Hardware Profile", "Machine preset used for new workspaces and debugger sessions",
        self->dd_profile));

    self->seg_video = create_segmented_group(AMIGA_VIDEO_LABELS, 0);
    segmented_group_connect(self->seg_video, on_seg_changed, self);
    gtk_box_append(GTK_BOX(c_emu), create_boxed_row(
        "📺", "Video Timing Standard", "PAL (312 lines / 50Hz) vs NTSC (262 lines / 60Hz)",
        self->seg_video));

    self->seg_boot = create_segmented_group(AMIGA_BOOT_LABELS, 0);
    segmented_group_connect(self->seg_boot, on_seg_changed, self);
    gtk_box_append(GTK_BOX(c_emu), create_boxed_row(
        "💾", "Default Boot Target Packaging", "Host DH0: staging directory or in-process adflib .adf floppy",
        self->seg_boot));

    gtk_box_append(GTK_BOX(c_emu), make_path_row(
        self, "🔐", "Kickstart 1.3 ROM (A500 OCS)", "Optional explicit ROM image path",
        "/path/to/kick13.rom", &self->entry_kick500, G_CALLBACK(on_browse_k500), self));

    gtk_box_append(GTK_BOX(c_emu), make_path_row(
        self, "🔐", "Kickstart 3.1 ROM (A1200 AGA)", "Optional explicit ROM image path",
        "/path/to/kick31.rom", &self->entry_kick1200, G_CALLBACK(on_browse_k1200), self));

    GtkWidget *gdb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    self->entry_gdb_host = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_gdb_host), 9);
    g_signal_connect(self->entry_gdb_host, "changed", G_CALLBACK(on_entry_changed), self);
    self->spin_gdb_port = gtk_spin_button_new_with_range(1024, 65535, 1);
    g_signal_connect(self->spin_gdb_port, "value-changed", G_CALLBACK(on_spin_changed), self);
    gtk_box_append(GTK_BOX(gdb_box), self->entry_gdb_host);
    gtk_box_append(GTK_BOX(gdb_box), self->spin_gdb_port);
    gtk_box_append(GTK_BOX(c_emu), create_boxed_row(
        "🔌", "GDB RSP Host & Port", "Single-socket TCP endpoint exposed by Amiberry gdbserver",
        gdb_box));

    gtk_box_append(GTK_BOX(c_emu), make_switch_row(
        "💬", "Enable uaeboard on ADF Boots", "Pass -s uaeboard=1 for Trap 86 guest-output on floppy boots",
        &self->sw_adf_uaeboard, self));

    /* Card 2: Toolchain, adflib & HRM Docs */
    GtkWidget *c_tc = create_section_card(
        SETTINGS_CARD_RIGHT, "2. TOOLCHAIN, ADFLIB & HRM DOCS", "LIVE PROBE", "success", NULL);

    self->seg_toolchain = create_segmented_group(AMIGA_TC_LABELS, 0);
    segmented_group_connect(self->seg_toolchain, on_seg_changed, self);
    gtk_box_append(GTK_BOX(c_tc), create_boxed_row(
        "🔨", "Default Link Pipeline", "elf2hunk (DWARF3), hunk_vlink (HUNK_DEBUG), or single_asm",
        self->seg_toolchain));

    gtk_box_append(GTK_BOX(c_tc), make_path_row(
        self, "📂", "Extra Cross-Toolchain Bin Paths", "Colon-separated directories prepended to PATH probes",
        "/opt/amiga/bin", &self->entry_extra_paths, NULL, self));

    gtk_box_append(GTK_BOX(c_tc), make_path_row(
        self, "📘", "Local HRM Markdown Directory", "Chapter files loaded on K lookup ([docs].hrm_dir)",
        "~/docs/amiga-hrm", &self->entry_hrm_dir, G_CALLBACK(on_browse_hrm), self));

    static const char *const k_tools[7] = {
        "amiberry", "adflib", "vasmm68k_mot", "m68k-amiga-elf-gcc", "elf2hunk / vlink", "m68k-lsp-server", "clangd"
    };
    for (int i = 0; i < 7; i++) {
        self->probe_badges[i] = gtk_label_new("...");
        gtk_widget_add_css_class(self->probe_badges[i], "shell-popover-badge");
        gtk_widget_set_valign(self->probe_badges[i], GTK_ALIGN_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(self->probe_badges[i]), PANGO_ELLIPSIZE_START);
        gtk_box_append(GTK_BOX(c_tc), create_boxed_row(
            "✓", k_tools[i], "Live binary / library readiness check", self->probe_badges[i]));
    }

    settings_bento_add_pair(GTK_WIDGET(self), c_emu, c_tc);

    /* Card 3: Editor & Disassembly */
    GtkWidget *c_ed = create_section_card(
        SETTINGS_CARD_LEFT, "3. EDITOR, CYCLE GUTTER & LSP", "IDE CORE", "accent", NULL);

    self->card_inherit_ide = create_toggle_card(
        "🮖", "Inherit Editor Prefs from Kajo IDE (ide.toml)",
        "Shares font family/size, GtkSourceView theme, tab width & modal bindings", TRUE);
    g_signal_connect(self->card_inherit_ide, "toggled", G_CALLBACK(on_toggle_card), self);
    gtk_box_append(GTK_BOX(c_ed), self->card_inherit_ide);

    gtk_box_append(GTK_BOX(c_ed), make_switch_row(
        "⏱", "68000 Line-by-Line Cycle Gutter (Col 7)", "Shows n(r/w) static cycle annotations on assembly lines",
        &self->sw_cycle_gutter, self));
    gtk_box_append(GTK_BOX(c_ed), make_switch_row(
        "∑", "Selection Cycle Sum Badge", "Sums static cycles when 2+ assembly lines are selected",
        &self->sw_sel_cycles, self));
    gtk_box_append(GTK_BOX(c_ed), make_switch_row(
        "💡", "Hover Decoders ($DFFxxx / LVO / RGB12)", "Decode hardware registers, LVOs, and minterms on hover",
        &self->sw_hover_decoders, self));
    gtk_box_append(GTK_BOX(c_ed), make_switch_row(
        "🔤", "Enable m68k-lsp-server", "Language intelligence for .s/.asm source files",
        &self->sw_m68k_lsp, self));
    gtk_box_append(GTK_BOX(c_ed), make_switch_row(
        "⚙", "Enable clangd (via compile_commands.json)", "C17 completion & diagnostics for Amiga C sources",
        &self->sw_clangd, self));

    /* Card 4: Debugger & Inspector Defaults */
    GtkWidget *c_dbg = create_section_card(
        SETTINGS_CARD_RIGHT, "4. DEBUGGER & HARDWARE INSPECTOR", "RSP 8.5+", NULL, NULL);

    self->seg_entry_break = create_segmented_group(AMIGA_ENTRY_LABELS, 0);
    segmented_group_connect(self->seg_entry_break, on_seg_changed, self);
    gtk_box_append(GTK_BOX(c_dbg), create_boxed_row(
        "🎯", "Program Entry Behavior", "Hold at segment+4 entry or continue directly to user breakpoints",
        self->seg_entry_break));

    self->seg_hex_group = create_segmented_group(AMIGA_HEX_LABELS, 1);
    segmented_group_connect(self->seg_hex_group, on_seg_changed, self);
    gtk_box_append(GTK_BOX(c_dbg), create_boxed_row(
        "▓", "Default Memory Hex Grouping", "Initial byte/word/longword formatting in MEMORY HEX drawer",
        self->seg_hex_group));

    self->card_trace_blits = create_toggle_card(
        "👁", "Arm Non-Breaking Blitter Watch (log_only=1)",
        "Off by default — records $DFF040..076 writes without halting guest CPU", FALSE);
    g_signal_connect(self->card_trace_blits, "toggled", G_CALLBACK(on_toggle_card), self);
    gtk_box_append(GTK_BOX(c_dbg), self->card_trace_blits);

    gtk_box_append(GTK_BOX(c_dbg), make_switch_row(
        "📜", "Capture Trap 86 Guest Output", "Poll guest-output ring on debugger stops",
        &self->sw_guest_output, self));
    gtk_box_append(GTK_BOX(c_dbg), make_switch_row(
        "🕒", "Enable 128-Entry Trace History", "Record pre-stop PC/registers via qRcmd,history on",
        &self->sw_trace_history, self));
    gtk_box_append(GTK_BOX(c_dbg), make_switch_row(
        "#", "Show 16px Word Grid in Bitplane Viewer", "Default overlay state for planar/HAM/EHB canvas",
        &self->sw_bpl_grid, self));
    gtk_box_append(GTK_BOX(c_dbg), make_switch_row(
        "🔧", "Show Hardware Inspector Pane", "Right-side Bitplanes, Copper, Chips, DMA & HRM deck",
        &self->sw_insp_visible, self));

    settings_bento_add_pair(GTK_WIDGET(self), c_ed, c_dbg);

    GtkWidget *root = GTK_WIDGET(self);
    settings_page_bind_actions(root,
                               "↺ RESET DEFAULTS",
                               action_reset_amiga_defaults,
                               action_revert_amiga,
                               action_save_amiga,
                               self);
    g_signal_connect(root, "map", G_CALLBACK(on_amiga_page_mapped), NULL);
    apply_cfg_to_ui(self, self->saved);
}

GtkWidget *
build_page_amiga(void)
{
    SettingsPageAmiga *self = g_object_new(settings_page_amiga_get_type(), NULL);
    return GTK_WIDGET(self);
}
