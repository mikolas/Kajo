/* Highlight queries for the query-driven tree-sitter languages.
 *
 * Capture names: @keyword @keyword.control @function @type @constant @variable
 * @string @string.raw @number @comment @operator @none. Later patterns override
 * earlier ones, so each query goes from generic (`(identifier) @variable`) to
 * specific. Comments, numbers, primitive/type identifiers, true/false/null and
 * (for programming languages) anonymous keyword/operator tokens are already
 * colored by the structural base pass in ide_ts.c; queries add what needs
 * context. Every query is compiled by the ide-ts test suite against the
 * installed grammar, so a grammar update that renames a node fails the build
 * tests instead of silently dropping colors. */
#include "ide_ts.h"

#include <string.h>

static const char kQueryRust[] =
  "(identifier) @variable\n"
  "(field_identifier) @variable\n"
  "(shorthand_field_identifier) @variable\n"
  "(lifetime) @type\n"
  "((identifier) @type (#match? @type \"^[A-Z][a-z]\"))\n"
  "((identifier) @constant (#match? @constant \"^[A-Z][A-Z0-9_]+$\"))\n"
  "(scoped_identifier path: (identifier) @type (#match? @type \"^[A-Z]\"))\n"
  "(scoped_identifier name: (identifier) @constant (#match? @constant \"^[A-Z][A-Z0-9_]+$\"))\n"
  "(function_item name: (identifier) @function)\n"
  "(function_signature_item name: (identifier) @function)\n"
  "(call_expression function: (identifier) @function)\n"
  "(call_expression function: (scoped_identifier name: (identifier) @function))\n"
  "(call_expression function: (field_expression field: (field_identifier) @function))\n"
  "(generic_function function: (identifier) @function)\n"
  "(generic_function function: (scoped_identifier name: (identifier) @function))\n"
  "(generic_function function: (field_expression field: (field_identifier) @function))\n"
  "(macro_invocation macro: (identifier) @constant)\n"
  "(macro_invocation macro: (scoped_identifier name: (identifier) @constant))\n"
  "(macro_definition name: (identifier) @constant)\n"
  "(const_item name: (identifier) @constant)\n"
  "(static_item name: (identifier) @constant)\n"
  "(enum_variant name: (identifier) @constant)\n"
  "(boolean_literal) @constant\n"
  "(self) @keyword\n"
  "(mutable_specifier) @keyword\n"
  "(crate) @keyword\n"
  "(super) @keyword\n"
  "(attribute_item) @constant\n"
  "(inner_attribute_item) @constant\n"
  "(string_literal) @string\n"
  "(raw_string_literal) @string.raw\n"
  "(char_literal) @string\n"
  "(integer_literal) @number\n"
  "(float_literal) @number\n";

static const char kQueryCpp[] =
  "(identifier) @variable\n"
  "(field_identifier) @variable\n"
  "(statement_identifier) @variable\n"
  "(namespace_identifier) @type\n"
  "((identifier) @constant (#match? @constant \"^[A-Z][A-Z0-9_]+$\"))\n"
  "(qualified_identifier scope: (namespace_identifier) @type name: (identifier) @constant\n"
  "  (#match? @type \"^[A-Z]\") (#match? @constant \"^[A-Z]\"))\n"
  "(attribute name: (identifier) @constant)\n"
  "(function_declarator declarator: (identifier) @function)\n"
  "(function_declarator declarator: (field_identifier) @function)\n"
  "(function_declarator declarator: (qualified_identifier name: (identifier) @function))\n"
  "(function_declarator declarator: (destructor_name) @function)\n"
  "(function_declarator declarator: (operator_name) @function)\n"
  "(call_expression function: (identifier) @function)\n"
  "(call_expression function: (qualified_identifier name: (identifier) @function))\n"
  "(call_expression function: (field_expression field: (field_identifier) @function))\n"
  "(call_expression function: (template_function name: (identifier) @function))\n"
  "(template_function name: (identifier) @function)\n"
  "(preproc_def name: (identifier) @constant)\n"
  "(preproc_function_def name: (identifier) @constant)\n"
  "(enumerator name: (identifier) @constant)\n"
  "(auto) @keyword\n"
  "(virtual_specifier) @keyword\n"
  "(access_specifier) @keyword\n"
  "(raw_string_literal) @string.raw\n";

