#include "settings_window.h"
#include "settings_common.h"
#include "settings_pages.h"
#include "../compositor/compositor.h"

static void
update_responsive_layout(SettingsApp *app_ctx, int width)
{
    gboolean narrow = (width < 768);
    app_ctx->is_narrow_mode = narrow;

    if (!narrow) {
        /* Widescreen Split-Pane Mode (>= 768px) */
        gtk_widget_set_visible(app_ctx->sidebar_vbox, TRUE);
        gtk_widget_set_visible(app_ctx->back_button, FALSE);
    } else {
        /* Narrow Stack Mode (< 768px) */
        const gchar *current_page = gtk_stack_get_visible_child_name(GTK_STACK(app_ctx->stack));
        if (g_strcmp0(current_page, "overview") == 0 || !current_page) {
            gtk_widget_set_visible(app_ctx->sidebar_vbox, TRUE);
            gtk_widget_set_visible(app_ctx->back_button, FALSE);
        } else {
            gtk_widget_set_visible(app_ctx->sidebar_vbox, FALSE);
            gtk_widget_set_visible(app_ctx->back_button, TRUE);
        }
    }
}

static void
on_window_size_changed(GtkWindow *win G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    int width = gtk_widget_get_width(GTK_WIDGET(app_ctx->window));
    if (width > 0) {
        update_responsive_layout(app_ctx, width);
    }
}

static void
on_back_button_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    gtk_stack_set_visible_child_name(GTK_STACK(app_ctx->stack), "overview");
    gtk_widget_set_visible(app_ctx->sidebar_vbox, TRUE);
    gtk_widget_set_visible(app_ctx->back_button, FALSE);
}

static void
on_sidebar_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    if (!row) return;
    SettingsApp *app_ctx = user_data;
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child) return;

    const gchar *page_name = g_object_get_data(G_OBJECT(child), "page-name");
    if (page_name && app_ctx->stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(app_ctx->stack), page_name);

        if (app_ctx->is_narrow_mode) {
            gtk_widget_set_visible(app_ctx->sidebar_vbox, FALSE);
            gtk_widget_set_visible(app_ctx->back_button, TRUE);
        }
    }
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!text || !app_ctx->sidebar_list) return;

    gchar *lower_query = g_ascii_strdown(text, -1);

    GtkWidget *child = gtk_widget_get_first_child(app_ctx->sidebar_list);
    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
            if (row_child) {
                const gchar *page_name = g_object_get_data(G_OBJECT(row_child), "page-name");
                if (page_name) {
                    if (lower_query[0] == '\0') {
                        gtk_widget_set_visible(child, TRUE);
                    } else {
                        gchar *lower_name = g_ascii_strdown(page_name, -1);
                        gboolean match = (strstr(lower_name, lower_query) != NULL);
                        g_free(lower_name);
                        gtk_widget_set_visible(child, match);
                    }
                }
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }

    g_free(lower_query);
}

static void
on_toggle_float_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    niri_send_action("{\"Action\":\"ToggleWindowFloating\"}");
}

static void
on_close_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWindow *win = GTK_WINDOW(user_data);
    gtk_window_close(win);
}

