#include <gtk/gtk.h>
#include "widget.h"

typedef struct {
    ShellWidget base;
    GtkWidget *button;        /* GtkMenuButton */
    GtkWidget *label;         /* time label inside button */
    GtkWidget *popover;       /* GtkPopover */
    GtkWidget *popover_box;   /* Container box inside popover */
    GtkWidget *date_label;    /* full date in popover header */
    GtkWidget *calendar;      /* GtkCalendar */
    GtkWidget *tz_label;      /* timezone identifier in footer */
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
            gtk_label_set_text(GTK_LABEL(clock->tz_label), tz_id ? tz_id : "");
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
    shell_widget_apply_mode_visibility(clock->base.mode, NULL, clock->label);

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

static void clock_build_popover(ClockWidget *clock)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    clock->popover_box = box;

    /* Header: full date label */
    GDateTime *now = g_date_time_new_now_local();
    gchar *date_text = g_date_time_format(now, "%A, %B %e, %Y");
    clock->date_label = gtk_label_new(date_text);
    gtk_widget_add_css_class(clock->date_label, "heading");
    gtk_box_append(GTK_BOX(box), clock->date_label);
    g_free(date_text);

    /* Calendar */
    clock->calendar = gtk_calendar_new();
    gtk_calendar_set_date(GTK_CALENDAR(clock->calendar), now);
    gtk_box_append(GTK_BOX(box), clock->calendar);

    /* Footer: timezone identifier */
    GTimeZone *tz = g_date_time_get_timezone(now);
    const gchar *tz_id = g_time_zone_get_identifier(tz);
    clock->tz_label = gtk_label_new(tz_id);
    gtk_widget_add_css_class(clock->tz_label, "dim-label");
    gtk_box_append(GTK_BOX(box), clock->tz_label);

    g_date_time_get_ymd(now, &clock->last_year, &clock->last_month, &clock->last_day);
    g_date_time_unref(now);

    clock->popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(clock->popover), box);
    gtk_widget_add_css_class(clock->popover, "shell-popover");
    gtk_widget_add_css_class(clock->popover, "shell-popover-clock");
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

    /* Time label */
    clock->label = gtk_label_new(NULL);

    /* Menu button (flat) */
    clock->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(clock->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(clock->button), clock->label);
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
