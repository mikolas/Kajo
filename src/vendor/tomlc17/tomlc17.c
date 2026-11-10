/*
 * tomlc17.c — Self-contained C17 TOML 1.0 Parser for Kajo Desktop Suite
 * SPDX-License-Identifier: MIT
 */

#include "tomlc17.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define TOML_MAX_DEPTH       32
#define TOML_FLAG_INLINED    (1u << 0)
#define TOML_FLAG_EXPLICIT   (1u << 1)
#define TOML_FLAG_AOT        (1u << 2)
#define TOML_FLAG_SEALED     (1u << 3)

typedef struct mem_block_t {
    struct mem_block_t *next;
} mem_block_t;

typedef struct {
    mem_block_t *blocks;
} toml_pool_t;

typedef struct {
    const char    *src;
    const char    *end;
    const char    *cur;
    int            line;
    toml_pool_t   *pool;
    char          *errmsg;
    size_t         errmsg_cap;
    bool           has_err;
} parser_t;

static void *
pool_alloc(toml_pool_t *pool, size_t size)
{
    if (size > (size_t)INT_MAX)
        return NULL;
    size_t total = sizeof(mem_block_t) + size;
    mem_block_t *blk = (mem_block_t *)calloc(1, total);
    if (!blk)
        return NULL;
    blk->next = pool->blocks;
    pool->blocks = blk;
    return (void *)(blk + 1);
}

