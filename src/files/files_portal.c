#include "files_portal.h"
#include <string.h>

static const gchar k_filechooser_introspection_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.FileChooser'>"
    "    <method name='OpenFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFiles'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static const gchar k_request_introspection_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Request'>"
    "    <method name='Close'/>"
    "  </interface>"
    "</node>";

struct _KajoPortalActiveRequest {
    KajoFilesPortal          *portal;
    GDBusMethodInvocation    *invocation;
    char                     *handle_path;
    guint                     req_reg_id;
    KajoPortalChooserRequest *spec;
    KajoPortalCloseFunc       close_cb;
    gpointer                  close_user_data;
    gboolean                  finished;
};

struct _KajoFilesPortal {
    GDBusConnection       *connection;
    GDBusNodeInfo         *fc_introspection;
    GDBusNodeInfo         *req_introspection;
    guint                  bus_name_id;
    guint                  fc_reg_id;
    KajoPortalPresentFunc  present_cb;
    gpointer               user_data;
    GPtrArray             *active_requests; /* KajoPortalActiveRequest* */
};

/* ─── Pure Helpers & Option Parsing ─── */

static void
filter_rule_free(gpointer data)
{
    KajoPortalFilterRule *r = data;
    if (!r) return;
    g_free(r->pattern);
    g_free(r);
}

static void
filter_free(gpointer data)
{
    KajoPortalFilter *f = data;
    if (!f) return;
    g_free(f->label);
    g_clear_pointer(&f->rules, g_ptr_array_unref);
    g_free(f);
}

static void
choice_option_free(gpointer data)
{
    KajoPortalChoiceOption *o = data;
    if (!o) return;
    g_free(o->id);
    g_free(o->label);
    g_free(o);
}

static void
choice_free(gpointer data)
{
    KajoPortalChoice *c = data;
    if (!c) return;
    g_free(c->id);
    g_free(c->label);
    g_free(c->initial);
    g_clear_pointer(&c->options, g_ptr_array_unref);
    g_free(c);
}

