#include "web_abp_compiler.h"
#include <string.h>

gboolean
web_abp_is_webkit_regex_safe(const gchar *regex)
{
    if (!regex || !*regex)
        return FALSE;
    gint paren_depth = 0;
    gboolean in_bracket = FALSE;
    gboolean escaped = FALSE;

    for (const gchar *p = regex; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c >= 0x7f)
            return FALSE;
        if (escaped) {
            /* WebKit ContentExtension rejects PCRE shorthand classes \d \w \s \b and backrefs */
            if (c == 'd' || c == 'D' || c == 'w' || c == 'W' ||
                c == 's' || c == 'S' || c == 'b' || c == 'B' ||
                (c >= '1' && c <= '9'))
                return FALSE;
            escaped = FALSE;
            continue;
        }
        if (c == '\\') {
            escaped = TRUE;
            continue;
        }
        if (in_bracket) {
            if (c == ']')
                in_bracket = FALSE;
            continue;
        }
        if (c == '[') {
            in_bracket = TRUE;
            continue;
        }
        if (c == ']')
            return FALSE;
        if (c == '|' || c == '{' || c == '}')
            return FALSE;
        if (c == '(') {
            /* Reject lookahead/lookbehind/non-capturing '(?' */
            if (p[1] == '?')
                return FALSE;
            paren_depth++;
            continue;
        }
        if (c == ')') {
            if (paren_depth <= 0)
                return FALSE;
            paren_depth--;
            continue;
        }
    }
    return !escaped && !in_bracket && paren_depth == 0;
}

static gboolean
is_ascii_only(const gchar *s)
{
    if (!s || !*s)
        return FALSE;
    for (const gchar *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c >= 0x7f)
            return FALSE;
    }
    return TRUE;
}

static gboolean
is_valid_domain_token(const gchar *s)
{
    if (!s || !*s || !strchr(s, '.'))
        return FALSE;
    if (s[0] == '.' || s[0] == '-')
        return FALSE;
    gsize len = strlen(s);
    if (len > 128 || s[len - 1] == '.' || s[len - 1] == '-')
        return FALSE;
    for (gsize i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(g_ascii_isalnum(c) || c == '.' || c == '-'))
            return FALSE;
    }
    if (g_strcmp0(s, "localhost") == 0 || g_strcmp0(s, "localhost.localdomain") == 0)
        return FALSE;
    return TRUE;
}

static void
append_json_escaped(GString *out, const gchar *s)
{
    g_string_append_c(out, '"');
    for (const gchar *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"')
            g_string_append(out, "\\\"");
        else if (c == '\\')
            g_string_append(out, "\\\\");
        else if (c == '\n')
            g_string_append(out, "\\n");
        else if (c == '\r')
            g_string_append(out, "\\r");
        else if (c == '\t')
            g_string_append(out, "\\t");
        else if (c >= 0x20 && c < 0x7f)
            g_string_append_c(out, (char)c);
    }
    g_string_append_c(out, '"');
}

static gboolean
is_safe_css_selector(const gchar *sel)
{
    if (!sel || !*sel)
        return FALSE;
    if (!is_ascii_only(sel))
        return FALSE;
    if (strlen(sel) > 320)
        return FALSE;
    /* Reject uBlock / AdGuard / ABP procedural, scriptlet, and :has() relational selectors
     * (:has() on high-mutation SPAs such as YouTube causes severe style-recalc sweeps). */
    if (strstr(sel, ":has(") ||
        strstr(sel, ":has-text(") || strstr(sel, ":-abp-") ||
        strstr(sel, ":upward(") || strstr(sel, ":xpath(") ||
        strstr(sel, ":matches-css") || strstr(sel, ":matches-path") ||
        strstr(sel, ":min-text-length") || strstr(sel, ":style(") ||
        strstr(sel, ":remove(") || strstr(sel, ":remove-attr(") ||
        strstr(sel, ":remove-class(") || strstr(sel, ":watch-attr(") ||
        strstr(sel, "+js(") || strstr(sel, "{") || strstr(sel, "}"))
        return FALSE;
    /* Reject unbalanced quotes/brackets/parens */
    gint paren = 0, brack = 0;
    gchar quote = 0;
    for (const gchar *p = sel; *p; p++) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p[1]) { p++; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') quote = c;
        else if (c == '(') paren++;
        else if (c == ')') { if (--paren < 0) return FALSE; }
        else if (c == '[') brack++;
        else if (c == ']') { if (--brack < 0) return FALSE; }
    }
    return quote == 0 && paren == 0 && brack == 0;
}

