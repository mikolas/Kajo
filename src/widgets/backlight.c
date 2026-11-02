#include <gtk/gtk.h>
#include "widget.h"
#include "../config.h"
#include "../hardware/hw_caps.h"
#include "../hardware/kbd_backlight.h"
#include "../osd/osd.h"
#include "../posture/posture_manager.h"
#include "../shell.h"

typedef struct {
    ShellWidget  base;
    GtkWidget   *button;
    GtkWidget   *popover;
    GtkWidget   *panel_box;
    GtkWidget   *panel_image;
    GtkWidget   *panel_label;

    /* Popover child widgets */
    GtkWidget   *status_badge;
    GtkWidget   *box_display;
    GtkWidget   *lbl_display_pct;
    GtkWidget   *scale_display;
    GtkWidget   *box_kbd;
    GtkWidget   *lbl_kbd_status;
    GtkWidget   *btn_kbd_wake;
    GtkWidget   *box_kbd_levels;
    GtkWidget   *btn_kbd_off;
    GtkWidget   *btn_kbd_low;
    GtkWidget   *btn_kbd_high;
    GtkWidget   *box_kbd_timeout;
    GtkWidget   *btn_timeout_always;
    GtkWidget   *btn_timeout_30s;
    GtkWidget   *btn_timeout_3m;
    GtkWidget   *box_posture;
    GtkWidget   *lbl_posture_status;
    GtkWidget   *sw_autorotate;

    gboolean     updating_ui;
} BacklightWidget;

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellBacklightPopover {
    GtkPopover parent_instance;

    GtkWidget *title_label;
    GtkWidget *status_badge;
    GtkWidget *box_display;
    GtkWidget *lbl_display_pct;
    GtkWidget *scale_display;
    GtkWidget *box_kbd;
    GtkWidget *lbl_kbd_status;
    GtkWidget *btn_kbd_wake;
    GtkWidget *box_kbd_levels;
    GtkWidget *btn_kbd_off;
    GtkWidget *btn_kbd_low;
    GtkWidget *btn_kbd_high;
    GtkWidget *box_kbd_timeout;
    GtkWidget *btn_timeout_always;
    GtkWidget *btn_timeout_30s;
    GtkWidget *btn_timeout_3m;
    GtkWidget *box_posture;
    GtkWidget *lbl_posture_status;
    GtkWidget *sw_autorotate;
} ShellBacklightPopover;

typedef struct _ShellBacklightPopoverClass {
    GtkPopoverClass parent_class;
} ShellBacklightPopoverClass;

G_DEFINE_TYPE(ShellBacklightPopover, shell_backlight_popover, GTK_TYPE_POPOVER)

static void
shell_backlight_popover_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), shell_backlight_popover_get_type());
    G_OBJECT_CLASS(shell_backlight_popover_parent_class)->dispose(object);
}

static void
shell_backlight_popover_init(ShellBacklightPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_backlight_popover_class_init(ShellBacklightPopoverClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = shell_backlight_popover_dispose;

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/backlight_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, title_label);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, status_badge);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, box_display);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, lbl_display_pct);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, scale_display);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, box_kbd);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, lbl_kbd_status);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, btn_kbd_wake);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, box_kbd_levels);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, btn_kbd_off);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, btn_kbd_low);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, btn_kbd_high);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, box_kbd_timeout);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, btn_timeout_always);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, btn_timeout_30s);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, btn_timeout_3m);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, box_posture);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, lbl_posture_status);
    gtk_widget_class_bind_template_child(widget_class, ShellBacklightPopover, sw_autorotate);
}

static void
set_active_button_style(GtkWidget *btn, gboolean active)
{
    if (!btn)
        return;
    if (active)
        gtk_widget_add_css_class(btn, "active");
    else
        gtk_widget_remove_css_class(btn, "active");
}

static void
backlight_save_config_Order(BacklightWidget *bl, gint kbd_level, gint kbd_timeout_sec, gint auto_rotate)
{
    ShellConfig *cfg = bl->base.app ? shell_app_get_config(bl->base.app) : NULL;
    gboolean own_cfg = FALSE;
    if (!cfg) {
        cfg = shell_config_load(NULL);
        own_cfg = TRUE;
    }
    if (!cfg)
        return;

    if (kbd_level >= 0)
        cfg->kbd_backlight_level = kbd_level;
    if (kbd_timeout_sec >= 0)
        cfg->kbd_backlight_timeout_sec = kbd_timeout_sec;
    if (auto_rotate >= 0)
        cfg->posture_auto_rotate = (auto_rotate != 0);

    g_autofree gchar *cfg_path = g_build_filename(g_get_user_config_dir(), "kajo", "config.json", NULL);
    shell_config_save(cfg, cfg_path);

    if (own_cfg)
        shell_config_destroy(cfg);
}