static const char kQueryBash[] =
  "(variable_name) @variable\n"
  "(special_variable_name) @variable\n"
  "(command_name) @function\n"
  "(function_definition name: (word) @function)\n"
  "(string) @string\n"
  "(raw_string) @string\n"
  "(ansi_c_string) @string\n"
  "(heredoc_body) @string\n"
  "(regex) @string\n"
  "(simple_expansion (variable_name) @variable)\n"
  "(simple_expansion (special_variable_name) @variable)\n"
  "(expansion (variable_name) @variable)\n"
  "(heredoc_start) @constant\n"
  "(heredoc_end) @constant\n"
  "(test_operator) @operator\n"
  "(file_descriptor) @number\n";

/* Shared by JavaScript, TypeScript and TSX. */
#define JS_COMMON_QUERY \
  "(identifier) @variable\n" \
  "(property_identifier) @variable\n" \
  "(shorthand_property_identifier) @variable\n" \
  "(shorthand_property_identifier_pattern) @variable\n" \
  "(statement_identifier) @variable\n" \
  "(private_property_identifier) @variable\n" \
  "((identifier) @type (#match? @type \"^[A-Z][a-z]\"))\n" \
  "((identifier) @constant (#match? @constant \"^[A-Z][A-Z0-9_]+$\"))\n" \
  "(function_declaration name: (identifier) @function)\n" \
  "(generator_function_declaration name: (identifier) @function)\n" \
  "(method_definition name: (property_identifier) @function)\n" \
  "(call_expression function: (identifier) @function)\n" \
  "(call_expression function: (member_expression property: (property_identifier) @function))\n" \
  "(new_expression constructor: (identifier) @type)\n" \
  "(variable_declarator name: (identifier) @function value: (arrow_function))\n" \
  "(variable_declarator name: (identifier) @function value: (function_expression))\n" \
  "(pair key: (property_identifier) @function value: (arrow_function))\n" \
  "(pair key: (property_identifier) @function value: (function_expression))\n" \
  "(decorator) @constant\n" \
  "(regex) @string\n" \
  "(string) @string\n" \
  "(template_string) @string\n" \
  "(template_substitution (identifier) @variable)\n"

#define JS_ONLY_QUERY \
  "(class_declaration name: (identifier) @type)\n" \
  "(class_heritage (identifier) @type)\n"

#define TS_ONLY_QUERY \
  "(extends_clause value: (identifier) @type)\n" \
  "(enum_declaration name: (identifier) @type)\n" \
  "(enum_assignment name: (property_identifier) @constant)\n" \
  "(method_signature name: (property_identifier) @function)\n" \
  "(accessibility_modifier) @keyword\n" \
  "(override_modifier) @keyword\n"

#define JSX_QUERY \
  "(jsx_opening_element name: (identifier) @type)\n" \
  "(jsx_closing_element name: (identifier) @type)\n" \
  "(jsx_self_closing_element name: (identifier) @type)\n" \
  "(jsx_attribute (property_identifier) @variable)\n"

static const char kQueryJavascript[] = JS_COMMON_QUERY JS_ONLY_QUERY JSX_QUERY;
static const char kQueryTypescript[] = JS_COMMON_QUERY TS_ONLY_QUERY;
static const char kQueryTsx[]        = JS_COMMON_QUERY TS_ONLY_QUERY JSX_QUERY;

static const char kQueryJson[] =
  "(string) @string\n"
  "(pair key: (string) @variable)\n";

static const char kQueryToml[] =
  "(bare_key) @variable\n"
  "(quoted_key) @variable\n"
  "(table (bare_key) @type)\n"
  "(table (dotted_key (bare_key) @type))\n"
  "(table_array_element (bare_key) @type)\n"
  "(table_array_element (dotted_key (bare_key) @type))\n"
  "(string) @string\n"
  "(boolean) @constant\n"
  "(offset_date_time) @number\n"
  "(local_date_time) @number\n"
  "(local_date) @number\n"
  "(local_time) @number\n";

