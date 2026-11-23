#include "web_picker.h"
#include "../config/kajo_config_store.h"
#include "../compositor/compositor.h"

typedef struct {
    GtkWindow             *win;
    KajoWebProfileManager *pm;
    KajoWebPickerSelectCb  on_select;
    gpointer               ud;
    GtkWidget             *grid;
    GtkWidget             *add_box;
    GtkWidget             *entry_name;
    GtkWidget             *chk_startup;
} PickerCtx;

static void rebuild_cards(PickerCtx *ctx);

static void
on_card_clicked(GtkButton *btn, gpointer user_data)
{
    PickerCtx *ctx = user_data;
    const gchar *pid = g_object_get_data(G_OBJECT(btn), "profile-id");
    if (ctx->on_select && pid)
        ctx->on_select(pid, FALSE, ctx->ud);
    gtk_window_close(ctx->win);
}

static void
on_guest_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PickerCtx *ctx = user_data;
    if (ctx->on_select)
        ctx->on_select("default", TRUE, ctx->ud);
    gtk_window_close(ctx->win);
}

static void
on_add_confirm_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PickerCtx *ctx = user_data;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(ctx->entry_name));
    if (!name || !*name)
        return;
    static const char *k_accents[] = { "#0078d4", "#10893e", "#d83b01", "#00b7c3", "#b4009e" };
    static const char *k_icons[]   = { "💼", "🏠", "⚙", "🧪", "❖" };
    KajoWebProfileRegistry *reg = web_profile_manager_get_registry(ctx->pm);
    guint idx = reg ? reg->profiles->len : 0;
    web_profile_registry_add(reg, name, k_icons[idx % G_N_ELEMENTS(k_icons)],
                             k_accents[idx % G_N_ELEMENTS(k_accents)]);
    web_profile_manager_save(ctx->pm, NULL);
    gtk_editable_set_text(GTK_EDITABLE(ctx->entry_name), "");
    gtk_widget_set_visible(ctx->add_box, FALSE);
    rebuild_cards(ctx);
}

static void
on_add_toggle(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    PickerCtx *ctx = user_data;
    gboolean show = !gtk_widget_get_visible(ctx->add_box);
    gtk_widget_set_visible(ctx->add_box, show);
    if (show)
        gtk_widget_grab_focus(ctx->entry_name);
}

static void
mutate_show_picker(KajoWebCfg *draft, gpointer ud)
{
    draft->show_picker_on_startup = GPOINTER_TO_INT(ud) != 0;
}

static void
on_startup_toggled(GtkCheckButton *chk, gpointer user_data G_GNUC_UNUSED)
{
    gboolean on = gtk_check_button_get_active(chk);
    kajo_config_store_update_web(NULL, mutate_show_picker, GINT_TO_POINTER(on ? 1 : 0), NULL);
}

static void
on_float_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED)
{
    shell_compositor_toggle_floating();
}

static void
on_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer ud)
{
    gtk_window_close(GTK_WINDOW(ud));
}

static gboolean
on_picker_key(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
              guint                  keyval,
              guint                  keycode G_GNUC_UNUSED,
              GdkModifierType        state G_GNUC_UNUSED,
              gpointer               user_data)
{
    PickerCtx *ctx = user_data;
    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(ctx->win);
        return TRUE;
    }
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9 && !gtk_widget_get_visible(ctx->add_box)) {
        guint idx = keyval - GDK_KEY_1;
        KajoWebProfileRegistry *reg = web_profile_manager_get_registry(ctx->pm);
        if (reg && idx < reg->profiles->len) {
            const KajoWebProfileInfo *p = g_ptr_array_index(reg->profiles, idx);
            if (ctx->on_select)
                ctx->on_select(p->id, FALSE, ctx->ud);
            gtk_window_close(ctx->win);
            return TRUE;
        }
    }
    return FALSE;
}

