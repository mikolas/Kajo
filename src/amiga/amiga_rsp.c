#include "amiga_rsp.h"

#include <ctype.h>
#include <errno.h>
#include <json-glib/json-glib.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GTask       *task;
  char        *wire_bytes; /* raw bytes to send ($...#cs or "\x03") */
  size_t       wire_len;
  bool         completes_on_stop;
  bool         sets_running_on_ok;
  bool         is_detach;
  bool         is_no_ack_handshake;
} RspPendingRpc;

typedef struct {
  GBytes *bytes;
  GTask  *notify_task; /* optional: completed TRUE when wire bytes are written */
} WireChunk;

struct _AmigaRspClient {
  GObject parent_instance;

  AmigaRspState      state;
  GSocketClient     *sock_client;
  GSocketConnection *conn;
  GInputStream      *in_stream;
  GOutputStream     *out_stream;
  GCancellable      *cancellable;

  bool               no_ack_mode;
  bool               rx_active;
  GString           *rx_buf;

  GQueue            *wire_tx_queue;    /* WireChunk* serialized on out_stream */
  WireChunk         *wire_tx_inflight; /* keeps GBytes buffer alive during write_all_async */

  GQueue            *tx_queue;         /* RspPendingRpc* awaiting response */
  RspPendingRpc     *in_flight;        /* currently awaiting wire reply */

  AmigaRspStopCb     stop_cb;
  gpointer           stop_ud;
};

G_DEFINE_FINAL_TYPE(AmigaRspClient, amiga_rsp_client, G_TYPE_OBJECT)

static void
rsp_pending_rpc_free(RspPendingRpc *rpc)
{
  if (!rpc) return;
  g_clear_object(&rpc->task);
  g_free(rpc->wire_bytes);
  g_free(rpc);
}

static void
stop_event_clear(AmigaStopEvent *ev)
{
  if (!ev) return;
  g_clear_pointer(&ev->entry_proc, g_free);
  g_clear_pointer(&ev->watch_kind, g_free);
  g_clear_pointer(&ev->raw, g_free);
}

void
amiga_stop_event_free(AmigaStopEvent *ev)
{
  if (!ev) return;
  stop_event_clear(ev);
  g_free(ev);
}

static void
amiga_segment_info_free(AmigaSegmentInfo *si)
{
  g_free(si);
}

void
amiga_segment_list_free(AmigaSegmentList *sl)
{
  if (!sl) return;
  g_free(sl->name);
  g_free(sl->command);
  if (sl->segments) g_ptr_array_unref(sl->segments);
  g_free(sl);
}

void
amiga_task_info_free(AmigaTaskInfo *ti)
{
  if (!ti) return;
  g_free(ti->name);
  g_free(ti->list_name);
  g_free(ti);
}

void
amiga_mem_region_free(AmigaMemRegion *mr)
{
  if (!mr) return;
  g_free(mr->type);
  g_free(mr->name);
  g_free(mr);
}

/* ─── Pure Helpers ─── */

uint8_t
amiga_rsp_checksum(const char *payload, size_t len)
{
  uint8_t sum = 0;
  if (!payload) return 0;
  for (size_t i = 0; i < len; i++) {
    sum = (uint8_t)(sum + (uint8_t)payload[i]);
  }
  return sum;
}

char *
amiga_rsp_frame_packet(const char *payload)
{
  const char *p = payload ? payload : "";
  size_t len = strlen(p);
  uint8_t cs = amiga_rsp_checksum(p, len);
  return g_strdup_printf("$%s#%02x", p, (unsigned)cs);
}

char *
amiga_rsp_hex_encode(const uint8_t *data, size_t len)
{
  static const char hex[] = "0123456789abcdef";
  char *out = g_malloc(len * 2u + 1u);
  for (size_t i = 0; i < len; i++) {
    out[i * 2u]      = hex[(data[i] >> 4) & 0x0Fu];
    out[i * 2u + 1u] = hex[data[i] & 0x0Fu];
  }
  out[len * 2u] = '\0';
  return out;
}

static int
hex_nibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool
amiga_rsp_hex_decode(const char *hex, uint8_t **out_bytes, size_t *out_len)
{
  if (!hex || !out_bytes || !out_len) return false;
  size_t hlen = strlen(hex);
  if ((hlen % 2u) != 0) return false;
  size_t n = hlen / 2u;
  uint8_t *buf = g_malloc(n + 1u);
  for (size_t i = 0; i < n; i++) {
    int hi = hex_nibble(hex[i * 2u]);
    int lo = hex_nibble(hex[i * 2u + 1u]);
    if (hi < 0 || lo < 0) {
      g_free(buf);
      return false;
    }
    buf[i] = (uint8_t)((hi << 4) | lo);
  }
  buf[n] = '\0';
  *out_bytes = buf;
  *out_len = n;
  return true;
}

char *
amiga_rsp_expand_rle(const char *payload, size_t len)
{
  if (!payload) return g_strdup("");
  GString *out = g_string_sized_new(len);
  for (size_t i = 0; i < len; i++) {
    if (payload[i] == '*' && i + 1 < len && out->len > 0) {
      unsigned char rc = (unsigned char)payload[i + 1];
      if (rc >= 32u) {
        size_t repeat = (size_t)(rc - 29u);
        char ch = out->str[out->len - 1];
        for (size_t k = 0; k < repeat; k++) {
          g_string_append_c(out, ch);
        }
        i++;
        continue;
      }
    }
    g_string_append_c(out, payload[i]);
  }
  return g_string_free(out, FALSE);
}

bool
amiga_rsp_parse_regs_g(const char *hex_reply, AmigaCpuRegs *out_regs)
{
  if (!hex_reply || !out_regs) return false;
  g_autofree uint8_t *raw = NULL;
  size_t len = 0;
  if (!amiga_rsp_hex_decode(hex_reply, &raw, &len) || len < 18u * 4u) {
    return false;
  }
  memset(out_regs, 0, sizeof(*out_regs));
  uint32_t vals[18];
  for (size_t i = 0; i < 18; i++) {
    const uint8_t *p = raw + i * 4u;
    vals[i] = ((uint32_t)p[0] << 24) |
              ((uint32_t)p[1] << 16) |
              ((uint32_t)p[2] << 8)  |
              (uint32_t)p[3];
  }
  for (int i = 0; i < 8; i++) out_regs->d[i] = vals[i];
  for (int i = 0; i < 8; i++) out_regs->a[i] = vals[8 + i];
  out_regs->sr = vals[16];
  out_regs->pc = vals[17];
  return true;
}

static void
put_be32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)((v >> 24) & 0xFFu);
  p[1] = (uint8_t)((v >> 16) & 0xFFu);
  p[2] = (uint8_t)((v >> 8)  & 0xFFu);
  p[3] = (uint8_t)(v & 0xFFu);
}

char *
amiga_rsp_format_regs_G(const AmigaCpuRegs *regs)
{
  if (!regs) return NULL;
  uint8_t raw[18 * 4];
  for (int i = 0; i < 8; i++) put_be32(raw + (size_t)i * 4u, regs->d[i]);
  for (int i = 0; i < 8; i++) put_be32(raw + (size_t)(8 + i) * 4u, regs->a[i]);
  put_be32(raw + 16u * 4u, regs->sr);
  put_be32(raw + 17u * 4u, regs->pc);
  g_autofree char *hex = amiga_rsp_hex_encode(raw, sizeof(raw));
  return g_strdup_printf("G%s", hex);
}