GtkWidget *
desktop_settings_window_new(GtkApplication *app)
{
    SettingsApp *app_ctx = g_new0(SettingsApp, 1);

    app_ctx->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(app_ctx->window), "SYSTEM SETTINGS");
    gtk_window_set_default_size(GTK_WINDOW(app_ctx->window), 960, 640);
    gtk_window_set_decorated(GTK_WINDOW(app_ctx->window), FALSE);
    gtk_widget_add_css_class(app_ctx->window, "shell-settings-window");

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Frameless Metro Header Bar */
    GtkWidget *header_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(header_bar, "settings-header-bar");

    app_ctx->back_button = gtk_button_new_with_label("‹ Back");
    gtk_widget_add_css_class(app_ctx->back_button, "flat");
    gtk_widget_set_visible(app_ctx->back_button, FALSE);
    g_signal_connect(app_ctx->back_button, "clicked", G_CALLBACK(on_back_button_clicked), app_ctx);

    GtkWidget *title_lbl = gtk_label_new("⚙ SYSTEM SETTINGS");
    gtk_widget_add_css_class(title_lbl, "settings-header-title");
    gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);

    GtkWidget *btn_float = gtk_button_new_with_label("🗗 FLOAT / TILE");
    gtk_widget_add_css_class(btn_float, "flat");
    g_signal_connect(btn_float, "clicked", G_CALLBACK(on_toggle_float_clicked), NULL);

    app_ctx->search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(app_ctx->search_entry, "shell-launcher-input");
    gtk_widget_set_size_request(app_ctx->search_entry, 220, -1);
    gtk_widget_set_hexpand(app_ctx->search_entry, TRUE);
    gtk_widget_set_halign(app_ctx->search_entry, GTK_ALIGN_END);
    g_signal_connect(app_ctx->search_entry, "search-changed", G_CALLBACK(on_search_changed), app_ctx);

    GtkWidget *btn_close = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(btn_close, "settings-close-btn");
    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_close_btn_clicked), app_ctx->window);

    gtk_box_append(GTK_BOX(header_bar), app_ctx->back_button);
    gtk_box_append(GTK_BOX(header_bar), title_lbl);
    gtk_box_append(GTK_BOX(header_bar), btn_float);
    gtk_box_append(GTK_BOX(header_bar), app_ctx->search_entry);
    gtk_box_append(GTK_BOX(header_bar), btn_close);

    gtk_box_append(GTK_BOX(main_vbox), header_bar);

    /* Main 2-Pane Split Box */
    app_ctx->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(app_ctx->paned), 260);

    /* Left Sidebar VBox */
    app_ctx->sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(app_ctx->sidebar_vbox, "settings-sidebar-vbox");
    gtk_widget_set_size_request(app_ctx->sidebar_vbox, 260, -1);

    app_ctx->sidebar_list = gtk_list_box_new();
    gtk_widget_add_css_class(app_ctx->sidebar_list, "navigation-sidebar");

    /* Domain 1: SYSTEM & HARDWARE */
    GtkWidget *r_disp = create_sidebar_row("display-brightness-symbolic", "Displays & Monitors", "Resolutions, Refresh, Scaling", "displays");
    GtkWidget *r_net  = create_sidebar_row("fl-wifi-symbolic", "Network & Wi-Fi", "Connections, Access Points", "network");
    GtkWidget *r_bt   = create_sidebar_row("fl-bluetooth-symbolic", "Bluetooth & Devices", "Pairing, Device Discovery", "bluetooth");
    GtkWidget *r_aud  = create_sidebar_row("fl-volume-symbolic", "Audio & PipeWire", "Stream Mixer, Volume", "audio");

    /* Domain 2: PERSONALIZATION */
    GtkWidget *r_thm  = create_sidebar_row("fl-color-symbolic", "Theme & Accents", "Metro Accent Color Switcher", "theme");
    GtkWidget *r_pnl  = create_sidebar_row("fl-sliders-symbolic", "Panel & Geometry", "Thickness, Auto-Hide, Position", "panel");
    GtkWidget *r_wgt  = create_sidebar_row("fl-apps-symbolic", "Widgets Layout", "Panel Widgets Ordering", "widgets");
    GtkWidget *r_not  = create_sidebar_row("fl-bell-symbolic", "Notifications", "Toast Timeouts, History", "notifications");

    /* Domain 3: DESKTOP & WORKSPACE */
    GtkWidget *r_niri = create_sidebar_row("fl-apps-symbolic", "Niri Compositor", "Window Rules & Keybindings", "niri");
    GtkWidget *r_abt  = create_sidebar_row("fl-power-symbolic", "About System", "Hardware Specs, OS Version", "about");

    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_disp);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_net);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_bt);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_aud);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_thm);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_pnl);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_wgt);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_not);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_niri);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_abt);

    g_signal_connect(app_ctx->sidebar_list, "row-selected", G_CALLBACK(on_sidebar_row_selected), app_ctx);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), app_ctx->sidebar_list);
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);

    gtk_box_append(GTK_BOX(app_ctx->sidebar_vbox), sidebar_scroll);
    gtk_paned_set_start_child(GTK_PANED(app_ctx->paned), app_ctx->sidebar_vbox);

    /* Right GtkStack Pages Container */
    app_ctx->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app_ctx->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_displays(), "displays");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_network(), "network");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_bluetooth(), "bluetooth");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_audio(), "audio");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_theme(), "theme");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_panel(), "panel");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_widgets(), "widgets");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_notifications(), "notifications");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_niri(), "niri");
    gtk_stack_add_named(GTK_STACK(app_ctx->stack), build_page_about(), "about");

    GtkWidget *stack_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(stack_scroll), app_ctx->stack);
    gtk_widget_set_hexpand(stack_scroll, TRUE);
    gtk_widget_set_vexpand(stack_scroll, TRUE);

    gtk_paned_set_end_child(GTK_PANED(app_ctx->paned), stack_scroll);
    gtk_box_append(GTK_BOX(main_vbox), app_ctx->paned);

    gtk_window_set_child(GTK_WINDOW(app_ctx->window), main_vbox);
    g_object_set_data_full(G_OBJECT(app_ctx->window), "app-ctx", app_ctx, g_free);

    g_signal_connect(app_ctx->window, "notify::default-width", G_CALLBACK(on_window_size_changed), app_ctx);

    return app_ctx->window;
}

GtkWidget *
desktop_settings_window_new_with_page(GtkApplication *app, const char *initial_page)
{
    GtkWidget *win = desktop_settings_window_new(app);
    if (initial_page && initial_page[0] != '\0') {
        SettingsApp *app_ctx = g_object_get_data(G_OBJECT(win), "app-ctx");
        if (app_ctx && app_ctx->stack) {
            gtk_stack_set_visible_child_name(GTK_STACK(app_ctx->stack), initial_page);
        }
    }
    return win;
}