static void
rebuild_cards(PickerCtx *ctx)
{
    GtkWidget *child = gtk_widget_get_first_child(ctx->grid);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_flow_box_remove(GTK_FLOW_BOX(ctx->grid), child);
        child = next;
    }

    KajoWebProfileRegistry *reg = web_profile_manager_get_registry(ctx->pm);
    if (!reg)
        return;

    for (guint i = 0; i < reg->profiles->len; i++) {
        const KajoWebProfileInfo *p = g_ptr_array_index(reg->profiles, i);
        GtkWidget *btn = gtk_button_new();
        gtk_widget_set_size_request(btn, 224, 134);
        gtk_widget_set_halign(btn, GTK_ALIGN_START);
        gtk_widget_set_valign(btn, GTK_ALIGN_START);
        gtk_widget_add_css_class(btn, "web-picker-card");
        g_object_set_data_full(G_OBJECT(btn), "profile-id", g_strdup(p->id), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_card_clicked), ctx);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(top, GTK_ALIGN_CENTER);
        GtkWidget *ico = gtk_label_new(p->icon ? p->icon : "🌐");
        gtk_widget_set_halign(ico, GTK_ALIGN_START);
        gtk_widget_set_hexpand(ico, TRUE);
        g_autofree gchar *kstr = g_strdup_printf("%u", i + 1);
        GtkWidget *kbadge = gtk_label_new(kstr);
        gtk_widget_add_css_class(kbadge, "shell-popover-badge");
        gtk_box_append(GTK_BOX(top), ico);
        gtk_box_append(GTK_BOX(top), kbadge);

        GtkWidget *nm = gtk_label_new(p->name);
        gtk_widget_set_halign(nm, GTK_ALIGN_START);
        gtk_widget_add_css_class(nm, "web-picker-name");

        GtkWidget *em = gtk_label_new((p->google_email && *p->google_email) ? p->google_email : "No Google Account");
        gtk_widget_set_halign(em, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(em), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(em, "web-dim-label");

        GtkWidget *sy = gtk_label_new(p->sync_enabled ? "☁ DRIVE APPDATA SYNC" : "LOCAL ONLY");
        gtk_widget_set_halign(sy, GTK_ALIGN_START);
        gtk_widget_add_css_class(sy, p->sync_enabled ? "web-badge-telemetry" : "web-dim-label");

        gtk_box_append(GTK_BOX(vbox), top);
        gtk_box_append(GTK_BOX(vbox), nm);
        gtk_box_append(GTK_BOX(vbox), em);
        gtk_box_append(GTK_BOX(vbox), sy);
        gtk_button_set_child(GTK_BUTTON(btn), vbox);
        gtk_flow_box_append(GTK_FLOW_BOX(ctx->grid), btn);
    }
}

