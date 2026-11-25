#include "web_model.h"
#include "../vendor/tomlc17/tomlc17.h"
#include <string.h>
#include <ctype.h>

/* ─── Autohide Vertical Tab Rail FSM ─── */

void
web_rail_fsm_init(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return;
    memset(fsm, 0, sizeof(*fsm));
    fsm->state = KAJO_RAIL_FSM_COLLAPSED;
}

KajoWebRailFsmAction
web_rail_fsm_trigger_enter(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    fsm->pointer_on_trigger = TRUE;
    if (fsm->state == KAJO_RAIL_FSM_COLLAPSED) {
        fsm->state = KAJO_RAIL_FSM_REVEAL_WAIT;
        return KAJO_RAIL_ACT_START_REVEAL_TIMER;
    }
    if (fsm->state == KAJO_RAIL_FSM_HIDE_WAIT) {
        fsm->state = KAJO_RAIL_FSM_EXPANDED;
        return KAJO_RAIL_ACT_CANCEL_HIDE_TIMER;
    }
    return KAJO_RAIL_ACT_NONE;
}

KajoWebRailFsmAction
web_rail_fsm_trigger_leave(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    fsm->pointer_on_trigger = FALSE;
    if (fsm->state == KAJO_RAIL_FSM_REVEAL_WAIT && !fsm->pointer_on_rail) {
        fsm->state = KAJO_RAIL_FSM_COLLAPSED;
        return KAJO_RAIL_ACT_CANCEL_REVEAL_TIMER;
    }
    if (fsm->state == KAJO_RAIL_FSM_EXPANDED && !fsm->pointer_on_rail && fsm->hold_count <= 0) {
        fsm->state = KAJO_RAIL_FSM_HIDE_WAIT;
        return KAJO_RAIL_ACT_START_HIDE_TIMER;
    }
    return KAJO_RAIL_ACT_NONE;
}

KajoWebRailFsmAction
web_rail_fsm_rail_enter(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    fsm->pointer_on_rail = TRUE;
    if (fsm->state == KAJO_RAIL_FSM_HIDE_WAIT) {
        fsm->state = KAJO_RAIL_FSM_EXPANDED;
        return KAJO_RAIL_ACT_CANCEL_HIDE_TIMER;
    }
    if (fsm->state == KAJO_RAIL_FSM_COLLAPSED) {
        fsm->state = KAJO_RAIL_FSM_REVEAL_WAIT;
        return KAJO_RAIL_ACT_START_REVEAL_TIMER;
    }
    return KAJO_RAIL_ACT_NONE;
}

KajoWebRailFsmAction
web_rail_fsm_rail_leave(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    fsm->pointer_on_rail = FALSE;
    if (fsm->state == KAJO_RAIL_FSM_REVEAL_WAIT && !fsm->pointer_on_trigger) {
        fsm->state = KAJO_RAIL_FSM_COLLAPSED;
        return KAJO_RAIL_ACT_CANCEL_REVEAL_TIMER;
    }
    if (fsm->state == KAJO_RAIL_FSM_EXPANDED && !fsm->pointer_on_trigger && fsm->hold_count <= 0) {
        fsm->state = KAJO_RAIL_FSM_HIDE_WAIT;
        return KAJO_RAIL_ACT_START_HIDE_TIMER;
    }
    return KAJO_RAIL_ACT_NONE;
}

KajoWebRailFsmAction
web_rail_fsm_reveal_timeout(KajoWebRailFsm *fsm)
{
    if (!fsm || fsm->state != KAJO_RAIL_FSM_REVEAL_WAIT)
        return KAJO_RAIL_ACT_NONE;
    if (!fsm->pointer_on_trigger && !fsm->pointer_on_rail) {
        fsm->state = KAJO_RAIL_FSM_COLLAPSED;
        return KAJO_RAIL_ACT_NONE;
    }
    /* When the overlay rail maps on top of the trigger strip under a stationary
     * pointer, GTK emits a synthetic leave on the trigger before/without an
     * enter on the rail. Mark pointer_on_rail=TRUE so the rail stays open
     * until the pointer actually leaves the expanded rail. */
    if (fsm->pointer_on_trigger)
        fsm->pointer_on_rail = TRUE;
    fsm->state = KAJO_RAIL_FSM_EXPANDED;
    return KAJO_RAIL_ACT_EXPAND;
}

KajoWebRailFsmAction
web_rail_fsm_trigger_click(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    fsm->pointer_on_rail = TRUE;
    fsm->state = KAJO_RAIL_FSM_EXPANDED;
    return KAJO_RAIL_ACT_EXPAND;
}

