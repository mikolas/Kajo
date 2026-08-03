#include "launcher_surface.h"
#include "emoji_dataset.h"
#include "../compositor/compositor.h"
#include "../icons.h"
#include "../vendor/tinyexpr/tinyexpr.h"
#include <gtk4-layer-shell.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef enum {
    LAUNCHER_ITEM_APP,
    LAUNCHER_ITEM_EMOJI,
    LAUNCHER_ITEM_CALC,
    LAUNCHER_ITEM_CMD,
    LAUNCHER_ITEM_WINDOW
} LauncherItemType;

typedef struct {
    LauncherItemType type;
    gchar *title;
    gchar *subtitle;
    gchar *icon;
    gchar *payload;
} LauncherItem;

typedef struct {
    gchar *name;
    gchar *exec;
    gchar *icon;
    gchar *keywords;
    guint launch_count;
} AppEntry;

struct _ShellLauncherSurface {
    ShellCompositor *compositor;
    GtkWindow *window;
    GtkWidget *card;
    GtkWidget *search_entry;
    GtkWidget *scrolled_win;
    GtkWidget *list_box;

    GPtrArray *all_apps;
    gboolean is_visible;
};

static void append_launcher_item(ShellLauncherSurface *ls, LauncherItem *item);
static void update_launcher_results(ShellLauncherSurface *ls, const gchar *query);
static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);

static gchar *
get_stats_file_path(void)
{
    const gchar *config_dir = g_get_user_config_dir();
    gchar *dir = g_build_filename(config_dir, "kajo", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *filepath = g_build_filename(dir, "stats.json", NULL);
    g_free(dir);
    return filepath;
}

static void
load_app_stats(GPtrArray *apps)
{
    gchar *filepath = get_stats_file_path();
    if (!g_file_test(filepath, G_FILE_TEST_EXISTS)) {
        g_free(filepath);
        return;
    }

    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_file(parser, filepath, &error)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            for (guint i = 0; i < apps->len; i++) {
                AppEntry *app = g_ptr_array_index(apps, i);
                if (app->exec && json_object_has_member(obj, app->exec)) {
                    app->launch_count = (guint)json_object_get_int_member(obj, app->exec);
                }
            }
        }
    } else {
        if (error) g_error_free(error);
    }
    g_object_unref(parser);
    g_free(filepath);
}

static void
save_app_stats(GPtrArray *apps)
{
    gchar *filepath = get_stats_file_path();
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    for (guint i = 0; i < apps->len; i++) {
        AppEntry *app = g_ptr_array_index(apps, i);
        if (app->exec && app->launch_count > 0) {
            json_builder_set_member_name(builder, app->exec);
            json_builder_add_int_value(builder, (gint64)app->launch_count);
        }
    }

    json_builder_end_object(builder);
    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    json_generator_to_file(gen, filepath, NULL);

    json_node_free(root);
    g_object_unref(builder);
    g_object_unref(gen);
    g_free(filepath);
}

static gint
sort_apps_by_frequency(gconstpointer a, gconstpointer b)
{
    const AppEntry *app_a = *(const AppEntry **)a;
    const AppEntry *app_b = *(const AppEntry **)b;

    if (app_a->launch_count != app_b->launch_count) {
        return (gint)app_b->launch_count - (gint)app_a->launch_count;
    }
    return g_strcmp0(app_a->name, app_b->name);
}

static void
free_launcher_item(gpointer data)
{
    LauncherItem *item = data;
    if (item) {
        g_free(item->title);
        g_free(item->subtitle);
        g_free(item->icon);
        g_free(item->payload);
        g_free(item);
    }
}

static void
free_app_entry(gpointer data)
{
    AppEntry *app = data;
    if (app) {
        g_free(app->name);
        g_free(app->exec);
        g_free(app->icon);
        g_free(app->keywords);
        g_free(app);
    }
}

