#include "settings_common.h"
#include "../ipc/ipc_client.h"
#include <gio/gunixsocketaddress.h>
#include <string.h>

static void
settings_page_state_free(gpointer data)
{
    SettingsPageState *st = data;
    if (!st)
        return;
    g_free(st->live_badge_text);
    g_free(st->live_status_text);
    g_free(st->deck_badge1);
    g_free(st->deck_badge2);
    for (int i = 0; i < 4; i++) {
        g_free(st->cnt_keys[i]);
        g_free(st->cnt_vals[i]);
        g_free(st->cnt_styles[i]);
    }
    g_free(st);
}

static SettingsPageState *
ensure_page_state(GtkWidget *page_root)
{
    SettingsPageState *st = g_object_get_data(G_OBJECT(page_root), "settings-page-state");
    if (!st) {
        st = g_new0(SettingsPageState, 1);
        g_object_set_data_full(G_OBJECT(page_root), "settings-page-state", st, settings_page_state_free);
    }
    return st;
}

static GtkWidget *
find_page_root(GtkWidget *widget)
{
    for (GtkWidget *cur = widget; cur != NULL; cur = gtk_widget_get_parent(cur)) {
        if (g_object_get_data(G_OBJECT(cur), "settings-page-state") != NULL)
            return cur;
        GtkWidget *parent = gtk_widget_get_parent(cur);
        if (parent && GTK_IS_STACK(parent))
            return cur;
    }
    return widget;
}

static SettingsApp *
find_app_ctx(GtkWidget *widget)
{
    if (!widget)
        return NULL;
    GtkRoot *root = gtk_widget_get_root(widget);
    if (!root)
        return NULL;
    return g_object_get_data(G_OBJECT(root), "app-ctx");
}

GtkWidget *
create_sidebar_category_header(const gchar *category_name)
{
    GtkWidget *lbl = gtk_label_new(category_name);
    gtk_widget_add_css_class(lbl, "sidebar-category-header");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    return lbl;
}

GtkWidget *
create_settings_nav_row(const gchar *glyph,
                        const gchar *label_text,
                        const gchar *badge_text,
                        const gchar *badge_style,
                        const gchar *tooltip_text,
                        const gchar *page_name,
                        const gchar *keywords)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "sidebar-row");
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    if (tooltip_text && tooltip_text[0] != '\0')
        gtk_widget_set_tooltip_text(row, tooltip_text);

    GtkWidget *icon_lbl = gtk_label_new(glyph ? glyph : "•");
    gtk_widget_add_css_class(icon_lbl, "sidebar-nav-icon");
    gtk_widget_set_valign(icon_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(icon_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(icon_lbl), 0.5f);

    GtkWidget *label = gtk_label_new(label_text ? label_text : "");
    gtk_widget_add_css_class(label, "sidebar-label");
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(label), 4);

    GtkWidget *badge = gtk_label_new(badge_text ? badge_text : "");
    gtk_widget_add_css_class(badge, "sidebar-nav-badge");
    if (badge_style && badge_style[0] != '\0')
        gtk_widget_add_css_class(badge, badge_style);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(badge, GTK_ALIGN_END);
    gtk_widget_set_visible(badge, badge_text != NULL && badge_text[0] != '\0');

    gtk_box_append(GTK_BOX(row), icon_lbl);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), badge);

    g_object_set_data(G_OBJECT(row), "nav-icon", icon_lbl);
    g_object_set_data(G_OBJECT(row), "nav-label", label);
    g_object_set_data(G_OBJECT(row), "nav-badge", badge);

    if (page_name)
        g_object_set_data_full(G_OBJECT(row), "page-name", g_strdup(page_name), g_free);
    if (label_text)
        g_object_set_data_full(G_OBJECT(row), "label-text", g_strdup(label_text), g_free);
    if (tooltip_text)
        g_object_set_data_full(G_OBJECT(row), "subtitle-text", g_strdup(tooltip_text), g_free);
    if (keywords)
        g_object_set_data_full(G_OBJECT(row), "keywords", g_strdup(keywords), g_free);

    return row;
}

GtkWidget *
create_sidebar_row(const gchar *icon_name,
                   const gchar *label_text,
                   const gchar *subtitle_text,
                   const gchar *page_name)
{
    return create_settings_nav_row(icon_name, label_text, NULL, NULL, subtitle_text, page_name, NULL);
}

