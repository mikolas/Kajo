#include "amiga_window_private.h"

#include <json-glib/json-glib.h>
#include <string.h>

static void
apply_publish_diagnostics(KajoAmigaWindow *self, JsonNode *params)
{
  if (!self->active_buf || !params || !JSON_NODE_HOLDS_OBJECT(params)) return;
  JsonObject *o = json_node_get_object(params);
  if (!json_object_has_member(o, "uri") || !json_object_has_member(o, "diagnostics")) return;
  const char *uri = json_object_get_string_member(o, "uri");
  const char *apath = ide_buffer_get_file_path(self->active_buf);
  if (!uri || !apath) return;
  g_autofree char *active_uri = g_filename_to_uri(apath, NULL, NULL);
  if (g_strcmp0(uri, active_uri) != 0) return;

  ide_buffer_clear_diagnostics(self->active_buf);
  JsonArray *arr = json_object_get_array_member(o, "diagnostics");
  guint len = arr ? json_array_get_length(arr) : 0;
  for (guint i = 0; i < len; i++) {
    JsonObject *d = json_array_get_object_element(arr, i);
    if (!d || !json_object_has_member(d, "range")) continue;
    JsonObject *rng = json_object_get_object_member(d, "range");
    JsonObject *st = rng ? json_object_get_object_member(rng, "start") : NULL;
    guint line1 = st ? (guint)json_object_get_int_member(st, "line") + 1u : 1u;
    guint col1 = st ? (guint)json_object_get_int_member(st, "character") + 1u : 1u;
    gint64 sev_num = json_object_has_member(d, "severity") ? json_object_get_int_member(d, "severity") : 1;
    IdeDiagSeverity sev = (sev_num == 1) ? IDE_DIAG_ERROR : (sev_num == 2) ? IDE_DIAG_WARNING
                        : (sev_num == 3) ? IDE_DIAG_INFO : IDE_DIAG_HINT;
    const char *msg = json_object_has_member(d, "message") ? json_object_get_string_member(d, "message") : "";
    const char *src = json_object_has_member(d, "source") ? json_object_get_string_member(d, "source") : "lsp";
    ide_buffer_add_diagnostic(self->active_buf, line1, col1, sev, msg, src);
  }
  gtk_widget_queue_draw(self->source_view);
}

static void
on_lsp_notification(IdeLspClient *c G_GNUC_UNUSED, const char *m, JsonNode *p, gpointer ud)
{
  if (g_strcmp0(m, "textDocument/publishDiagnostics") == 0) {
    apply_publish_diagnostics(KAJO_AMIGA_WINDOW(ud), p);
  }
}

static void
append_m68k_config_members(JsonBuilder *b, KajoAmigaWindow *self)
{
  const AmigaTarget *t = self ? amiga_window_get_selected_target(self) : NULL;
  g_autoptr(GPtrArray) inc_dirs = g_ptr_array_new_with_free_func(g_free);
  amiga_project_collect_target_includes(self ? self->project : NULL, t,
                                        self ? self->toolchain : NULL, true, true, inc_dirs);

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "includePaths");
  json_builder_begin_array(b);
  for (guint i = 0; i < inc_dirs->len; i++) {
    json_builder_add_string_value(b, (const char *)g_ptr_array_index(inc_dirs, i));
  }
  json_builder_end_array(b);

  const char *cpu = (t && t->cpu && t->cpu[0]) ? t->cpu : "68000";
  g_autofree char *proc = g_str_has_prefix(cpu, "mc") ? g_strdup(cpu) : g_strdup_printf("mc%s", cpu);
  json_builder_set_member_name(b, "processors");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, proc);
  json_builder_end_array(b);

  if (self && self->toolchain && self->toolchain->vasmm68k_mot && self->toolchain->vasmm68k_mot[0]) {
    json_builder_set_member_name(b, "vasm");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "binPath");
    json_builder_add_string_value(b, self->toolchain->vasmm68k_mot);
    json_builder_end_object(b);
  }
  json_builder_end_object(b);
}

static JsonNode *
on_m68k_server_request(IdeLspClient *c G_GNUC_UNUSED, const char *method, JsonNode *params, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (g_strcmp0(method, "workspace/configuration") != 0 ||
      !params || !JSON_NODE_HOLDS_OBJECT(params)) {
    return NULL;
  }
  JsonObject *po = json_node_get_object(params);
  JsonArray *items = json_object_has_member(po, "items") ? json_object_get_array_member(po, "items") : NULL;
  guint n = items ? json_array_get_length(items) : 0;
  g_autoptr(JsonBuilder) rb = json_builder_new();
  json_builder_begin_array(rb);
  for (guint i = 0; i < n; i++) {
    JsonObject *it = json_array_get_object_element(items, i);
    const char *sec = (it && json_object_has_member(it, "section"))
                          ? json_object_get_string_member(it, "section") : NULL;
    if (g_strcmp0(sec, "m68k") == 0) {
      append_m68k_config_members(rb, self);
    } else {
      json_builder_add_null_value(rb);
    }
  }
  json_builder_end_array(rb);
  return json_builder_get_root(rb);
}