static void
index_desktop_files_dir(GPtrArray *apps, const gchar *dir_path)
{
    if (!g_file_test(dir_path, G_FILE_TEST_IS_DIR))
        return;

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir)
        return;

    const gchar *filename;
    while ((filename = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(filename, ".desktop"))
            continue;

        gchar *filepath = g_build_filename(dir_path, filename, NULL);
        GKeyFile *keyfile = g_key_file_new();

        if (g_key_file_load_from_file(keyfile, filepath, G_KEY_FILE_NONE, NULL)) {
            gchar *type = g_key_file_get_string(keyfile, "Desktop Entry", "Type", NULL);
            gboolean nodisplay = g_key_file_get_boolean(keyfile, "Desktop Entry", "NoDisplay", NULL);

            if (g_strcmp0(type, "Application") == 0 && !nodisplay) {
                gchar *name = g_key_file_get_string(keyfile, "Desktop Entry", "Name", NULL);
                gchar *exec = g_key_file_get_string(keyfile, "Desktop Entry", "Exec", NULL);
                gchar *icon = g_key_file_get_string(keyfile, "Desktop Entry", "Icon", NULL);
                gchar *gen_name = g_key_file_get_string(keyfile, "Desktop Entry", "GenericName", NULL);
                gchar *kw_str = g_key_file_get_string(keyfile, "Desktop Entry", "Keywords", NULL);
                gchar *comment = g_key_file_get_string(keyfile, "Desktop Entry", "Comment", NULL);

                if (name && exec) {
                    gchar *clean_exec = g_strdup(exec);
                    gchar *p = strchr(clean_exec, '%');
                    if (p) *p = '\0';
                    g_strstrip(clean_exec);

                    AppEntry *app = g_new0(AppEntry, 1);
                    app->name = name;
                    app->exec = clean_exec;
                    app->icon = icon;
                    app->keywords = g_strdup_printf("%s %s %s %s %s",
                                                   name ? name : "",
                                                   gen_name ? gen_name : "",
                                                   kw_str ? kw_str : "",
                                                   comment ? comment : "",
                                                   clean_exec ? clean_exec : "");

                    g_ptr_array_add(apps, app);
                    g_free(exec);
                } else {
                    g_free(name);
                    g_free(exec);
                    g_free(icon);
                }
                g_free(gen_name);
                g_free(kw_str);
                g_free(comment);
            }
            g_free(type);
        }

        g_key_file_free(keyfile);
        g_free(filepath);
    }

    g_dir_close(dir);
}

static void
populate_app_index(ShellLauncherSurface *ls)
{
    ls->all_apps = g_ptr_array_new_full(0, free_app_entry);

    const gchar *user_apps = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    index_desktop_files_dir(ls->all_apps, user_apps);
    g_free((gchar *)user_apps);

    index_desktop_files_dir(ls->all_apps, "/usr/share/applications");
    index_desktop_files_dir(ls->all_apps, "/usr/local/share/applications");

    load_app_stats(ls->all_apps);
    g_ptr_array_sort(ls->all_apps, sort_apps_by_frequency);
}

static void
copy_text_to_clipboard(GtkWidget *widget, const gchar *text)
{
    if (!text || !*text) return;
    GdkDisplay *display = gtk_widget_get_display(widget);
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        gdk_clipboard_set_text(clipboard, text);
    }
}