GtkWidget *
create_sidebar_row_with_keywords(const gchar *icon_name,
                                 const gchar *label_text,
                                 const gchar *subtitle_text,
                                 const gchar *page_name,
                                 const gchar *keywords)
{
    return create_settings_nav_row(icon_name, label_text, NULL, NULL, subtitle_text, page_name, keywords);
}

/* ─── Zero-Min-Width Clamping Container (Prevents set_min_size Inflation Without Clipping) ─── */

typedef struct _SettingsFlexBin {
    GtkWidget  parent_instance;
    GtkWidget *child;
} SettingsFlexBin;

typedef struct _SettingsFlexBinClass {
    GtkWidgetClass parent_class;
} SettingsFlexBinClass;

G_DEFINE_TYPE(SettingsFlexBin, settings_flex_bin, GTK_TYPE_WIDGET)

static GtkSizeRequestMode
settings_flex_bin_get_request_mode(GtkWidget *widget G_GNUC_UNUSED)
{
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
settings_flex_bin_measure(GtkWidget     *widget,
                          GtkOrientation orientation,
                          int            for_size,
                          int           *minimum,
                          int           *natural,
                          int           *minimum_baseline,
                          int           *natural_baseline)
{
    SettingsFlexBin *self = (SettingsFlexBin *)widget;
    if (!self->child || !gtk_widget_get_visible(self->child)) {
        *minimum = 0;
        *natural = 0;
        if (minimum_baseline) *minimum_baseline = -1;
        if (natural_baseline) *natural_baseline = -1;
        return;
    }

    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        int c_min = 0, c_nat = 0;
        gtk_widget_measure(self->child, GTK_ORIENTATION_HORIZONTAL, for_size,
                           &c_min, &c_nat, NULL, NULL);
        *minimum = MIN(c_min, 240);
        *natural = c_nat;
        if (minimum_baseline) *minimum_baseline = -1;
        if (natural_baseline) *natural_baseline = -1;
        return;
    }

    int child_for_w = for_size;
    if (for_size >= 0) {
        int c_min_w = 0;
        gtk_widget_measure(self->child, GTK_ORIENTATION_HORIZONTAL, -1,
                           &c_min_w, NULL, NULL, NULL);
        if (child_for_w < c_min_w)
            child_for_w = c_min_w;
    }
    gtk_widget_measure(self->child, GTK_ORIENTATION_VERTICAL, child_for_w,
                       minimum, natural, minimum_baseline, natural_baseline);
}

static void
settings_flex_bin_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
    SettingsFlexBin *self = (SettingsFlexBin *)widget;
    if (!self->child || !gtk_widget_get_visible(self->child))
        return;

    int c_min_w = 0, c_min_h = 0;
    gtk_widget_measure(self->child, GTK_ORIENTATION_HORIZONTAL, -1, &c_min_w, NULL, NULL, NULL);
    int alloc_w = MAX(width, c_min_w);
    gtk_widget_measure(self->child, GTK_ORIENTATION_VERTICAL, alloc_w, &c_min_h, NULL, NULL, NULL);
    int alloc_h = MAX(height, c_min_h);

    gtk_widget_size_allocate(self->child, &(GtkAllocation){ 0, 0, alloc_w, alloc_h }, baseline);
}

static void
settings_flex_bin_dispose(GObject *object)
{
    SettingsFlexBin *self = (SettingsFlexBin *)object;
    g_clear_pointer(&self->child, gtk_widget_unparent);
    G_OBJECT_CLASS(settings_flex_bin_parent_class)->dispose(object);
}

static void
settings_flex_bin_init(SettingsFlexBin *self G_GNUC_UNUSED)
{
}

static void
settings_flex_bin_class_init(SettingsFlexBinClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_flex_bin_dispose;
    widget_class->get_request_mode = settings_flex_bin_get_request_mode;
    widget_class->measure = settings_flex_bin_measure;
    widget_class->size_allocate = settings_flex_bin_size_allocate;
}

GtkWidget *
settings_flex_bin_new(GtkWidget *child)
{
    SettingsFlexBin *self = g_object_new(settings_flex_bin_get_type(), NULL);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    if (child) {
        self->child = child;
        gtk_widget_set_parent(child, GTK_WIDGET(self));
    }
    return GTK_WIDGET(self);
}

/* ─── Responsive Bento Canvas (Zone 3) ─── */

