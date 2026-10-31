#include "settings_window.h"
#include "settings_common.h"
#include "settings_pages.h"
#include "../compositor/compositor.h"

static void select_sidebar_row_by_name(SettingsApp *app_ctx, const gchar *target_page);

static void
update_responsive_layout(SettingsApp *app_ctx, int width)
{
    gboolean narrow = (width < 768);
    app_ctx->is_narrow_mode = narrow;

    if (!narrow) {
        /* Widescreen Split-Pane Mode (>= 768px) */
        gtk_widget_set_visible(app_ctx->sidebar_vbox, TRUE);
        if (app_ctx->stack_scroll) gtk_widget_set_visible(app_ctx->stack_scroll, TRUE);
        gtk_widget_set_visible(app_ctx->back_button, FALSE);

        GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(app_ctx->sidebar_list));
        if (!sel && app_ctx->stack) {
            const gchar *vis = gtk_stack_get_visible_child_name(GTK_STACK(app_ctx->stack));
            if (vis) select_sidebar_row_by_name(app_ctx, vis);
        }
    } else {
        /* Narrow Stack Mode (< 768px) */
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(app_ctx->sidebar_list));
        if (!sel) {
            gtk_widget_set_visible(app_ctx->sidebar_vbox, TRUE);
            if (app_ctx->stack_scroll) gtk_widget_set_visible(app_ctx->stack_scroll, FALSE);
            gtk_widget_set_visible(app_ctx->back_button, FALSE);
        } else {
            gtk_widget_set_visible(app_ctx->sidebar_vbox, FALSE);
            if (app_ctx->stack_scroll) gtk_widget_set_visible(app_ctx->stack_scroll, TRUE);
            gtk_widget_set_visible(app_ctx->back_button, TRUE);
        }
    }
}

static void
on_surface_width_changed(GdkSurface *surface, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    int width = gdk_surface_get_width(surface);
    if (width > 0) {
        update_responsive_layout(app_ctx, width);
    }
}

static void
on_window_realize(GtkWidget *widget, gpointer user_data)
{
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (surface) {
        g_signal_connect(surface, "notify::width", G_CALLBACK(on_surface_width_changed), user_data);
        int width = gdk_surface_get_width(surface);
        if (width > 0) {
            update_responsive_layout((SettingsApp *)user_data, width);
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
    gtk_widget_set_visible(app_ctx->sidebar_vbox, TRUE);
    if (app_ctx->stack_scroll) gtk_widget_set_visible(app_ctx->stack_scroll, FALSE);
    gtk_widget_set_visible(app_ctx->back_button, FALSE);
    gtk_list_box_unselect_all(GTK_LIST_BOX(app_ctx->sidebar_list));
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
            if (app_ctx->stack_scroll) gtk_widget_set_visible(app_ctx->stack_scroll, TRUE);
            gtk_widget_set_visible(app_ctx->back_button, TRUE);
        }
    }
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!app_ctx->sidebar_list) return;

    gchar *query = text ? g_ascii_strdown(text, -1) : g_strdup("");
    g_strstrip(query);
    gboolean empty_query = (query[0] == '\0');

    GtkWidget *child = gtk_widget_get_first_child(app_ctx->sidebar_list);
    GtkWidget *current_header_row = NULL;
    gboolean header_has_visible_child = FALSE;

    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
            if (gtk_widget_has_css_class(child, "sidebar-header-row")) {
                if (current_header_row && !empty_query) {
                    gtk_widget_set_visible(current_header_row, header_has_visible_child);
                }
                current_header_row = child;
                header_has_visible_child = FALSE;
                if (empty_query) {
                    gtk_widget_set_visible(child, TRUE);
                }
            } else if (row_child) {
                const gchar *page_name = g_object_get_data(G_OBJECT(row_child), "page-name");
                const gchar *label_text = g_object_get_data(G_OBJECT(row_child), "label-text");
                const gchar *sub_text = g_object_get_data(G_OBJECT(row_child), "subtitle-text");
                const gchar *keywords = g_object_get_data(G_OBJECT(row_child), "keywords");

                if (empty_query) {
                    gtk_widget_set_visible(child, TRUE);
                } else {
                    gboolean match = FALSE;
                    if (page_name && strstr(page_name, query)) match = TRUE;
                    if (!match && label_text) {
                        gchar *l = g_ascii_strdown(label_text, -1);
                        if (strstr(l, query)) match = TRUE;
                        g_free(l);
                    }
                    if (!match && sub_text) {
                        gchar *s = g_ascii_strdown(sub_text, -1);
                        if (strstr(s, query)) match = TRUE;
                        g_free(s);
                    }
                    if (!match && keywords) {
                        gchar *k = g_ascii_strdown(keywords, -1);
                        if (strstr(k, query)) match = TRUE;
                        g_free(k);
                    }
                    gtk_widget_set_visible(child, match);
                    if (match) header_has_visible_child = TRUE;
                }
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }

    if (current_header_row && !empty_query) {
        gtk_widget_set_visible(current_header_row, header_has_visible_child);
    }

    g_free(query);
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

static void
add_sidebar_category(GtkListBox *box, const gchar *name)
{
    GtkWidget *lbl = create_sidebar_category_header(name);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "sidebar-header-row");
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
    gtk_list_box_append(box, row);
}