bool
amiga_rsp_parse_stop_reply(const char *pkt, AmigaStopEvent *out_stop)
{
  if (!pkt || !out_stop || !pkt[0]) return false;
  memset(out_stop, 0, sizeof(*out_stop));
  out_stop->raw = g_strdup(pkt);

  char ch = pkt[0];
  if (ch == 'W' || ch == 'X') {
    out_stop->kind = AMIGA_STOP_EXIT;
    out_stop->exit_code = (int)strtol(pkt + 1, NULL, 16);
    return true;
  }
  if (ch == 'S') {
    out_stop->kind = AMIGA_STOP_SIGNAL;
    out_stop->signal_num = (uint8_t)strtoul(pkt + 1, NULL, 16);
    return true;
  }
  if (ch != 'T') {
    g_free(out_stop->raw);
    out_stop->raw = NULL;
    return false;
  }

  out_stop->kind = AMIGA_STOP_SIGNAL;
  if (isxdigit((unsigned char)pkt[1]) && isxdigit((unsigned char)pkt[2])) {
    char sigbuf[3] = { pkt[1], pkt[2], '\0' };
    out_stop->signal_num = (uint8_t)strtoul(sigbuf, NULL, 16);
  }
  const char *p = (strlen(pkt) >= 3) ? pkt + 3 : pkt + 1;
  g_auto(GStrv) pairs = g_strsplit(p, ";", -1);
  for (int i = 0; pairs[i] != NULL; i++) {
    if (!pairs[i][0]) continue;
    char *colon = strchr(pairs[i], ':');
    if (!colon) continue;
    *colon = '\0';
    const char *k = pairs[i];
    const char *v = colon + 1;
    if (g_strcmp0(k, "11") == 0) {
      out_stop->has_pc = true;
      out_stop->pc = (uint32_t)strtoul(v, NULL, 16);
    } else if (g_strcmp0(k, "winuae-entry") == 0) {
      out_stop->kind = AMIGA_STOP_WINUAE_ENTRY;
      g_free(out_stop->entry_proc);
      out_stop->entry_proc = g_strdup(v);
    } else if (g_strcmp0(k, "watch") == 0 ||
               g_strcmp0(k, "rwatch") == 0 ||
               g_strcmp0(k, "awatch") == 0) {
      out_stop->kind = AMIGA_STOP_WATCH;
      g_free(out_stop->watch_kind);
      out_stop->watch_kind = g_strdup(k);
      out_stop->watch_addr = (uint32_t)strtoul(v, NULL, 16);
    }
  }
  return true;
}