char *
kajo_portal_strip_mnemonic(const char *label)
{
    if (!label)
        return NULL;
    GString *out = g_string_new(NULL);
    for (const char *p = label; *p; p++) {
        if (*p == '_') {
            if (*(p + 1) == '_') {
                g_string_append_c(out, '_');
                p++;
            }
            continue;
        }
        g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

static char *
extract_bytestring(GVariant *v)
{
    if (!v)
        return NULL;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) {
        const char *s = g_variant_get_bytestring(v);
        return (s && s[0] != '\0') ? g_strdup(s) : NULL;
    }
    gsize len = 0;
    const char *raw = g_variant_get_fixed_array(v, &len, 1);
    if (!raw || len == 0)
        return NULL;
    const char *nul = memchr(raw, '\0', len);
    gsize slen = nul ? (gsize)(nul - raw) : len;
    return slen > 0 ? g_strndup(raw, slen) : NULL;
}

static KajoPortalFilter *
parse_single_filter(GVariant *fv)
{
    if (!fv || !g_variant_is_of_type(fv, G_VARIANT_TYPE("(sa(us))")))
        return NULL;

    const char *label = NULL;
    g_autoptr(GVariantIter) rules_iter = NULL;
    g_variant_get(fv, "(&sa(us))", &label, &rules_iter);

    KajoPortalFilter *f = g_new0(KajoPortalFilter, 1);
    f->label = g_strdup((label && label[0]) ? label : "Files");
    f->rules = g_ptr_array_new_with_free_func(filter_rule_free);

    guint32 rtype = 0;
    const char *rpat = NULL;
    while (rules_iter && g_variant_iter_next(rules_iter, "(u&s)", &rtype, &rpat)) {
        if (!rpat || rpat[0] == '\0')
            continue;
        KajoPortalFilterRule *rule = g_new0(KajoPortalFilterRule, 1);
        rule->type = rtype;
        rule->pattern = g_strdup(rpat);
        g_ptr_array_add(f->rules, rule);
    }
    return f;
}

static gboolean
filters_equal(const KajoPortalFilter *a, const KajoPortalFilter *b)
{
    if (!a || !b || g_strcmp0(a->label, b->label) != 0)
        return FALSE;
    if (a->rules->len != b->rules->len)
        return FALSE;
    for (guint i = 0; i < a->rules->len; i++) {
        const KajoPortalFilterRule *ra = g_ptr_array_index(a->rules, i);
        const KajoPortalFilterRule *rb = g_ptr_array_index(b->rules, i);
        if (ra->type != rb->type || g_strcmp0(ra->pattern, rb->pattern) != 0)
            return FALSE;
    }
    return TRUE;
}

static GVariant *
serialize_filter(const KajoPortalFilter *f)
{
    GVariantBuilder rules_b;
    g_variant_builder_init(&rules_b, G_VARIANT_TYPE("a(us)"));
    if (f && f->rules) {
        for (guint i = 0; i < f->rules->len; i++) {
            const KajoPortalFilterRule *r = g_ptr_array_index(f->rules, i);
            g_variant_builder_add(&rules_b, "(us)", r->type, r->pattern ? r->pattern : "");
        }
    }
    return g_variant_new("(sa(us))", (f && f->label) ? f->label : "", &rules_b);
}

KajoPortalChooserRequest *
kajo_portal_parse_options(KajoPortalChooserMode mode,
                          const char           *app_id,
                          const char           *parent_window,
                          const char           *title,
                          GVariant             *options)
{
    KajoPortalChooserRequest *req = g_new0(KajoPortalChooserRequest, 1);
    req->mode = mode;
    req->app_id = g_strdup(app_id ? app_id : "");
    req->parent_window = g_strdup(parent_window ? parent_window : "");
    req->title = g_strdup((title && title[0]) ? title :
                          (mode == KAJO_PORTAL_MODE_SAVE_FILE ? "Save File" :
                           mode == KAJO_PORTAL_MODE_SAVE_FILES ? "Save Files" : "Open File"));
    req->modal = TRUE;
    req->multiple = FALSE;
    req->directory = (mode == KAJO_PORTAL_MODE_SAVE_FILES);
    req->save_files = g_ptr_array_new_with_free_func(g_free);
    req->filters = g_ptr_array_new_with_free_func(filter_free);
    req->current_filter_index = -1;
    req->choices = g_ptr_array_new_with_free_func(choice_free);

    if (!options || !g_variant_is_of_type(options, G_VARIANT_TYPE_VARDICT))
        return req;

    GVariantDict dict;
    g_variant_dict_init(&dict, options);

    const char *acc = NULL;
    if (g_variant_dict_lookup(&dict, "accept_label", "&s", &acc) && acc && acc[0])
        req->accept_label = kajo_portal_strip_mnemonic(acc);

    gboolean bval = FALSE;
    if (g_variant_dict_lookup(&dict, "modal", "b", &bval))
        req->modal = bval;
    if (mode == KAJO_PORTAL_MODE_OPEN_FILE) {
        if (g_variant_dict_lookup(&dict, "multiple", "b", &bval))
            req->multiple = bval;
        if (g_variant_dict_lookup(&dict, "directory", "b", &bval))
            req->directory = bval;
    }

    const char *cname = NULL;
    if (g_variant_dict_lookup(&dict, "current_name", "&s", &cname) && cname && cname[0])
        req->current_name = g_strdup(cname);

    g_autoptr(GVariant) folder_v = g_variant_dict_lookup_value(&dict, "current_folder", NULL);
    if (folder_v)
        req->current_folder = extract_bytestring(folder_v);

    g_autoptr(GVariant) file_v = g_variant_dict_lookup_value(&dict, "current_file", NULL);
    if (file_v) {
        g_autofree char *cfile = extract_bytestring(file_v);
        if (cfile && cfile[0]) {
            if (!req->current_folder)
                req->current_folder = g_path_get_dirname(cfile);
            if (!req->current_name)
                req->current_name = g_path_get_basename(cfile);
        }
    }

    g_autoptr(GVariant) files_v = g_variant_dict_lookup_value(&dict, "files", G_VARIANT_TYPE("aay"));
    if (files_v) {
        GVariantIter iter;
        GVariant *elem = NULL;
        g_variant_iter_init(&iter, files_v);
        while ((elem = g_variant_iter_next_value(&iter)) != NULL) {
            char *fn = extract_bytestring(elem);
            if (fn && fn[0])
                g_ptr_array_add(req->save_files, fn);
            else
                g_free(fn);
            g_variant_unref(elem);
        }
    }

    g_autoptr(GVariant) filters_v = g_variant_dict_lookup_value(&dict, "filters", G_VARIANT_TYPE("a(sa(us))"));
    if (filters_v) {
        GVariantIter iter;
        GVariant *elem = NULL;
        g_variant_iter_init(&iter, filters_v);
        while ((elem = g_variant_iter_next_value(&iter)) != NULL) {
            KajoPortalFilter *f = parse_single_filter(elem);
            if (f)
                g_ptr_array_add(req->filters, f);
            g_variant_unref(elem);
        }
    }

    g_autoptr(GVariant) cur_filter_v = g_variant_dict_lookup_value(&dict, "current_filter", G_VARIANT_TYPE("(sa(us))"));
    if (cur_filter_v) {
        KajoPortalFilter *cf = parse_single_filter(cur_filter_v);
        if (cf) {
            gint match = -1;
            for (guint i = 0; i < req->filters->len; i++) {
                if (filters_equal(g_ptr_array_index(req->filters, i), cf)) {
                    match = (gint)i;
                    break;
                }
            }
            if (match >= 0) {
                req->current_filter_index = match;
                filter_free(cf);
            } else {
                req->current_filter_index = (gint)req->filters->len;
                g_ptr_array_add(req->filters, cf);
            }
        }
    } else if (req->filters->len > 0) {
        req->current_filter_index = 0;
    }

    g_autoptr(GVariant) choices_v = g_variant_dict_lookup_value(&dict, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"));
    if (choices_v) {
        GVariantIter iter;
        const char *cid = NULL, *clabel = NULL, *cinit = NULL;
        GVariantIter *opts_iter = NULL;
        g_variant_iter_init(&iter, choices_v);
        while (g_variant_iter_next(&iter, "(&s&sa(ss)&s)", &cid, &clabel, &opts_iter, &cinit)) {
            KajoPortalChoice *ch = g_new0(KajoPortalChoice, 1);
            ch->id = g_strdup(cid ? cid : "");
            ch->label = kajo_portal_strip_mnemonic(clabel ? clabel : cid);
            ch->initial = g_strdup(cinit ? cinit : "");
            ch->options = g_ptr_array_new_with_free_func(choice_option_free);
            const char *oid = NULL, *olbl = NULL;
            while (opts_iter && g_variant_iter_next(opts_iter, "(&s&s)", &oid, &olbl)) {
                KajoPortalChoiceOption *opt = g_new0(KajoPortalChoiceOption, 1);
                opt->id = g_strdup(oid ? oid : "");
                opt->label = g_strdup((olbl && olbl[0]) ? olbl : opt->id);
                g_ptr_array_add(ch->options, opt);
            }
            g_variant_iter_free(opts_iter);
            g_ptr_array_add(req->choices, ch);
        }
    }

    g_variant_dict_clear(&dict);
    return req;
}

void
kajo_portal_chooser_request_free(KajoPortalChooserRequest *req)
{
    if (!req) return;
    g_free(req->app_id);
    g_free(req->parent_window);
    g_free(req->title);
    g_free(req->accept_label);
    g_free(req->current_folder);
    g_free(req->current_name);
    g_clear_pointer(&req->save_files, g_ptr_array_unref);
    g_clear_pointer(&req->filters, g_ptr_array_unref);
    g_clear_pointer(&req->choices, g_ptr_array_unref);
    g_free(req);
}

KajoPortalChooserResult *
kajo_portal_chooser_result_new(void)
{
    KajoPortalChooserResult *r = g_new0(KajoPortalChooserResult, 1);
    r->uris = g_ptr_array_new_with_free_func(g_free);
    r->choice_ids = g_ptr_array_new_with_free_func(g_free);
    r->choice_values = g_ptr_array_new_with_free_func(g_free);
    r->selected_filter_index = -1;
    r->writable = TRUE;
    return r;
}

void
kajo_portal_chooser_result_free(KajoPortalChooserResult *res)
{
    if (!res) return;
    g_clear_pointer(&res->uris, g_ptr_array_unref);
    g_clear_pointer(&res->choice_ids, g_ptr_array_unref);
    g_clear_pointer(&res->choice_values, g_ptr_array_unref);
    g_free(res);
}

gboolean
kajo_portal_filter_matches_item(const KajoPortalFilter *filter,
                                const KajoFileItem     *item)
{
    if (!item)
        return FALSE;
    /* Directories and mountable network locations are always shown so the user can navigate */
    if (kajo_file_item_is_dir(item) || kajo_file_item_is_mountable(item))
        return TRUE;
    if (!filter || !filter->rules || filter->rules->len == 0)
        return TRUE;

    const char *name = kajo_file_item_get_name(item);
    const char *ctype = kajo_file_item_get_content_type(item);
    g_autofree char *name_lo = name ? g_utf8_casefold(name, -1) : NULL;

    for (guint i = 0; i < filter->rules->len; i++) {
        const KajoPortalFilterRule *rule = g_ptr_array_index(filter->rules, i);
        if (!rule || !rule->pattern)
            continue;

        if (rule->type == 0) {
            /* Glob pattern (case-insensitive) */
            if (g_strcmp0(rule->pattern, "*") == 0)
                return TRUE;
            if (name_lo) {
                g_autofree char *pat_lo = g_utf8_casefold(rule->pattern, -1);
                if (pat_lo && g_pattern_match_simple(pat_lo, name_lo))
                    return TRUE;
            }
        } else if (rule->type == 1) {
            /* MIME type */
            if (g_strcmp0(rule->pattern, "*/*") == 0)
                return TRUE;
            if (ctype && ctype[0] != '\0') {
                if (g_content_type_is_a(ctype, rule->pattern))
                    return TRUE;
                if (g_str_has_suffix(rule->pattern, "/*")) {
                    gsize pfx_len = strlen(rule->pattern) - 1; /* keep '/' */
                    if (g_ascii_strncasecmp(ctype, rule->pattern, pfx_len) == 0)
                        return TRUE;
                }
            }
        }
    }
    return FALSE;
}

char *
kajo_portal_normalize_file_uri(GFile *file)
{
    if (!file)
        return NULL;
    g_autofree char *path = g_file_get_path(file);
    if (!path || path[0] == '\0')
        return NULL;
    return g_filename_to_uri(path, NULL, NULL);
}

static char *
unique_child_path(const char *dir_path, const char *filename)
{
    g_autofree char *base = g_path_get_basename(filename);
    g_autofree char *cand = g_build_filename(dir_path, base, NULL);
    if (!g_file_test(cand, G_FILE_TEST_EXISTS))
        return g_steal_pointer(&cand);

    const char *dot = strrchr(base, '.');
    g_autofree char *stem = (dot && dot != base)
        ? g_strndup(base, (gsize)(dot - base)) : g_strdup(base);
    const char *ext = (dot && dot != base) ? dot : "";

    for (int n = 1; n < 1000; n++) {
        g_autofree char *alt = g_strdup_printf("%s (%d)%s", stem, n, ext);
        g_autofree char *alt_path = g_build_filename(dir_path, alt, NULL);
        if (!g_file_test(alt_path, G_FILE_TEST_EXISTS))
            return g_steal_pointer(&alt_path);
    }
    return g_steal_pointer(&cand);
}

GPtrArray *
kajo_portal_build_save_files_uris(GFile *folder, const GPtrArray *filenames)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    if (!folder || !filenames)
        return out;

    g_autofree char *dir_path = g_file_get_path(folder);
    if (!dir_path || dir_path[0] == '\0')
        return out;

    for (guint i = 0; i < filenames->len; i++) {
        const char *fn = g_ptr_array_index(filenames, i);
        if (!fn || fn[0] == '\0')
            continue;
        g_autofree char *target = unique_child_path(dir_path, fn);
        char *uri = g_filename_to_uri(target, NULL, NULL);
        if (uri)
            g_ptr_array_add(out, uri);
    }
    return out;
}

/* ─── D-Bus Request Lifecycle & FileChooser Interface ─── */

const KajoPortalChooserRequest *
kajo_portal_active_request_get_spec(const KajoPortalActiveRequest *req)
{
    return req ? req->spec : NULL;
}

void
kajo_portal_active_request_set_close_cb(KajoPortalActiveRequest *req,
                                        KajoPortalCloseFunc      close_cb,
                                        gpointer                 user_data)
{
    if (!req) return;
    req->close_cb = close_cb;
    req->close_user_data = user_data;
}

static void
active_request_free(KajoPortalActiveRequest *req)
{
    if (!req) return;
    if (req->portal && req->portal->connection && req->req_reg_id != 0) {
        g_dbus_connection_unregister_object(req->portal->connection, req->req_reg_id);
        req->req_reg_id = 0;
    }
    g_clear_object(&req->invocation);
    g_clear_pointer(&req->handle_path, g_free);
    g_clear_pointer(&req->spec, kajo_portal_chooser_request_free);
    g_free(req);
}

void
kajo_portal_active_request_finish(KajoPortalActiveRequest *req,
                                  guint32                  response,
                                  KajoPortalChooserResult *result)
{
    if (!req || req->finished) {
        kajo_portal_chooser_result_free(result);
        return;
    }
    req->finished = TRUE;

    GVariantBuilder res_dict;
    g_variant_builder_init(&res_dict, G_VARIANT_TYPE("a{sv}"));

    if (response == 0 && result) {
        GVariantBuilder uris_b;
        g_variant_builder_init(&uris_b, G_VARIANT_TYPE("as"));
        for (guint i = 0; result->uris && i < result->uris->len; i++) {
            const char *u = g_ptr_array_index(result->uris, i);
            if (u && g_str_has_prefix(u, "file://"))
                g_variant_builder_add(&uris_b, "s", u);
        }
        g_variant_builder_add(&res_dict, "{sv}", "uris", g_variant_builder_end(&uris_b));

        if (result->choice_ids && result->choice_values &&
            result->choice_ids->len > 0 &&
            result->choice_ids->len == result->choice_values->len) {
            GVariantBuilder ch_b;
            g_variant_builder_init(&ch_b, G_VARIANT_TYPE("a(ss)"));
            for (guint i = 0; i < result->choice_ids->len; i++) {
                g_variant_builder_add(&ch_b, "(ss)",
                                      (const char *)g_ptr_array_index(result->choice_ids, i),
                                      (const char *)g_ptr_array_index(result->choice_values, i));
            }
            g_variant_builder_add(&res_dict, "{sv}", "choices", g_variant_builder_end(&ch_b));
        }

        if (result->selected_filter_index >= 0 && req->spec && req->spec->filters &&
            (guint)result->selected_filter_index < req->spec->filters->len) {
            const KajoPortalFilter *sel_f = g_ptr_array_index(
                req->spec->filters, (guint)result->selected_filter_index);
            g_variant_builder_add(&res_dict, "{sv}", "current_filter", serialize_filter(sel_f));
        }

        if (req->spec && req->spec->mode == KAJO_PORTAL_MODE_OPEN_FILE) {
            g_variant_builder_add(&res_dict, "{sv}", "writable",
                                  g_variant_new_boolean(result->writable));
        }
    }

    if (req->invocation) {
        g_dbus_method_invocation_return_value(
            req->invocation,
            g_variant_new("(ua{sv})", response, &res_dict));
        g_clear_object(&req->invocation);
    } else {
        g_variant_builder_clear(&res_dict);
    }

    kajo_portal_chooser_result_free(result);

    if (req->portal && req->portal->active_requests)
        g_ptr_array_remove(req->portal->active_requests, req);
    active_request_free(req);
}

static void
handle_request_method_call(GDBusConnection       *connection G_GNUC_UNUSED,
                           const gchar           *sender G_GNUC_UNUSED,
                           const gchar           *object_path G_GNUC_UNUSED,
                           const gchar           *interface_name G_GNUC_UNUSED,
                           const gchar           *method_name,
                           GVariant              *parameters G_GNUC_UNUSED,
                           GDBusMethodInvocation *invocation,
                           gpointer               user_data)
{
    KajoPortalActiveRequest *req = user_data;
    if (g_strcmp0(method_name, "Close") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        if (!req->finished) {
            KajoFilesPortal *portal = req->portal;
            KajoPortalCloseFunc cb = req->close_cb;
            gpointer cb_ud = req->close_user_data;
            req->close_cb = NULL;
            if (cb)
                cb(req, cb_ud);
            if (portal && portal->active_requests &&
                g_ptr_array_find(portal->active_requests, req, NULL))
                kajo_portal_active_request_finish(req, 2, NULL);
        }
        return;
    }

    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
        "Unknown Request method '%s'", method_name);
}

