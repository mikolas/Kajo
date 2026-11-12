/* comp_diag.h — collected, human-readable config diagnostics (pure C).
 *
 * Parsers never abort on bad user input: they skip the offending entry and add
 * a message here ("binds.toml:12: unknown action 'focus-colum-left'"). The
 * compositor logs them and can show them to the user after a reload.
 */
#ifndef KAJO_COMP_DIAG_H
#define KAJO_COMP_DIAG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char  **msgs;
    size_t  n, cap;
    bool    oom;      /* set if a message could not be stored */
} CompDiagList;

void   comp_diag_init(CompDiagList *d);
void   comp_diag_clear(CompDiagList *d);
/* `source` may be NULL; `line` <= 0 omits the line number. */
void   comp_diag_add(CompDiagList *d, const char *source, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
/* True if any stored message contains `needle` (tests). */
bool   comp_diag_contains(const CompDiagList *d, const char *needle);

#endif /* KAJO_COMP_DIAG_H */