GtkWidget *
settings_bento_new(void)
{
    GtkWidget *bento = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(bento, "settings-bento-canvas");
    gtk_widget_set_hexpand(bento, TRUE);
    g_object_set_data(G_OBJECT(bento), "is-bento-canvas", GINT_TO_POINTER(1));
    return bento;
}

void
settings_bento_add_full(GtkWidget *bento, GtkWidget *card)
{
    g_return_if_fail(GTK_IS_BOX(bento));
    g_return_if_fail(GTK_IS_WIDGET(card));
    gtk_widget_set_hexpand(card, TRUE);
    gtk_box_append(GTK_BOX(bento), card);
}

void
settings_bento_add_pair(GtkWidget *bento, GtkWidget *card_left, GtkWidget *card_right)
{
    g_return_if_fail(GTK_IS_BOX(bento));
    g_return_if_fail(GTK_IS_WIDGET(card_left));
    g_return_if_fail(GTK_IS_WIDGET(card_right));

    GtkWidget *pair = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_hexpand(pair, TRUE);
    g_object_set_data(G_OBJECT(pair), "is-bento-pair", GINT_TO_POINTER(1));

    gtk_widget_set_hexpand(card_left, TRUE);
    gtk_widget_set_hexpand(card_right, TRUE);
    gtk_widget_set_valign(card_left, GTK_ALIGN_FILL);
    gtk_widget_set_valign(card_right, GTK_ALIGN_FILL);

    gtk_box_append(GTK_BOX(pair), card_left);
    gtk_box_append(GTK_BOX(pair), card_right);
    gtk_box_append(GTK_BOX(bento), pair);
}

void
settings_bento_update_columns(GtkWidget *bento, gboolean allow_two_col, int avail_canvas_width)
{
    if (!bento || !GTK_IS_WIDGET(bento))
        return;

    for (GtkWidget *child = gtk_widget_get_first_child(bento);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        if (!g_object_get_data(G_OBJECT(child), "is-bento-pair"))
            continue;

        gboolean use_horiz = FALSE;
        if (allow_two_col) {
            GtkWidget *c1 = gtk_widget_get_first_child(child);
            GtkWidget *c2 = c1 ? gtk_widget_get_next_sibling(c1) : NULL;
            if (c1 && c2 && gtk_widget_get_visible(c1) && gtk_widget_get_visible(c2)) {
                int min1 = 0, min2 = 0;
                gtk_widget_measure(c1, GTK_ORIENTATION_HORIZONTAL, -1, &min1, NULL, NULL, NULL);
                gtk_widget_measure(c2, GTK_ORIENTATION_HORIZONTAL, -1, &min2, NULL, NULL, NULL);
                int max_min = MAX(min1, min2);
                int required = (2 * max_min) + 10;
                if (avail_canvas_width <= 0 || required <= avail_canvas_width)
                    use_horiz = TRUE;
            }
        }

        gtk_orientable_set_orientation(
            GTK_ORIENTABLE(child),
            use_horiz ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
        gtk_box_set_homogeneous(GTK_BOX(child), use_horiz);
    }
}

void
settings_update_descendant_scales(GtkWidget *root, gboolean narrow)
{
    if (!root || !GTK_IS_WIDGET(root))
        return;

    if (g_object_get_data(G_OBJECT(root), "is-scale-wrap")) {
        gtk_widget_set_size_request(root, narrow ? 135 : 165, -1);
    }

    if (GTK_IS_POPOVER(root))
        return;

    for (GtkWidget *c = gtk_widget_get_first_child(root);
         c != NULL;
         c = gtk_widget_get_next_sibling(c)) {
        settings_update_descendant_scales(c, narrow);
    }
}

/* ─── Canonical Section Cards ─── */

static void
apply_badge_style_class(GtkWidget *badge, const gchar *style_class)
{
    gtk_widget_remove_css_class(badge, "accent");
    gtk_widget_remove_css_class(badge, "success");
    gtk_widget_remove_css_class(badge, "urgent");
    gtk_widget_remove_css_class(badge, "peak-warn");
    if (style_class && style_class[0] != '\0')
        gtk_widget_add_css_class(badge, style_class);
}

GtkWidget *
create_section_card(SettingsCardAccent accent,
                    const gchar       *heading_text,
                    const gchar       *badge_text,
                    const gchar       *badge_style_class,
                    GtkWidget         *extra_header_widget)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "section-card");
    switch (accent) {
    case SETTINGS_CARD_LEFT:
        gtk_widget_add_css_class(card, "section-card-left");
        break;
    case SETTINGS_CARD_CENTER:
        gtk_widget_add_css_class(card, "section-card-center");
        break;
    case SETTINGS_CARD_RIGHT:
        gtk_widget_add_css_class(card, "section-card-right");
        break;
    case SETTINGS_CARD_PLAIN:
    default:
        break;
    }
    gtk_widget_set_hexpand(card, TRUE);

    if (heading_text || badge_text || extra_header_widget) {
        GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(hdr, "card-header-row");
        gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

        GtkWidget *lbl_h = gtk_label_new(heading_text ? heading_text : "");
        gtk_widget_add_css_class(lbl_h, "heading");
        gtk_widget_set_hexpand(lbl_h, TRUE);
        gtk_widget_set_halign(lbl_h, GTK_ALIGN_FILL);
        gtk_widget_set_valign(lbl_h, GTK_ALIGN_CENTER);
        gtk_label_set_xalign(GTK_LABEL(lbl_h), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_h), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(lbl_h), 6);
        gtk_box_append(GTK_BOX(hdr), lbl_h);

        GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_hexpand(right_box, FALSE);
        gtk_widget_set_halign(right_box, GTK_ALIGN_END);
        gtk_widget_set_valign(right_box, GTK_ALIGN_CENTER);

        GtkWidget *lbl_b = gtk_label_new(badge_text ? badge_text : "");
        gtk_widget_add_css_class(lbl_b, "shell-popover-badge");
        apply_badge_style_class(lbl_b, badge_style_class);
        gtk_widget_set_valign(lbl_b, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(lbl_b, badge_text != NULL && badge_text[0] != '\0');
        gtk_box_append(GTK_BOX(right_box), lbl_b);

        if (extra_header_widget) {
            gtk_widget_set_valign(extra_header_widget, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(right_box), extra_header_widget);
        }

        gtk_box_append(GTK_BOX(hdr), right_box);
        gtk_box_append(GTK_BOX(card), hdr);
        g_object_set_data(G_OBJECT(card), "card-badge-label", lbl_b);
    }

    return card;
}