static void
send_m68k_did_change_config(KajoAmigaWindow *self)
{
  if (!self || !self->lsp_m68k) return;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "settings");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "m68k");
  append_m68k_config_members(b, self);
  json_builder_end_object(b);
  json_builder_end_object(b);
  ide_lsp_client_notify(self->lsp_m68k, "workspace/didChangeConfiguration", json_builder_get_root(b));
}

void
amiga_window_lsp_shutdown(KajoAmigaWindow *self)
{
  if (!self) return;
  if (self->lsp_clangd) {
    ide_lsp_client_stop(self->lsp_clangd);
    ide_lsp_client_free(self->lsp_clangd);
    self->lsp_clangd = NULL;
  }
  if (self->lsp_m68k) {
    ide_lsp_client_stop(self->lsp_m68k);
    ide_lsp_client_free(self->lsp_m68k);
    self->lsp_m68k = NULL;
  }
  g_clear_pointer(&self->lsp_root_clangd, g_free);
  g_clear_pointer(&self->lsp_root_m68k, g_free);
  g_clear_pointer(&self->lsp_open_uri_c, g_free);
  g_clear_pointer(&self->lsp_open_uri_m68k, g_free);
}

void
amiga_window_lsp_sync_project(KajoAmigaWindow *self)
{
  if (!self) return;
  const char *root = (self->project && self->project->root_dir) ? self->project->root_dir : NULL;
  if (!root) {
    amiga_window_lsp_shutdown(self);
    return;
  }

  if (!self->cfg_enable_clangd || !self->toolchain || !self->toolchain->clangd) {
    if (self->lsp_clangd) {
      ide_lsp_client_stop(self->lsp_clangd);
      ide_lsp_client_free(self->lsp_clangd);
      self->lsp_clangd = NULL;
      g_clear_pointer(&self->lsp_root_clangd, g_free);
      g_clear_pointer(&self->lsp_open_uri_c, g_free);
    }
  } else if (!self->lsp_clangd || g_strcmp0(self->lsp_root_clangd, root) != 0) {
    if (self->lsp_clangd) {
      ide_lsp_client_stop(self->lsp_clangd);
      ide_lsp_client_free(self->lsp_clangd);
      self->lsp_clangd = NULL;
    }
    g_autofree char *cc_flag = g_strdup_printf("--compile-commands-dir=%s/.kajo-amiga", root);
    const char *argv[] = { self->toolchain->clangd, cc_flag, "--background-index=false", NULL };
    self->lsp_clangd = ide_lsp_client_new("clangd", argv, root, NULL);
    ide_lsp_client_set_callbacks(self->lsp_clangd, on_lsp_notification, NULL, NULL, self);
    if (ide_lsp_client_start(self->lsp_clangd, NULL)) {
      g_free(self->lsp_root_clangd);
      self->lsp_root_clangd = g_strdup(root);
      g_clear_pointer(&self->lsp_open_uri_c, g_free);
    } else {
      ide_lsp_client_free(self->lsp_clangd);
      self->lsp_clangd = NULL;
    }
  }

  if (!self->cfg_enable_m68k_lsp || !self->toolchain || !self->toolchain->m68k_lsp_server) {
    if (self->lsp_m68k) {
      ide_lsp_client_stop(self->lsp_m68k);
      ide_lsp_client_free(self->lsp_m68k);
      self->lsp_m68k = NULL;
      g_clear_pointer(&self->lsp_root_m68k, g_free);
      g_clear_pointer(&self->lsp_open_uri_m68k, g_free);
    }
  } else if (!self->lsp_m68k || g_strcmp0(self->lsp_root_m68k, root) != 0) {
    if (self->lsp_m68k) {
      ide_lsp_client_stop(self->lsp_m68k);
      ide_lsp_client_free(self->lsp_m68k);
      self->lsp_m68k = NULL;
    }
    const char *argv[] = { self->toolchain->m68k_lsp_server, "--stdio", NULL };
    self->lsp_m68k = ide_lsp_client_new("m68k-lsp-server", argv, root, NULL);
    ide_lsp_client_set_callbacks(self->lsp_m68k, on_lsp_notification, on_m68k_server_request, NULL, self);
    if (ide_lsp_client_start(self->lsp_m68k, NULL)) {
      g_free(self->lsp_root_m68k);
      self->lsp_root_m68k = g_strdup(root);
      g_clear_pointer(&self->lsp_open_uri_m68k, g_free);
      send_m68k_did_change_config(self);
    } else {
      ide_lsp_client_free(self->lsp_m68k);
      self->lsp_m68k = NULL;
    }
  } else {
    send_m68k_did_change_config(self);
  }
  amiga_window_sync_status_bar(self);
}