static void
on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ShellLauncherSurface *ls = user_data;
    if (!row) return;

    LauncherItem *item = g_object_get_data(G_OBJECT(row), "launcher-item");
    if (item && item->payload) {
        switch (item->type) {
            case LAUNCHER_ITEM_APP:
                for (guint i = 0; i < ls->all_apps->len; i++) {
                    AppEntry *app = g_ptr_array_index(ls->all_apps, i);
                    if (g_strcmp0(app->exec, item->payload) == 0) {
                        app->launch_count++;
                        save_app_stats(ls->all_apps);
                        g_ptr_array_sort(ls->all_apps, sort_apps_by_frequency);
                        break;
                    }
                }
                g_spawn_command_line_async(item->payload, NULL);
                break;
            case LAUNCHER_ITEM_EMOJI:
            case LAUNCHER_ITEM_CALC:
                copy_text_to_clipboard(GTK_WIDGET(ls->window), item->payload);
                break;
            case LAUNCHER_ITEM_CMD: {
                gchar *terminal_cmd = g_strdup_printf("x-terminal-emulator -e %s", item->payload);
                if (!g_spawn_command_line_async(terminal_cmd, NULL)) {
                    g_spawn_command_line_async(item->payload, NULL);
                }
                g_free(terminal_cmd);
                break;
            }
            case LAUNCHER_ITEM_WINDOW: {
                guint64 win_id = g_ascii_strtoull(item->payload, NULL, 10);
                if (win_id > 0) {
                    shell_compositor_focus_window(ls->compositor, win_id);
                }
                break;
            }
        }
    }
    shell_launcher_surface_hide(ls);
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    ShellLauncherSurface *ls = user_data;
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    update_launcher_results(ls, text);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint keyval,
               guint keycode,
               GdkModifierType state,
               gpointer user_data)
{
    ShellLauncherSurface *ls = user_data;

    if (keyval == GDK_KEY_Escape) {
        shell_launcher_surface_hide(ls);
        return GDK_EVENT_STOP;
    } else if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ls->list_box));
        if (!row) {
            row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ls->list_box), 0);
        }
        if (row) {
            on_row_activated(GTK_LIST_BOX(ls->list_box), row, ls);
        }
        return GDK_EVENT_STOP;
    } else if (keyval == GDK_KEY_Down) {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ls->list_box));
        gint idx = row ? gtk_list_box_row_get_index(row) : -1;
        row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ls->list_box), idx + 1);
        if (row) {
            gtk_list_box_select_row(GTK_LIST_BOX(ls->list_box), row);
            gdouble row_y = 0.0;
            if (gtk_widget_translate_coordinates(GTK_WIDGET(row), ls->list_box, 0, 0, NULL, &row_y)) {
                GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ls->scrolled_win));
                gdouble page_size = gtk_adjustment_get_page_size(vadj);
                gdouble val = gtk_adjustment_get_value(vadj);
                if (row_y + 40 > val + page_size) {
                    gtk_adjustment_set_value(vadj, row_y + 40 - page_size);
                }
            }
        }
        return GDK_EVENT_STOP;
    } else if (keyval == GDK_KEY_Up) {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ls->list_box));
        gint idx = row ? gtk_list_box_row_get_index(row) : 0;
        if (idx > 0) {
            row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ls->list_box), idx - 1);
            if (row) {
                gtk_list_box_select_row(GTK_LIST_BOX(ls->list_box), row);
                gdouble row_y = 0.0;
                if (gtk_widget_translate_coordinates(GTK_WIDGET(row), ls->list_box, 0, 0, NULL, &row_y)) {
                    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ls->scrolled_win));
                    gdouble val = gtk_adjustment_get_value(vadj);
                    if (row_y < val) {
                        gtk_adjustment_set_value(vadj, row_y);
                    }
                }
            }
        }
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

static void
append_launcher_item(ShellLauncherSurface *ls, LauncherItem *item)
{
    GtkWidget *list_row = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(list_row), "launcher-item", item, free_launcher_item);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(row, "shell-launcher-row");

    GtkWidget *img = NULL;
    if (item->type == LAUNCHER_ITEM_EMOJI) {
        img = gtk_label_new(item->icon ? item->icon : "😀");
        gtk_widget_add_css_class(img, "shell-launcher-emoji-icon");
    } else {
        img = shell_icons_create_app_icon_widget(item->icon, 24);
    }

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);

    GtkWidget *lbl = gtk_label_new(item->title);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(vbox), lbl);

    if (item->subtitle && *item->subtitle) {
        GtkWidget *sub_lbl = gtk_label_new(item->subtitle);
        gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
        gtk_widget_add_css_class(sub_lbl, "shell-popover-section-title");
        gtk_box_append(GTK_BOX(vbox), sub_lbl);
    }

    gtk_box_append(GTK_BOX(row), img);
    gtk_box_append(GTK_BOX(row), vbox);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
    gtk_list_box_append(GTK_LIST_BOX(ls->list_box), list_row);
}