static char *
pool_strndup(toml_pool_t *pool, const char *s, size_t n)
{
    char *p = (char *)pool_alloc(pool, n + 1);
    if (!p)
        return NULL;
    if (n > 0 && s)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void
pool_destroy(toml_pool_t *pool)
{
    if (!pool)
        return;
    mem_block_t *b = pool->blocks;
    while (b) {
        mem_block_t *next = b->next;
        free(b);
        b = next;
    }
    free(pool);
}

static bool
parse_err(parser_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool
parse_err(parser_t *p, const char *fmt, ...)
{
    if (!p->has_err) {
        p->has_err = true;
        int n = snprintf(p->errmsg, p->errmsg_cap, "line %d: ", p->line);
        if (n > 0 && (size_t)n < p->errmsg_cap) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(p->errmsg + n, p->errmsg_cap - (size_t)n, fmt, ap);
            va_end(ap);
        }
    }
    return false;
}

static inline bool
is_ws(char c)
{
    return c == ' ' || c == '\t';
}

static inline bool
is_bare_key_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static void
skip_ws(parser_t *p)
{
    while (p->cur < p->end && is_ws(*p->cur))
        p->cur++;
}

static bool
skip_comment(parser_t *p)
{
    if (p->cur < p->end && *p->cur == '#') {
        p->cur++;
        while (p->cur < p->end && *p->cur != '\n') {
            unsigned char uc = (unsigned char)*p->cur;
            if ((uc < 0x20 && uc != '\t' && uc != '\r') || uc == 0x7f) {
                return parse_err(p, "invalid control character in comment");
            }
            if (uc == '\r') {
                if (p->cur + 1 >= p->end || p->cur[1] != '\n')
                    return parse_err(p, "bare CR in comment");
            }
            p->cur++;
        }
    }
    return true;
}

static bool
skip_ws_and_comments_newlines(parser_t *p)
{
    while (p->cur < p->end) {
        if (is_ws(*p->cur)) {
            p->cur++;
        } else if (*p->cur == '\r') {
            if (p->cur + 1 < p->end && p->cur[1] == '\n') {
                p->cur += 2;
                p->line++;
            } else {
                return parse_err(p, "bare CR not allowed in TOML");
            }
        } else if (*p->cur == '\n') {
            p->cur++;
            p->line++;
        } else if (*p->cur == '#') {
            if (!skip_comment(p))
                return false;
        } else {
            break;
        }
    }
    return true;
}

static bool
expect_eol_or_eof(parser_t *p)
{
    skip_ws(p);
    if (!skip_comment(p))
        return false;
    if (p->cur >= p->end)
        return true;
    if (*p->cur == '\r') {
        if (p->cur + 1 < p->end && p->cur[1] == '\n') {
            p->cur += 2;
            p->line++;
            return true;
        }
        return parse_err(p, "bare CR at end of line");
    }
    if (*p->cur == '\n') {
        p->cur++;
        p->line++;
        return true;
    }
    return parse_err(p, "unexpected token '%c' after statement", *p->cur);
}

static int
encode_utf8(uint32_t cp, char out[4])
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static int
hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool
parse_string(parser_t *p, bool allow_multiline, const char **out_str, int *out_len)
{
    if (p->cur >= p->end)
        return parse_err(p, "unexpected EOF expecting string");

    char quote = *p->cur;
    if (quote != '"' && quote != '\'')
        return parse_err(p, "expected string quote");

    bool is_literal = (quote == '\'');
    bool is_multiline = false;

    if (p->end - p->cur >= 3 && p->cur[0] == quote && p->cur[1] == quote && p->cur[2] == quote) {
        if (!allow_multiline)
            return parse_err(p, "multiline string not allowed in key");
        is_multiline = true;
        p->cur += 3;
        /* Trim immediate newline after opening ''' or """ */
        if (p->cur < p->end && *p->cur == '\r') {
            if (p->cur + 1 < p->end && p->cur[1] == '\n') {
                p->cur += 2;
                p->line++;
            } else {
                return parse_err(p, "bare CR in multiline string");
            }
        } else if (p->cur < p->end && *p->cur == '\n') {
            p->cur++;
            p->line++;
        }
    } else {
        p->cur++;
    }

    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return parse_err(p, "out of memory");

#define APPEND_CH(ch) do { \
    if (len + 1 >= cap) { \
        size_t new_cap = cap * 2; \
        char *nb = (char *)realloc(buf, new_cap); \
        if (!nb) { free(buf); return parse_err(p, "out of memory"); } \
        buf = nb; \
        cap = new_cap; \
    } \
    buf[len++] = (ch); \
} while (0)

    while (p->cur < p->end) {
        if (is_multiline) {
            if (p->end - p->cur >= 3 &&
                p->cur[0] == quote && p->cur[1] == quote && p->cur[2] == quote) {
                /* Up to 2 extra quotes allowed inside closing """"" */
                p->cur += 3;
                int extra_q = 0;
                while (p->cur < p->end && *p->cur == quote && extra_q < 2) {
                    APPEND_CH(quote);
                    p->cur++;
                    extra_q++;
                }
                if (p->cur < p->end && *p->cur == quote) {
                    free(buf);
                    return parse_err(p, "too many consecutive quotes in multiline string");
                }
                char *res = pool_strndup(p->pool, buf, len);
                free(buf);
                if (!res)
                    return parse_err(p, "out of memory");
                *out_str = res;
                *out_len = (int)len;
                return true;
            }
        } else {
            if (*p->cur == quote) {
                p->cur++;
                char *res = pool_strndup(p->pool, buf, len);
                free(buf);
                if (!res)
                    return parse_err(p, "out of memory");
                *out_str = res;
                *out_len = (int)len;
                return true;
            }
            if (*p->cur == '\n' || *p->cur == '\r') {
                free(buf);
                return parse_err(p, "unterminated single-line string");
            }
        }

        if (!is_literal && *p->cur == '\\') {
            p->cur++;
            if (p->cur >= p->end) {
                free(buf);
                return parse_err(p, "unterminated escape sequence");
            }
            if (is_multiline && (is_ws(*p->cur) || *p->cur == '\r' || *p->cur == '\n')) {
                /* Line-ending backslash trims whitespace and newlines */
                bool saw_nl = false;
                while (p->cur < p->end) {
                    if (is_ws(*p->cur)) {
                        p->cur++;
                    } else if (*p->cur == '\r' && p->cur + 1 < p->end && p->cur[1] == '\n') {
                        p->cur += 2;
                        p->line++;
                        saw_nl = true;
                    } else if (*p->cur == '\n') {
                        p->cur++;
                        p->line++;
                        saw_nl = true;
                    } else {
                        break;
                    }
                }
                if (!saw_nl) {
                    free(buf);
                    return parse_err(p, "invalid line-ending backslash escape");
                }
                continue;
            }

            char esc = *p->cur++;
            switch (esc) {
            case 'b':  APPEND_CH('\b'); break;
            case 't':  APPEND_CH('\t'); break;
            case 'n':  APPEND_CH('\n'); break;
            case 'f':  APPEND_CH('\f'); break;
            case 'r':  APPEND_CH('\r'); break;
            case '"':  APPEND_CH('"');  break;
            case '\\': APPEND_CH('\\'); break;
            case 'u':
            case 'U': {
                int nhex = (esc == 'u') ? 4 : 8;
                if (p->end - p->cur < nhex) {
                    free(buf);
                    return parse_err(p, "incomplete unicode escape");
                }
                uint32_t cp = 0;
                for (int i = 0; i < nhex; i++) {
                    int hv = hex_val(p->cur[i]);
                    if (hv < 0) {
                        free(buf);
                        return parse_err(p, "invalid hex digit in unicode escape");
                    }
                    cp = (cp << 4) | (uint32_t)hv;
                }
                p->cur += nhex;
                char u8[4];
                int ulen = encode_utf8(cp, u8);
                if (ulen <= 0) {
                    free(buf);
                    return parse_err(p, "invalid unicode scalar value");
                }
                for (int i = 0; i < ulen; i++)
                    APPEND_CH(u8[i]);
                break;
            }
            default:
                free(buf);
                return parse_err(p, "unknown escape sequence '\\%c'", esc);
            }
        } else {
            unsigned char uc = (unsigned char)*p->cur;
            if (uc == '\r') {
                if (is_multiline && p->cur + 1 < p->end && p->cur[1] == '\n') {
                    p->cur += 2;
                    p->line++;
                    APPEND_CH('\n');
                    continue;
                }
                free(buf);
                return parse_err(p, "bare CR in string");
            }
            if (uc == '\n') {
                p->cur++;
                p->line++;
                APPEND_CH('\n');
                continue;
            }
            if ((uc < 0x20 && uc != '\t') || uc == 0x7f) {
                free(buf);
                return parse_err(p, "invalid control character 0x%02x in string", uc);
            }
            APPEND_CH((char)uc);
            p->cur++;
        }
    }

#undef APPEND_CH
    free(buf);
    return parse_err(p, "unterminated string literal");
}