/* Safe JsonObject readers that never fire g_critical on missing/null keys */
static const char *
jobj_str(JsonObject *o, const char *k)
{
  if (!o || !json_object_has_member(o, k)) return NULL;
  JsonNode *n = json_object_get_member(o, k);
  if (!n || JSON_NODE_HOLDS_NULL(n) || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
  return json_node_get_string(n);
}

static uint64_t
jobj_u64(JsonObject *o, const char *k, uint64_t fallback)
{
  if (!o || !json_object_has_member(o, k)) return fallback;
  JsonNode *n = json_object_get_member(o, k);
  if (!n || JSON_NODE_HOLDS_NULL(n)) return fallback;
  GType t = json_node_get_value_type(n);
  if (t == G_TYPE_INT64) {
    gint64 v = json_node_get_int(n);
    return v >= 0 ? (uint64_t)v : fallback;
  }
  if (t == G_TYPE_STRING) {
    const char *s = json_node_get_string(n);
    if (s && s[0]) return g_ascii_strtoull(s, NULL, 0);
  }
  return fallback;
}

static bool
jobj_bool(JsonObject *o, const char *k, bool fallback)
{
  if (!o || !json_object_has_member(o, k)) return fallback;
  JsonNode *n = json_object_get_member(o, k);
  if (!n || JSON_NODE_HOLDS_NULL(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return fallback;
  return json_node_get_boolean(n);
}

static JsonObject *
parse_json_root_obj(const char *json_text, JsonParser **out_parser)
{
  if (!json_text || !json_text[0] || !out_parser) return NULL;
  JsonParser *p = json_parser_new();
  if (!json_parser_load_from_data(p, json_text, -1, NULL)) {
    g_object_unref(p);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(p);
    return NULL;
  }
  *out_parser = p;
  return json_node_get_object(root);
}

bool
amiga_rsp_parse_execution_status(const char *json_text, AmigaExecStatus *out_st)
{
  if (!out_st) return false;
  g_autoptr(JsonParser) parser = NULL;
  JsonObject *o = parse_json_root_obj(json_text, &parser);
  if (!o) return false;

  memset(out_st, 0, sizeof(*out_st));
  const char *ex = jobj_str(o, "execution");
  g_strlcpy(out_st->execution, ex ? ex : "unknown", sizeof(out_st->execution));
  out_st->debugger_stopped = jobj_bool(o, "debugger_stopped", false);
  out_st->host_pause = (int)jobj_u64(o, "host_pause", 0);
  out_st->frame = (uint32_t)jobj_u64(o, "frame", 0);
  out_st->vpos = (uint32_t)jobj_u64(o, "vpos", 0);
  out_st->hpos = (uint32_t)jobj_u64(o, "hpos", 0);
  out_st->cycles = jobj_u64(o, "cycles", 0);
  return true;
}

bool
amiga_rsp_parse_segments(const char *json_text, AmigaSegmentList **out_list)
{
  if (!out_list) return false;
  *out_list = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonObject *o = parse_json_root_obj(json_text, &parser);
  if (!o) return false;

  AmigaSegmentList *sl = g_new0(AmigaSegmentList, 1);
  sl->process = (uint32_t)jobj_u64(o, "process", 0);
  sl->name = g_strdup(jobj_str(o, "name"));
  sl->command = g_strdup(jobj_str(o, "command"));
  sl->segments = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_segment_info_free);

  if (json_object_has_member(o, "segments")) {
    JsonNode *sn = json_object_get_member(o, "segments");
    if (sn && JSON_NODE_HOLDS_ARRAY(sn)) {
      JsonArray *arr = json_node_get_array(sn);
      guint n = json_array_get_length(arr);
      for (guint i = 0; i < n; i++) {
        JsonObject *so = json_array_get_object_element(arr, i);
        if (!so) continue;
        AmigaSegmentInfo *si = g_new0(AmigaSegmentInfo, 1);
        si->index = (uint32_t)jobj_u64(so, "index", i);
        si->address = (uint32_t)jobj_u64(so, "address", 0);
        si->size = (uint32_t)jobj_u64(so, "size", 0);
        g_ptr_array_add(sl->segments, si);
      }
    }
  }

  *out_list = sl;
  return true;
}

bool
amiga_rsp_parse_tasks(const char *json_text, GPtrArray **out_tasks)
{
  if (!out_tasks) return false;
  *out_tasks = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonObject *o = parse_json_root_obj(json_text, &parser);
  if (!o) return false;

  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_task_info_free);
  if (json_object_has_member(o, "tasks")) {
    JsonNode *tn = json_object_get_member(o, "tasks");
    if (tn && JSON_NODE_HOLDS_ARRAY(tn)) {
      JsonArray *arr = json_node_get_array(tn);
      guint n = json_array_get_length(arr);
      for (guint i = 0; i < n; i++) {
        JsonObject *to = json_array_get_object_element(arr, i);
        if (!to) continue;
        AmigaTaskInfo *ti = g_new0(AmigaTaskInfo, 1);
        ti->address = (uint32_t)jobj_u64(to, "address", 0);
        ti->name = g_strdup(jobj_str(to, "name"));
        ti->type = (uint32_t)jobj_u64(to, "type", 0);
        ti->state = (uint32_t)jobj_u64(to, "state", 0);
        ti->list_name = g_strdup(jobj_str(to, "list"));
        ti->signals_wait = (uint32_t)jobj_u64(to, "signals_wait", 0);
        ti->saved_sp = (uint32_t)jobj_u64(to, "saved_sp", 0);
        ti->stack_lower = (uint32_t)jobj_u64(to, "stack_lower", 0);
        ti->stack_upper = (uint32_t)jobj_u64(to, "stack_upper", 0);
        g_ptr_array_add(out, ti);
      }
    }
  }
  *out_tasks = out;
  return true;
}

bool
amiga_rsp_parse_guest_output(const char *json_text, GPtrArray **out_lines)
{
  if (!out_lines) return false;
  *out_lines = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonObject *o = parse_json_root_obj(json_text, &parser);
  if (!o) return false;

  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  const char *key = json_object_has_member(o, "records") ? "records"
                  : json_object_has_member(o, "entries") ? "entries"
                  : json_object_has_member(o, "lines") ? "lines" : NULL;
  if (key) {
    JsonNode *rn = json_object_get_member(o, key);
    if (rn && JSON_NODE_HOLDS_ARRAY(rn)) {
      JsonArray *arr = json_node_get_array(rn);
      guint n = json_array_get_length(arr);
      for (guint i = 0; i < n; i++) {
        JsonNode *el = json_array_get_element(arr, i);
        if (!el) continue;
        if (JSON_NODE_HOLDS_VALUE(el) && json_node_get_value_type(el) == G_TYPE_STRING) {
          g_ptr_array_add(out, g_strdup(json_node_get_string(el)));
        } else if (JSON_NODE_HOLDS_OBJECT(el)) {
          JsonObject *ro = json_node_get_object(el);
          const char *msg = jobj_str(ro, "text");
          if (!msg) msg = jobj_str(ro, "message");
          if (!msg) msg = jobj_str(ro, "line");
          if (msg) g_ptr_array_add(out, g_strdup(msg));
        }
      }
    }
  }
  *out_lines = out;
  return true;
}

bool
amiga_rsp_parse_watch_events(const char *json_text, GPtrArray **out_events)
{
  if (!out_events) return false;
  *out_events = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonObject *o = parse_json_root_obj(json_text, &parser);
  if (!o) return false;

  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (json_object_has_member(o, "events")) {
    JsonNode *en = json_object_get_member(o, "events");
    if (en && JSON_NODE_HOLDS_ARRAY(en)) {
      JsonArray *arr = json_node_get_array(en);
      guint n = json_array_get_length(arr);
      for (guint i = 0; i < n; i++) {
        JsonObject *eo = json_array_get_object_element(arr, i);
        if (!eo) continue;
        AmigaWatchEvent *ev = g_new0(AmigaWatchEvent, 1);
        ev->pc = (uint32_t)jobj_u64(eo, "pc", 0);
        ev->addr = (uint32_t)jobj_u64(eo, "addr", jobj_u64(eo, "address", 0));
        ev->size = (uint32_t)jobj_u64(eo, "size", 2);
        ev->rwi = (uint32_t)jobj_u64(eo, "rwi", 2);
        ev->src = (uint32_t)jobj_u64(eo, "src", 0);
        ev->old_val = (uint32_t)jobj_u64(eo, "old", jobj_u64(eo, "old_value", 0));
        ev->new_val = (uint32_t)jobj_u64(eo, "value", jobj_u64(eo, "new_value", 0));
        ev->frame = (uint32_t)jobj_u64(eo, "frame", 0);
        ev->vpos = (uint32_t)jobj_u64(eo, "vpos", 0);
        ev->hpos = (uint32_t)jobj_u64(eo, "hpos", 0);
        g_ptr_array_add(out, ev);
      }
    }
  }
  *out_events = out;
  return true;
}

bool
amiga_rsp_parse_history(const char *json_text, GPtrArray **out_entries)
{
  if (!out_entries) return false;
  *out_entries = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonObject *o = parse_json_root_obj(json_text, &parser);
  if (!o) return false;

  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  const char *key = json_object_has_member(o, "history") ? "history"
                  : json_object_has_member(o, "entries") ? "entries" : NULL;
  if (key) {
    JsonNode *hn = json_object_get_member(o, key);
    if (hn && JSON_NODE_HOLDS_ARRAY(hn)) {
      JsonArray *arr = json_node_get_array(hn);
      guint n = json_array_get_length(arr);
      for (guint i = 0; i < n; i++) {
        JsonObject *ho = json_array_get_object_element(arr, i);
        if (!ho) continue;
        AmigaHistoryEntry *he = g_new0(AmigaHistoryEntry, 1);
        he->pc = (uint32_t)jobj_u64(ho, "pc", 0);
        he->frame = (uint32_t)jobj_u64(ho, "frame", 0);
        he->vpos = (uint32_t)jobj_u64(ho, "vpos", 0);
        he->hpos = (uint32_t)jobj_u64(ho, "hpos", 0);
        if (json_object_has_member(ho, "registers")) {
          JsonNode *rn = json_object_get_member(ho, "registers");
          if (rn && JSON_NODE_HOLDS_ARRAY(rn)) {
            JsonArray *ra = json_node_get_array(rn);
            guint rlen = MIN(json_array_get_length(ra), 16u);
            for (guint r = 0; r < rlen; r++) {
              JsonNode *rnode = json_array_get_element(ra, r);
              if (rnode && json_node_get_value_type(rnode) == G_TYPE_INT64) {
                he->regs[r] = (uint32_t)json_node_get_int(rnode);
              }
            }
          }
        }
        g_ptr_array_add(out, he);
      }
    }
  }
  *out_entries = out;
  return true;
}

static bool
extract_xml_attr(const char *tag, const char *attr, char *out, size_t cap)
{
  g_autofree char *pat = g_strdup_printf("%s=\"", attr);
  const char *p = strstr(tag, pat);
  if (!p) {
    g_free(pat);
    pat = g_strdup_printf("%s='", attr);
    p = strstr(tag, pat);
    if (!p) return false;
  }
  char q = pat[strlen(pat) - 1];
  p += strlen(pat);
  const char *end = strchr(p, q);
  if (!end) return false;
  size_t n = MIN((size_t)(end - p), cap - 1u);
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

bool
amiga_rsp_parse_memory_map(const char *text, GPtrArray **out_regions)
{
  if (!text || !text[0] || !out_regions) return false;
  *out_regions = NULL;

  /* JSON form */
  const char *ws = text;
  while (*ws && isspace((unsigned char)*ws)) ws++;
  if (*ws == '{') {
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *o = parse_json_root_obj(ws, &parser);
    if (!o) return false;
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_mem_region_free);
    const char *key = json_object_has_member(o, "regions") ? "regions"
                    : json_object_has_member(o, "memory") ? "memory" : NULL;
    if (key) {
      JsonNode *rn = json_object_get_member(o, key);
      if (rn && JSON_NODE_HOLDS_ARRAY(rn)) {
        JsonArray *arr = json_node_get_array(rn);
        guint n = json_array_get_length(arr);
        for (guint i = 0; i < n; i++) {
          JsonObject *ro = json_array_get_object_element(arr, i);
          if (!ro) continue;
          AmigaMemRegion *mr = g_new0(AmigaMemRegion, 1);
          mr->type = g_strdup(jobj_str(ro, "type"));
          mr->start = (uint32_t)jobj_u64(ro, "address", jobj_u64(ro, "start", 0));
          mr->length = (uint32_t)jobj_u64(ro, "size", jobj_u64(ro, "length", 0));
          mr->name = g_strdup(jobj_str(ro, "name"));
          g_ptr_array_add(out, mr);
        }
      }
    }
    *out_regions = out;
    return true;
  }

  /* XML GDB memory-map form */
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_mem_region_free);
  const char *p = ws;
  while ((p = strstr(p, "<memory")) != NULL) {
    const char *close = strchr(p, '>');
    if (!close) break;
    g_autofree char *tag = g_strndup(p, (gsize)(close - p + 1));
    char tbuf[32] = "", sbuf[32] = "", lbuf[32] = "", nbuf[64] = "";
    extract_xml_attr(tag, "type", tbuf, sizeof(tbuf));
    extract_xml_attr(tag, "start", sbuf, sizeof(sbuf));
    extract_xml_attr(tag, "length", lbuf, sizeof(lbuf));
    extract_xml_attr(tag, "name", nbuf, sizeof(nbuf));
    if (sbuf[0] && lbuf[0]) {
      AmigaMemRegion *mr = g_new0(AmigaMemRegion, 1);
      mr->type = g_strdup(tbuf[0] ? tbuf : "ram");
      mr->start = (uint32_t)g_ascii_strtoull(sbuf, NULL, 0);
      mr->length = (uint32_t)g_ascii_strtoull(lbuf, NULL, 0);
      mr->name = nbuf[0] ? g_strdup(nbuf) : NULL;
      g_ptr_array_add(out, mr);
    }
    p = close + 1;
  }
  *out_regions = out;
  return out->len > 0;
}