/* Convert ABP URL pattern into a WebKit-DFA-safe ASCII regex (no '|' disjunctions). */
static gchar *
abp_pattern_to_webkit_regex(const gchar *pat)
{
    if (!pat || !*pat || !is_ascii_only(pat))
        return NULL;
    gsize len = strlen(pat);
    if (len < 3 || len > 240)
        return NULL;
    /* Skip raw /regex/ rules (often contain PCRE disjunctions/lookaheads) */
    if (pat[0] == '/' && len > 2 && pat[len - 1] == '/')
        return NULL;

    GString *rx = g_string_new(NULL);
    const gchar *p = pat;

    if (g_str_has_prefix(p, "||")) {
        g_string_append(rx, "^https?://([^/]+\\.)?");
        p += 2;
    } else if (*p == '|') {
        g_string_append_c(rx, '^');
        p += 1;
    }

    gboolean anchor_end = FALSE;
    gsize rem = strlen(p);
    if (rem > 0 && p[rem - 1] == '|') {
        anchor_end = TRUE;
        rem--;
    }

    for (gsize i = 0; i < rem; i++) {
        char c = p[i];
        if (c == '*') {
            /* Collapse consecutive wildcards */
            if (rx->len < 2 || rx->str[rx->len - 2] != '.' || rx->str[rx->len - 1] != '*')
                g_string_append(rx, ".*");
        } else if (c == '^') {
            /* ABP separator: end of host/segment (/, ?, &, :, =) */
            g_string_append(rx, "[/:?&=#]");
        } else if (c == '.' || c == '+' || c == '?' || c == '(' || c == ')' ||
                   c == '[' || c == ']' || c == '\\' || c == '$') {
            g_string_append_c(rx, '\\');
            g_string_append_c(rx, c);
        } else if (c == '|' || c == '{' || c == '}') {
            g_string_free(rx, TRUE);
            return NULL;
        } else {
            g_string_append_c(rx, c);
        }
    }

    if (anchor_end)
        g_string_append_c(rx, '$');

    if (rx->len < 4 || !web_abp_is_webkit_regex_safe(rx->str)) {
        g_string_free(rx, TRUE);
        return NULL;
    }
    return g_string_free(rx, FALSE);
}

static gboolean
is_youtube_player_telemetry_pattern(const gchar *rx)
{
    if (!rx || !*rx)
        return FALSE;
    return strstr(rx, "youtube\\.com/api/stats/qoe") != NULL ||
           strstr(rx, "youtube\\.com/api/stats/atr") != NULL ||
           strstr(rx, "youtube\\.com/ptracking") != NULL ||
           strstr(rx, "youtube\\.com/youtubei/v1/log_event") != NULL ||
           strstr(rx, "/api/stats/qoe") != NULL ||
           strstr(rx, "/ptracking") != NULL ||
           strstr(rx, "/youtubei/v1/log_event") != NULL;
}

static gchar *
build_network_rule_json(const gchar     *rx,
                        gboolean         is_exception,
                        const gchar     *load_type,
                        const GPtrArray *res_types,
                        const GPtrArray *if_domains,
                        const GPtrArray *unless_domains)
{
    /* WebKit forbids both if-domain and unless-domain in the same trigger */
    if (if_domains && if_domains->len > 0 && unless_domains && unless_domains->len > 0)
        return NULL;

    GString *j = g_string_new("{\"trigger\":{\"url-filter\":");
    append_json_escaped(j, rx);

    if (load_type && *load_type) {
        g_string_append(j, ",\"load-type\":[\"");
        g_string_append(j, load_type);
        g_string_append(j, "\"]");
    }
    if (res_types && res_types->len > 0) {
        g_string_append(j, ",\"resource-type\":[");
        for (guint i = 0; i < res_types->len; i++) {
            if (i > 0)
                g_string_append_c(j, ',');
            append_json_escaped(j, g_ptr_array_index(res_types, i));
        }
        g_string_append_c(j, ']');
    }
    if (if_domains && if_domains->len > 0) {
        g_string_append(j, ",\"if-domain\":[");
        for (guint i = 0; i < if_domains->len; i++) {
            if (i > 0)
                g_string_append_c(j, ',');
            append_json_escaped(j, g_ptr_array_index(if_domains, i));
        }
        g_string_append_c(j, ']');
    } else if (unless_domains && unless_domains->len > 0) {
        g_string_append(j, ",\"unless-domain\":[");
        for (guint i = 0; i < unless_domains->len; i++) {
            if (i > 0)
                g_string_append_c(j, ',');
            append_json_escaped(j, g_ptr_array_index(unless_domains, i));
        }
        g_string_append_c(j, ']');
    }

    g_string_append(j, "},\"action\":{\"type\":\"");
    g_string_append(j, is_exception ? "ignore-previous-rules" : "block");
    g_string_append(j, "\"}}");
    return g_string_free(j, FALSE);
}