static bool
parse_single_key(parser_t *p, const char **out_key, int *out_len)
{
    skip_ws(p);
    if (p->cur >= p->end)
        return parse_err(p, "unexpected EOF expecting key");

    if (*p->cur == '"' || *p->cur == '\'') {
        return parse_string(p, false, out_key, out_len);
    }

    const char *start = p->cur;
    while (p->cur < p->end && is_bare_key_char(*p->cur))
        p->cur++;

    if (p->cur == start)
        return parse_err(p, "expected key name");

    size_t klen = (size_t)(p->cur - start);
    char *k = pool_strndup(p->pool, start, klen);
    if (!k)
        return parse_err(p, "out of memory");
    *out_key = k;
    *out_len = (int)klen;
    return true;
}

typedef struct {
    const char *ptr;
    int         len;
} key_part_t;

static bool
parse_multipart_key(parser_t *p, key_part_t *parts, int max_parts, int *out_nparts)
{
    int n = 0;
    while (true) {
        if (n >= max_parts)
            return parse_err(p, "dotted key exceeds maximum depth (%d)", max_parts);
        if (!parse_single_key(p, &parts[n].ptr, &parts[n].len))
            return false;
        n++;
        skip_ws(p);
        if (p->cur < p->end && *p->cur == '.') {
            p->cur++;
            continue;
        }
        break;
    }
    *out_nparts = n;
    return true;
}

static toml_datum_t *
table_find_mut(toml_datum_t *tab, const char *key, int klen)
{
    if (!tab || tab->type != TOML_TABLE)
        return NULL;
    for (int i = 0; i < tab->u.tab.size; i++) {
        if (tab->u.tab.len[i] == klen &&
            memcmp(tab->u.tab.key[i], key, (size_t)klen) == 0) {
            return &tab->u.tab.value[i];
        }
    }
    return NULL;
}