KajoWebRailFsmAction
web_rail_fsm_hide_timeout(KajoWebRailFsm *fsm)
{
    if (!fsm || fsm->state != KAJO_RAIL_FSM_HIDE_WAIT)
        return KAJO_RAIL_ACT_NONE;
    if (fsm->pointer_on_trigger || fsm->pointer_on_rail || fsm->hold_count > 0) {
        fsm->state = KAJO_RAIL_FSM_EXPANDED;
        return KAJO_RAIL_ACT_NONE;
    }
    fsm->state = KAJO_RAIL_FSM_COLLAPSED;
    return KAJO_RAIL_ACT_COLLAPSE;
}

KajoWebRailFsmAction
web_rail_fsm_keyboard_reveal(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    if (fsm->state == KAJO_RAIL_FSM_EXPANDED)
        return KAJO_RAIL_ACT_NONE;
    fsm->state = KAJO_RAIL_FSM_EXPANDED;
    return KAJO_RAIL_ACT_EXPAND;
}

KajoWebRailFsmAction
web_rail_fsm_escape(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    if (fsm->state == KAJO_RAIL_FSM_COLLAPSED)
        return KAJO_RAIL_ACT_NONE;
    fsm->state = KAJO_RAIL_FSM_COLLAPSED;
    return KAJO_RAIL_ACT_COLLAPSE;
}

KajoWebRailFsmAction
web_rail_fsm_focus_lost(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    fsm->pointer_on_trigger = FALSE;
    fsm->pointer_on_rail = FALSE;
    if (fsm->hold_count > 0)
        return KAJO_RAIL_ACT_NONE;
    if (fsm->state == KAJO_RAIL_FSM_REVEAL_WAIT) {
        fsm->state = KAJO_RAIL_FSM_COLLAPSED;
        return KAJO_RAIL_ACT_CANCEL_REVEAL_TIMER;
    }
    if (fsm->state == KAJO_RAIL_FSM_EXPANDED || fsm->state == KAJO_RAIL_FSM_HIDE_WAIT) {
        fsm->state = KAJO_RAIL_FSM_COLLAPSED;
        return KAJO_RAIL_ACT_COLLAPSE;
    }
    return KAJO_RAIL_ACT_NONE;
}

void
web_rail_fsm_hold(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return;
    fsm->hold_count++;
    if (fsm->state == KAJO_RAIL_FSM_HIDE_WAIT)
        fsm->state = KAJO_RAIL_FSM_EXPANDED;
}

KajoWebRailFsmAction
web_rail_fsm_release(KajoWebRailFsm *fsm)
{
    if (!fsm)
        return KAJO_RAIL_ACT_NONE;
    if (fsm->hold_count > 0)
        fsm->hold_count--;
    if (fsm->hold_count == 0 && fsm->state == KAJO_RAIL_FSM_EXPANDED &&
        !fsm->pointer_on_trigger && !fsm->pointer_on_rail) {
        fsm->state = KAJO_RAIL_FSM_HIDE_WAIT;
        return KAJO_RAIL_ACT_START_HIDE_TIMER;
    }
    return KAJO_RAIL_ACT_NONE;
}

/* ─── Pure Tab, Group, MRU & Undo-Close Model ─── */

static void
tab_meta_free(gpointer ptr)
{
    KajoWebTabMeta *t = ptr;
    if (!t)
        return;
    g_free(t->title);
    g_free(t->uri);
    g_free(t->group);
    g_free(t->favicon_url);
    g_free(t);
}

static void
closed_tab_free(gpointer ptr)
{
    KajoWebClosedTab *c = ptr;
    if (!c)
        return;
    g_free(c->title);
    g_free(c->uri);
    g_free(c->group);
    g_free(c->favicon_url);
    g_free(c);
}

static void
mru_bump(KajoWebModel *m, guint64 id)
{
    for (guint i = 0; i < m->mru_ids->len; i++) {
        if (g_array_index(m->mru_ids, guint64, i) == id) {
            g_array_remove_index(m->mru_ids, i);
            break;
        }
    }
    g_array_prepend_val(m->mru_ids, id);
}

static void
mru_remove(KajoWebModel *m, guint64 id)
{
    for (guint i = 0; i < m->mru_ids->len; i++) {
        if (g_array_index(m->mru_ids, guint64, i) == id) {
            g_array_remove_index(m->mru_ids, i);
            return;
        }
    }
}

KajoWebModel *
web_model_new(void)
{
    KajoWebModel *m = g_new0(KajoWebModel, 1);
    m->tabs = g_ptr_array_new_with_free_func(tab_meta_free);
    m->mru_ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    m->closed_stack = g_ptr_array_new_with_free_func(closed_tab_free);
    m->collapsed_grps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    m->next_id = 1;
    return m;
}

void
web_model_free(KajoWebModel *m)
{
    if (!m)
        return;
    g_ptr_array_unref(m->tabs);
    g_array_unref(m->mru_ids);
    g_ptr_array_unref(m->closed_stack);
    g_hash_table_unref(m->collapsed_grps);
    g_free(m);
}