static void
backlight_refresh_ui(BacklightWidget *bl)
{
    if (!bl)
        return;

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    bl->updating_ui = TRUE;

    /* 1. Display Brightness */
    gboolean has_disp = (caps && caps->has_display_backlight);
    if (bl->box_display)
        gtk_widget_set_visible(bl->box_display, has_disp);

    if (has_disp) {
        gint pct = shell_hw_caps_get_display_brightness_pct(caps);
        if (pct >= 0) {
            g_autofree gchar *pct_str = g_strdup_printf("%d%%", pct);
            gtk_label_set_text(GTK_LABEL(bl->panel_label), pct_str);
            if (bl->lbl_display_pct)
                gtk_label_set_text(GTK_LABEL(bl->lbl_display_pct), pct_str);
            if (bl->status_badge) {
                g_autofree gchar *badge = g_strdup_printf("[ %d%% ]", pct);
                gtk_label_set_text(GTK_LABEL(bl->status_badge), badge);
            }
            if (bl->scale_display)
                gtk_range_set_value(GTK_RANGE(bl->scale_display), (gdouble)pct);
        }
    }

    /* 2. Keyboard Backlight & Timeout */
    gboolean has_kbd = shell_kbd_backlight_available();
    if (bl->box_kbd)
        gtk_widget_set_visible(bl->box_kbd, has_kbd);

    if (has_kbd) {
        gint lvl = shell_kbd_backlight_get_level();
        gboolean is_hp_ec = (caps && caps->kbd_bl_backend == KBD_BL_HP_EC);

        if (bl->lbl_kbd_status) {
            const gchar *lvl_name = (lvl <= 0) ? "Off" : ((lvl == 1) ? "Low (50%)" : "High (100%)");
            g_autofree gchar *st = is_hp_ec
                ? g_strdup_printf("Status: %s (Use F4 to cycle brightness)", lvl_name)
                : g_strdup_printf("Status: %s", lvl_name);
            gtk_label_set_text(GTK_LABEL(bl->lbl_kbd_status), st);
        }

        if (bl->btn_kbd_wake)
            gtk_widget_set_visible(bl->btn_kbd_wake, is_hp_ec && lvl <= 0);
        if (bl->box_kbd_levels)
            gtk_widget_set_visible(bl->box_kbd_levels, !is_hp_ec);

        set_active_button_style(bl->btn_kbd_off,  lvl == 0);
        set_active_button_style(bl->btn_kbd_low,  lvl == 1);
        set_active_button_style(bl->btn_kbd_high, lvl >= 2);

        gboolean has_timeout = shell_kbd_backlight_has_timeout();
        if (bl->box_kbd_timeout)
            gtk_widget_set_visible(bl->box_kbd_timeout, has_timeout);

        if (has_timeout) {
            gint tsec = shell_kbd_backlight_get_timeout_sec();
            set_active_button_style(bl->btn_timeout_always, tsec == 0);
            set_active_button_style(bl->btn_timeout_30s,    tsec > 0 && tsec <= 60);
            set_active_button_style(bl->btn_timeout_3m,     tsec > 60);
        }
    }

    /* 3. 360° Foldable Posture */
    gboolean has_posture = (caps && (caps->has_hinge_sensor || caps->has_accel_sensor));
    if (bl->box_posture)
        gtk_widget_set_visible(bl->box_posture, has_posture);

    if (has_posture) {
        gdouble deg = 0.0;
        if (shell_hw_caps_read_hinge_angle_deg(caps, &deg)) {
            const gchar *mode_name = (deg >= 180.0) ? "Tablet / Tent Mode" : "Laptop Mode";
            g_autofree gchar *status = g_strdup_printf("Posture: %s (%.0f°)", mode_name, deg);
            if (bl->lbl_posture_status)
                gtk_label_set_text(GTK_LABEL(bl->lbl_posture_status), status);
        }

        ShellConfig *cfg = bl->base.app ? shell_app_get_config(bl->base.app) : NULL;
        if (cfg && bl->sw_autorotate)
            gtk_switch_set_active(GTK_SWITCH(bl->sw_autorotate), cfg->posture_auto_rotate);
    }

    shell_widget_apply_mode_visibility(bl->base.mode, bl->panel_image, bl->panel_label);
    bl->updating_ui = FALSE;
}

static void
on_scale_display_changed(GtkRange *range, gpointer user_data)
{
    BacklightWidget *bl = (BacklightWidget *)user_data;
    if (!bl || bl->updating_ui)
        return;

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    gint pct = (gint)gtk_range_get_value(range);
    shell_hw_caps_set_display_brightness_pct(caps, pct);

    g_autofree gchar *pct_str = g_strdup_printf("%d%%", pct);
    gtk_label_set_text(GTK_LABEL(bl->panel_label), pct_str);
    if (bl->lbl_display_pct)
        gtk_label_set_text(GTK_LABEL(bl->lbl_display_pct), pct_str);
    if (bl->status_badge) {
        g_autofree gchar *badge = g_strdup_printf("[ %d%% ]", pct);
        gtk_label_set_text(GTK_LABEL(bl->status_badge), badge);
    }
}