static const char kQueryYaml[] =
  "(block_mapping_pair key: (flow_node (plain_scalar (string_scalar) @variable)))\n"
  "(block_mapping_pair key: (flow_node (double_quote_scalar) @variable))\n"
  "(block_mapping_pair key: (flow_node (single_quote_scalar) @variable))\n"
  "(flow_pair key: (flow_node (plain_scalar (string_scalar) @variable)))\n"
  "(double_quote_scalar) @string\n"
  "(single_quote_scalar) @string\n"
  "(block_scalar) @string\n"
  "(integer_scalar) @number\n"
  "(float_scalar) @number\n"
  "(boolean_scalar) @constant\n"
  "(null_scalar) @constant\n"
  "(anchor) @constant\n"
  "(alias) @constant\n"
  "(tag) @type\n";

static const char kQueryCss[] =
  "(tag_name) @type\n"
  "(class_name) @type\n"
  "(id_name) @constant\n"
  "(namespace_name) @type\n"
  "(property_name) @variable\n"
  "(feature_name) @variable\n"
  "(attribute_name) @variable\n"
  "(function_name) @function\n"
  "(at_keyword) @keyword\n"
  "[\"@import\" \"@media\" \"@charset\" \"@namespace\" \"@supports\" \"@keyframes\" \"and\" \"or\" \"not\" \"only\"] @keyword\n"
  "(keyframes_name) @type\n"
  "(important) @keyword\n"
  "(color_value) @number\n"
  "(unit) @number\n"
  "(string_value) @string\n"
  "(pseudo_class_selector (class_name) @function)\n"
  "(pseudo_element_selector (tag_name) @function)\n";

static const char kQueryHtml[] =
  "(tag_name) @type\n"
  "(erroneous_end_tag_name) @type\n"
  "(attribute_name) @variable\n"
  "(attribute_value) @string\n"
  "(quoted_attribute_value) @string\n"
  "(doctype) @keyword\n"
  "(entity) @constant\n"
  "[\"<\" \">\" \"</\" \"/>\" \"<!\"] @operator\n";

static const char kQueryLua[] =
  "(identifier) @variable\n"
  "((identifier) @constant (#match? @constant \"^[A-Z][A-Z0-9_]+$\"))\n"
  "((identifier) @keyword (#eq? @keyword \"self\"))\n"
  "(function_call name: (identifier) @function)\n"
  "(function_call name: (dot_index_expression field: (identifier) @function))\n"
  "(function_call name: (method_index_expression method: (identifier) @function))\n"
  "(function_declaration name: (identifier) @function)\n"
  "(function_declaration name: (dot_index_expression field: (identifier) @function))\n"
  "(function_declaration name: (method_index_expression method: (identifier) @function))\n"
  "(string) @string\n"
  "(hash_bang_line) @comment\n";

const char *
ide_ts_query_source(IdeTsLang lang)
{
  switch (lang) {
    case IDE_TS_LANG_RUST:       return kQueryRust;
    case IDE_TS_LANG_CPP:        return kQueryCpp;
    case IDE_TS_LANG_BASH:       return kQueryBash;
    case IDE_TS_LANG_JAVASCRIPT: return kQueryJavascript;
    case IDE_TS_LANG_TYPESCRIPT: return kQueryTypescript;
    case IDE_TS_LANG_TSX:        return kQueryTsx;
    case IDE_TS_LANG_JSON:       return kQueryJson;
    case IDE_TS_LANG_TOML:       return kQueryToml;
    case IDE_TS_LANG_YAML:       return kQueryYaml;
    case IDE_TS_LANG_CSS:        return kQueryCss;
    case IDE_TS_LANG_HTML:       return kQueryHtml;
    case IDE_TS_LANG_LUA:        return kQueryLua;
    default:                     return NULL;
  }
}