static KajoWebTabMeta *
web_model_insert_tab_at(KajoWebModel *m,
                        guint         insert_at,
                        const gchar  *uri,
                        const gchar  *title,
                        const gchar  *group,
                        gboolean      pinned,
                        gboolean      activate,
                        gint64        now_us)
{
    KajoWebTabMeta *t = g_new0(KajoWebTabMeta, 1);
    t->id = m->next_id++;
    t->uri = g_strdup(uri && *uri ? uri : "kajo://newtab");
    t->title = g_strdup(title && *title ? title : t->uri);
    t->group = (group && *group) ? g_strdup(group) : NULL;
    t->pinned = pinned;
    t->last_active_us = now_us;

    if (insert_at > m->tabs->len)
        insert_at = m->tabs->len;
    g_ptr_array_insert(m->tabs, (gint)insert_at, t);

    if (activate || m->tabs->len == 1) {
        m->active_id = t->id;
        mru_bump(m, t->id);
    } else {
        g_array_append_val(m->mru_ids, t->id);
    }
    return t;
}

KajoWebTabMeta *
web_model_add_tab_ex(KajoWebModel *m,
                     const gchar  *uri,
                     const gchar  *title,
                     const gchar  *group,
                     gboolean      pinned,
                     gboolean      activate,
                     gint64        now_us)
{
    if (!m)
        return NULL;
    guint insert_at = m->tabs->len;
    if (pinned) {
        insert_at = 0;
        while (insert_at < m->tabs->len &&
               ((KajoWebTabMeta *)g_ptr_array_index(m->tabs, insert_at))->pinned) {
            insert_at++;
        }
    }
    return web_model_insert_tab_at(m, insert_at, uri, title, group, pinned, activate, now_us);
}

KajoWebTabMeta *
web_model_add_tab_after(KajoWebModel *m,
                        guint64       after_tab_id,
                        const gchar  *uri,
                        const gchar  *title,
                        const gchar  *group,
                        gboolean      pinned,
                        gboolean      activate,
                        gint64        now_us)
{
    if (!m)
        return NULL;
    if (pinned || after_tab_id == 0)
        return web_model_add_tab_ex(m, uri, title, group, pinned, activate, now_us);

    gint found_idx = -1;
    for (guint i = 0; i < m->tabs->len; i++) {
        KajoWebTabMeta *cand = g_ptr_array_index(m->tabs, i);
        if (cand->id == after_tab_id) {
            found_idx = (gint)i;
            break;
        }
    }
    if (found_idx < 0)
        return web_model_add_tab_ex(m, uri, title, group, pinned, activate, now_us);

    guint insert_at = (guint)found_idx + 1;
    while (insert_at < m->tabs->len &&
           ((KajoWebTabMeta *)g_ptr_array_index(m->tabs, insert_at))->pinned) {
        insert_at++;
    }
    return web_model_insert_tab_at(m, insert_at, uri, title, group, pinned, activate, now_us);
}

KajoWebTabMeta *
web_model_add_tab(KajoWebModel *m,
                  const gchar  *uri,
                  const gchar  *title,
                  const gchar  *group,
                  gboolean      pinned,
                  gint64        now_us)
{
    return web_model_add_tab_ex(m, uri, title, group, pinned, TRUE, now_us);
}

gboolean
web_model_close_tab(KajoWebModel *m, guint64 id)
{
    if (!m)
        return FALSE;
    for (guint i = 0; i < m->tabs->len; i++) {
        KajoWebTabMeta *t = g_ptr_array_index(m->tabs, i);
        if (t->id != id)
            continue;

        KajoWebClosedTab *c = g_new0(KajoWebClosedTab, 1);
        c->title = g_strdup(t->title);
        c->uri = g_strdup(t->uri);
        c->group = g_strdup(t->group);
        c->favicon_url = g_strdup(t->favicon_url);
        c->pinned = t->pinned;
        if (m->closed_stack->len >= 64)
            g_ptr_array_remove_index(m->closed_stack, 0);
        g_ptr_array_add(m->closed_stack, c);

        mru_remove(m, id);
        g_ptr_array_remove_index(m->tabs, i);

        if (m->active_id == id) {
            if (m->mru_ids->len > 0) {
                m->active_id = g_array_index(m->mru_ids, guint64, 0);
            } else if (m->tabs->len > 0) {
                guint pick = (i < m->tabs->len) ? i : (m->tabs->len - 1);
                m->active_id = ((KajoWebTabMeta *)g_ptr_array_index(m->tabs, pick))->id;
            } else {
                m->active_id = 0;
            }
        }
        return TRUE;
    }
    return FALSE;
}