void
section_card_set_badge(GtkWidget   *card,
                       const gchar *badge_text,
                       const gchar *badge_style_class)
{
    GtkWidget *lbl = card ? g_object_get_data(G_OBJECT(card), "card-badge-label") : NULL;
    if (!lbl || !GTK_IS_LABEL(lbl))
        return;
    gtk_label_set_text(GTK_LABEL(lbl), badge_text ? badge_text : "");
    apply_badge_style_class(lbl, badge_style_class);
    gtk_widget_set_visible(lbl, badge_text != NULL && badge_text[0] != '\0');
}

/* ─── Canonical Boxed Control Rows & Shrinkable Controls ─── */

GtkWidget *
create_boxed_row(const gchar *glyph_or_icon,
                 const gchar *title,
                 const gchar *subtitle,
                 GtkWidget   *right_control)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(card, "widget-row-card");
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(card, TRUE);

    if (glyph_or_icon && glyph_or_icon[0] != '\0') {
        GtkWidget *icon_box = gtk_center_box_new();
        gtk_widget_add_css_class(icon_box, "widget-row-icon");
        gtk_widget_set_size_request(icon_box, 28, 28);
        gtk_widget_set_hexpand(icon_box, FALSE);
        gtk_widget_set_halign(icon_box, GTK_ALIGN_START);
        gtk_widget_set_valign(icon_box, GTK_ALIGN_CENTER);

        if (g_str_has_suffix(glyph_or_icon, "-symbolic")) {
            GtkWidget *img = gtk_image_new_from_icon_name(glyph_or_icon);
            gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
            gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
            gtk_center_box_set_center_widget(GTK_CENTER_BOX(icon_box), img);
        } else {
            GtkWidget *gl = gtk_label_new(glyph_or_icon);
            gtk_widget_set_halign(gl, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(gl, GTK_ALIGN_CENTER);
            gtk_label_set_xalign(GTK_LABEL(gl), 0.5f);
            gtk_center_box_set_center_widget(GTK_CENTER_BOX(icon_box), gl);
        }
        gtk_box_append(GTK_BOX(card), icon_box);
    }

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl_title = gtk_label_new(title ? title : "");
    gtk_widget_add_css_class(lbl_title, "row-title");
    gtk_widget_set_hexpand(lbl_title, TRUE);
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_FILL);
    gtk_widget_set_valign(lbl_title, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(lbl_title), 6);
    gtk_box_append(GTK_BOX(vbox), lbl_title);
    g_object_set_data(G_OBJECT(card), "row-title-label", lbl_title);

    GtkWidget *lbl_sub = gtk_label_new(subtitle ? subtitle : "");
    gtk_widget_add_css_class(lbl_sub, "row-subtitle");
    gtk_widget_set_hexpand(lbl_sub, TRUE);
    gtk_widget_set_halign(lbl_sub, GTK_ALIGN_FILL);
    gtk_widget_set_valign(lbl_sub, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(lbl_sub), 6);
    gtk_widget_set_visible(lbl_sub, subtitle != NULL && subtitle[0] != '\0');
    gtk_box_append(GTK_BOX(vbox), lbl_sub);
    g_object_set_data(G_OBJECT(card), "row-subtitle-label", lbl_sub);

    gtk_box_append(GTK_BOX(card), vbox);

    if (right_control) {
        gtk_widget_set_valign(right_control, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(card), right_control);
    }

    return card;
}

