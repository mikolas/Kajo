/*
 * tomlc17.h — Self-contained C17 TOML 1.0 Parser for Kajo Desktop Suite
 * SPDX-License-Identifier: MIT
 */

#ifndef TOMLC17_H
#define TOMLC17_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOML_UNKNOWN = 0,
    TOML_STRING,
    TOML_INT64,
    TOML_FP64,
    TOML_BOOLEAN,
    TOML_DATE,
    TOML_TIME,
    TOML_DATETIME,
    TOML_DATETIMETZ,
    TOML_ARRAY,
    TOML_TABLE,
} toml_type_t;

typedef struct toml_datum_t toml_datum_t;

struct toml_datum_t {
    toml_type_t type;
    uint32_t    flag;
    union {
        bool    boolean;
        int64_t int64;
        double  fp64;
        struct {
            const char *ptr;
            int         len;
        } str;
        struct {
            int           size;
            toml_datum_t *elem;
        } arr;
        struct {
            int           size;
            const char  **key;
            int          *len;
            toml_datum_t *value;
        } tab;
    } u;
};

typedef struct {
    bool         ok;
    toml_datum_t toptab;
    char         errmsg[200];
    void        *_internal;
} toml_result_t;

typedef struct {
    bool    check_utf8;
    void *(*mem_realloc)(void *ptr, size_t size);
    void  (*mem_free)(void *ptr);
} toml_option_t;

toml_option_t toml_default_option(void);
toml_result_t toml_parse(const char *src, int len);
toml_result_t toml_parse_file(FILE *fp);
toml_result_t toml_parse_file_ex(const char *fname);
void          toml_free(toml_result_t result);

toml_datum_t  toml_get(toml_datum_t datum, const char *key);
toml_datum_t  toml_seek(toml_datum_t table, const char *multipart_key);

#ifdef __cplusplus
}
#endif

#endif /* TOMLC17_H */