static toml_datum_t *
table_insert(parser_t *p, toml_datum_t *tab, const char *key, int klen, toml_datum_t val)
{
    if (table_find_mut(tab, key, klen) != NULL) {
        parse_err(p, "duplicate key '%.*s'", klen, key);
        return NULL;
    }
    int old_sz = tab->u.tab.size;
    int new_sz = old_sz + 1;

    const char **new_keys = (const char **)pool_alloc(p->pool, (size_t)new_sz * sizeof(const char *));
    int         *new_lens = (int *)pool_alloc(p->pool, (size_t)new_sz * sizeof(int));
    toml_datum_t *new_vals = (toml_datum_t *)pool_alloc(p->pool, (size_t)new_sz * sizeof(toml_datum_t));
    if (!new_keys || !new_lens || !new_vals) {
        parse_err(p, "out of memory");
        return NULL;
    }

    if (old_sz > 0) {
        memcpy(new_keys, tab->u.tab.key, (size_t)old_sz * sizeof(const char *));
        memcpy(new_lens, tab->u.tab.len, (size_t)old_sz * sizeof(int));
        memcpy(new_vals, tab->u.tab.value, (size_t)old_sz * sizeof(toml_datum_t));
    }

    new_keys[old_sz] = key;
    new_lens[old_sz] = klen;
    new_vals[old_sz] = val;

    tab->u.tab.key   = new_keys;
    tab->u.tab.len   = new_lens;
    tab->u.tab.value = new_vals;
    tab->u.tab.size  = new_sz;
    return &tab->u.tab.value[old_sz];
}

static toml_datum_t *
array_append(parser_t *p, toml_datum_t *arr, toml_datum_t elem)
{
    int old_sz = arr->u.arr.size;
    int new_sz = old_sz + 1;
    toml_datum_t *new_elems = (toml_datum_t *)pool_alloc(p->pool, (size_t)new_sz * sizeof(toml_datum_t));
    if (!new_elems) {
        parse_err(p, "out of memory");
        return NULL;
    }
    if (old_sz > 0) {
        memcpy(new_elems, arr->u.arr.elem, (size_t)old_sz * sizeof(toml_datum_t));
    }
    new_elems[old_sz] = elem;
    arr->u.arr.elem = new_elems;
    arr->u.arr.size = new_sz;
    return &arr->u.arr.elem[old_sz];
}

static bool
parse_number_or_bool(parser_t *p, toml_datum_t *out)
{
    const char *start = p->cur;
    while (p->cur < p->end) {
        char c = *p->cur;
        if (is_ws(c) || c == '\r' || c == '\n' || c == '#' || c == ',' || c == ']' || c == '}')
            break;
        p->cur++;
    }

    size_t raw_len = (size_t)(p->cur - start);
    if (raw_len == 0 || raw_len >= 128)
        return parse_err(p, "invalid value token");

    if (raw_len == 4 && memcmp(start, "true", 4) == 0) {
        out->type = TOML_BOOLEAN;
        out->u.boolean = true;
        return true;
    }
    if (raw_len == 5 && memcmp(start, "false", 5) == 0) {
        out->type = TOML_BOOLEAN;
        out->u.boolean = false;
        return true;
    }
    if ((raw_len == 3 && memcmp(start, "inf", 3) == 0) ||
        (raw_len == 4 && memcmp(start, "+inf", 4) == 0)) {
        out->type = TOML_FP64;
        out->u.fp64 = INFINITY;
        return true;
    }
    if (raw_len == 4 && memcmp(start, "-inf", 4) == 0) {
        out->type = TOML_FP64;
        out->u.fp64 = -INFINITY;
        return true;
    }
    if ((raw_len == 3 && memcmp(start, "nan", 3) == 0) ||
        (raw_len == 4 && (memcmp(start, "+nan", 4) == 0 || memcmp(start, "-nan", 4) == 0))) {
        out->type = TOML_FP64;
        out->u.fp64 = NAN;
        return true;
    }

    /* Strip valid underscores and check for float vs int */
    char clean[128];
    size_t clen = 0;
    bool is_float = false;
    int base = 10;
    size_t idx = 0;

    if (start[0] == '+' || start[0] == '-') {
        clean[clen++] = start[0];
        idx = 1;
    }

    if (idx + 2 <= raw_len && start[idx] == '0') {
        char pfx = start[idx + 1];
        if ((pfx == 'x' || pfx == 'o' || pfx == 'b') && clen == 0) {
            base = (pfx == 'x') ? 16 : ((pfx == 'o') ? 8 : 2);
            idx += 2;
            if (idx >= raw_len)
                return parse_err(p, "incomplete prefixed integer");
        }
    }

    bool prev_underscore = false;
    for (size_t i = idx; i < raw_len; i++) {
        char c = start[i];
        if (c == '_') {
            if (i == idx || i + 1 == raw_len || prev_underscore)
                return parse_err(p, "invalid underscore placement in number");
            char prev = start[i - 1];
            char next = start[i + 1];
            if (prev == '.' || prev == 'e' || prev == 'E' || prev == '+' || prev == '-' ||
                next == '.' || next == 'e' || next == 'E' || next == '+' || next == '-') {
                return parse_err(p, "invalid underscore placement in number");
            }
            prev_underscore = true;
            continue;
        }
        prev_underscore = false;
        if (base == 10 && (c == '.' || c == 'e' || c == 'E')) {
            is_float = true;
            if (c == '.') {
                if (i == idx || i + 1 == raw_len ||
                    !isdigit((unsigned char)start[i - 1]) ||
                    !isdigit((unsigned char)start[i + 1])) {
                    return parse_err(p, "invalid decimal point in float");
                }
            }
        }
        clean[clen++] = c;
    }
    clean[clen] = '\0';

    if (clen == 0 || (clen == 1 && (clean[0] == '+' || clean[0] == '-')))
        return parse_err(p, "invalid number literal");

    /* Disallow leading zeros in base-10 integers and float integer parts */
    if (base == 10) {
        size_t dstart = (clean[0] == '+' || clean[0] == '-') ? 1 : 0;
        if (clean[dstart] == '0' && dstart + 1 < clen &&
            clean[dstart + 1] != '.' && clean[dstart + 1] != 'e' && clean[dstart + 1] != 'E') {
            return parse_err(p, "leading zeros are not allowed in decimal numbers");
        }
    }

    if (is_float) {
        /* Reject multiple dots or multiple exponents */
        int dots = 0, exps = 0;
        for (size_t i = 0; i < clen; i++) {
            if (clean[i] == '.') {
                if (exps > 0)
                    return parse_err(p, "decimal point after exponent");
                dots++;
            } else if (clean[i] == 'e' || clean[i] == 'E') {
                exps++;
            }
        }
        if (dots > 1 || exps > 1)
            return parse_err(p, "malformed floating-point number");

        char *endptr = NULL;
        errno = 0;
        double dv = strtod(clean, &endptr);
        if (endptr != clean + clen)
            return parse_err(p, "invalid floating-point literal '%.*s'", (int)raw_len, start);
        out->type = TOML_FP64;
        out->u.fp64 = dv;
        return true;
    } else {
        char *endptr = NULL;
        errno = 0;
        long long iv = strtoll(clean, &endptr, base);
        if (errno == ERANGE || endptr != clean + clen)
            return parse_err(p, "invalid integer literal '%.*s'", (int)raw_len, start);
        out->type = TOML_INT64;
        out->u.int64 = (int64_t)iv;
        return true;
    }
}