void
boxed_row_set_subtitle(GtkWidget *row, const gchar *subtitle)
{
    GtkWidget *lbl = row ? g_object_get_data(G_OBJECT(row), "row-subtitle-label") : NULL;
    if (!lbl || !GTK_IS_LABEL(lbl))
        return;
    gtk_label_set_text(GTK_LABEL(lbl), subtitle ? subtitle : "");
    gtk_widget_set_visible(lbl, subtitle != NULL && subtitle[0] != '\0');
}

void
boxed_row_set_title(GtkWidget *row, const gchar *title)
{
    GtkWidget *lbl = row ? g_object_get_data(G_OBJECT(row), "row-title-label") : NULL;
    if (!lbl || !GTK_IS_LABEL(lbl))
        return;
    gtk_label_set_text(GTK_LABEL(lbl), title ? title : "");
}

GtkWidget *
create_action_row(const gchar *icon_name,
                  const gchar *title,
                  const gchar *subtitle,
                  GtkWidget   *control)
{
    return create_boxed_row(icon_name, title, subtitle, control);
}

static void
ellipsize_dropdown_button_labels(GtkWidget *w)
{
    if (!w || GTK_IS_POPOVER(w))
        return;
    if (GTK_IS_LABEL(w)) {
        gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(w), 0.0f);
        gtk_label_set_width_chars(GTK_LABEL(w), 6);
    }
    for (GtkWidget *c = gtk_widget_get_first_child(w);
         c != NULL;
         c = gtk_widget_get_next_sibling(c)) {
        ellipsize_dropdown_button_labels(c);
    }
}

static void
on_dropdown_face_refresh(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    ellipsize_dropdown_button_labels(GTK_WIDGET(obj));
}

static void
on_dropdown_realize(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    ellipsize_dropdown_button_labels(widget);
}

void
settings_make_dropdown_shrinkable(GtkWidget *dropdown)
{
    if (!dropdown || !GTK_IS_DROP_DOWN(dropdown))
        return;
    ellipsize_dropdown_button_labels(dropdown);
    g_signal_connect(dropdown, "realize", G_CALLBACK(on_dropdown_realize), NULL);
    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_dropdown_face_refresh), NULL);
    g_signal_connect(dropdown, "notify::model", G_CALLBACK(on_dropdown_face_refresh), NULL);
}

GtkWidget *
create_string_dropdown(const char *const items[])
{
    GtkWidget *dropdown = gtk_drop_down_new_from_strings(items);
    settings_make_dropdown_shrinkable(dropdown);
    return dropdown;
}

static void
on_scale_wrap_value_changed(GtkRange *range, gpointer user_data)
{
    GtkWidget *badge = GTK_WIDGET(user_data);
    const gchar *fmt = g_object_get_data(G_OBJECT(badge), "badge-fmt");
    if (!fmt)
        return;
    double val = gtk_range_get_value(range);
    g_autofree gchar *txt = g_strdup_printf(fmt, val);
    gtk_label_set_text(GTK_LABEL(badge), txt);
}