static void
update_launcher_results(ShellLauncherSurface *ls, const gchar *query)
{
    /* Clear listbox */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ls->list_box)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(ls->list_box), child);
    }

    gchar *clean_query = g_strdup(query ? query : "");
    g_strstrip(clean_query);

    /* ── 1. Emoji Plugin (Prefix ':') ── */
    if (clean_query[0] == ':') {
        gchar *eq = g_utf8_casefold(clean_query + 1, -1);
        guint count = 0;
        for (int i = 0; full_emoji_dataset[i].name != NULL && count < 100; i++) {
            gchar *name_fold = g_utf8_casefold(full_emoji_dataset[i].name, -1);
            gchar *kw_fold = full_emoji_dataset[i].keywords ? g_utf8_casefold(full_emoji_dataset[i].keywords, -1) : NULL;

            if (*eq == '\0' ||
                g_strrstr(name_fold, eq) != NULL ||
                (kw_fold && g_strrstr(kw_fold, eq) != NULL)) {
                LauncherItem *item = g_new0(LauncherItem, 1);
                item->type = LAUNCHER_ITEM_EMOJI;
                item->title = g_strdup_printf("%s  :%s:", full_emoji_dataset[i].emoji, full_emoji_dataset[i].name);
                item->subtitle = NULL;
                item->icon = g_strdup(full_emoji_dataset[i].emoji);
                item->payload = g_strdup(full_emoji_dataset[i].emoji);
                append_launcher_item(ls, item);
                count++;
            }

            g_free(name_fold);
            if (kw_fold) g_free(kw_fold);
        }
        g_free(eq);
    }
    /* ── 2. Calculator Plugin (Prefix '=') ── */
    else if (clean_query[0] == '=') {
        const gchar *expr = clean_query + 1;
        while (*expr == ' ') expr++;
        if (*expr != '\0') {
            int err = 0;
            double res = te_interp(expr, &err);
            if (err == 0 && !isnan(res) && !isinf(res)) {
                gchar *res_str = g_strdup_printf("%.6g", res);
                LauncherItem *item = g_new0(LauncherItem, 1);
                item->type = LAUNCHER_ITEM_CALC;
                item->title = g_strdup_printf("= %s", res_str);
                item->subtitle = g_strdup("Copy calculation result to clipboard");
                item->icon = g_strdup("accessories-calculator-symbolic");
                item->payload = res_str;
                append_launcher_item(ls, item);
            }
        }
    }
    /* ── 3. Shell Command Plugin (Prefix '>') ── */
    else if (clean_query[0] == '>') {
        const gchar *cmd = clean_query + 1;
        if (*cmd != '\0') {
            LauncherItem *item = g_new0(LauncherItem, 1);
            item->type = LAUNCHER_ITEM_CMD;
            item->title = g_strdup_printf("> Run: %s", cmd);
            item->subtitle = g_strdup("Execute terminal command");
            item->icon = g_strdup("utilities-terminal-symbolic");
            item->payload = g_strdup(cmd);
            append_launcher_item(ls, item);
        }
    }
    /* ── 4. Window Switcher Plugin (Prefix 'w:') ── */
    else if ((clean_query[0] == 'w' || clean_query[0] == 'W') && clean_query[1] == ':') {
        const gchar *wq = clean_query + 2;
        gchar *wq_fold = g_utf8_casefold(wq, -1);
        const GArray *windows = shell_compositor_get_windows(ls->compositor);
        if (windows) {
            for (guint i = 0; i < windows->len; i++) {
                const CompositorWindow *win = &g_array_index(windows, CompositorWindow, i);
                gchar *title_fold = win->title ? g_utf8_casefold(win->title, -1) : g_strdup("");
                gchar *app_fold = win->app_id ? g_utf8_casefold(win->app_id, -1) : g_strdup("");

                if (*wq_fold == '\0' ||
                    g_strrstr(title_fold, wq_fold) != NULL ||
                    g_strrstr(app_fold, wq_fold) != NULL) {
                    LauncherItem *item = g_new0(LauncherItem, 1);
                    item->type = LAUNCHER_ITEM_WINDOW;
                    item->title = g_strdup_printf("%s — %s", win->app_id ? win->app_id : "Window", win->title ? win->title : "Untitled");
                    item->subtitle = g_strdup_printf("Workspace #%" G_GUINT64_FORMAT " • Click or Enter to focus", win->workspace_id);
                    item->icon = g_strdup(win->app_id ? win->app_id : "window-new-symbolic");
                    item->payload = g_strdup_printf("%" G_GUINT64_FORMAT, win->id);
                    append_launcher_item(ls, item);
                }
                g_free(title_fold);
                g_free(app_fold);
            }
        }
        g_free(wq_fold);
    }
    /* ── 5. Applications Plugin (Bare Text) ── */
    else {
        guint count = 0;
        gchar *query_fold = clean_query ? g_utf8_casefold(clean_query, -1) : g_strdup("");
        g_strstrip(query_fold);

        for (guint i = 0; i < ls->all_apps->len && count < 50; i++) {
            AppEntry *app = g_ptr_array_index(ls->all_apps, i);

            if (*query_fold != '\0') {
                gchar *kw_fold = app->keywords ? g_utf8_casefold(app->keywords, -1) : g_strdup("");

                if (strstr(kw_fold, query_fold) == NULL) {
                    g_free(kw_fold);
                    continue;
                }
                g_free(kw_fold);
            }

            LauncherItem *item = g_new0(LauncherItem, 1);
            item->type = LAUNCHER_ITEM_APP;
            item->title = g_strdup(app->name);
            item->subtitle = NULL;
            item->icon = g_strdup(app->icon);
            item->payload = g_strdup(app->exec);

            append_launcher_item(ls, item);
            count++;
        }
        g_free(query_fold);
    }

    /* Auto select first item */
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ls->list_box), 0);
    if (first) {
        gtk_list_box_select_row(GTK_LIST_BOX(ls->list_box), first);
    }

    g_free(clean_query);
}