static uint16_t
snap_u16(const uint8_t buf[512], uint16_t reg_off)
{
  return (uint16_t)(((uint16_t)buf[reg_off] << 8) | (uint16_t)buf[reg_off + 1u]);
}

static uint32_t
snap_ptr32(const uint8_t buf[512], uint16_t hi_off)
{
  uint16_t hi = snap_u16(buf, hi_off);
  uint16_t lo = snap_u16(buf, (uint16_t)(hi_off + 2u));
  return (((uint32_t)hi << 16) | (uint32_t)lo) & 0x00FFFFFFu;
}

void
amiga_rsp_decode_custom_snapshot(const uint8_t buf[512], AmigaCustomSnapshot *out)
{
  if (!buf || !out) return;
  memset(out, 0, sizeof(*out));
  memcpy(out->raw, buf, 512);

  out->dmaconr = snap_u16(buf, 0x002u);
  out->vposr   = snap_u16(buf, 0x004u);
  out->vhposr  = snap_u16(buf, 0x006u);
  out->clxdat  = snap_u16(buf, 0x00Eu);
  out->adkconr = snap_u16(buf, 0x010u);
  out->intenar = snap_u16(buf, 0x01Cu);
  out->intreqr = snap_u16(buf, 0x01Eu);
  out->copcon  = snap_u16(buf, 0x02Eu);

  out->bltcon0 = snap_u16(buf, 0x040u);
  out->bltcon1 = snap_u16(buf, 0x042u);
  out->bltafwm = snap_u16(buf, 0x044u);
  out->bltalwm = snap_u16(buf, 0x046u);
  for (int i = 0; i < 4; i++) {
    out->bltpt[i]  = snap_ptr32(buf, (uint16_t)(0x048u + i * 4));
    out->bltmod[i] = (int16_t)snap_u16(buf, (uint16_t)(0x060u + i * 2));
  }
  out->bltsize = snap_u16(buf, 0x058u);

  out->cop1lc  = snap_ptr32(buf, 0x080u);
  out->cop2lc  = snap_ptr32(buf, 0x084u);
  out->diwstrt = snap_u16(buf, 0x08Eu);
  out->diwstop = snap_u16(buf, 0x090u);
  out->ddfstrt = snap_u16(buf, 0x092u);
  out->ddfstop = snap_u16(buf, 0x094u);
  out->dmacon  = snap_u16(buf, 0x096u);
  out->intena  = snap_u16(buf, 0x09Au);
  out->intreq  = snap_u16(buf, 0x09Cu);

  for (int i = 0; i < 8; i++) {
    out->bplpt[i] = snap_ptr32(buf, (uint16_t)(0x0E0u + i * 4));
    out->sprpt[i] = snap_ptr32(buf, (uint16_t)(0x120u + i * 4));
  }
  out->bplcon[0] = snap_u16(buf, 0x100u);
  out->bplcon[1] = snap_u16(buf, 0x102u);
  out->bplcon[2] = snap_u16(buf, 0x104u);
  out->bplcon[3] = snap_u16(buf, 0x106u);
  out->bpl1mod   = (int16_t)snap_u16(buf, 0x108u);
  out->bpl2mod   = (int16_t)snap_u16(buf, 0x10Au);
  out->bplcon[4] = snap_u16(buf, 0x10Cu);

  for (int i = 0; i < 32; i++) {
    out->color[i] = snap_u16(buf, (uint16_t)(0x180u + i * 2)) & 0x0FFFu;
  }
  out->fmode = snap_u16(buf, 0x1FCu);
  out->ntsc  = (snap_u16(buf, 0x1FAu) & 0x0001u) != 0;
}

/* ─── Async TCP Wire Engine ─── */

static void fail_all_pending(AmigaRspClient *self, GQuark dom, gint code, const char *msg);
static void pump_tx(AmigaRspClient *self);
static void arm_rx(AmigaRspClient *self);

static void
wire_chunk_free(WireChunk *wc)
{
  if (!wc) return;
  if (wc->bytes) g_bytes_unref(wc->bytes);
  g_clear_object(&wc->notify_task);
  g_free(wc);
}

static void
close_connection_locked(AmigaRspClient *self)
{
  self->rx_active = false;
  self->no_ack_mode = false;
  if (self->rx_buf) g_string_set_size(self->rx_buf, 0);
  if (self->wire_tx_queue) {
    while (!g_queue_is_empty(self->wire_tx_queue)) {
      WireChunk *wc = g_queue_pop_head(self->wire_tx_queue);
      if (wc->notify_task) {
        g_task_return_new_error(wc->notify_task, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                                "Connection closed");
      }
      wire_chunk_free(wc);
    }
  }
  if (self->conn) {
    g_io_stream_close(G_IO_STREAM(self->conn), NULL, NULL);
    g_clear_object(&self->conn);
  }
  self->in_stream = NULL;
  self->out_stream = NULL;
  self->state = AMIGA_RSP_DISCONNECTED;
}

