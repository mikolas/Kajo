#include <gtk/gtk.h>
#include "widget.h"

typedef struct {
    ShellWidget base;
    GtkWidget *button;        /* GtkMenuButton */
    GtkWidget *icon_img;      /* clock icon inside button */
    GtkWidget *label;         /* time label inside button */
    GtkWidget *popover;       /* GtkPopover */
    GtkWidget *popover_box;   /* Zone 2 body inside popover */
    GtkWidget *date_label;    /* full date in popover body */
    GtkWidget *calendar;      /* GtkCalendar */
    GtkWidget *tz_label;      /* timezone badge in header */
    GtkWidget *today_btn;     /* Today button in footer */
    gint last_year;
    gint last_month;
    gint last_day;
    guint timer_id;
} ClockWidget;

static void clock_update_date_widgets(ClockWidget *clock, GDateTime *now, gboolean force_select_today)
{
    gint year = 0, month = 0, day = 0;
    g_date_time_get_ymd(now, &year, &month, &day);

    gboolean day_changed = (year != clock->last_year ||
                            month != clock->last_month ||
                            day != clock->last_day);

    if (day_changed || force_select_today) {
        if (clock->date_label) {
            gchar *date_text = g_date_time_format(now, "%A, %B %e, %Y");
            gtk_label_set_text(GTK_LABEL(clock->date_label), date_text);
            g_free(date_text);
        }

        if (clock->tz_label) {
            GTimeZone *tz = g_date_time_get_timezone(now);
            const gchar *tz_id = g_time_zone_get_identifier(tz);
            g_autofree gchar *badge = g_strdup_printf("[ %s ]", (tz_id && *tz_id) ? tz_id : "LOCAL");
            gtk_label_set_text(GTK_LABEL(clock->tz_label), badge);
        }

        if (day_changed && clock->popover_box && clock->calendar && clock->date_label) {
            /* Recreate GtkCalendar when the system date rolls over so GTK4's
             * internal 'today' marker and selected day both reflect the new day. */
            gtk_box_remove(GTK_BOX(clock->popover_box), clock->calendar);
            clock->calendar = gtk_calendar_new();
            gtk_box_insert_child_after(GTK_BOX(clock->popover_box),
                                       clock->calendar,
                                       clock->date_label);
        }

        if (clock->calendar) {
            gtk_calendar_set_date(GTK_CALENDAR(clock->calendar), now);
        }

        clock->last_year  = year;
        clock->last_month = month;
        clock->last_day   = day;
    }
}

static void clock_update_time(ClockWidget *clock)
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *time_text = g_date_time_format(now, "%H:%M");
    gtk_label_set_text(GTK_LABEL(clock->label), time_text);
    g_free(time_text);
    shell_widget_apply_mode_visibility(clock->base.mode, clock->icon_img, clock->label);

    clock_update_date_widgets(clock, now, FALSE);

    g_date_time_unref(now);
}

static void on_clock_popover_map(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    ClockWidget *clock = user_data;
    if (!clock) return;

    GDateTime *now = g_date_time_new_now_local();
    clock_update_date_widgets(clock, now, TRUE);
    g_date_time_unref(now);
}

static void on_clock_today_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ClockWidget *clock = user_data;
    if (!clock) return;

    GDateTime *now = g_date_time_new_now_local();
    clock_update_date_widgets(clock, now, TRUE);
    g_date_time_unref(now);
}