static const GDBusInterfaceVTable k_request_vtable = {
    .method_call  = handle_request_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

static void
handle_fc_method_call(GDBusConnection       *connection,
                      const gchar           *sender G_GNUC_UNUSED,
                      const gchar           *object_path G_GNUC_UNUSED,
                      const gchar           *interface_name G_GNUC_UNUSED,
                      const gchar           *method_name,
                      GVariant              *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
    KajoFilesPortal *self = user_data;
    KajoPortalChooserMode mode = KAJO_PORTAL_MODE_OPEN_FILE;

    if (g_strcmp0(method_name, "OpenFile") == 0) {
        mode = KAJO_PORTAL_MODE_OPEN_FILE;
    } else if (g_strcmp0(method_name, "SaveFile") == 0) {
        mode = KAJO_PORTAL_MODE_SAVE_FILE;
    } else if (g_strcmp0(method_name, "SaveFiles") == 0) {
        mode = KAJO_PORTAL_MODE_SAVE_FILES;
    } else {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown FileChooser method '%s'", method_name);
        return;
    }

    const char *handle = NULL, *app_id = NULL, *parent_win = NULL, *title = NULL;
    g_autoptr(GVariant) options = NULL;
    g_variant_get(parameters, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent_win, &title, &options);

    KajoPortalActiveRequest *req = g_new0(KajoPortalActiveRequest, 1);
    req->portal = self;
    req->invocation = g_object_ref(invocation);
    req->handle_path = g_strdup(handle ? handle : "");
    req->spec = kajo_portal_parse_options(mode, app_id, parent_win, title, options);

    if (handle && g_variant_is_object_path(handle)) {
        req->req_reg_id = g_dbus_connection_register_object(
            connection,
            handle,
            self->req_introspection->interfaces[0],
            &k_request_vtable,
            req,
            NULL,
            NULL);
    }

    g_ptr_array_add(self->active_requests, req);

    if (self->present_cb) {
        self->present_cb(req, self->user_data);
    } else {
        kajo_portal_active_request_finish(req, 2, NULL);
    }
}

static const GDBusInterfaceVTable k_fc_vtable = {
    .method_call  = handle_fc_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

static void
register_fc_on_connection(KajoFilesPortal *self, GDBusConnection *conn)
{
    if (!self || !conn || self->fc_reg_id != 0)
        return;

    g_set_object(&self->connection, conn);

    g_autoptr(GError) err = NULL;
    self->fc_reg_id = g_dbus_connection_register_object(
        conn,
        KAJO_PORTAL_FILECHOOSER_OBJECT_PATH,
        self->fc_introspection->interfaces[0],
        &k_fc_vtable,
        self,
        NULL,
        &err);
    if (self->fc_reg_id == 0 && err)
        g_warning("files-portal: failed to register FileChooser object: %s", err->message);
}

static void
on_fc_bus_acquired(GDBusConnection *connection,
                   const gchar     *name G_GNUC_UNUSED,
                   gpointer         user_data)
{
    register_fc_on_connection((KajoFilesPortal *)user_data, connection);
}

static KajoFilesPortal *
files_portal_alloc(KajoPortalPresentFunc present_cb, gpointer user_data)
{
    KajoFilesPortal *self = g_new0(KajoFilesPortal, 1);
    self->fc_introspection  = g_dbus_node_info_new_for_xml(k_filechooser_introspection_xml, NULL);
    self->req_introspection = g_dbus_node_info_new_for_xml(k_request_introspection_xml, NULL);
    self->present_cb = present_cb;
    self->user_data = user_data;
    self->active_requests = g_ptr_array_new();
    return self;
}

KajoFilesPortal *
kajo_files_portal_new(KajoPortalPresentFunc present_cb, gpointer user_data)
{
    KajoFilesPortal *self = files_portal_alloc(present_cb, user_data);
    self->bus_name_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        KAJO_PORTAL_FILECHOOSER_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_fc_bus_acquired,
        NULL,
        NULL,
        self,
        NULL);
    return self;
}

KajoFilesPortal *
kajo_files_portal_new_for_connection(GDBusConnection      *connection,
                                     KajoPortalPresentFunc present_cb,
                                     gpointer              user_data)
{
    g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), NULL);
    KajoFilesPortal *self = files_portal_alloc(present_cb, user_data);
    register_fc_on_connection(self, connection);
    return self;
}

void
kajo_files_portal_destroy(KajoFilesPortal *self)
{
    if (!self)
        return;

    while (self->active_requests && self->active_requests->len > 0) {
        KajoPortalActiveRequest *req = g_ptr_array_index(self->active_requests, 0);
        KajoPortalCloseFunc cb = req->close_cb;
        gpointer cb_ud = req->close_user_data;
        req->close_cb = NULL;
        if (cb)
            cb(req, cb_ud);
        if (g_ptr_array_find(self->active_requests, req, NULL))
            kajo_portal_active_request_finish(req, 2, NULL);
    }

    if (self->connection && self->fc_reg_id != 0) {
        g_dbus_connection_unregister_object(self->connection, self->fc_reg_id);
        self->fc_reg_id = 0;
    }
    if (self->bus_name_id != 0) {
        g_bus_unown_name(self->bus_name_id);
        self->bus_name_id = 0;
    }

    g_clear_pointer(&self->active_requests, g_ptr_array_unref);
    g_clear_object(&self->connection);
    g_clear_pointer(&self->fc_introspection, g_dbus_node_info_unref);
    g_clear_pointer(&self->req_introspection, g_dbus_node_info_unref);
    g_free(self);
}