static bool parse_value(parser_t *p, int depth, toml_datum_t *out);

static bool
parse_array(parser_t *p, int depth, toml_datum_t *out)
{
    if (depth > TOML_MAX_DEPTH)
        return parse_err(p, "maximum nesting depth exceeded");

    p->cur++; /* skip '[' */
    memset(out, 0, sizeof(*out));
    out->type = TOML_ARRAY;

    while (true) {
        if (!skip_ws_and_comments_newlines(p))
            return false;
        if (p->cur >= p->end)
            return parse_err(p, "unterminated array");
        if (*p->cur == ']') {
            p->cur++;
            return true;
        }

        toml_datum_t elem;
        memset(&elem, 0, sizeof(elem));
        if (!parse_value(p, depth + 1, &elem))
            return false;
        if (!array_append(p, out, elem))
            return false;

        if (!skip_ws_and_comments_newlines(p))
            return false;
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            continue;
        }
        if (p->cur < p->end && *p->cur == ']') {
            p->cur++;
            return true;
        }
        return parse_err(p, "expected ',' or ']' in array");
    }
}

static bool
insert_dotted_keyval(parser_t *p, toml_datum_t *root_tab,
                     const key_part_t *parts, int nparts,
                     toml_datum_t val)
{
    toml_datum_t *cur = root_tab;
    for (int i = 0; i < nparts - 1; i++) {
        toml_datum_t *existing = table_find_mut(cur, parts[i].ptr, parts[i].len);
        if (!existing) {
            toml_datum_t sub;
            memset(&sub, 0, sizeof(sub));
            sub.type = TOML_TABLE;
            existing = table_insert(p, cur, parts[i].ptr, parts[i].len, sub);
            if (!existing)
                return false;
        } else if (existing->type != TOML_TABLE || (existing->flag & TOML_FLAG_SEALED)) {
            return parse_err(p, "cannot extend non-table or inline table '%.*s'",
                             parts[i].len, parts[i].ptr);
        }
        cur = existing;
    }

    const key_part_t *last = &parts[nparts - 1];
    return table_insert(p, cur, last->ptr, last->len, val) != NULL;
}