KajoWebTabMeta *
web_model_reopen_closed(KajoWebModel *m, gint64 now_us)
{
    if (!m || m->closed_stack->len == 0)
        return NULL;
    KajoWebClosedTab *c = g_ptr_array_index(m->closed_stack, m->closed_stack->len - 1);
    KajoWebTabMeta *t = web_model_add_tab(m, c->uri, c->title, c->group, c->pinned, now_us);
    if (t && c->favicon_url) {
        t->favicon_url = g_strdup(c->favicon_url);
    }
    g_ptr_array_remove_index(m->closed_stack, m->closed_stack->len - 1);
    return t;
}

gboolean
web_model_activate(KajoWebModel *m, guint64 id, gint64 now_us)
{
    KajoWebTabMeta *t = web_model_find_tab(m, id);
    if (!t)
        return FALSE;
    if (m->active_id != 0 && m->active_id != id) {
        KajoWebTabMeta *prev = web_model_find_tab(m, m->active_id);
        if (prev)
            prev->last_active_us = now_us;
    }
    t->suspended = FALSE;
    t->last_active_us = now_us;
    m->active_id = id;
    mru_bump(m, id);
    return TRUE;
}

void
web_model_set_tab_favicon(KajoWebModel *m, guint64 id, const gchar *favicon_url)
{
    KajoWebTabMeta *t = web_model_find_tab(m, id);
    if (!t)
        return;
    gchar *copy = (favicon_url && *favicon_url) ? g_strdup(favicon_url) : NULL;
    g_free(t->favicon_url);
    t->favicon_url = copy;
}

gboolean
web_model_set_pinned(KajoWebModel *m, guint64 id, gboolean pinned)
{
    if (!m)
        return FALSE;
    gint idx = -1;
    KajoWebTabMeta *t = NULL;
    for (guint i = 0; i < m->tabs->len; i++) {
        KajoWebTabMeta *cand = g_ptr_array_index(m->tabs, i);
        if (cand->id == id) {
            idx = (gint)i;
            t = cand;
            break;
        }
    }
    if (!t || t->pinned == pinned)
        return t != NULL;

    t->pinned = pinned;
    if (pinned)
        t->suspended = FALSE;

    /* Reposition to maintain invariant: all pinned tabs precede unpinned tabs */
    g_ptr_array_steal_index(m->tabs, (guint)idx);
    guint insert_at = 0;
    while (insert_at < m->tabs->len &&
           ((KajoWebTabMeta *)g_ptr_array_index(m->tabs, insert_at))->pinned) {
        insert_at++;
    }
    g_ptr_array_insert(m->tabs, (gint)insert_at, t);
    return TRUE;
}

gboolean
web_model_move_tab(KajoWebModel *m, guint64 id, guint target_index)
{
    if (!m || m->tabs->len < 2)
        return FALSE;
    gint src_idx = -1;
    KajoWebTabMeta *t = NULL;
    for (guint i = 0; i < m->tabs->len; i++) {
        KajoWebTabMeta *cand = g_ptr_array_index(m->tabs, i);
        if (cand->id == id) {
            src_idx = (gint)i;
            t = cand;
            break;
        }
    }
    if (!t)
        return FALSE;

    guint pin_count = 0;
    for (guint i = 0; i < m->tabs->len; i++) {
        if (((KajoWebTabMeta *)g_ptr_array_index(m->tabs, i))->pinned)
            pin_count++;
    }

    if (t->pinned) {
        if (pin_count == 0)
            return FALSE;
        if (target_index >= pin_count)
            target_index = pin_count - 1;
    } else {
        if (target_index < pin_count)
            target_index = pin_count;
        if (target_index >= m->tabs->len)
            target_index = m->tabs->len - 1;
    }
    if ((guint)src_idx == target_index)
        return TRUE;

    g_ptr_array_steal_index(m->tabs, (guint)src_idx);
    g_ptr_array_insert(m->tabs, (gint)target_index, t);
    return TRUE;
}

KajoWebTabMeta *
web_model_find_tab(KajoWebModel *m, guint64 id)
{
    if (!m || id == 0)
        return NULL;
    for (guint i = 0; i < m->tabs->len; i++) {
        KajoWebTabMeta *t = g_ptr_array_index(m->tabs, i);
        if (t->id == id)
            return t;
    }
    return NULL;
}

KajoWebTabMeta *
web_model_get_active(KajoWebModel *m)
{
    return m ? web_model_find_tab(m, m->active_id) : NULL;
}

guint64
web_model_cycle_mru(KajoWebModel *m, gboolean forward)
{
    if (!m || m->mru_ids->len == 0)
        return 0;
    if (m->mru_ids->len == 1)
        return g_array_index(m->mru_ids, guint64, 0);
    return forward ? g_array_index(m->mru_ids, guint64, 1)
                   : g_array_index(m->mru_ids, guint64, m->mru_ids->len - 1);
}

