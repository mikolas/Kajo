#ifndef KAJO_WEB_ABP_COMPILER_H
#define KAJO_WEB_ABP_COMPILER_H

#include <glib.h>

G_BEGIN_DECLS

/* Validate that a url-filter regex string is safe for WebKit's ContentExtension DFA
 * compiler (pure ASCII, no '|' disjunctions, no '{'/'}' quantifiers, balanced parens/brackets). */
gboolean web_abp_is_webkit_regex_safe(const gchar *regex);

/* Compile standard Adblock Plus / EasyList / EasyPrivacy / regional filter list text
 * (or plain hosts-file lines) into a WebKit Content Blocker JSON array string.
 *
 * Supported ABP syntax:
 *   - Network block rules: ||domain.tld^, ||domain.tld/path, |https://..., plain substring
 *   - Options: $third-party, $~third-party, $script, $image, $stylesheet, $xmlhttprequest,
 *              $subdocument, $font, $media, $domain=a.fi|b.fi|~c.fi
 *   - Exceptions: @@||domain^... -> {"action":{"type":"ignore-previous-rules"}}
 *   - Cosmetic hiding: ##selector and domain1,domain2##selector -> css-display-none
 *     (skipping uBlock-only procedural pseudo-classes like :has-text, :-abp-*, :upward,
 *      :style, #?#, #@#, ##+js)
 *   - Hosts lines: 0.0.0.0 ad.example.com / 127.0.0.1 ad.example.com
 *
 * max_rules caps the output count to stay safely under WebKit's per-filter ceiling
 * (recommended <= 35000 per filter bundle). Returns newly allocated JSON string. */
gchar   *web_abp_compile_text_to_json(const gchar *filter_text,
                                      guint        max_rules,
                                      guint       *out_rule_count);

G_END_DECLS

#endif /* KAJO_WEB_ABP_COMPILER_H */
