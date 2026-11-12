/* comp_diag.c — see comp_diag.h. */
#include "comp_diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
comp_diag_init(CompDiagList *d)
{
    memset(d, 0, sizeof(*d));
}

void
comp_diag_clear(CompDiagList *d)
{
    for (size_t i = 0; i < d->n; i++)
        free(d->msgs[i]);
    free(d->msgs);
    memset(d, 0, sizeof(*d));
}

void
comp_diag_add(CompDiagList *d, const char *source, int line, const char *fmt, ...)
{
    if (!d)
        return;
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char full[640];
    if (source && line > 0)
        snprintf(full, sizeof(full), "%s:%d: %s", source, line, body);
    else if (source)
        snprintf(full, sizeof(full), "%s: %s", source, body);
    else
        snprintf(full, sizeof(full), "%s", body);

    if (d->n == d->cap) {
        size_t ncap = d->cap ? d->cap * 2 : 8;
        char **p = realloc(d->msgs, ncap * sizeof(char *));
        if (!p) {
            d->oom = true;
            return;
        }
        d->msgs = p;
        d->cap  = ncap;
    }
    char *copy = strdup(full);
    if (!copy) {
        d->oom = true;
        return;
    }
    d->msgs[d->n++] = copy;
}

bool
comp_diag_contains(const CompDiagList *d, const char *needle)
{
    for (size_t i = 0; d && needle && i < d->n; i++)
        if (strstr(d->msgs[i], needle))
            return true;
    return false;
}