static bool
parse_inline_table(parser_t *p, int depth, toml_datum_t *out)
{
    if (depth > TOML_MAX_DEPTH)
        return parse_err(p, "maximum nesting depth exceeded");

    p->cur++; /* skip '{' */
    memset(out, 0, sizeof(*out));
    out->type = TOML_TABLE;
    out->flag = TOML_FLAG_INLINED;

    skip_ws(p);
    if (p->cur < p->end && *p->cur == '}') {
        p->cur++;
        out->flag |= TOML_FLAG_SEALED;
        return true;
    }

    while (true) {
        skip_ws(p);
        if (p->cur >= p->end)
            return parse_err(p, "unterminated inline table");

        key_part_t parts[TOML_MAX_DEPTH];
        int nparts = 0;
        if (!parse_multipart_key(p, parts, TOML_MAX_DEPTH, &nparts))
            return false;

        skip_ws(p);
        if (p->cur >= p->end || *p->cur != '=')
            return parse_err(p, "expected '=' in inline table");
        p->cur++;

        skip_ws(p);
        toml_datum_t val;
        memset(&val, 0, sizeof(val));
        if (!parse_value(p, depth + 1, &val))
            return false;

        if (!insert_dotted_keyval(p, out, parts, nparts, val))
            return false;

        skip_ws(p);
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            skip_ws(p);
            continue;
        }
        if (p->cur < p->end && *p->cur == '}') {
            p->cur++;
            out->flag |= TOML_FLAG_SEALED;
            return true;
        }
        return parse_err(p, "expected ',' or '}' in inline table");
    }
}

static bool
parse_value(parser_t *p, int depth, toml_datum_t *out)
{
    skip_ws(p);
    if (p->cur >= p->end)
        return parse_err(p, "unexpected EOF expecting value");

    char c = *p->cur;
    if (c == '"' || c == '\'') {
        out->type = TOML_STRING;
        return parse_string(p, true, &out->u.str.ptr, &out->u.str.len);
    }
    if (c == '[') {
        return parse_array(p, depth, out);
    }
    if (c == '{') {
        return parse_inline_table(p, depth, out);
    }
    return parse_number_or_bool(p, out);
}

static toml_datum_t *
resolve_table_header(parser_t *p, toml_datum_t *toptab,
                     const key_part_t *parts, int nparts, bool is_aot)
{
    toml_datum_t *cur = toptab;
    for (int i = 0; i < nparts - 1; i++) {
        toml_datum_t *existing = table_find_mut(cur, parts[i].ptr, parts[i].len);
        if (!existing) {
            toml_datum_t sub;
            memset(&sub, 0, sizeof(sub));
            sub.type = TOML_TABLE;
            existing = table_insert(p, cur, parts[i].ptr, parts[i].len, sub);
            if (!existing)
                return NULL;
        } else if (existing->type == TOML_ARRAY && (existing->flag & TOML_FLAG_AOT)) {
            if (existing->u.arr.size == 0) {
                parse_err(p, "empty array of tables '%.*s'", parts[i].len, parts[i].ptr);
                return NULL;
            }
            existing = &existing->u.arr.elem[existing->u.arr.size - 1];
        } else if (existing->type != TOML_TABLE || (existing->flag & TOML_FLAG_SEALED)) {
            parse_err(p, "key '%.*s' is not a standard table", parts[i].len, parts[i].ptr);
            return NULL;
        }
        cur = existing;
    }

    const key_part_t *last = &parts[nparts - 1];
    toml_datum_t *target = table_find_mut(cur, last->ptr, last->len);

    if (is_aot) {
        if (!target) {
            toml_datum_t arr;
            memset(&arr, 0, sizeof(arr));
            arr.type = TOML_ARRAY;
            arr.flag = TOML_FLAG_AOT;
            target = table_insert(p, cur, last->ptr, last->len, arr);
            if (!target)
                return NULL;
        } else if (target->type != TOML_ARRAY || !(target->flag & TOML_FLAG_AOT)) {
            parse_err(p, "cannot redefine '%.*s' as array of tables", last->len, last->ptr);
            return NULL;
        }
        toml_datum_t elem_tab;
        memset(&elem_tab, 0, sizeof(elem_tab));
        elem_tab.type = TOML_TABLE;
        elem_tab.flag = TOML_FLAG_EXPLICIT;
        return array_append(p, target, elem_tab);
    } else {
        if (!target) {
            toml_datum_t sub;
            memset(&sub, 0, sizeof(sub));
            sub.type = TOML_TABLE;
            sub.flag = TOML_FLAG_EXPLICIT;
            return table_insert(p, cur, last->ptr, last->len, sub);
        }
        if (target->type != TOML_TABLE ||
            (target->flag & TOML_FLAG_EXPLICIT) ||
            (target->flag & TOML_FLAG_SEALED)) {
            parse_err(p, "duplicate or invalid table header '%.*s'", last->len, last->ptr);
            return NULL;
        }
        target->flag |= TOML_FLAG_EXPLICIT;
        return target;
    }
}