guint64
web_model_cycle_visual(const KajoWebModel *m, gboolean forward)
{
    if (!m || m->tabs->len == 0)
        return 0;
    if (m->tabs->len == 1)
        return ((const KajoWebTabMeta *)g_ptr_array_index(m->tabs, 0))->id;

    guint cur = 0;
    for (guint i = 0; i < m->tabs->len; i++) {
        const KajoWebTabMeta *t = g_ptr_array_index(m->tabs, i);
        if (t->id == m->active_id) {
            cur = i;
            break;
        }
    }
    guint nxt = forward ? (cur + 1) % m->tabs->len
                        : (cur + m->tabs->len - 1) % m->tabs->len;
    return ((const KajoWebTabMeta *)g_ptr_array_index(m->tabs, nxt))->id;
}

gboolean
web_model_can_suspend(const KajoWebModel   *m,
                      const KajoWebTabMeta *t,
                      gint                  suspend_idle_min,
                      gint64                now_us)
{
    if (!m || !t || suspend_idle_min <= 0)
        return FALSE;
    if (t->id == m->active_id || t->pinned || t->audible || t->suspended ||
        t->capture_active || t->download_active)
        return FALSE;
    if (t->uri && g_str_has_prefix(t->uri, "kajo://"))
        return FALSE;
    gint64 threshold_us = (gint64)suspend_idle_min * 60 * G_USEC_PER_SEC;
    return (now_us - t->last_active_us) >= threshold_us;
}

void
web_model_set_group_collapsed(KajoWebModel *m, const gchar *group, gboolean collapsed)
{
    if (!m || !group || !*group)
        return;
    if (collapsed)
        g_hash_table_insert(m->collapsed_grps, g_strdup(group), GINT_TO_POINTER(1));
    else
        g_hash_table_remove(m->collapsed_grps, group);
}

gboolean
web_model_is_group_collapsed(const KajoWebModel *m, const gchar *group)
{
    if (!m || !group || !*group)
        return FALSE;
    return g_hash_table_contains(m->collapsed_grps, group);
}

/* ─── Omnibar URL / Bang / Search Classifier ─── */

static gchar *
build_search_url(const gchar *tmpl, const gchar *query)
{
    g_autofree gchar *enc = g_uri_escape_string(query ? query : "", NULL, TRUE);
    const char *pos = strstr(tmpl, "%s");
    if (!pos)
        return g_strdup_printf("%s%s", tmpl, enc);
    g_autofree gchar *prefix = g_strndup(tmpl, (gsize)(pos - tmpl));
    return g_strdup_printf("%s%s%s", prefix, enc, pos + 2);
}

static gboolean
looks_like_host(const gchar *s)
{
    if (!s || !*s || strchr(s, ' ') || strchr(s, '\t'))
        return FALSE;
    if (g_str_has_prefix(s, "localhost") &&
        (s[9] == '\0' || s[9] == ':' || s[9] == '/')) {
        return TRUE;
    }
    const char *slash = strchr(s, '/');
    gsize host_len = slash ? (gsize)(slash - s) : strlen(s);
    if (host_len < 3)
        return FALSE;
    const char *dot = memchr(s, '.', host_len);
    if (!dot || dot == s || dot == s + host_len - 1)
        return FALSE;
    for (gsize i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '.' && c != '-' && c != ':' && c != '[' && c != ']')
            return FALSE;
    }
    return TRUE;
}

static gboolean
has_explicit_scheme(const gchar *s)
{
    return s &&
           (g_str_has_prefix(s, "http://") ||
            g_str_has_prefix(s, "https://") ||
            g_str_has_prefix(s, "kajo://") ||
            g_str_has_prefix(s, "chrome://") ||
            g_str_has_prefix(s, "devtools://") ||
            g_str_has_prefix(s, "view-source:") ||
            g_str_has_prefix(s, "file://") ||
            g_str_has_prefix(s, "about:"));
}

static gchar *
local_path_to_file_uri(const gchar *path)
{
    if (!path || !*path)
        return NULL;

    g_autofree gchar *expanded = NULL;
    if (g_strcmp0(path, "~") == 0) {
        expanded = g_strdup(g_get_home_dir());
    } else if (g_str_has_prefix(path, "~/")) {
        expanded = g_build_filename(g_get_home_dir(), path + 2, NULL);
    } else {
        expanded = g_strdup(path);
    }

    g_autofree gchar *canon = g_canonicalize_filename(expanded, NULL);
    if (!canon)
        return g_strdup(path);

    if (!g_file_test(canon, G_FILE_TEST_EXISTS)) {
        const char *sep = strpbrk(canon, "#?");
        if (sep && sep > canon) {
            g_autofree gchar *prefix = g_strndup(canon, (gsize)(sep - canon));
            if (g_file_test(prefix, G_FILE_TEST_EXISTS)) {
                g_autofree gchar *uri = g_filename_to_uri(prefix, NULL, NULL);
                if (uri)
                    return g_strconcat(uri, sep, NULL);
            }
        }
    }

    g_autofree gchar *uri = g_filename_to_uri(canon, NULL, NULL);
    return uri ? g_steal_pointer(&uri) : g_strdup(path);
}