static void
amiga_rsp_client_dispose(GObject *obj)
{
  AmigaRspClient *self = AMIGA_RSP_CLIENT(obj);
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  fail_all_pending(self, G_IO_ERROR, G_IO_ERROR_CANCELLED, "RSP client disposed");
  close_connection_locked(self);
  g_clear_object(&self->sock_client);
  G_OBJECT_CLASS(amiga_rsp_client_parent_class)->dispose(obj);
}

static void
amiga_rsp_client_finalize(GObject *obj)
{
  AmigaRspClient *self = AMIGA_RSP_CLIENT(obj);
  if (self->rx_buf) g_string_free(self->rx_buf, TRUE);
  if (self->wire_tx_queue) g_queue_free_full(self->wire_tx_queue, (GDestroyNotify)wire_chunk_free);
  wire_chunk_free(self->wire_tx_inflight);
  if (self->tx_queue) g_queue_free_full(self->tx_queue, (GDestroyNotify)rsp_pending_rpc_free);
  rsp_pending_rpc_free(self->in_flight);
  G_OBJECT_CLASS(amiga_rsp_client_parent_class)->finalize(obj);
}

static void
amiga_rsp_client_class_init(AmigaRspClientClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = amiga_rsp_client_dispose;
  oc->finalize = amiga_rsp_client_finalize;
}

static void
amiga_rsp_client_init(AmigaRspClient *self)
{
  self->state = AMIGA_RSP_DISCONNECTED;
  self->sock_client = g_socket_client_new();
  g_socket_client_set_enable_proxy(self->sock_client, FALSE);
  self->cancellable = g_cancellable_new();
  self->rx_buf = g_string_new("");
  self->wire_tx_queue = g_queue_new();
  self->tx_queue = g_queue_new();
}

static void
fail_all_pending(AmigaRspClient *self, GQuark dom, gint code, const char *msg)
{
  if (self->in_flight) {
    RspPendingRpc *rpc = g_steal_pointer(&self->in_flight);
    if (rpc->is_detach) {
      g_task_return_pointer(rpc->task, g_strdup("OK"), g_free);
    } else {
      g_task_return_new_error(rpc->task, dom, code, "%s", msg);
    }
    rsp_pending_rpc_free(rpc);
  }
  while (!g_queue_is_empty(self->tx_queue)) {
    RspPendingRpc *rpc = g_queue_pop_head(self->tx_queue);
    g_task_return_new_error(rpc->task, dom, code, "%s", msg);
    rsp_pending_rpc_free(rpc);
  }
}

static void pump_wire_tx(AmigaRspClient *self);

static void
enqueue_wire_bytes(AmigaRspClient *self, const char *data, size_t len, GTask *notify_task)
{
  if (!self->out_stream) {
    if (notify_task) {
      g_task_return_new_error(notify_task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Not connected");
    }
    return;
  }
  WireChunk *wc = g_new0(WireChunk, 1);
  wc->bytes = g_bytes_new(data, len);
  wc->notify_task = notify_task ? g_object_ref(notify_task) : NULL;
  g_queue_push_tail(self->wire_tx_queue, wc);
  pump_wire_tx(self);
}

static void
send_ack_byte(AmigaRspClient *self, const char *ack)
{
  if (!self->out_stream || self->no_ack_mode) return;
  enqueue_wire_bytes(self, ack, 1, NULL);
}

static void
handle_decoded_packet(AmigaRspClient *self, const char *payload)
{
  bool is_stop_pkt = (payload[0] == 'T' || payload[0] == 'S' ||
                      payload[0] == 'W' || payload[0] == 'X');

  if (is_stop_pkt) {
    AmigaStopEvent ev = {0};
    bool ok = amiga_rsp_parse_stop_reply(payload, &ev);
    if (ok) {
      self->state = (ev.kind == AMIGA_STOP_EXIT) ? AMIGA_RSP_DISCONNECTED : AMIGA_RSP_STOPPED;
    }

    if (self->in_flight && self->in_flight->completes_on_stop) {
      RspPendingRpc *rpc = g_steal_pointer(&self->in_flight);
      g_task_return_pointer(rpc->task, g_strdup(payload), g_free);
      rsp_pending_rpc_free(rpc);
      pump_tx(self);
    }

    if (ok) {
      if (self->stop_cb) {
        g_object_ref(self);
        self->stop_cb(self, &ev, self->stop_ud);
        g_object_unref(self);
      }
      stop_event_clear(&ev);
    }
    return;
  }

  if (self->in_flight) {
    RspPendingRpc *rpc = g_steal_pointer(&self->in_flight);
    if (rpc->is_no_ack_handshake && g_strcmp0(payload, "OK") == 0) {
      send_ack_byte(self, "+");
      self->no_ack_mode = true;
    }
    if (rpc->sets_running_on_ok && g_strcmp0(payload, "OK") == 0) {
      self->state = AMIGA_RSP_RUNNING;
    } else if (rpc->is_detach) {
      close_connection_locked(self);
    }
    g_task_return_pointer(rpc->task, g_strdup(payload), g_free);
    rsp_pending_rpc_free(rpc);
    pump_tx(self);
  }
}

static void
process_rx_buffer(AmigaRspClient *self)
{
  while (self->rx_buf && self->rx_buf->len > 0) {
    char c0 = self->rx_buf->str[0];
    if (c0 == '+' || c0 == '-') {
      g_string_erase(self->rx_buf, 0, 1);
      continue;
    }
    if (c0 != '$') {
      const char *dollar = memchr(self->rx_buf->str, '$', self->rx_buf->len);
      if (!dollar) {
        g_string_set_size(self->rx_buf, 0);
        break;
      }
      g_string_erase(self->rx_buf, 0, (gssize)(dollar - self->rx_buf->str));
      continue;
    }

    const char *hash = memchr(self->rx_buf->str + 1, '#', self->rx_buf->len - 1);
    if (!hash) break;
    size_t hash_idx = (size_t)(hash - self->rx_buf->str);
    if (hash_idx + 2u >= self->rx_buf->len) break;

    int hi = hex_nibble(self->rx_buf->str[hash_idx + 1u]);
    int lo = hex_nibble(self->rx_buf->str[hash_idx + 2u]);
    size_t plen = hash_idx - 1u;
    uint8_t expect_cs = (hi >= 0 && lo >= 0) ? (uint8_t)((hi << 4) | lo) : 0;
    uint8_t got_cs = amiga_rsp_checksum(self->rx_buf->str + 1u, plen);

    g_autofree char *expanded = NULL;
    bool valid_cs = (hi >= 0 && lo >= 0 && expect_cs == got_cs);
    if (valid_cs) {
      expanded = amiga_rsp_expand_rle(self->rx_buf->str + 1u, plen);
    }
    g_string_erase(self->rx_buf, 0, (gssize)(hash_idx + 3u));

    if (!valid_cs) {
      send_ack_byte(self, "-");
      continue;
    }
    if (!(self->in_flight && self->in_flight->is_no_ack_handshake &&
          g_strcmp0(expanded, "OK") == 0)) {
      send_ack_byte(self, "+");
    }
    handle_decoded_packet(self, expanded);
  }
}

static void
on_rx_chunk(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);

  g_autoptr(AmigaRspClient) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);

  if (!self) return;
  self->rx_active = false;

  if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return;
  }
  if (!bytes || g_bytes_get_size(bytes) == 0) {
    fail_all_pending(self, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                     err ? err->message : "Amiberry RSP socket closed");
    close_connection_locked(self);
    return;
  }

  gsize sz = 0;
  const char *data = g_bytes_get_data(bytes, &sz);
  g_string_append_len(self->rx_buf, data, (gssize)sz);
  process_rx_buffer(self);

  if (self->in_stream && self->state != AMIGA_RSP_DISCONNECTED) {
    arm_rx(self);
  }
}