toml_option_t
toml_default_option(void)
{
    toml_option_t opt = { true, realloc, free };
    return opt;
}

toml_result_t
toml_parse(const char *src, int len)
{
    toml_result_t res;
    memset(&res, 0, sizeof(res));

    if (!src || len < 0) {
        snprintf(res.errmsg, sizeof(res.errmsg), "invalid NULL input buffer");
        return res;
    }

    /* Reject embedded NUL bytes inside len */
    if (memchr(src, '\0', (size_t)len) != NULL) {
        snprintf(res.errmsg, sizeof(res.errmsg), "embedded NUL byte in TOML input");
        return res;
    }

    toml_pool_t *pool = (toml_pool_t *)calloc(1, sizeof(toml_pool_t));
    if (!pool) {
        snprintf(res.errmsg, sizeof(res.errmsg), "out of memory");
        return res;
    }

    res._internal = pool;
    res.toptab.type = TOML_TABLE;

    parser_t p;
    memset(&p, 0, sizeof(p));
    p.src        = src;
    p.end        = src + len;
    p.cur        = src;
    p.line       = 1;
    p.pool       = pool;
    p.errmsg     = res.errmsg;
    p.errmsg_cap = sizeof(res.errmsg);

    /* Skip UTF-8 BOM if present */
    if (len >= 3 &&
        (unsigned char)p.cur[0] == 0xEF &&
        (unsigned char)p.cur[1] == 0xBB &&
        (unsigned char)p.cur[2] == 0xBF) {
        p.cur += 3;
    }

    toml_datum_t *cur_tab = &res.toptab;

    while (p.cur < p.end) {
        if (!skip_ws_and_comments_newlines(&p)) {
            toml_free(res);
            memset(&res.toptab, 0, sizeof(res.toptab));
            res._internal = NULL;
            return res;
        }
        if (p.cur >= p.end)
            break;

        if (*p.cur == '[') {
            bool is_aot = false;
            p.cur++;
            if (p.cur < p.end && *p.cur == '[') {
                is_aot = true;
                p.cur++;
            }

            key_part_t parts[TOML_MAX_DEPTH];
            int nparts = 0;
            if (!parse_multipart_key(&p, parts, TOML_MAX_DEPTH, &nparts)) {
                toml_free(res);
                memset(&res.toptab, 0, sizeof(res.toptab));
                res._internal = NULL;
                return res;
            }

            skip_ws(&p);
            if (is_aot) {
                if (p.end - p.cur < 2 || p.cur[0] != ']' || p.cur[1] != ']') {
                    parse_err(&p, "expected ']]' closing array of tables");
                    toml_free(res);
                    memset(&res.toptab, 0, sizeof(res.toptab));
                    res._internal = NULL;
                    return res;
                }
                p.cur += 2;
            } else {
                if (p.cur >= p.end || *p.cur != ']') {
                    parse_err(&p, "expected ']' closing table header");
                    toml_free(res);
                    memset(&res.toptab, 0, sizeof(res.toptab));
                    res._internal = NULL;
                    return res;
                }
                p.cur++;
            }

            if (!expect_eol_or_eof(&p)) {
                toml_free(res);
                memset(&res.toptab, 0, sizeof(res.toptab));
                res._internal = NULL;
                return res;
            }

            cur_tab = resolve_table_header(&p, &res.toptab, parts, nparts, is_aot);
            if (!cur_tab) {
                toml_free(res);
                memset(&res.toptab, 0, sizeof(res.toptab));
                res._internal = NULL;
                return res;
            }
        } else {
            key_part_t parts[TOML_MAX_DEPTH];
            int nparts = 0;
            if (!parse_multipart_key(&p, parts, TOML_MAX_DEPTH, &nparts)) {
                toml_free(res);
                memset(&res.toptab, 0, sizeof(res.toptab));
                res._internal = NULL;
                return res;
            }

            skip_ws(&p);
            if (p.cur >= p.end || *p.cur != '=') {
                parse_err(&p, "expected '=' after key");
                toml_free(res);
                memset(&res.toptab, 0, sizeof(res.toptab));
                res._internal = NULL;
                return res;
            }
            p.cur++;

            toml_datum_t val;
            memset(&val, 0, sizeof(val));
            if (!parse_value(&p, 1, &val) ||
                !insert_dotted_keyval(&p, cur_tab, parts, nparts, val) ||
                !expect_eol_or_eof(&p)) {
                toml_free(res);
                memset(&res.toptab, 0, sizeof(res.toptab));
                res._internal = NULL;
                return res;
            }
        }
    }

    res.ok = true;
    return res;
}