GtkWidget *
create_scale_with_badge(double       min,
                        double       max,
                        double       step,
                        double       initial,
                        const gchar *badge_fmt,
                        GtkWidget  **out_scale,
                        GtkWidget  **out_badge)
{
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(wrap, "kajo-scale-wrap");
    gtk_widget_set_valign(wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(wrap, 165, -1);
    g_object_set_data(G_OBJECT(wrap), "is-scale-wrap", GINT_TO_POINTER(1));

    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step > 0.0 ? step : 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_value(GTK_RANGE(scale), initial);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);

    g_autofree gchar *init_txt = badge_fmt ? g_strdup_printf(badge_fmt, initial) : g_strdup("");
    GtkWidget *badge = gtk_label_new(init_txt);
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    gtk_widget_add_css_class(badge, "accent");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    if (badge_fmt) {
        g_object_set_data_full(G_OBJECT(badge), "badge-fmt", g_strdup(badge_fmt), g_free);
        g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale_wrap_value_changed), badge);
    }

    gtk_box_append(GTK_BOX(wrap), scale);
    gtk_box_append(GTK_BOX(wrap), badge);

    if (out_scale) *out_scale = scale;
    if (out_badge) *out_badge = badge;
    return wrap;
}

/* ─── Toggle Card (gallery: "Toggle Cards") ─── */
GtkWidget *
create_toggle_card(const gchar *icon_name,
                   const gchar *title,
                   const gchar *subtitle,
                   gboolean     active)
{
    GtkWidget *btn = gtk_toggle_button_new();
    gtk_widget_add_css_class(btn, "toggle-card");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), active);
    gtk_widget_set_hexpand(btn, TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    if (icon_name && icon_name[0] != '\0') {
        if (g_str_has_suffix(icon_name, "-symbolic")) {
            GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
            gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(hbox), icon);
        } else {
            GtkWidget *gl = gtk_label_new(icon_name);
            gtk_widget_set_valign(gl, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(hbox), gl);
        }
    }

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl_title = gtk_label_new(title);
    gtk_widget_add_css_class(lbl_title, "tile-title");
    gtk_widget_set_hexpand(lbl_title, TRUE);
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_FILL);
    gtk_widget_set_valign(lbl_title, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(lbl_title), 6);

    GtkWidget *lbl_sub = gtk_label_new(subtitle ? subtitle : "");
    gtk_widget_add_css_class(lbl_sub, "tile-subtitle");
    gtk_widget_set_hexpand(lbl_sub, TRUE);
    gtk_widget_set_halign(lbl_sub, GTK_ALIGN_FILL);
    gtk_widget_set_valign(lbl_sub, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(lbl_sub), 6);
    g_object_set_data(G_OBJECT(btn), "tile-subtitle-label", lbl_sub);

    gtk_box_append(GTK_BOX(vbox), lbl_title);
    gtk_box_append(GTK_BOX(vbox), lbl_sub);
    gtk_box_append(GTK_BOX(hbox), vbox);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);

    return btn;
}

void
toggle_card_set_subtitle(GtkWidget *card, const gchar *subtitle)
{
    GtkWidget *lbl = card ? g_object_get_data(G_OBJECT(card), "tile-subtitle-label") : NULL;
    if (lbl && GTK_IS_LABEL(lbl))
        gtk_label_set_text(GTK_LABEL(lbl), subtitle ? subtitle : "");
}

/* ─── Segmented Group (gallery §04) ─── */

typedef struct {
    SegmentedChangedFunc cb;
    gpointer             user_data;
    guint                selected;
    gboolean             syncing;
} SegmentedState;

static void
on_segment_toggled(GtkToggleButton *btn, gpointer user_data)
{
    GtkWidget *group = GTK_WIDGET(user_data);
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    if (!st || !gtk_toggle_button_get_active(btn))
        return;

    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "segment-index"));
    if (idx == st->selected)
        return;
    st->selected = idx;
    if (!st->syncing && st->cb)
        st->cb(group, idx, st->user_data);
}

GtkWidget *
create_segmented_group(const char *const labels[], guint selected)
{
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(group, "segmented-group");
    gtk_widget_set_valign(group, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(group, GTK_ALIGN_END);

    SegmentedState *st = g_new0(SegmentedState, 1);
    st->selected = selected;
    g_object_set_data_full(G_OBJECT(group), "segmented-state", st, g_free);

    GtkWidget *first = NULL;
    for (guint i = 0; labels && labels[i]; i++) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(labels[i]);
        gtk_widget_add_css_class(btn, "files-nav-btn");
        gtk_widget_set_focus_on_click(btn, FALSE);
        g_object_set_data(G_OBJECT(btn), "segment-index", GUINT_TO_POINTER(i));
        if (first)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), GTK_TOGGLE_BUTTON(first));
        else
            first = btn;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), i == selected);
        g_signal_connect(btn, "toggled", G_CALLBACK(on_segment_toggled), group);
        gtk_box_append(GTK_BOX(group), btn);
    }
    return group;
}