static void
arm_rx(AmigaRspClient *self)
{
  if (!self->in_stream || self->rx_active ||
      g_cancellable_is_cancelled(self->cancellable)) {
    return;
  }
  self->rx_active = true;
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  g_input_stream_read_bytes_async(
      self->in_stream, 8192, G_PRIORITY_DEFAULT, self->cancellable, on_rx_chunk, weak);
}

static void
on_tx_written(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  gsize bw = 0;
  bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res, &bw, &err);

  g_autoptr(AmigaRspClient) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);

  if (!self) return;
  WireChunk *done = g_steal_pointer(&self->wire_tx_inflight);

  if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    if (done && done->notify_task) {
      g_task_return_new_error(done->notify_task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    }
    wire_chunk_free(done);
    return;
  }
  if (!ok) {
    if (done && done->notify_task) {
      g_task_return_new_error(done->notify_task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "%s", err ? err->message : "RSP write failed");
    }
    wire_chunk_free(done);
    fail_all_pending(self, G_IO_ERROR, G_IO_ERROR_FAILED,
                     err ? err->message : "RSP write failed");
    close_connection_locked(self);
    return;
  }
  if (done && done->notify_task) {
    g_task_return_boolean(done->notify_task, TRUE);
  }
  wire_chunk_free(done);
  pump_wire_tx(self);
  pump_tx(self);
}

static void
pump_wire_tx(AmigaRspClient *self)
{
  if (!self->out_stream || self->wire_tx_inflight != NULL ||
      g_queue_is_empty(self->wire_tx_queue) ||
      g_cancellable_is_cancelled(self->cancellable)) {
    return;
  }
  WireChunk *wc = g_queue_pop_head(self->wire_tx_queue);
  self->wire_tx_inflight = wc;

  gsize len = 0;
  const void *data = g_bytes_get_data(wc->bytes, &len);
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  g_output_stream_write_all_async(
      self->out_stream, data, len,
      G_PRIORITY_DEFAULT, self->cancellable, on_tx_written, weak);
}

static void
pump_tx(AmigaRspClient *self)
{
  if (!self->out_stream || self->in_flight != NULL || g_queue_is_empty(self->tx_queue)) {
    return;
  }
  RspPendingRpc *rpc = g_queue_pop_head(self->tx_queue);
  self->in_flight = rpc;
  enqueue_wire_bytes(self, rpc->wire_bytes, rpc->wire_len, NULL);
}

static void
queue_wire_rpc(AmigaRspClient      *self,
               const char          *wire,
               size_t               wire_len,
               bool                 completes_on_stop,
               bool                 sets_running_on_ok,
               bool                 is_detach,
               bool                 is_no_ack_handshake,
               GCancellable        *cancellable,
               GAsyncReadyCallback  cb,
               gpointer             ud)
{
  GTask *task = g_task_new(self, cancellable, cb, ud);
  if (!self->conn || self->state == AMIGA_RSP_DISCONNECTED) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                            "Not connected to Amiberry RSP");
    g_object_unref(task);
    return;
  }
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    g_object_unref(task);
    return;
  }

  RspPendingRpc *rpc = g_new0(RspPendingRpc, 1);
  rpc->task = task;
  rpc->wire_bytes = g_memdup2(wire, wire_len + 1u);
  rpc->wire_len = wire_len;
  rpc->completes_on_stop = completes_on_stop;
  rpc->sets_running_on_ok = sets_running_on_ok;
  rpc->is_detach = is_detach;
  rpc->is_no_ack_handshake = is_no_ack_handshake;
  g_queue_push_tail(self->tx_queue, rpc);
  pump_tx(self);
}

/* ─── Public Client API ─── */

AmigaRspClient *
amiga_rsp_client_new(void)
{
  return g_object_new(AMIGA_TYPE_RSP_CLIENT, NULL);
}

AmigaRspState
amiga_rsp_client_get_state(AmigaRspClient *client)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), AMIGA_RSP_DISCONNECTED);
  return client->state;
}

void
amiga_rsp_client_set_stop_callback(AmigaRspClient *client,
                                   AmigaRspStopCb  cb,
                                   gpointer        user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  client->stop_cb = cb;
  client->stop_ud = user_data;
}

typedef struct {
  GTask    *outer_task;
  GWeakRef  weak_client;
} ConnectCtx;

static void
on_no_ack_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  ConnectCtx *cx = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *reply = amiga_rsp_client_raw_command_finish(AMIGA_RSP_CLIENT(src), res, &err);
  g_autoptr(AmigaRspClient) self = g_weak_ref_get(&cx->weak_client);

  if (err) {
    if (self) close_connection_locked(self);
    g_task_return_error(cx->outer_task, g_steal_pointer(&err));
  } else {
    g_task_return_boolean(cx->outer_task, TRUE);
  }
  g_object_unref(cx->outer_task);
  g_weak_ref_clear(&cx->weak_client);
  g_free(cx);
}

static void
on_tcp_connected(GObject *src, GAsyncResult *res, gpointer ud)
{
  ConnectCtx *cx = ud;
  g_autoptr(GError) err = NULL;
  GSocketConnection *conn = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &err);
  g_autoptr(AmigaRspClient) self = g_weak_ref_get(&cx->weak_client);

  if (!self || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    if (conn) g_object_unref(conn);
    g_task_return_new_error(cx->outer_task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    g_object_unref(cx->outer_task);
    g_weak_ref_clear(&cx->weak_client);
    g_free(cx);
    return;
  }
  if (!conn) {
    self->state = AMIGA_RSP_DISCONNECTED;
    g_task_return_error(cx->outer_task, g_steal_pointer(&err));
    g_object_unref(cx->outer_task);
    g_weak_ref_clear(&cx->weak_client);
    g_free(cx);
    return;
  }

  GSocket *sock = g_socket_connection_get_socket(conn);
  if (sock) {
    g_socket_set_option(sock, IPPROTO_TCP, TCP_NODELAY, 1, NULL);
  }

  self->conn = conn;
  self->in_stream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
  self->out_stream = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  self->state = AMIGA_RSP_STOPPED;
  arm_rx(self);

  g_autofree char *pkt = amiga_rsp_frame_packet("QStartNoAckMode");
  queue_wire_rpc(self, pkt, strlen(pkt), false, false, false, true,
                 g_task_get_cancellable(cx->outer_task), on_no_ack_done, cx);
}