static void
add_unique_str(GPtrArray *arr, const gchar *s)
{
    for (guint i = 0; i < arr->len; i++) {
        if (g_strcmp0(g_ptr_array_index(arr, i), s) == 0)
            return;
    }
    g_ptr_array_add(arr, g_strdup(s));
}

static gboolean
parse_abp_options(const gchar  *opts_str,
                  gchar       **out_load_type,
                  GPtrArray    *res_types,
                  GPtrArray    *if_domains,
                  GPtrArray    *unless_domains)
{
    g_auto(GStrv) parts = g_strsplit(opts_str, ",", -1);
    for (gint i = 0; parts && parts[i]; i++) {
        gchar *opt = g_strstrip(parts[i]);
        if (!*opt)
            continue;
        g_autofree gchar *low = g_ascii_strdown(opt, -1);

        if (g_strcmp0(low, "third-party") == 0 || g_strcmp0(low, "3p") == 0) {
            g_free(*out_load_type);
            *out_load_type = g_strdup("third-party");
        } else if (g_strcmp0(low, "~third-party") == 0 || g_strcmp0(low, "1p") == 0 || g_strcmp0(low, "first-party") == 0) {
            g_free(*out_load_type);
            *out_load_type = g_strdup("first-party");
        } else if (g_strcmp0(low, "script") == 0) {
            add_unique_str(res_types, "script");
        } else if (g_strcmp0(low, "image") == 0) {
            add_unique_str(res_types, "image");
        } else if (g_strcmp0(low, "stylesheet") == 0 || g_strcmp0(low, "css") == 0) {
            add_unique_str(res_types, "style-sheet");
        } else if (g_strcmp0(low, "xmlhttprequest") == 0 || g_strcmp0(low, "xhr") == 0 || g_strcmp0(low, "fetch") == 0 || g_strcmp0(low, "ping") == 0) {
            add_unique_str(res_types, "raw");
        } else if (g_strcmp0(low, "subdocument") == 0 || g_strcmp0(low, "frame") == 0) {
            add_unique_str(res_types, "document");
        } else if (g_strcmp0(low, "font") == 0) {
            add_unique_str(res_types, "font");
        } else if (g_strcmp0(low, "media") == 0) {
            add_unique_str(res_types, "media");
        } else if (g_strcmp0(low, "popup") == 0) {
            add_unique_str(res_types, "popup");
        } else if (g_str_has_prefix(low, "domain=")) {
            g_auto(GStrv) doms = g_strsplit(low + 7, "|", -1);
            for (gint d = 0; doms && doms[d]; d++) {
                const gchar *dm = doms[d];
                gboolean neg = FALSE;
                if (dm[0] == '~') {
                    neg = TRUE;
                    dm++;
                }
                if (!is_valid_domain_token(dm))
                    return FALSE;
                g_autofree gchar *star = g_strdup_printf("*%s", dm);
                add_unique_str(neg ? unless_domains : if_domains, star);
            }
        } else if (g_strcmp0(low, "important") == 0 || g_strcmp0(low, "match-case") == 0) {
            /* Safe to accept without breaking semantics */
        } else {
            /* Unsupported modifier (e.g. redirect=, csp=, removeparam=, elemhide, document, badfilter)
             * — skip rule safely rather than over-blocking or failing compilation! */
            return FALSE;
        }
    }
    if (if_domains->len > 0 && unless_domains->len > 0)
        return FALSE;
    return TRUE;
}