void
segmented_group_connect(GtkWidget *group, SegmentedChangedFunc cb, gpointer user_data)
{
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    if (!st) return;
    st->cb = cb;
    st->user_data = user_data;
}

guint
segmented_group_get_selected(GtkWidget *group)
{
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    return st ? st->selected : 0;
}

void
segmented_group_set_selected(GtkWidget *group, guint idx)
{
    SegmentedState *st = g_object_get_data(G_OBJECT(group), "segmented-state");
    if (!st) return;
    st->syncing = TRUE;
    for (GtkWidget *c = gtk_widget_get_first_child(group); c; c = gtk_widget_get_next_sibling(c)) {
        if (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(c), "segment-index")) == idx)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), TRUE);
    }
    st->selected = idx;
    st->syncing = FALSE;
}

GtkWidget *
create_simple_item_row(const char *icon_name,
                       const char *title,
                       const char *subtitle,
                       GtkWidget  *action_btn)
{
    return create_boxed_row(icon_name, title, subtitle, action_btn);
}

GtkWidget *
create_action_button(const char *label,
                     const char *payload_key,
                     const char *payload_val,
                     GCallback   callback,
                     gpointer    user_data)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "files-nav-btn");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);

    if (payload_key && payload_val) {
        g_object_set_data_full(G_OBJECT(btn), payload_key, g_strdup(payload_val), g_free);
    }

    if (callback) {
        g_signal_connect(btn, "clicked", callback, user_data);
    }

    return btn;
}