gchar *
web_model_resolve_cli_arg(const gchar *arg)
{
    if (!arg || !*arg)
        return g_strdup("");
    if (has_explicit_scheme(arg))
        return g_strdup(arg);

    if (arg[0] == '/' || g_str_has_prefix(arg, "~/") || g_strcmp0(arg, "~") == 0 ||
        g_file_test(arg, G_FILE_TEST_EXISTS)) {
        return local_path_to_file_uri(arg);
    }

    const char *sep = strpbrk(arg, "#?");
    if (sep && sep > arg) {
        g_autofree gchar *prefix = g_strndup(arg, (gsize)(sep - arg));
        if (g_file_test(prefix, G_FILE_TEST_EXISTS))
            return local_path_to_file_uri(arg);
    }

    return g_strdup(arg);
}

gchar *
web_model_resolve_input(const gchar         *raw_input,
                        KajoWebSearchEngine  engine,
                        const gchar         *custom_search_url)
{
    if (!raw_input)
        return g_strdup("kajo://newtab");
    g_autofree gchar *trimmed = g_strstrip(g_strdup(raw_input));
    if (!*trimmed)
        return g_strdup("kajo://newtab");

    /* Explicit schemes */
    if (has_explicit_scheme(trimmed)) {
        return g_strdup(trimmed);
    }

    /* Local POSIX / home paths */
    if (trimmed[0] == '/' || g_str_has_prefix(trimmed, "~/") || g_strcmp0(trimmed, "~") == 0) {
        return local_path_to_file_uri(trimmed);
    }

    /* Keyword bangs (!g, !gh, !arch, !mdn, !w, !yt, !ddg) */
    static const struct { const char *bang; const char *tmpl; } k_bangs[] = {
        { "!g ",    "https://www.google.com/search?q=%s" },
        { "!gh ",   "https://github.com/search?q=%s" },
        { "!arch ", "https://wiki.archlinux.org/index.php?search=%s" },
        { "!mdn ",  "https://developer.mozilla.org/en-US/search?q=%s" },
        { "!w ",    "https://en.wikipedia.org/w/index.php?search=%s" },
        { "!yt ",   "https://www.youtube.com/results?search_query=%s" },
        { "!ddg ",  "https://duckduckgo.com/?q=%s" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_bangs); i++) {
        gsize blen = strlen(k_bangs[i].bang);
        if (g_ascii_strncasecmp(trimmed, k_bangs[i].bang, blen) == 0) {
            return build_search_url(k_bangs[i].tmpl, trimmed + blen);
        }
    }

    /* Bare hostname or localhost */
    if (looks_like_host(trimmed)) {
        if (g_str_has_prefix(trimmed, "localhost") ||
            g_str_has_prefix(trimmed, "127.") ||
            g_str_has_prefix(trimmed, "192.168.") ||
            g_str_has_prefix(trimmed, "10.")) {
            return g_strdup_printf("http://%s", trimmed);
        }
        return g_strdup_printf("https://%s", trimmed);
    }

    const char *tmpl = "https://duckduckgo.com/?q=%s";
    switch (engine) {
    case KAJO_WEB_SEARCH_KAGI:      tmpl = "https://kagi.com/search?q=%s"; break;
    case KAJO_WEB_SEARCH_STARTPAGE: tmpl = "https://www.startpage.com/sp/search?query=%s"; break;
    case KAJO_WEB_SEARCH_BRAVE:     tmpl = "https://search.brave.com/search?q=%s"; break;
    case KAJO_WEB_SEARCH_GOOGLE:    tmpl = "https://www.google.com/search?q=%s"; break;
    case KAJO_WEB_SEARCH_CUSTOM:
        if (custom_search_url && *custom_search_url)
            tmpl = custom_search_url;
        break;
    case KAJO_WEB_SEARCH_DUCKDUCKGO:
    default:
        break;
    }
    return build_search_url(tmpl, trimmed);
}

gchar *
web_model_extract_host(const gchar *uri)
{
    if (!uri || !*uri)
        return g_strdup("");
    if (g_str_has_prefix(uri, "kajo://"))
        return g_strdup(uri);
    g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    if (parsed && g_uri_get_host(parsed))
        return g_strdup(g_uri_get_host(parsed));
    return g_strdup(uri);
}

/* ─── Multi-Profile Registry (profiles.toml) ─── */