static IdeLspClient *
pick_lsp_for_path(KajoAmigaWindow *self, const char *path, const char **out_lang, char ***out_slot)
{
  if (!path) return NULL;
  if (g_str_has_suffix(path, ".c") || g_str_has_suffix(path, ".C") ||
      g_str_has_suffix(path, ".h") || g_str_has_suffix(path, ".H")) {
    if (out_lang) *out_lang = "c";
    if (out_slot) *out_slot = &self->lsp_open_uri_c;
    return self->lsp_clangd;
  }
  if (g_str_has_suffix(path, ".asm") || g_str_has_suffix(path, ".s") ||
      g_str_has_suffix(path, ".S") || g_str_has_suffix(path, ".i")) {
    if (out_lang) *out_lang = "m68k";
    if (out_slot) *out_slot = &self->lsp_open_uri_m68k;
    return self->lsp_m68k;
  }
  return NULL;
}

void
amiga_window_lsp_notify_open(KajoAmigaWindow *self, IdeBuffer *buf)
{
  if (!self || !buf) return;
  const char *path = ide_buffer_get_file_path(buf);
  const char *lang = NULL;
  char **slot = NULL;
  IdeLspClient *cli = pick_lsp_for_path(self, path, &lang, &slot);
  if (!cli) return;

  g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
  if (!uri || g_strcmp0(*slot, uri) == 0) return;
  if (*slot && (*slot)[0]) {
    g_autoptr(JsonBuilder) cb = json_builder_new();
    json_builder_begin_object(cb);
    json_builder_set_member_name(cb, "textDocument");
    json_builder_begin_object(cb);
    json_builder_set_member_name(cb, "uri");
    json_builder_add_string_value(cb, *slot);
    json_builder_end_object(cb);
    json_builder_end_object(cb);
    ide_lsp_client_notify(cli, "textDocument/didClose", json_builder_get_root(cb));
  }
  g_free(*slot);
  *slot = g_strdup(uri);

  g_autofree char *text = ide_buffer_get_text(buf);
  self->lsp_doc_ver++;
  g_autoptr(JsonBuilder) ob = json_builder_new();
  json_builder_begin_object(ob);
  json_builder_set_member_name(ob, "textDocument");
  json_builder_begin_object(ob);
  json_builder_set_member_name(ob, "uri"); json_builder_add_string_value(ob, uri);
  json_builder_set_member_name(ob, "languageId"); json_builder_add_string_value(ob, lang);
  json_builder_set_member_name(ob, "version"); json_builder_add_int_value(ob, self->lsp_doc_ver);
  json_builder_set_member_name(ob, "text"); json_builder_add_string_value(ob, text ? text : "");
  json_builder_end_object(ob);
  json_builder_end_object(ob);
  ide_lsp_client_notify(cli, "textDocument/didOpen", json_builder_get_root(ob));
}

void
amiga_window_lsp_notify_save(KajoAmigaWindow *self, IdeBuffer *buf)
{
  if (!self || !buf) return;
  const char *path = ide_buffer_get_file_path(buf);
  IdeLspClient *cli = pick_lsp_for_path(self, path, NULL, NULL);
  if (!cli) return;

  g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
  if (!uri) return;
  g_autofree char *text = ide_buffer_get_text(buf);
  self->lsp_doc_ver++;

  g_autoptr(JsonBuilder) chb = json_builder_new();
  json_builder_begin_object(chb);
  json_builder_set_member_name(chb, "textDocument");
  json_builder_begin_object(chb);
  json_builder_set_member_name(chb, "uri"); json_builder_add_string_value(chb, uri);
  json_builder_set_member_name(chb, "version"); json_builder_add_int_value(chb, self->lsp_doc_ver);
  json_builder_end_object(chb);
  json_builder_set_member_name(chb, "contentChanges");
  json_builder_begin_array(chb);
  json_builder_begin_object(chb);
  json_builder_set_member_name(chb, "text"); json_builder_add_string_value(chb, text ? text : "");
  json_builder_end_object(chb);
  json_builder_end_array(chb);
  json_builder_end_object(chb);
  ide_lsp_client_notify(cli, "textDocument/didChange", json_builder_get_root(chb));

  g_autoptr(JsonBuilder) sb = json_builder_new();
  json_builder_begin_object(sb);
  json_builder_set_member_name(sb, "textDocument");
  json_builder_begin_object(sb);
  json_builder_set_member_name(sb, "uri"); json_builder_add_string_value(sb, uri);
  json_builder_end_object(sb);
  json_builder_end_object(sb);
  ide_lsp_client_notify(cli, "textDocument/didSave", json_builder_get_root(sb));
}