static void
append_cosmetic_rule(GPtrArray       *out_rules,
                     const gchar     *domains_csv,
                     const GPtrArray *selectors)
{
    if (!domains_csv || !*domains_csv || !selectors || selectors->len == 0)
        return;

    /* Domain-scoped if-domain only: global generic and unless-domain-only
     * selectors inject CSS into every site and severely hurt DOM style recalc. */
    g_auto(GStrv) doms = g_strsplit(domains_csv, ",", -1);
    g_autoptr(GPtrArray) inc = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) exc = g_ptr_array_new_with_free_func(g_free);
    for (gint d = 0; doms && doms[d]; d++) {
        gchar *dm = g_strstrip(doms[d]);
        gboolean neg = FALSE;
        if (dm[0] == '~') { neg = TRUE; dm++; }
        g_autofree gchar *low = g_ascii_strdown(dm, -1);
        if (!is_valid_domain_token(low))
            continue;
        g_autofree gchar *star = g_strdup_printf("*%s", low);
        add_unique_str(neg ? exc : inc, star);
    }
    if (inc->len == 0 || exc->len > 0)
        return;

    /* Batch selectors into groups of 24 so selector strings stay compact */
    const guint batch_sz = 24;
    for (guint start = 0; start < selectors->len; start += batch_sz) {
        GString *sel_buf = g_string_new(NULL);
        guint end = MIN(start + batch_sz, selectors->len);
        for (guint i = start; i < end; i++) {
            if (i > start)
                g_string_append(sel_buf, ", ");
            g_string_append(sel_buf, g_ptr_array_index(selectors, i));
        }

        GString *j = g_string_new("{\"trigger\":{\"url-filter\":\".*\",\"if-domain\":[");
        for (guint k = 0; k < inc->len; k++) {
            if (k > 0)
                g_string_append_c(j, ',');
            append_json_escaped(j, g_ptr_array_index(inc, k));
        }
        g_string_append(j, "]},\"action\":{\"type\":\"css-display-none\",\"selector\":");
        append_json_escaped(j, sel_buf->str);
        g_string_append(j, "}}");

        g_ptr_array_add(out_rules, g_string_free(j, FALSE));
        g_string_free(sel_buf, TRUE);
    }
}