void
amiga_rsp_client_connect_async(AmigaRspClient      *client,
                               const char          *host,
                               uint16_t             port,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *task = g_task_new(client, cancellable, callback, user_data);
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    g_object_unref(task);
    return;
  }
  close_connection_locked(client);
  g_cancellable_cancel(client->cancellable);
  g_clear_object(&client->cancellable);
  client->cancellable = g_cancellable_new();
  client->state = AMIGA_RSP_CONNECTING;

  ConnectCtx *cx = g_new0(ConnectCtx, 1);
  cx->outer_task = task;
  g_weak_ref_init(&cx->weak_client, client);

  const char *h = (host && host[0]) ? host : "127.0.0.1";
  g_socket_client_connect_to_host_async(
      client->sock_client, h, port ? port : 2345, cancellable, on_tcp_connected, cx);
}

bool
amiga_rsp_client_connect_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_raw_command_async(AmigaRspClient      *client,
                                   const char          *payload,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  g_autofree char *pkt = amiga_rsp_frame_packet(payload ? payload : "");
  queue_wire_rpc(client, pkt, strlen(pkt), false, false, false, false,
                 cancellable, callback, user_data);
}

char *
amiga_rsp_client_raw_command_finish(AmigaRspClient *client,
                                    GAsyncResult   *result,
                                    GError        **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static bool
is_rcmd_allowed_while_running(const char *cmd)
{
  if (!cmd) return false;
  return g_strcmp0(cmd, "capabilities") == 0 ||
         g_strcmp0(cmd, "execution-status") == 0 ||
         g_str_has_prefix(cmd, "input ");
}

static void
on_rcmd_raw_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GTask *outer = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *raw = amiga_rsp_client_raw_command_finish(AMIGA_RSP_CLIENT(src), res, &err);
  if (err) {
    g_task_return_error(outer, g_steal_pointer(&err));
    g_object_unref(outer);
    return;
  }
  if (!raw || raw[0] == '\0') {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Unsupported qRcmd command");
    g_object_unref(outer);
    return;
  }
  if (g_strcmp0(raw, "OK") == 0) {
    g_task_return_pointer(outer, g_strdup("OK"), g_free);
    g_object_unref(outer);
    return;
  }
  if (raw[0] == 'E' && strlen(raw) == 3) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Amiberry qRcmd error %s", raw);
    g_object_unref(outer);
    return;
  }
  uint8_t *decoded = NULL;
  size_t len = 0;
  if (!amiga_rsp_hex_decode(raw, &decoded, &len)) {
    g_task_return_pointer(outer, g_strdup(raw), g_free);
  } else {
    g_task_return_pointer(outer, (char *)decoded, g_free);
  }
  g_object_unref(outer);
}

void
amiga_rsp_client_rcmd_async(AmigaRspClient      *client,
                            const char          *cmd_utf8,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (client->state == AMIGA_RSP_RUNNING && !is_rcmd_allowed_while_running(cmd_utf8)) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "qRcmd '%s' requires halted CPU (E16)", cmd_utf8 ? cmd_utf8 : "");
    g_object_unref(outer);
    return;
  }
  g_autofree char *hex = amiga_rsp_hex_encode(
      (const uint8_t *)(cmd_utf8 ? cmd_utf8 : ""), cmd_utf8 ? strlen(cmd_utf8) : 0);
  g_autofree char *payload = g_strdup_printf("qRcmd,%s", hex);
  amiga_rsp_client_raw_command_async(client, payload, cancellable, on_rcmd_raw_done, outer);
}

char *
amiga_rsp_client_rcmd_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* Expect-OK or hex forwarder helpers */
static void
on_expect_ok_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GTask *outer = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *reply = amiga_rsp_client_raw_command_finish(AMIGA_RSP_CLIENT(src), res, &err);
  if (err) {
    g_task_return_error(outer, g_steal_pointer(&err));
  } else if (g_strcmp0(reply, "OK") == 0 || (reply && (reply[0] == 'T' || reply[0] == 'S'))) {
    g_task_return_boolean(outer, TRUE);
  } else {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "RSP command failed: %s", reply && reply[0] ? reply : "empty");
  }
  g_object_unref(outer);
}

static void
on_read_regs_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GTask *outer = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *reply = amiga_rsp_client_raw_command_finish(AMIGA_RSP_CLIENT(src), res, &err);
  if (err) {
    g_task_return_error(outer, g_steal_pointer(&err));
    g_object_unref(outer);
    return;
  }
  AmigaCpuRegs *regs = g_new0(AmigaCpuRegs, 1);
  if (!amiga_rsp_parse_regs_g(reply, regs)) {
    g_free(regs);
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Invalid g register reply: %s", reply ? reply : "");
  } else {
    g_task_return_pointer(outer, regs, g_free);
  }
  g_object_unref(outer);
}

void
amiga_rsp_client_read_regs_async(AmigaRspClient      *client,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (client->state == AMIGA_RSP_RUNNING) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_BUSY, "CPU is running (E16)");
    g_object_unref(outer);
    return;
  }
  amiga_rsp_client_raw_command_async(client, "g", cancellable, on_read_regs_done, outer);
}

bool
amiga_rsp_client_read_regs_finish(AmigaRspClient *client,
                                  GAsyncResult   *result,
                                  AmigaCpuRegs   *out_regs,
                                  GError        **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  g_autofree AmigaCpuRegs *r = g_task_propagate_pointer(G_TASK(result), error);
  if (!r) return false;
  if (out_regs) *out_regs = *r;
  return true;
}

void
amiga_rsp_client_write_regs_async(AmigaRspClient      *client,
                                  const AmigaCpuRegs  *regs,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (!regs || client->state == AMIGA_RSP_RUNNING) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_BUSY, "Cannot write regs while running");
    g_object_unref(outer);
    return;
  }
  g_autofree char *payload = amiga_rsp_format_regs_G(regs);
  amiga_rsp_client_raw_command_async(client, payload, cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_write_regs_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

typedef struct {
  GTask      *outer;
  GWeakRef    weak_client;
  uint32_t    addr;
  size_t      remaining;
  size_t      cur_chunk;
  GByteArray *buf;
} ReadMemCtx;

static void
read_mem_ctx_free(ReadMemCtx *mc)
{
  if (!mc) return;
  g_clear_object(&mc->outer);
  g_weak_ref_clear(&mc->weak_client);
  if (mc->buf) g_byte_array_unref(mc->buf);
  g_free(mc);
}

static void on_read_mem_chunk_done(GObject *src, GAsyncResult *res, gpointer ud);

static void
dispatch_next_mem_chunk(AmigaRspClient *client, ReadMemCtx *mc)
{
  mc->cur_chunk = MIN(mc->remaining, (size_t)2048);
  g_autofree char *cmd = g_strdup_printf("m%x,%zx", mc->addr, mc->cur_chunk);
  amiga_rsp_client_raw_command_async(
      client, cmd, g_task_get_cancellable(mc->outer), on_read_mem_chunk_done, mc);
}

static void
on_read_mem_chunk_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  ReadMemCtx *mc = ud;
  g_autoptr(GError) err = NULL;
  g_autofree char *reply = amiga_rsp_client_raw_command_finish(AMIGA_RSP_CLIENT(src), res, &err);
  if (err) {
    g_task_return_error(mc->outer, g_steal_pointer(&err));
    read_mem_ctx_free(mc);
    return;
  }
  if (!reply || reply[0] == 'E') {
    g_task_return_new_error(mc->outer, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Memory read failed at $%x: %s", mc->addr, reply ? reply : "");
    read_mem_ctx_free(mc);
    return;
  }
  g_autofree uint8_t *bytes = NULL;
  size_t blen = 0;
  if (!amiga_rsp_hex_decode(reply, &bytes, &blen) || blen == 0) {
    g_task_return_new_error(mc->outer, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Invalid hex memory reply at $%x", mc->addr);
    read_mem_ctx_free(mc);
    return;
  }
  g_byte_array_append(mc->buf, bytes, (guint)MIN(blen, mc->cur_chunk));
  mc->addr += (uint32_t)MIN(blen, mc->cur_chunk);
  mc->remaining -= MIN(blen, mc->cur_chunk);

  g_autoptr(AmigaRspClient) client = g_weak_ref_get(&mc->weak_client);
  if (mc->remaining == 0 || !client) {
    GBytes *out = g_byte_array_free_to_bytes(g_steal_pointer(&mc->buf));
    g_task_return_pointer(mc->outer, out, (GDestroyNotify)g_bytes_unref);
    read_mem_ctx_free(mc);
    return;
  }
  dispatch_next_mem_chunk(client, mc);
}

void
amiga_rsp_client_read_mem_async(AmigaRspClient      *client,
                                uint32_t             addr,
                                size_t               len,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (len == 0) {
    g_task_return_pointer(outer, g_bytes_new_static("", 0), (GDestroyNotify)g_bytes_unref);
    g_object_unref(outer);
    return;
  }
  if (client->state == AMIGA_RSP_RUNNING) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_BUSY, "CPU is running (E16)");
    g_object_unref(outer);
    return;
  }
  ReadMemCtx *mc = g_new0(ReadMemCtx, 1);
  mc->outer = outer;
  g_weak_ref_init(&mc->weak_client, client);
  mc->addr = addr;
  mc->remaining = len;
  mc->buf = g_byte_array_sized_new((guint)len);
  dispatch_next_mem_chunk(client, mc);
}

