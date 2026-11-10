#ifndef KAJO_CONFIG_SCHEMA_H
#define KAJO_CONFIG_SCHEMA_H

#include "kajo_config_types.h"
#include "kajo_config_keys.h"
#include "../vendor/tomlc17/tomlc17.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KAJO_FIELD_BOOL = 0,
    KAJO_FIELD_INT,
    KAJO_FIELD_DOUBLE,
    KAJO_FIELD_STRING,
    KAJO_FIELD_HEX_COLOR,
    KAJO_FIELD_ENUM,
} KajoCfgFieldType;

typedef struct {
    const char *str;
    int         val;
} KajoEnumMap;

typedef struct {
    const char         *section;     /* NULL for top-level keys, or section name */
    const char         *key;         /* Key name from kajo_config_keys.h */
    KajoCfgFieldType    type;
    size_t              offset;      /* offsetof(DomainStruct, member) */
    union {
        gboolean    b;
        gint        i;
        gdouble     d;
        const char *s;
        gint        e;
    } def;
    gint                min_int;
    gint                max_int;
    gdouble             min_dbl;
    gdouble             max_dbl;
    const KajoEnumMap  *enum_map;    /* NULL-terminated { str, val } table */
    gint              (*normalize_int)(gint val, gint def_val);
} KajoFieldSpec;

typedef struct {
    KajoConfigDomain     domain;
    const char          *filename;           /* e.g. "shell.toml" */
    const char          *domain_table_name;  /* e.g. "shell" (for conf.d/ drop-ins) */
    size_t               struct_size;
    const KajoFieldSpec *fields;
    size_t               n_fields;
    void               (*init_custom_defaults)(void *struct_ptr);
    void               (*parse_custom_toml)(void *struct_ptr, toml_datum_t root_table, gboolean is_overlay);
    void               (*write_custom_toml)(const void *struct_ptr, GString *out);
    void               (*copy_custom_fields)(void *dst_ptr, const void *src_ptr);
    gboolean           (*equal_custom_fields)(const void *a_ptr, const void *b_ptr);
    void               (*free_custom_fields)(void *struct_ptr);
} KajoDomainSchema;

/* Individual domain schema getters */
const KajoDomainSchema *kajo_schema_get_theme(void);
const KajoDomainSchema *kajo_schema_get_shell(void);
const KajoDomainSchema *kajo_schema_get_ide(void);
const KajoDomainSchema *kajo_schema_get_files(void);
const KajoDomainSchema *kajo_schema_get_mod(void);
const KajoDomainSchema *kajo_config_get_domain_schema(KajoConfigDomain domain);

/* Generic Schema Engine Operations */
void     kajo_cfg_domain_init_defaults(const KajoDomainSchema *schema, void *dst);
void     kajo_cfg_domain_clear(const KajoDomainSchema *schema, void *ptr);
void     kajo_cfg_domain_copy_into(const KajoDomainSchema *schema, void *dst, const void *src);

void    *kajo_cfg_domain_new_rc_default(const KajoDomainSchema *schema);
void    *kajo_cfg_domain_copy_rc(const KajoDomainSchema *schema, const void *src);
void     kajo_cfg_domain_unref_rc(const KajoDomainSchema *schema, const void *rc_ptr);

void     kajo_cfg_domain_apply_toml_table(const KajoDomainSchema *schema,
                                          void                   *dst,
                                          toml_datum_t            tab,
                                          gboolean                is_overlay,
                                          const char             *source_label);

gboolean kajo_cfg_domain_parse_toml_str(const KajoDomainSchema *schema,
                                        void                   *dst,
                                        const char             *toml_str,
                                        gssize                  len,
                                        gboolean                is_overlay,
                                        const char             *source_label);

gboolean kajo_cfg_domain_parse_toml_file(const KajoDomainSchema *schema,
                                         void                   *dst,
                                         const char             *filepath,
                                         gboolean                is_overlay);

gchar   *kajo_cfg_domain_serialize_toml(const KajoDomainSchema *schema,
                                        const void             *src);

gboolean kajo_cfg_domain_save_atomic(const KajoDomainSchema *schema,
                                     const void             *src,
                                     const char             *filepath,
                                     GError                **error);

gboolean kajo_cfg_domain_equal(const KajoDomainSchema *schema,
                               const void             *a,
                               const void             *b);

/* Resolves shipped default asset path for a domain filename */
gchar   *kajo_config_find_default_file(const char *filename);

/* Shared helper to validate "#RRGGBB" hex strings */
gboolean kajo_config_is_valid_hex_color(const char *str);

/* Shared helper to append escaped TOML basic or literal strings */
void     kajo_toml_append_quoted_string(GString *out, const char *str);
void     kajo_toml_append_literal_or_quoted_string(GString *out, const char *str);

#ifdef __cplusplus
}
#endif

#endif /* KAJO_CONFIG_SCHEMA_H */