static void
on_window_is_active_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data)
{
    ShellLauncherSurface *ls = user_data;
    if (ls && ls->is_visible && !gtk_window_is_active(GTK_WINDOW(gobject))) {
        shell_launcher_surface_hide(ls);
    }
}

/* ─── Declarative GtkWindow Template Subclass ─── */

typedef struct _ShellLauncherWindow {
    GtkWindow parent_instance;

    GtkWidget *card;
    GtkWidget *search_entry;
    GtkWidget *scrolled_win;
    GtkWidget *list_box;
} ShellLauncherWindow;

typedef struct _ShellLauncherWindowClass {
    GtkWindowClass parent_class;
} ShellLauncherWindowClass;

G_DEFINE_TYPE(ShellLauncherWindow, shell_launcher_window, GTK_TYPE_WINDOW)

static void
shell_launcher_window_init(ShellLauncherWindow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_launcher_window_class_init(ShellLauncherWindowClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/launcher_window.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellLauncherWindow, card);
    gtk_widget_class_bind_template_child(widget_class, ShellLauncherWindow, search_entry);
    gtk_widget_class_bind_template_child(widget_class, ShellLauncherWindow, scrolled_win);
    gtk_widget_class_bind_template_child(widget_class, ShellLauncherWindow, list_box);
}

ShellLauncherSurface *
shell_launcher_surface_new(ShellCompositor *compositor)
{
    ShellLauncherSurface *ls = g_new0(ShellLauncherSurface, 1);
    ls->compositor = compositor;
    ls->is_visible = FALSE;

    populate_app_index(ls);

    ShellLauncherWindow *win = g_object_new(shell_launcher_window_get_type(), NULL);
    ls->window = GTK_WINDOW(win);
    ls->card = win->card;
    ls->search_entry = win->search_entry;
    ls->scrolled_win = win->scrolled_win;
    ls->list_box = win->list_box;

    /* Configure Wayland Layer-Shell Overlay */
    gtk_layer_init_for_window(ls->window);
    gtk_layer_set_layer(ls->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(ls->window, "kajo-launcher");
    gtk_layer_set_keyboard_mode(ls->window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_exclusive_zone(ls->window, 0);

    /* Key Event Controller on Main Window with CAPTURE phase */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), ls);
    gtk_widget_add_controller(GTK_WIDGET(ls->window), key_ctrl);

    /* Dismiss when window loses active status / click outside */
    g_signal_connect(ls->window, "notify::is-active", G_CALLBACK(on_window_is_active_changed), ls);

    g_signal_connect(ls->search_entry, "search-changed", G_CALLBACK(on_search_changed), ls);
    g_signal_connect(ls->list_box, "row-activated", G_CALLBACK(on_row_activated), ls);

    gtk_widget_set_visible(GTK_WIDGET(ls->window), FALSE);

    return ls;
}

void
shell_launcher_surface_show(ShellLauncherSurface *ls)
{
    if (!ls) return;
    ls->is_visible = TRUE;

    gtk_editable_set_text(GTK_EDITABLE(ls->search_entry), "");
    update_launcher_results(ls, "");

    gtk_widget_set_visible(GTK_WIDGET(ls->window), TRUE);
    gtk_window_present(ls->window);
    gtk_widget_grab_focus(ls->search_entry);
}

void
shell_launcher_surface_hide(ShellLauncherSurface *ls)
{
    if (!ls) return;
    ls->is_visible = FALSE;
    gtk_widget_set_visible(GTK_WIDGET(ls->window), FALSE);
}

void
shell_launcher_surface_toggle(ShellLauncherSurface *ls)
{
    if (!ls) return;
    if (ls->is_visible) {
        shell_launcher_surface_hide(ls);
    } else {
        shell_launcher_surface_show(ls);
    }
}

void
shell_launcher_surface_destroy(ShellLauncherSurface *ls)
{
    if (!ls) return;

    if (ls->all_apps) {
        g_ptr_array_free(ls->all_apps, TRUE);
    }

    if (ls->window) {
        gtk_window_destroy(ls->window);
    }

    g_free(ls);
}