static void clock_build_popover(ClockWidget *clock)
{
    GtkWidget *body = NULL;
    GtkWidget *footer = NULL;

    GDateTime *now = g_date_time_new_now_local();
    GTimeZone *tz = g_date_time_get_timezone(now);
    const gchar *tz_id = g_time_zone_get_identifier(tz);
    g_autofree gchar *badge = g_strdup_printf("[ %s ]", (tz_id && *tz_id) ? tz_id : "LOCAL");

    clock->popover = GTK_WIDGET(shell_widget_create_popover_shell(
        "popover-compact", "CALENDAR", badge,
        &clock->tz_label, &body, &footer));
    clock->popover_box = body;
    gtk_widget_add_css_class(clock->popover, "shell-popover-clock");

    /* Zone 2: Full date subtitle + GtkCalendar */
    gchar *date_text = g_date_time_format(now, "%A, %B %e, %Y");
    clock->date_label = gtk_label_new(date_text);
    gtk_label_set_xalign(GTK_LABEL(clock->date_label), 0.0f);
    gtk_widget_add_css_class(clock->date_label, "row-title");
    gtk_box_append(GTK_BOX(body), clock->date_label);
    g_free(date_text);

    clock->calendar = gtk_calendar_new();
    gtk_calendar_set_date(GTK_CALENDAR(clock->calendar), now);
    gtk_box_append(GTK_BOX(body), clock->calendar);

    /* Zone 3: Jump to Today action */
    clock->today_btn = gtk_button_new_with_label("SELECT TODAY");
    gtk_widget_set_hexpand(clock->today_btn, TRUE);
    gtk_widget_set_halign(clock->today_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(clock->today_btn, GTK_ALIGN_CENTER);
    g_signal_connect(clock->today_btn, "clicked", G_CALLBACK(on_clock_today_clicked), clock);
    gtk_box_append(GTK_BOX(footer), clock->today_btn);

    g_date_time_get_ymd(now, &clock->last_year, &clock->last_month, &clock->last_day);
    g_date_time_unref(now);

    g_signal_connect(clock->popover, "map", G_CALLBACK(on_clock_popover_map), clock);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(clock->button), clock->popover);
}

static gboolean clock_tick(gpointer user_data)
{
    ClockWidget *clock = user_data;
    clock_update_time(clock);
    return G_SOURCE_CONTINUE;
}

static ShellWidget *clock_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    ClockWidget *clock = g_new0(ClockWidget, 1);

    clock->icon_img = gtk_image_new_from_icon_name("tb-clock-symbolic");
    clock->label = gtk_label_new(NULL);
    GtkWidget *hbox = shell_widget_pack_bar_box(clock->icon_img, clock->label);

    /* Menu button (flat) */
    clock->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(clock->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(clock->button), hbox);
    gtk_widget_add_css_class(clock->button, "flat");
    gtk_widget_add_css_class(clock->button, "shell-widget");
    gtk_widget_add_css_class(clock->button, "shell-widget-clock");

    /* Build popover immediately — content is cheap */
    clock_build_popover(clock);

    /* Set initial time */
    clock_update_time(clock);

    clock->timer_id = 0;

    return (ShellWidget *)clock;
}

static void clock_destroy(ShellWidget *widget)
{
    ClockWidget *clock = (ClockWidget *)widget;

    if (clock->today_btn) {
        g_signal_handlers_disconnect_by_data(clock->today_btn, clock);
    }
    if (clock->popover) {
        g_signal_handlers_disconnect_by_data(clock->popover, clock);
    }

    if (clock->timer_id > 0) {
        g_source_remove(clock->timer_id);
        clock->timer_id = 0;
    }

    g_free(clock);
}

static GtkWidget *clock_get_widget(ShellWidget *widget)
{
    ClockWidget *clock = (ClockWidget *)widget;
    return clock->button;
}

static void clock_enable(ShellWidget *widget)
{
    ClockWidget *clock = (ClockWidget *)widget;

    if (clock->icon_img)
        gtk_image_set_pixel_size(GTK_IMAGE(clock->icon_img), shell_widget_get_icon_size(widget));
    shell_widget_apply_mode_visibility(widget->mode, clock->icon_img, clock->label);

    if (clock->timer_id == 0) {
        clock_update_time(clock);
        clock->timer_id = g_timeout_add_seconds(1, clock_tick, clock);
    }
}

static void clock_disable(ShellWidget *widget)
{
    ClockWidget *clock = (ClockWidget *)widget;

    if (clock->timer_id > 0) {
        g_source_remove(clock->timer_id);
        clock->timer_id = 0;
    }
}

const ShellWidgetClass clock_widget_class = {
    .id         = "clock",
    .name       = "Clock",
    .create     = clock_create,
    .destroy    = clock_destroy,
    .get_widget = clock_get_widget,
    .enable     = clock_enable,
    .disable    = clock_disable,
};