static void
apply_kbd_level(BacklightWidget *bl, gint level)
{
    if (!bl)
        return;
    shell_kbd_backlight_set_level(level);
    backlight_save_config_Order(bl, level, -1, -1);
    backlight_refresh_ui(bl);
}

static void
on_btn_kbd_off_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    apply_kbd_level((BacklightWidget *)user_data, 0);
}

static void
on_btn_kbd_low_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    apply_kbd_level((BacklightWidget *)user_data, 1);
}

static void
on_btn_kbd_high_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    apply_kbd_level((BacklightWidget *)user_data, 2);
}

static void
apply_kbd_timeout(BacklightWidget *bl, gint timeout_sec)
{
    if (!bl)
        return;
    shell_kbd_backlight_set_timeout_sec(timeout_sec);
    backlight_save_config_Order(bl, -1, timeout_sec, -1);
    backlight_refresh_ui(bl);
}

static void
on_btn_timeout_always_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    apply_kbd_timeout((BacklightWidget *)user_data, 0);
}

static void
on_btn_timeout_30s_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    apply_kbd_timeout((BacklightWidget *)user_data, 30);
}

static void
on_btn_timeout_3m_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    apply_kbd_timeout((BacklightWidget *)user_data, 180);
}

static gboolean
on_autorotate_state_set(GtkSwitch *sw G_GNUC_UNUSED, gboolean state, gpointer user_data)
{
    BacklightWidget *bl = (BacklightWidget *)user_data;
    if (!bl || bl->updating_ui)
        return FALSE;

    backlight_save_config_Order(bl, -1, -1, state ? 1 : 0);
    if (bl->base.app && bl->base.app->posture_manager) {
        shell_posture_manager_reload_config(bl->base.app->posture_manager);
    }
    return FALSE;
}

static void
on_popover_mapped(GtkWidget *popover G_GNUC_UNUSED, gpointer user_data)
{
    BacklightWidget *bl = (BacklightWidget *)user_data;
    backlight_refresh_ui(bl);
}

static gboolean
on_panel_button_scroll(GtkEventControllerScroll *ctrl G_GNUC_UNUSED,
                       gdouble                   dx G_GNUC_UNUSED,
                       gdouble                   dy,
                       gpointer                  user_data)
{
    BacklightWidget *bl = (BacklightWidget *)user_data;
    if (!bl)
        return FALSE;

    gint delta = (dy < 0.0) ? 5 : -5;
    if (bl->base.app) {
        shell_brightness_change(bl->base.app, delta);
    } else {
        const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
        gint cur = shell_hw_caps_get_display_brightness_pct(caps);
        if (cur >= 0)
            shell_hw_caps_set_display_brightness_pct(caps, CLAMP(cur + delta, 1, 100));
    }
    backlight_refresh_ui(bl);
    return TRUE;
}

static GtkWidget *
backlight_build_popover(BacklightWidget *bl)
{
    ShellBacklightPopover *pop = g_object_new(shell_backlight_popover_get_type(), NULL);

    bl->status_badge       = pop->status_badge;
    bl->box_display        = pop->box_display;
    bl->lbl_display_pct    = pop->lbl_display_pct;
    bl->scale_display      = pop->scale_display;
    bl->box_kbd            = pop->box_kbd;
    bl->lbl_kbd_status     = pop->lbl_kbd_status;
    bl->btn_kbd_wake       = pop->btn_kbd_wake;
    bl->box_kbd_levels     = pop->box_kbd_levels;
    bl->btn_kbd_off        = pop->btn_kbd_off;
    bl->btn_kbd_low        = pop->btn_kbd_low;
    bl->btn_kbd_high       = pop->btn_kbd_high;
    bl->box_kbd_timeout    = pop->box_kbd_timeout;
    bl->btn_timeout_always = pop->btn_timeout_always;
    bl->btn_timeout_30s    = pop->btn_timeout_30s;
    bl->btn_timeout_3m     = pop->btn_timeout_3m;
    bl->box_posture        = pop->box_posture;
    bl->lbl_posture_status = pop->lbl_posture_status;
    bl->sw_autorotate      = pop->sw_autorotate;

    g_signal_connect(bl->scale_display, "value-changed", G_CALLBACK(on_scale_display_changed), bl);
    g_signal_connect(bl->btn_kbd_wake, "clicked", G_CALLBACK(on_btn_kbd_high_clicked), bl);
    g_signal_connect(bl->btn_kbd_off, "clicked", G_CALLBACK(on_btn_kbd_off_clicked), bl);
    g_signal_connect(bl->btn_kbd_low, "clicked", G_CALLBACK(on_btn_kbd_low_clicked), bl);
    g_signal_connect(bl->btn_kbd_high, "clicked", G_CALLBACK(on_btn_kbd_high_clicked), bl);
    g_signal_connect(bl->btn_timeout_always, "clicked", G_CALLBACK(on_btn_timeout_always_clicked), bl);
    g_signal_connect(bl->btn_timeout_30s, "clicked", G_CALLBACK(on_btn_timeout_30s_clicked), bl);
    g_signal_connect(bl->btn_timeout_3m, "clicked", G_CALLBACK(on_btn_timeout_3m_clicked), bl);
    g_signal_connect(bl->sw_autorotate, "state-set", G_CALLBACK(on_autorotate_state_set), bl);
    g_signal_connect(GTK_WIDGET(pop), "map", G_CALLBACK(on_popover_mapped), bl);

    return GTK_WIDGET(pop);
}

