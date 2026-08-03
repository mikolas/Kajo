#include <gtk/gtk.h>
#include "widget.h"

typedef struct {
    ShellWidget base;
    GtkWidget *button;        /* GtkMenuButton */
    GtkWidget *label;         /* time label inside button */
    GtkWidget *popover;       /* NULL until first open, then built */
    GtkWidget *date_label;    /* full date in popover header */
    GtkWidget *calendar;      /* GtkCalendar */
    guint timer_id;
} ClockWidget;

static void clock_update_time(ClockWidget *clock)
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *time_text = g_date_time_format(now, "%H:%M");
    gtk_label_set_text(GTK_LABEL(clock->label), time_text);
    g_free(time_text);
    shell_widget_apply_mode_visibility(clock->base.mode, NULL, clock->label);

    if (clock->popover && gtk_widget_get_visible(clock->popover)) {
        gchar *date_text = g_date_time_format(now, "%A, %B %e, %Y");
        gtk_label_set_text(GTK_LABEL(clock->date_label), date_text);
        g_free(date_text);
    }

    g_date_time_unref(now);
}

static void clock_build_popover(ClockWidget *clock)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    /* Header: full date label */
    GDateTime *now = g_date_time_new_now_local();
    gchar *date_text = g_date_time_format(now, "%A, %B %e, %Y");
    clock->date_label = gtk_label_new(date_text);
    gtk_widget_add_css_class(clock->date_label, "heading");
    gtk_box_append(GTK_BOX(box), clock->date_label);
    g_free(date_text);

    /* Calendar */
    clock->calendar = gtk_calendar_new();
    gtk_box_append(GTK_BOX(box), clock->calendar);

    /* Footer: timezone identifier */
    GTimeZone *tz = g_date_time_get_timezone(now);
    const gchar *tz_id = g_time_zone_get_identifier(tz);
    GtkWidget *tz_label = gtk_label_new(tz_id);
    gtk_widget_add_css_class(tz_label, "dim-label");
    gtk_box_append(GTK_BOX(box), tz_label);

    g_date_time_unref(now);

    clock->popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(clock->popover), box);
    gtk_widget_add_css_class(clock->popover, "shell-popover");
    gtk_widget_add_css_class(clock->popover, "shell-popover-clock");

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