static void
profile_info_free(gpointer ptr)
{
    KajoWebProfileInfo *p = ptr;
    if (!p)
        return;
    g_free(p->id);
    g_free(p->name);
    g_free(p->icon);
    g_free(p->accent_color);
    g_free(p->google_email);
    g_free(p);
}

gchar *
web_profile_sanitize_id(const gchar *name)
{
    if (!name || !*name)
        return g_strdup("profile");
    GString *out = g_string_new(NULL);
    for (const char *p = name; *p && out->len < 32; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c))
            g_string_append_c(out, (char)tolower(c));
        else if ((c == ' ' || c == '-' || c == '_') && out->len > 0 && out->str[out->len - 1] != '-')
            g_string_append_c(out, '-');
    }
    while (out->len > 0 && out->str[out->len - 1] == '-')
        g_string_truncate(out, out->len - 1);
    if (out->len == 0)
        g_string_assign(out, "profile");
    return g_string_free(out, FALSE);
}

KajoWebProfileRegistry *
web_profile_registry_new(void)
{
    KajoWebProfileRegistry *reg = g_new0(KajoWebProfileRegistry, 1);
    reg->profiles = g_ptr_array_new_with_free_func(profile_info_free);
    KajoWebProfileInfo *def = g_new0(KajoWebProfileInfo, 1);
    def->id = g_strdup("default");
    def->name = g_strdup("DEFAULT");
    def->icon = g_strdup("🌐");
    def->accent_color = g_strdup("#0078d4");
    def->google_email = g_strdup("");
    g_ptr_array_add(reg->profiles, def);
    reg->last_profile_id = g_strdup("default");
    return reg;
}

void
web_profile_registry_free(KajoWebProfileRegistry *reg)
{
    if (!reg)
        return;
    g_ptr_array_unref(reg->profiles);
    g_free(reg->last_profile_id);
    g_free(reg);
}

KajoWebProfileInfo *
web_profile_registry_find(KajoWebProfileRegistry *reg, const gchar *id)
{
    if (!reg || !id || !*id)
        return NULL;
    for (guint i = 0; i < reg->profiles->len; i++) {
        KajoWebProfileInfo *p = g_ptr_array_index(reg->profiles, i);
        if (g_strcmp0(p->id, id) == 0)
            return p;
    }
    return NULL;
}

KajoWebProfileInfo *
web_profile_registry_add(KajoWebProfileRegistry *reg,
                         const gchar            *name,
                         const gchar            *icon,
                         const gchar            *accent_color)
{
    if (!reg || !name || !*name)
        return NULL;
    g_autofree gchar *base_id = web_profile_sanitize_id(name);
    g_autofree gchar *id = g_strdup(base_id);
    for (int suffix = 2; web_profile_registry_find(reg, id) != NULL; suffix++) {
        g_free(id);
        id = g_strdup_printf("%s-%d", base_id, suffix);
    }

    KajoWebProfileInfo *p = g_new0(KajoWebProfileInfo, 1);
    p->id = g_strdup(id);
    p->name = g_utf8_strup(name, -1);
    p->icon = g_strdup((icon && *icon) ? icon : "❖");
    p->accent_color = g_strdup((accent_color && *accent_color) ? accent_color : "#0078d4");
    p->google_email = g_strdup("");
    g_ptr_array_add(reg->profiles, p);
    return p;
}