static ShellWidget *
backlight_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    BacklightWidget *bl = g_new0(BacklightWidget, 1);

    bl->panel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    bl->panel_image = gtk_image_new_from_icon_name("display-brightness-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(bl->panel_image), shell_widget_get_icon_size((ShellWidget *)bl));
    bl->panel_label = gtk_label_new("--%");
    gtk_box_append(GTK_BOX(bl->panel_box), bl->panel_image);
    gtk_box_append(GTK_BOX(bl->panel_box), bl->panel_label);

    bl->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(bl->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(bl->button), bl->panel_box);
    gtk_widget_add_css_class(bl->button, "flat");
    gtk_widget_add_css_class(bl->button, "shell-widget");
    gtk_widget_add_css_class(bl->button, "shell-widget-backlight");

    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_panel_button_scroll), bl);
    gtk_widget_add_controller(bl->button, scroll_ctrl);

    bl->popover = backlight_build_popover(bl);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(bl->button), bl->popover);

    return (ShellWidget *)bl;
}

static void
backlight_destroy(ShellWidget *widget)
{
    BacklightWidget *bl = (BacklightWidget *)widget;
    if (!bl)
        return;

    bl->updating_ui = TRUE;
    if (bl->scale_display)
        g_signal_handlers_disconnect_by_data(bl->scale_display, bl);
    if (bl->btn_kbd_wake)
        g_signal_handlers_disconnect_by_data(bl->btn_kbd_wake, bl);
    if (bl->btn_kbd_off)
        g_signal_handlers_disconnect_by_data(bl->btn_kbd_off, bl);
    if (bl->btn_kbd_low)
        g_signal_handlers_disconnect_by_data(bl->btn_kbd_low, bl);
    if (bl->btn_kbd_high)
        g_signal_handlers_disconnect_by_data(bl->btn_kbd_high, bl);
    if (bl->btn_timeout_always)
        g_signal_handlers_disconnect_by_data(bl->btn_timeout_always, bl);
    if (bl->btn_timeout_30s)
        g_signal_handlers_disconnect_by_data(bl->btn_timeout_30s, bl);
    if (bl->btn_timeout_3m)
        g_signal_handlers_disconnect_by_data(bl->btn_timeout_3m, bl);
    if (bl->sw_autorotate)
        g_signal_handlers_disconnect_by_data(bl->sw_autorotate, bl);
    if (bl->popover)
        g_signal_handlers_disconnect_by_data(bl->popover, bl);

    g_free(bl);
}

static GtkWidget *
backlight_get_widget(ShellWidget *widget)
{
    return ((BacklightWidget *)widget)->button;
}

static void
backlight_enable(ShellWidget *widget)
{
    BacklightWidget *bl = (BacklightWidget *)widget;
    if (!bl)
        return;

    if (bl->panel_image) {
        gtk_image_set_pixel_size(GTK_IMAGE(bl->panel_image), shell_widget_get_icon_size(widget));
    }

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    gboolean supported = caps && (caps->has_display_backlight ||
                                  caps->kbd_bl_backend != KBD_BL_NONE ||
                                  caps->has_hinge_sensor ||
                                  caps->has_accel_sensor);
    gtk_widget_set_visible(bl->button, supported);
    if (supported) {
        backlight_refresh_ui(bl);
    }
}

static void
backlight_disable(ShellWidget *widget G_GNUC_UNUSED)
{
}

const ShellWidgetClass backlight_widget_class = {
    .id         = "backlight",
    .name       = "Backlight",
    .create     = backlight_create,
    .destroy    = backlight_destroy,
    .get_widget = backlight_get_widget,
    .enable     = backlight_enable,
    .disable    = backlight_disable,
};