GtkWindow *
web_picker_window_new(GtkApplication        *app,
                      KajoWebProfileManager *pm,
                      KajoWebPickerSelectCb  on_select,
                      gpointer               user_data)
{
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 480);
    gtk_window_set_title(GTK_WINDOW(win), "Kajo Web — Select Profile");
    gtk_widget_add_css_class(win, "shell-settings-window");
    gtk_widget_add_css_class(win, "shell-web-window");
    gtk_widget_add_css_class(win, "shell-web-picker");

    PickerCtx *ctx = g_new0(PickerCtx, 1);
    ctx->win = GTK_WINDOW(win);
    ctx->pm = pm;
    ctx->on_select = on_select;
    ctx->ud = user_data;
    g_object_set_data_full(G_OBJECT(win), "picker-ctx", ctx, g_free);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_picker_key), ctx);
    gtk_widget_add_controller(win, kc);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), root);

    /* Aero + Metro Top Drag Bar */
    GtkWidget *handle = gtk_window_handle_new();
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(hdr, "settings-header-bar");

    GtkWidget *title = gtk_label_new("🌐 KAJO WEB // SELECT PROFILE");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(title, "settings-header-title");

    GtkWidget *b_float = gtk_button_new_with_label("🗗 FLOAT / TILE");
    gtk_widget_add_css_class(b_float, "files-nav-btn");
    g_signal_connect(b_float, "clicked", G_CALLBACK(on_float_clicked), NULL);

    GtkWidget *b_close = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(b_close, "settings-close-btn");
    g_signal_connect(b_close, "clicked", G_CALLBACK(on_close_clicked), win);

    gtk_box_append(GTK_BOX(hdr), title);
    gtk_box_append(GTK_BOX(hdr), b_float);
    gtk_box_append(GTK_BOX(hdr), b_close);
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), hdr);
    gtk_box_append(GTK_BOX(root), handle);

    /* Body */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(body, 22);
    gtk_widget_set_margin_end(body, 22);
    gtk_widget_set_margin_top(body, 18);
    gtk_widget_set_margin_bottom(body, 16);
    gtk_widget_set_vexpand(body, TRUE);

    GtkWidget *sub = gtk_label_new("Each profile isolates cookies, cache, storage, Google web logins & Drive AppData sync. Press 1–9 to launch.");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);
    gtk_widget_add_css_class(sub, "web-dim-label");
    gtk_box_append(GTK_BOX(body), sub);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    ctx->grid = gtk_flow_box_new();
    gtk_widget_set_valign(ctx->grid, GTK_ALIGN_START);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(ctx->grid), FALSE);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ctx->grid), GTK_SELECTION_NONE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(ctx->grid), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(ctx->grid), 12);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ctx->grid), 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ctx->grid);
    gtk_box_append(GTK_BOX(body), scroll);

    /* Inline New Profile Creator */
    ctx->add_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_visible(ctx->add_box, FALSE);
    ctx->entry_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entry_name), "New profile name (e.g. WORK, PERSONAL, DEV)...");
    gtk_widget_set_hexpand(ctx->entry_name, TRUE);
    g_signal_connect(ctx->entry_name, "activate", G_CALLBACK(on_add_confirm_clicked), ctx);
    GtkWidget *b_ok = gtk_button_new_with_label("CREATE");
    gtk_widget_add_css_class(b_ok, "action-btn-primary");
    g_signal_connect(b_ok, "clicked", G_CALLBACK(on_add_confirm_clicked), ctx);
    gtk_box_append(GTK_BOX(ctx->add_box), ctx->entry_name);
    gtk_box_append(GTK_BOX(ctx->add_box), b_ok);
    gtk_box_append(GTK_BOX(body), ctx->add_box);
    gtk_box_append(GTK_BOX(root), body);

    /* Footer */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(foot, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(foot, "web-picker-footer");

    GtkWidget *b_add = gtk_button_new_with_label("+ ADD PROFILE");
    gtk_widget_add_css_class(b_add, "action-btn-primary");
    g_signal_connect(b_add, "clicked", G_CALLBACK(on_add_toggle), ctx);

    GtkWidget *b_guest = gtk_button_new_with_label("🕶 GUEST / PRIVATE");
    gtk_widget_add_css_class(b_guest, "files-nav-btn");
    g_signal_connect(b_guest, "clicked", G_CALLBACK(on_guest_clicked), ctx);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);

    g_autoptr(KajoWebCfgRef) wcfg = kajo_config_store_acquire_web(NULL);
    ctx->chk_startup = gtk_check_button_new_with_label("SHOW ON STARTUP (>1 PROFILE)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ctx->chk_startup), wcfg ? wcfg->show_picker_on_startup : TRUE);
    g_signal_connect(ctx->chk_startup, "toggled", G_CALLBACK(on_startup_toggled), ctx);

    gtk_box_append(GTK_BOX(foot), b_add);
    gtk_box_append(GTK_BOX(foot), b_guest);
    gtk_box_append(GTK_BOX(foot), spacer);
    gtk_box_append(GTK_BOX(foot), ctx->chk_startup);
    gtk_box_append(GTK_BOX(root), foot);

    rebuild_cards(ctx);
    return GTK_WINDOW(win);
}