toml_result_t
toml_parse_file(FILE *fp)
{
    toml_result_t res;
    memset(&res, 0, sizeof(res));
    if (!fp) {
        snprintf(res.errmsg, sizeof(res.errmsg), "NULL file pointer");
        return res;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        snprintf(res.errmsg, sizeof(res.errmsg), "fseek failed");
        return res;
    }
    long fsz = ftell(fp);
    if (fsz < 0 || fsz > 64L * 1024L * 1024L) {
        snprintf(res.errmsg, sizeof(res.errmsg), "invalid or oversized TOML file");
        return res;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        snprintf(res.errmsg, sizeof(res.errmsg), "fseek failed");
        return res;
    }

    char *buf = (char *)malloc((size_t)fsz + 1);
    if (!buf) {
        snprintf(res.errmsg, sizeof(res.errmsg), "out of memory");
        return res;
    }

    size_t nread = fread(buf, 1, (size_t)fsz, fp);
    buf[nread] = '\0';
    res = toml_parse(buf, (int)nread);
    free(buf);
    return res;
}

toml_result_t
toml_parse_file_ex(const char *fname)
{
    toml_result_t res;
    memset(&res, 0, sizeof(res));
    if (!fname) {
        snprintf(res.errmsg, sizeof(res.errmsg), "NULL filename");
        return res;
    }
    FILE *fp = fopen(fname, "rb");
    if (!fp) {
        snprintf(res.errmsg, sizeof(res.errmsg), "cannot open '%s'", fname);
        return res;
    }
    res = toml_parse_file(fp);
    fclose(fp);
    return res;
}

void
toml_free(toml_result_t result)
{
    if (result._internal) {
        pool_destroy((toml_pool_t *)result._internal);
    }
}

toml_datum_t
toml_get(toml_datum_t datum, const char *key)
{
    toml_datum_t none;
    memset(&none, 0, sizeof(none));
    none.type = TOML_UNKNOWN;

    if (datum.type != TOML_TABLE || !key)
        return none;

    int klen = (int)strlen(key);
    for (int i = 0; i < datum.u.tab.size; i++) {
        if (datum.u.tab.len[i] == klen &&
            memcmp(datum.u.tab.key[i], key, (size_t)klen) == 0) {
            return datum.u.tab.value[i];
        }
    }
    return none;
}

toml_datum_t
toml_seek(toml_datum_t table, const char *multipart_key)
{
    toml_datum_t none;
    memset(&none, 0, sizeof(none));
    none.type = TOML_UNKNOWN;

    if (table.type != TOML_TABLE || !multipart_key || !*multipart_key)
        return none;

    toml_datum_t cur = table;
    const char *p = multipart_key;
    while (*p) {
        if (cur.type != TOML_TABLE)
            return none;
        const char *dot = strchr(p, '.');
        size_t klen = dot ? (size_t)(dot - p) : strlen(p);
        bool found = false;
        for (int i = 0; i < cur.u.tab.size; i++) {
            if ((size_t)cur.u.tab.len[i] == klen &&
                memcmp(cur.u.tab.key[i], p, klen) == 0) {
                cur = cur.u.tab.value[i];
                found = true;
                break;
            }
        }
        if (!found)
            return none;
        if (!dot)
            return cur;
        p = dot + 1;
    }
    return cur;
}