GBytes *
amiga_rsp_client_read_mem_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
amiga_rsp_client_write_mem_async(AmigaRspClient      *client,
                                 uint32_t             addr,
                                 const uint8_t       *bytes,
                                 size_t               len,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (!bytes && len > 0) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "NULL write buffer");
    g_object_unref(outer);
    return;
  }
  g_autofree char *hex = amiga_rsp_hex_encode(bytes, len);
  g_autofree char *cmd = g_strdup_printf("M%x,%zx:%s", addr, len, hex);
  amiga_rsp_client_raw_command_async(client, cmd, cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_write_mem_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_bp_set_async(AmigaRspClient      *client,
                              uint32_t             addr,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if ((addr & 1u) != 0) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "68000 breakpoint address $%x must be even (E01)", addr);
    g_object_unref(outer);
    return;
  }
  g_autofree char *cmd = g_strdup_printf("Z0,%x,2", addr);
  amiga_rsp_client_raw_command_async(client, cmd, cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_bp_set_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_bp_remove_async(AmigaRspClient      *client,
                                 uint32_t             addr,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  g_autofree char *cmd = g_strdup_printf("z0,%x,2", addr);
  amiga_rsp_client_raw_command_async(client, cmd, cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_bp_remove_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_watch_set_async(AmigaRspClient      *client,
                                 uint8_t              z_type,
                                 uint32_t             addr,
                                 uint32_t             len,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (z_type < 2 || z_type > 4 || len == 0 || len > 0x10000u) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid Z%u watchpoint range len=0x%x", z_type, len);
    g_object_unref(outer);
    return;
  }
  g_autofree char *cmd = g_strdup_printf("Z%u,%x,%x", z_type, addr, len);
  amiga_rsp_client_raw_command_async(client, cmd, cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_watch_set_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_watch_remove_async(AmigaRspClient      *client,
                                    uint8_t              z_type,
                                    uint32_t             addr,
                                    uint32_t             len,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  g_autofree char *cmd = g_strdup_printf("z%u,%x,%x", z_type, addr, len);
  amiga_rsp_client_raw_command_async(client, cmd, cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_watch_remove_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
dispatch_vcont_safe(AmigaRspClient      *client,
                    const char          *payload,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GTask *task = g_task_new(client, cancellable, callback, user_data);
  if (!client->conn || client->state == AMIGA_RSP_DISCONNECTED) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Not connected");
    g_object_unref(task);
    return;
  }
  client->state = AMIGA_RSP_RUNNING;
  g_autofree char *pkt = amiga_rsp_frame_packet(payload);
  enqueue_wire_bytes(client, pkt, strlen(pkt), task);
  g_object_unref(task);
}

void
amiga_rsp_client_continue_async(AmigaRspClient      *client,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  dispatch_vcont_safe(client, "vCont;c", cancellable, callback, user_data);
}

bool
amiga_rsp_client_continue_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_step_into_async(AmigaRspClient      *client,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  dispatch_vcont_safe(client, "vCont;s", cancellable, callback, user_data);
}

bool
amiga_rsp_client_step_into_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_step_range_async(AmigaRspClient      *client,
                                  uint32_t             start_addr,
                                  uint32_t             end_addr,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  g_autofree char *cmd = g_strdup_printf("vCont;r%x,%x", start_addr, end_addr);
  dispatch_vcont_safe(client, cmd, cancellable, callback, user_data);
}

bool
amiga_rsp_client_step_range_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_step_over_async(AmigaRspClient      *client,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (client->state == AMIGA_RSP_RUNNING) {
    g_task_return_new_error(outer, G_IO_ERROR, G_IO_ERROR_BUSY, "CPU is running (E16)");
    g_object_unref(outer);
    return;
  }
  g_autofree char *hex = amiga_rsp_hex_encode((const uint8_t *)"step-over", 9);
  g_autofree char *payload = g_strdup_printf("qRcmd,%s", hex);
  g_autofree char *pkt = amiga_rsp_frame_packet(payload);
  /* Stage 1 returns "OK" while setting state=RUNNING; stage 2 fires stop_cb with T05... */
  queue_wire_rpc(client, pkt, strlen(pkt), false, true, false, false,
                 cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_step_over_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_interrupt_async(AmigaRspClient      *client,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (client->state == AMIGA_RSP_STOPPED) {
    g_task_return_boolean(outer, TRUE);
    g_object_unref(outer);
    return;
  }
  const char brk[2] = { 0x03, '\0' };
  queue_wire_rpc(client, brk, 1, true, false, false, false,
                 cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_interrupt_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
amiga_rsp_client_detach_async(AmigaRspClient      *client,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  g_return_if_fail(AMIGA_IS_RSP_CLIENT(client));
  GTask *outer = g_task_new(client, cancellable, callback, user_data);
  if (!client->conn || client->state == AMIGA_RSP_DISCONNECTED) {
    g_task_return_boolean(outer, TRUE);
    g_object_unref(outer);
    return;
  }
  client->state = AMIGA_RSP_DETACHING;
  g_autofree char *pkt = amiga_rsp_frame_packet("D");
  queue_wire_rpc(client, pkt, strlen(pkt), false, false, true, false,
                 cancellable, on_expect_ok_done, outer);
}

bool
amiga_rsp_client_detach_finish(AmigaRspClient *client, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(AMIGA_IS_RSP_CLIENT(client), false);
  return g_task_propagate_boolean(G_TASK(result), error);
}