gchar *
web_abp_compile_text_to_json(const gchar *filter_text,
                             guint        max_rules,
                             guint       *out_rule_count)
{
    if (max_rules == 0)
        max_rules = 30000;

    g_autoptr(GPtrArray) block_rules = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) exc_rules   = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) dom_cosmetic = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    g_autoptr(GHashTable) seen_dom_sel = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) seen_rules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_auto(GStrv) lines = g_strsplit(filter_text ? filter_text : "", "\n", -1);
    for (gint i = 0; lines && lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line || line[0] == '!' || line[0] == '[')
            continue;
        /* Ignore hosts-file comments */
        if (line[0] == '#' && line[1] != '#' && line[1] != '@' && line[1] != '?')
            continue;

        /* 1. Hosts file format: 0.0.0.0 domain.tld / 127.0.0.1 domain.tld */
        if (g_str_has_prefix(line, "0.0.0.0 ") || g_str_has_prefix(line, "0.0.0.0\t") ||
            g_str_has_prefix(line, "127.0.0.1 ") || g_str_has_prefix(line, "127.0.0.1\t")) {
            gchar *dom = line + (line[0] == '1' ? 9 : 7);
            while (*dom == ' ' || *dom == '\t') dom++;
            gchar *cmt = strchr(dom, '#');
            if (cmt) *cmt = '\0';
            g_strstrip(dom);
            g_autofree gchar *low = g_ascii_strdown(dom, -1);
            if (!is_valid_domain_token(low))
                continue;
            GString *rx = g_string_new("^https?://([^/]+\\.)?");
            for (const gchar *p = low; *p; p++) {
                if (*p == '.') g_string_append(rx, "\\.");
                else g_string_append_c(rx, *p);
            }
            g_string_append_c(rx, '/');
            gchar *rule = build_network_rule_json(rx->str, FALSE, NULL, NULL, NULL, NULL);
            g_string_free(rx, TRUE);
            if (rule && !g_hash_table_contains(seen_rules, rule)) {
                g_hash_table_add(seen_rules, g_strdup(rule));
                g_ptr_array_add(block_rules, rule);
            } else {
                g_free(rule);
            }
            continue;
        }

        /* 2. Skip cosmetic exceptions (#@#), extended (#?#), scriptlets (#$# / ##+js) */
        if (strstr(line, "#@#") || strstr(line, "#?#") || strstr(line, "#$#") || strstr(line, "##+js("))
            continue;

        /* 3. Cosmetic hiding rule: domains##selector (skip global generic ##selector) */
        const gchar *hh = strstr(line, "##");
        if (hh) {
            if (hh == line)
                continue;
            const gchar *sel = hh + 2;
            if (!is_safe_css_selector(sel))
                continue;
            g_autofree gchar *dkey = g_strndup(line, (gsize)(hh - line));
            if (!is_ascii_only(dkey))
                continue;
            GPtrArray *slist = g_hash_table_lookup(dom_cosmetic, dkey);
            if (!slist) {
                slist = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(dom_cosmetic, g_strdup(dkey), slist);
            }
            g_autofree gchar *dskey = g_strdup_printf("%s\x1f%s", dkey, sel);
            if (!g_hash_table_contains(seen_dom_sel, dskey)) {
                g_hash_table_add(seen_dom_sel, g_steal_pointer(&dskey));
                g_ptr_array_add(slist, g_strdup(sel));
            }
            continue;
        }

        /* 4. Network block or exception (@@) rule */
        gboolean is_exc = FALSE;
        const gchar *body = line;
        if (g_str_has_prefix(body, "@@")) {
            is_exc = TRUE;
            body += 2;
        }

        g_autofree gchar *pat_str = NULL;
        g_autofree gchar *load_type = NULL;
        g_autoptr(GPtrArray) res_types = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) if_doms = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) unless_doms = g_ptr_array_new_with_free_func(g_free);

        const gchar *dollar = strrchr(body, '$');
        if (dollar) {
            pat_str = g_strndup(body, (gsize)(dollar - body));
            if (!parse_abp_options(dollar + 1, &load_type, res_types, if_doms, unless_doms))
                continue;
        } else {
            pat_str = g_strdup(body);
        }

        g_autofree gchar *rx = abp_pattern_to_webkit_regex(pat_str);
        if (!rx)
            continue;
        if (!is_exc && is_youtube_player_telemetry_pattern(rx))
            continue;

        gchar *rule = build_network_rule_json(rx, is_exc, load_type, res_types, if_doms, unless_doms);
        if (!rule)
            continue;
        if (!g_hash_table_contains(seen_rules, rule)) {
            g_hash_table_add(seen_rules, g_strdup(rule));
            g_ptr_array_add(is_exc ? exc_rules : block_rules, rule);
        } else {
            g_free(rule);
        }
    }

    /* Assemble domain-scoped cosmetic rules (batched) */
    g_autoptr(GPtrArray) cosmetic_rules = g_ptr_array_new_with_free_func(g_free);

    GHashTableIter iter;
    gpointer k = NULL, v = NULL;
    g_hash_table_iter_init(&iter, dom_cosmetic);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        append_cosmetic_rule(cosmetic_rules, (const gchar *)k, (const GPtrArray *)v);
    }

    /* Emit final JSON in WebKit order: block rules -> exceptions -> cosmetic rules.
     * Reserve headroom for exceptions and cosmetics so they are never truncated. */
    guint cos_keep = MIN(cosmetic_rules->len, max_rules / 3);
    guint exc_keep = MIN(exc_rules->len, (max_rules - cos_keep) / 5);
    guint blk_keep = MIN(block_rules->len, max_rules - cos_keep - exc_keep);

    GString *out = g_string_new("[\n");
    guint count = 0;

    for (guint i = 0; i < blk_keep; i++) {
        if (count > 0) g_string_append(out, ",\n");
        g_string_append(out, g_ptr_array_index(block_rules, i));
        count++;
    }
    for (guint i = 0; i < exc_keep; i++) {
        if (count > 0) g_string_append(out, ",\n");
        g_string_append(out, g_ptr_array_index(exc_rules, i));
        count++;
    }
    for (guint i = 0; i < cos_keep; i++) {
        if (count > 0) g_string_append(out, ",\n");
        g_string_append(out, g_ptr_array_index(cosmetic_rules, i));
        count++;
    }
    g_string_append(out, "\n]\n");

    if (out_rule_count)
        *out_rule_count = count;
    return g_string_free(out, FALSE);
}