gboolean
web_profile_registry_remove(KajoWebProfileRegistry *reg, const gchar *id)
{
    if (!reg || !id || reg->profiles->len <= 1 || g_strcmp0(id, "default") == 0)
        return FALSE;
    for (guint i = 0; i < reg->profiles->len; i++) {
        KajoWebProfileInfo *p = g_ptr_array_index(reg->profiles, i);
        if (g_strcmp0(p->id, id) == 0) {
            g_ptr_array_remove_index(reg->profiles, i);
            if (g_strcmp0(reg->last_profile_id, id) == 0) {
                g_free(reg->last_profile_id);
                KajoWebProfileInfo *first = g_ptr_array_index(reg->profiles, 0);
                reg->last_profile_id = g_strdup(first->id);
            }
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
web_profile_registry_load(KajoWebProfileRegistry *reg, const gchar *filepath)
{
    if (!reg || !filepath || !g_file_test(filepath, G_FILE_TEST_EXISTS))
        return FALSE;
    g_autofree gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(filepath, &contents, &len, NULL))
        return FALSE;

    toml_result_t res = toml_parse(contents, (int)len);
    if (!res.ok)
        return FALSE;

    toml_datum_t last = toml_get(res.toptab, "last_profile");
    if (last.type == TOML_STRING && last.u.s && *last.u.s) {
        g_free(reg->last_profile_id);
        reg->last_profile_id = g_strdup(last.u.s);
    }

    toml_datum_t arr = toml_get(res.toptab, "profile");
    if (arr.type == TOML_ARRAY && arr.u.arr.size > 0) {
        g_ptr_array_set_size(reg->profiles, 0);
        for (int i = 0; i < arr.u.arr.size; i++) {
            toml_datum_t elem = arr.u.arr.elem[i];
            if (elem.type != TOML_TABLE)
                continue;
            toml_datum_t d_id   = toml_get(elem, "id");
            toml_datum_t d_name = toml_get(elem, "name");
            if (d_id.type != TOML_STRING || !d_id.u.s || !*d_id.u.s)
                continue;
            KajoWebProfileInfo *p = g_new0(KajoWebProfileInfo, 1);
            p->id = web_profile_sanitize_id(d_id.u.s);
            p->name = g_strdup((d_name.type == TOML_STRING && d_name.u.s && *d_name.u.s) ? d_name.u.s : p->id);
            toml_datum_t d_ico = toml_get(elem, "icon");
            p->icon = g_strdup((d_ico.type == TOML_STRING && d_ico.u.s && *d_ico.u.s) ? d_ico.u.s : "🌐");
            toml_datum_t d_acc = toml_get(elem, "accent_color");
            p->accent_color = g_strdup((d_acc.type == TOML_STRING && d_acc.u.s && *d_acc.u.s) ? d_acc.u.s : "#0078d4");
            toml_datum_t d_mail = toml_get(elem, "google_email");
            p->google_email = g_strdup((d_mail.type == TOML_STRING && d_mail.u.s) ? d_mail.u.s : "");
            toml_datum_t d_sync = toml_get(elem, "sync_enabled");
            p->sync_enabled = (d_sync.type == TOML_BOOLEAN) ? d_sync.u.boolean : FALSE;
            toml_datum_t d_ts = toml_get(elem, "last_sync_epoch");
            p->last_sync_epoch = (d_ts.type == TOML_INT64) ? d_ts.u.int64 : 0;
            if (!web_profile_registry_find(reg, p->id))
                g_ptr_array_add(reg->profiles, p);
            else
                profile_info_free(p);
        }
    }

    if (reg->profiles->len == 0) {
        web_profile_registry_add(reg, "DEFAULT", "🌐", "#0078d4");
    }
    if (!web_profile_registry_find(reg, reg->last_profile_id)) {
        g_free(reg->last_profile_id);
        reg->last_profile_id = g_strdup(((KajoWebProfileInfo *)g_ptr_array_index(reg->profiles, 0))->id);
    }

    toml_free(res);
    return TRUE;
}

static void
append_toml_str(GString *out, const gchar *val)
{
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)(val ? val : ""); *p; p++) {
        switch (*p) {
        case '\\': g_string_append(out, "\\\\"); break;
        case '"':  g_string_append(out, "\\\""); break;
        case '\b': g_string_append(out, "\\b"); break;
        case '\t': g_string_append(out, "\\t"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\f': g_string_append(out, "\\f"); break;
        case '\r': g_string_append(out, "\\r"); break;
        default:
            if (*p < 0x20 || *p == 0x7f)
                g_string_append_printf(out, "\\u%04x", *p);
            else
                g_string_append_c(out, (gchar)*p);
            break;
        }
    }
    g_string_append_c(out, '"');
}

gboolean
web_profile_registry_save(const KajoWebProfileRegistry *reg,
                          const gchar                  *filepath,
                          GError                      **error)
{
    if (!reg || !filepath)
        return FALSE;
    g_autofree gchar *dir = g_path_get_dirname(filepath);
    g_mkdir_with_parents(dir, 0700);

    GString *out = g_string_new("# Kajo Web Profiles Registry\nlast_profile = ");
    append_toml_str(out, reg->last_profile_id ? reg->last_profile_id : "default");
    g_string_append(out, "\n\n");

    for (guint i = 0; i < reg->profiles->len; i++) {
        const KajoWebProfileInfo *p = g_ptr_array_index(reg->profiles, i);
        g_string_append(out, "[[profile]]\nid = ");
        append_toml_str(out, p->id);
        g_string_append(out, "\nname = ");
        append_toml_str(out, p->name);
        g_string_append(out, "\nicon = ");
        append_toml_str(out, p->icon);
        g_string_append(out, "\naccent_color = ");
        append_toml_str(out, p->accent_color);
        g_string_append(out, "\ngoogle_email = ");
        append_toml_str(out, p->google_email);
        g_string_append_printf(out, "\nsync_enabled = %s\nlast_sync_epoch = %" G_GINT64_FORMAT "\n\n",
                               p->sync_enabled ? "true" : "false", p->last_sync_epoch);
    }

    gboolean ok = g_file_set_contents(filepath, out->str, (gssize)out->len, error);
    g_string_free(out, TRUE);
    return ok;
}