GtkWidget *
create_empty_state_label(const char *message)
{
    GtkWidget *lbl = gtk_label_new(message);
    gtk_widget_add_css_class(lbl, "empty-state-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
    return lbl;
}

/* ─── Zone 2 Telemetry & Zone 4 Footer Binding API ─── */

static void
sync_if_active_page(GtkWidget *page_root)
{
    SettingsApp *app_ctx = find_app_ctx(page_root);
    if (app_ctx && app_ctx->stack &&
        gtk_stack_get_visible_child(GTK_STACK(app_ctx->stack)) == page_root) {
        settings_window_sync_active_page_chrome(app_ctx);
    }
}

void
settings_page_bind_actions(GtkWidget         *page_root,
                           const gchar       *secondary_label,
                           SettingsActionFunc on_secondary,
                           SettingsActionFunc on_revert,
                           SettingsActionFunc on_save,
                           gpointer           user_data)
{
    g_return_if_fail(GTK_IS_WIDGET(page_root));
    SettingsPageState *st = ensure_page_state(page_root);
    st->secondary_label = secondary_label;
    st->on_secondary    = on_secondary;
    st->on_revert       = on_revert;
    st->on_save         = on_save;
    st->user_data       = user_data;
    st->has_save_revert = (on_save != NULL);
    sync_if_active_page(page_root);
}

void
settings_page_set_dirty(GtkWidget *page_widget, gboolean dirty)
{
    g_return_if_fail(GTK_IS_WIDGET(page_widget));
    GtkWidget *root = find_page_root(page_widget);
    SettingsPageState *st = ensure_page_state(root);
    st->is_dirty = dirty;
    sync_if_active_page(root);
}

void
settings_page_set_live_status(GtkWidget   *page_widget,
                              const gchar *badge_text,
                              const gchar *status_text)
{
    g_return_if_fail(GTK_IS_WIDGET(page_widget));
    GtkWidget *root = find_page_root(page_widget);
    SettingsPageState *st = ensure_page_state(root);

    gchar *new_badge = g_strdup(badge_text);
    g_free(st->live_badge_text);
    st->live_badge_text = new_badge;

    gchar *new_status = g_strdup(status_text);
    g_free(st->live_status_text);
    st->live_status_text = new_status;

    sync_if_active_page(root);
}

void
settings_page_set_counter(GtkWidget   *page_widget,
                          int          cell_idx,
                          const gchar *key_text,
                          const gchar *val_text,
                          const gchar *style_class)
{
    g_return_if_fail(GTK_IS_WIDGET(page_widget));
    if (cell_idx < 0 || cell_idx >= 4)
        return;

    GtkWidget *root = find_page_root(page_widget);
    SettingsPageState *st = ensure_page_state(root);

    if (key_text) {
        gchar *nk = g_strdup(key_text);
        g_free(st->cnt_keys[cell_idx]);
        st->cnt_keys[cell_idx] = nk;
    }
    if (val_text) {
        gchar *nv = g_strdup(val_text);
        g_free(st->cnt_vals[cell_idx]);
        st->cnt_vals[cell_idx] = nv;
    }
    gchar *ns = g_strdup(style_class);
    g_free(st->cnt_styles[cell_idx]);
    st->cnt_styles[cell_idx] = ns;

    sync_if_active_page(root);
}

void
settings_page_set_deck_badges(GtkWidget   *page_widget,
                              const gchar *badge1,
                              const gchar *badge2)
{
    g_return_if_fail(GTK_IS_WIDGET(page_widget));
    GtkWidget *root = find_page_root(page_widget);
    SettingsPageState *st = ensure_page_state(root);

    if (badge1) {
        gchar *b1 = g_strdup(badge1);
        g_free(st->deck_badge1);
        st->deck_badge1 = b1;
    }
    if (badge2) {
        gchar *b2 = g_strdup(badge2);
        g_free(st->deck_badge2);
        st->deck_badge2 = b2;
    }

    sync_if_active_page(root);
}

void
settings_page_set_nav_badge(GtkWidget   *any_widget_in_win,
                            const gchar *page_name,
                            const gchar *badge_text,
                            const gchar *badge_style)
{
    SettingsApp *app_ctx = find_app_ctx(any_widget_in_win);
    if (!app_ctx || !app_ctx->sidebar_list || !page_name)
        return;

    for (GtkWidget *row = gtk_widget_get_first_child(app_ctx->sidebar_list);
         row != NULL;
         row = gtk_widget_get_next_sibling(row)) {
        if (!GTK_IS_LIST_BOX_ROW(row))
            continue;
        GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        if (!child)
            continue;
        const gchar *name = g_object_get_data(G_OBJECT(child), "page-name");
        if (g_strcmp0(name, page_name) == 0) {
            GtkWidget *badge = g_object_get_data(G_OBJECT(child), "nav-badge");
            if (badge && GTK_IS_LABEL(badge)) {
                gtk_label_set_text(GTK_LABEL(badge), badge_text ? badge_text : "");
                apply_badge_style_class(badge, badge_style);
                gtk_widget_set_visible(
                    badge,
                    !app_ctx->is_compact_rail && badge_text != NULL && badge_text[0] != '\0');
            }
            return;
        }
    }
}

/* ─── IPC, Theme & String Utilities ─── */

const gchar *
settings_accent_short_name(const gchar *hex_color)
{
    if (!hex_color)
        return "COBALT";
    static const char *const short_names[SHELL_N_ACCENT_PRESETS] = {
        "COBALT", "PURPLE", "LAVENDER", "FROST",
        "EMERALD", "CRIMSON", "AMBER", "CYAN"
    };
    for (int i = 0; i < SHELL_N_ACCENT_PRESETS; i++) {
        if (g_ascii_strcasecmp(hex_color, shell_accent_presets[i].accent) == 0)
            return short_names[i];
    }
    return "CUSTOM";
}

gboolean
is_mac_address_string(const char *str)
{
    if (!str || strlen(str) != 17) return FALSE;
    return (str[2] == ':' && str[5] == ':' && str[8] == ':' && str[11] == ':' && str[14] == ':');
}

gchar *
format_mac_address_from_bluez_path(const char *path)
{
    if (!path) return g_strdup("--");
    const char *dev = strstr(path, "dev_");
    if (dev && strlen(dev) >= 21) {
        dev += 4;
        return g_strdup_printf("%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
            dev[0], dev[1], dev[3], dev[4], dev[6], dev[7],
            dev[9], dev[10], dev[12], dev[13], dev[15], dev[16]);
    }
    return g_strdup(path);
}

gboolean
settings_send_ipc_command(const gchar *command)
{
    return shell_ipc_client_send(command);
}

static ShellTheme *g_shared_settings_theme = NULL;

void
settings_set_global_theme(ShellTheme *theme)
{
    g_shared_settings_theme = theme;
}

void
settings_reload_global_theme(void)
{
    if (g_shared_settings_theme) {
        shell_theme_reload(g_shared_settings_theme);
    }
}