static void
select_sidebar_row_by_name(SettingsApp *app_ctx, const gchar *target_page)
{
    if (!app_ctx || !app_ctx->sidebar_list || !target_page) return;
    GtkWidget *child = gtk_widget_get_first_child(app_ctx->sidebar_list);
    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
            if (row_child) {
                const gchar *name = g_object_get_data(G_OBJECT(row_child), "page-name");
                if (g_strcmp0(name, target_page) == 0) {
                    gtk_list_box_select_row(GTK_LIST_BOX(app_ctx->sidebar_list), GTK_LIST_BOX_ROW(child));
                    if (app_ctx->stack) {
                        gtk_stack_set_visible_child_name(GTK_STACK(app_ctx->stack), target_page);
                    }
                    return;
                }
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }
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

    /* Frameless Metro Header Bar wrapped in GtkWindowHandle for dragging */
    GtkWidget *window_handle = gtk_window_handle_new();
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

    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(window_handle), header_bar);
    gtk_box_append(GTK_BOX(main_vbox), window_handle);

    /* Main 2-Pane Split Box */
    app_ctx->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(app_ctx->paned), 260);
    gtk_paned_set_shrink_start_child(GTK_PANED(app_ctx->paned), FALSE);
    gtk_paned_set_resize_start_child(GTK_PANED(app_ctx->paned), FALSE);

    /* Left Sidebar VBox */
    app_ctx->sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(app_ctx->sidebar_vbox, "settings-sidebar-vbox");
    gtk_widget_set_size_request(app_ctx->sidebar_vbox, 260, -1);

    app_ctx->sidebar_list = gtk_list_box_new();
    gtk_widget_add_css_class(app_ctx->sidebar_list, "navigation-sidebar");

    /* Domain 1: SYSTEM & HARDWARE */
    add_sidebar_category(GTK_LIST_BOX(app_ctx->sidebar_list), "SYSTEM & HARDWARE");
    GtkWidget *r_disp = create_sidebar_row_with_keywords(
        "display-brightness-symbolic", "Displays & Monitors", "Resolutions, Refresh, Scaling", "displays",
        "display monitor screen resolution refresh scale scaling vrr hdr orientation mode");
    GtkWidget *r_net  = create_sidebar_row_with_keywords(
        "fl-wifi-symbolic", "Network & Wi-Fi", "Connections, Access Points", "network",
        "network wifi wireless ethernet connection access point ssid internet ip wpa");
    GtkWidget *r_bt   = create_sidebar_row_with_keywords(
        "fl-bluetooth-symbolic", "Bluetooth & Devices", "Pairing, Device Discovery", "bluetooth",
        "bluetooth devices paired pairing discovery scan controller connect");
    GtkWidget *r_aud  = create_sidebar_row_with_keywords(
        "fl-volume-symbolic", "Audio & PipeWire", "Stream Mixer, Volume", "audio",
        "audio sound volume speaker microphone mic headset pipewire sink source mixer");

    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_disp);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_net);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_bt);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_aud);

    /* Domain 2: PERSONALIZATION */
    add_sidebar_category(GTK_LIST_BOX(app_ctx->sidebar_list), "PERSONALIZATION");
    GtkWidget *r_thm  = create_sidebar_row_with_keywords(
        "fl-color-symbolic", "Theme & Accents", "Metro Accent Color Switcher", "theme",
        "theme accent color palette font css metro style appearance");
    GtkWidget *r_pnl  = create_sidebar_row_with_keywords(
        "fl-sliders-symbolic", "Panel & Geometry", "Thickness, Auto-Hide, Position", "panel",
        "panel geometry autohide height position top bottom thickness delay edge bar");
    GtkWidget *r_wgt  = create_sidebar_row_with_keywords(
        "fl-apps-symbolic", "Widgets Layout", "Panel Widgets Ordering", "widgets",
        "widgets layout ordering panel left center right items dock add remove");
    GtkWidget *r_not  = create_sidebar_row_with_keywords(
        "fl-bell-symbolic", "Notifications", "Toast Timeouts, History", "notifications",
        "notifications toast banner timeout history dnd alert os daemon");

    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_thm);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_pnl);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_wgt);
    gtk_list_box_append(GTK_LIST_BOX(app_ctx->sidebar_list), r_not);

    /* Domain 3: DESKTOP & WORKSPACE */
    add_sidebar_category(GTK_LIST_BOX(app_ctx->sidebar_list), "DESKTOP & WORKSPACE");
    GtkWidget *r_niri = create_sidebar_row_with_keywords(
        "view-grid-symbolic", "Niri Compositor", "Window Rules & Keybindings", "niri",
        "niri compositor rules layout gaps blur xray window floating tiling kdl");
    GtkWidget *r_abt  = create_sidebar_row_with_keywords(
        "fl-power-symbolic", "About System", "Hardware Specs, OS Version", "about",
        "about system hardware specs cpu memory ram kernel version os linux desktop");

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

    app_ctx->stack_scroll = stack_scroll;
    gtk_paned_set_end_child(GTK_PANED(app_ctx->paned), stack_scroll);
    gtk_box_append(GTK_BOX(main_vbox), app_ctx->paned);

    gtk_window_set_child(GTK_WINDOW(app_ctx->window), main_vbox);
    g_object_set_data_full(G_OBJECT(app_ctx->window), "app-ctx", app_ctx, g_free);

    /* Select initial default page */
    select_sidebar_row_by_name(app_ctx, "displays");

    g_signal_connect(app_ctx->window, "notify::default-width", G_CALLBACK(on_window_size_changed), app_ctx);
    g_signal_connect(app_ctx->window, "realize", G_CALLBACK(on_window_realize), app_ctx);

    return app_ctx->window;
}

GtkWidget *
desktop_settings_window_new_with_page(GtkApplication *app, const char *initial_page)
{
    GtkWidget *win = desktop_settings_window_new(app);
    if (initial_page && initial_page[0] != '\0') {
        SettingsApp *app_ctx = g_object_get_data(G_OBJECT(win), "app-ctx");
        select_sidebar_row_by_name(app_ctx, initial_page);
    }
    return win;
}
