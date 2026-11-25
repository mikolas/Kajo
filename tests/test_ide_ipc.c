#include "../src/ide/ide_agent_ipc.h"
#include "../src/ide/ide_buffer.h"
#include "../src/ide/ide_dap.h"
#include "../src/ide/ide_lsp.h"
#include "../src/ide/ide_dap_launch.h"
#include "../src/ide/ide_markdown.h"
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_jsonrpc_framing_and_extraction(void) {
  const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/hover\"}";
  g_autofree char *framed = ide_jsonrpc_frame_message(body);
  g_assert_nonnull(framed);
  g_assert_true(g_str_has_prefix(framed, "Content-Length: 54\r\n\r\n"));

  /* Partial stream returns NULL without consuming */
  gsize consumed = 99;
  char *partial = ide_jsonrpc_extract_body(framed, 12, &consumed);
  g_assert_null(partial);
  g_assert_cmpuint(consumed, ==, 0);

  /* Full stream extracts body and reports exact consumed byte count */
  g_autofree char *extracted =
      ide_jsonrpc_extract_body(framed, strlen(framed), &consumed);
  g_assert_cmpstr(extracted, ==, body);
  g_assert_cmpuint(consumed, ==, strlen(framed));

  /* Back-to-back two messages in stream */
  g_autofree char *double_stream = g_strconcat(framed, framed, NULL);
  gsize c1 = 0, c2 = 0;
  g_autofree char *msg1 =
      ide_jsonrpc_extract_body(double_stream, strlen(double_stream), &c1);
  g_assert_cmpstr(msg1, ==, body);
  g_autofree char *msg2 = ide_jsonrpc_extract_body(
      double_stream + c1, strlen(double_stream) - c1, &c2);
  g_assert_cmpstr(msg2, ==, body);
}

static void test_lsp_and_dap_detection(void) {
  const char *srv_id = NULL;
  g_assert_cmpstr(ide_lsp_detect_server_for_file("/src/main.c", &srv_id),
                  ==,
                  "clangd");
  g_assert_cmpstr(srv_id, ==, "clangd");

  g_assert_cmpstr(ide_lsp_detect_server_for_file("/src/lib.rs", &srv_id),
                  ==,
                  "rust-analyzer");
  g_assert_cmpstr(srv_id, ==, "rust-analyzer");

  g_assert_cmpstr(ide_lsp_detect_server_for_file("/app/script.py", &srv_id),
                  ==,
                  "pyright-langserver");
  g_assert_cmpstr(srv_id, ==, "pyright");

  g_assert_cmpstr(ide_dap_detect_adapter_for_file("/src/main.c"), ==, "gdb -i=dap");
  g_assert_cmpstr(ide_dap_detect_adapter_for_file("/src/Main.java"),
                  ==,
                  "java-debug (jdtls)");
  g_assert_cmpstr(ide_dap_detect_adapter_for_file("/src/main.py"), ==, "none");
  g_assert_cmpstr(ide_dap_detect_adapter_for_file("/src/main.go"), ==, "dlv dap");
}

typedef struct {
  char  last_open_path[256];
  guint last_open_line;
  guint last_open_col;
  char  last_badge[128];
  char  last_toast[128];
  guint pending_count;
} TestAgentCtx;

static void on_test_open(const char *path,
                         guint       line,
                         guint       col,
                         gpointer    user_data) {
  TestAgentCtx *ctx = user_data;
  g_strlcpy(ctx->last_open_path, path, sizeof(ctx->last_open_path));
  ctx->last_open_line = line;
  ctx->last_open_col = col;
}

static void on_test_status(const char *badge,
                           const char *toast,
                           guint       pending,
                           gpointer    user_data) {
  TestAgentCtx *ctx = user_data;
  g_strlcpy(ctx->last_badge, badge ? badge : "", sizeof(ctx->last_badge));
  if (toast) g_strlcpy(ctx->last_toast, toast, sizeof(ctx->last_toast));
  ctx->pending_count = pending;
}

static void test_agent_ipc_transactions_and_rollback(void) {
  TestAgentCtx ctx = {0};
  IdeAgentIpc *ipc =
      ide_agent_ipc_new(on_test_open, NULL, on_test_status, &ctx);

  /* 1. Open command */
  g_autofree char *r_open = ide_agent_ipc_dispatch_json(
      ipc, "{\"type\":\"open\",\"path\":\"/tmp/test.c\",\"line\":42,\"col\":7}");
  g_assert_nonnull(strstr(r_open, "\"ok\":true"));
  g_assert_cmpstr(ctx.last_open_path, ==, "/tmp/test.c");
  g_assert_cmpuint(ctx.last_open_line, ==, 42);
  g_assert_cmpuint(ctx.last_open_col, ==, 7);

  /* 2. Status badge command */
  g_autofree char *r_badge = ide_agent_ipc_dispatch_json(
      ipc,
      "{\"type\":\"status-badge\",\"text\":\"🤖 AGENT WORKING\",\"active\":true}");
  g_assert_nonnull(strstr(r_badge, "\"ok\":true"));
  g_assert_cmpstr(ctx.last_badge, ==, "🤖 AGENT WORKING");

  /* 3. Diff transaction + rollback verification on real temp file */
  g_autofree char *tmp_dir =
      g_dir_make_tmp("kajo_ide_agent_XXXXXX", NULL);
  g_autofree char *tmp_file = g_build_filename(tmp_dir, "sample.c", NULL);
  const char *orig_code = "int a = 1;\nint b = 2;\n";
  const char *agent_code = "int a = 1;\nint b = 999;\nint c = 3;\n";
  g_assert_true(
      ide_save_file_atomic(tmp_file, orig_code, strlen(orig_code), NULL));

  /* Agent writes new code to disk and notifies IDE with diff */
  g_assert_true(
      ide_save_file_atomic(tmp_file, agent_code, strlen(agent_code), NULL));
  ide_agent_ipc_record_edit(ipc, tmp_file, orig_code, agent_code);
  g_assert_cmpuint(ide_agent_ipc_get_pending_count(ipc), ==, 1);

  /* Reject All restores original code atomically */
  ide_agent_ipc_reject_all(ipc);
  g_assert_cmpuint(ide_agent_ipc_get_pending_count(ipc), ==, 0);

  g_autofree char *restored = NULL;
  g_assert_true(g_file_get_contents(tmp_file, &restored, NULL, NULL));
  g_assert_cmpstr(restored, ==, orig_code);

  unlink(tmp_file);
  rmdir(tmp_dir);
  ide_agent_ipc_free(ipc);
}

static void test_agent_unix_socket_server(void) {
  TestAgentCtx ctx = {0};
  IdeAgentIpc *ipc =
      ide_agent_ipc_new(on_test_open, NULL, on_test_status, &ctx);

  g_autofree char *tmp_dir = g_dir_make_tmp("kajo_sock_XXXXXX", NULL);
  g_autofree char *sock_path = g_build_filename(tmp_dir, "ide.sock", NULL);

  g_autoptr(GError) err = NULL;
  g_assert_true(ide_agent_ipc_start_on_path(ipc, sock_path, &err));
  g_assert_no_error(err);

  /* Verify chmod 0600 on socket */
  struct stat st;
  g_assert_cmpint(stat(sock_path, &st), ==, 0);
  g_assert_cmpuint(st.st_mode & 0777, ==, 0600);

  ide_agent_ipc_free(ipc);
  rmdir(tmp_dir);
}

/* ── Socket behaviour: real round-trip + stalled client must not block the loop ── */

typedef struct {
  const char *sock_path;
  const char *request;     /* NULL => connect and send nothing (stalled client) */
  char       *response;    /* what the server sent back (may be empty) */
  gint64      elapsed_ms;
  gint        done;        /* atomic */
} ClientJob;

static gpointer client_thread(gpointer data) {
  ClientJob *job = data;
  gint64 t0 = g_get_monotonic_time();
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(job->sock_path);
  g_autoptr(GSocketConnection) conn =
      g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
  if (conn) {
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    if (job->request)
      g_output_stream_write_all(out, job->request, strlen(job->request), NULL, NULL, NULL);
    g_autoptr(GDataInputStream) in =
        g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    gsize len = 0;
    job->response = g_data_input_stream_read_line(in, &len, NULL, NULL);
  }
  job->elapsed_ms = (g_get_monotonic_time() - t0) / 1000;
  g_atomic_int_set(&job->done, 1);
  return NULL;
}

typedef struct { guint ticks; gint *done; gboolean timed_out; GMainLoop *loop; } PumpCtx;

static gboolean pump_tick(gpointer data) {
  PumpCtx *p = data;
  p->ticks++;
  if (g_atomic_int_get(p->done)) {
    g_main_loop_quit(p->loop);
    return G_SOURCE_REMOVE;
  }
  if (p->ticks > 80) { /* 8 s safety net */
    p->timed_out = TRUE;
    g_main_loop_quit(p->loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static guint run_client_against(IdeAgentIpc *ipc, ClientJob *job) {
  (void)ipc;
  PumpCtx p = { 0, &job->done, FALSE, g_main_loop_new(NULL, FALSE) };
  GThread *th = g_thread_new("test-client", client_thread, job);
  g_timeout_add(100, pump_tick, &p);
  g_main_loop_run(p.loop);
  g_thread_join(th);
  g_main_loop_unref(p.loop);
  g_assert_false(p.timed_out);
  return p.ticks;
}

static void test_agent_socket_roundtrip_and_stalled_client(void) {
  TestAgentCtx ctx = {0};
  IdeAgentIpc *ipc = ide_agent_ipc_new(on_test_open, NULL, on_test_status, &ctx);
  g_autofree char *tmp_dir = g_dir_make_tmp("kajo_sock2_XXXXXX", NULL);
  g_autofree char *sock_path = g_build_filename(tmp_dir, "ide.sock", NULL);
  g_autoptr(GError) err = NULL;
  g_assert_true(ide_agent_ipc_start_on_path(ipc, sock_path, &err));
  g_assert_no_error(err);

  /* 1. A well-formed request is answered. */
  ClientJob ok = { sock_path, "{\"type\":\"status-badge\",\"text\":\"AGENT\"}\n", NULL, 0, 0 };
  run_client_against(ipc, &ok);
  g_assert_nonnull(ok.response);
  g_assert_nonnull(strstr(ok.response, "\"ok\":true"));
  g_free(ok.response);

  /* 2. A client that connects and never sends a newline must not freeze the
   * main loop (it keeps ticking) and is dropped after the read timeout. */
  ClientJob stalled = { sock_path, NULL, NULL, 0, 0 };
  guint ticks = run_client_against(ipc, &stalled);
  g_assert_cmpuint(ticks, >=, 15);           /* loop kept running for ~2 s */
  g_assert_cmpint(stalled.elapsed_ms, >=, 1500);
  g_assert_cmpint(stalled.elapsed_ms, <, 6000);
  g_assert_null(stalled.response);           /* connection closed, no reply */

  ide_agent_ipc_free(ipc);
  unlink(sock_path);
  rmdir(tmp_dir);
}

static void test_mermaid_vector_parser_and_markdown_checklist(void) {
  const char *mermaid_src =
      "flowchart LR\n"
      "  A[\"Key Press\"] -->|FSM| B[\"Modal Engine\"]\n"
      "  B -.->|JSON-RPC| C[\"LSP Server\"]\n";

  IdeMermaidGraph *g = ide_mermaid_parse_graph(mermaid_src, 10);
  g_assert_nonnull(g);
  g_assert_cmpint(g->kind, ==, IDE_MERMAID_FLOWCHART_LR);
  g_assert_cmpuint(g->nodes->len, ==, 3);
  g_assert_cmpuint(g->edges->len, ==, 2);

  IdeMermaidNode *n0 = g_ptr_array_index(g->nodes, 0);
  IdeMermaidNode *n1 = g_ptr_array_index(g->nodes, 1);
  IdeMermaidNode *n2 = g_ptr_array_index(g->nodes, 2);

  g_assert_cmpstr(n0->id, ==, "A");
  g_assert_cmpstr(n0->label, ==, "Key Press");
  g_assert_cmpuint(n0->source_line_1based, ==, 11);

  /* Layered layout ranks: A (0) -> B (1) -> C (2) */
  g_assert_cmpint(n0->rank, ==, 0);
  g_assert_cmpint(n1->rank, ==, 1);
  g_assert_cmpint(n2->rank, ==, 2);
  g_assert_cmpfloat(n1->x, >, n0->x);
  g_assert_cmpfloat(n2->x, >, n1->x);

  /* Hit-test node B center at 100% zoom */
  const IdeMermaidNode *hit = ide_mermaid_hit_test_node(
      g, 1.0, n1->x + n1->width * 0.5, n1->y + n1->height * 0.5);
  g_assert_nonnull(hit);
  g_assert_cmpstr(hit->id, ==, "B");

  /* Export vector SVG via Cairo SVG surface */
  g_autofree char *tmp_dir = g_dir_make_tmp("kajo_mermaid_XXXXXX", NULL);
  g_autofree char *svg_path = g_build_filename(tmp_dir, "diagram.svg", NULL);
  g_assert_true(ide_mermaid_export_svg(g, svg_path));
  g_autofree char *svg_data = NULL;
  g_assert_true(g_file_get_contents(svg_path, &svg_data, NULL, NULL));
  g_assert_nonnull(strstr(svg_data, "<svg"));

  unlink(svg_path);
  rmdir(tmp_dir);
  ide_mermaid_graph_free(g);

  /* Bi-directional checklist toggle */
  const char *md_doc = "# Tasks\n- [ ] Write parser\n- [x] Ship UI\n";
  g_autofree char *toggled1 = ide_markdown_toggle_checklist_line(md_doc, 2);
  g_assert_nonnull(strstr(toggled1, "- [x] Write parser"));
  g_autofree char *toggled2 = ide_markdown_toggle_checklist_line(toggled1, 3);
  g_assert_nonnull(strstr(toggled2, "- [ ] Ship UI"));
}

/* Validate GtkLabel markup: well-formed XML, no nested <a>, and valid Pango
 * once GtkLabel's <a href="...">/</a> wrappers are stripped. */
static void assert_valid_gtk_label_markup(const char *markup) {
  g_assert_nonnull(markup);
  g_autoptr(GRegex) re_open = g_regex_new("<a\\s+href=\"[^\"]*\">", 0, 0, NULL);
  g_autoptr(GRegex) re_close = g_regex_new("</a>", 0, 0, NULL);
  /* Verify balanced, non-nested <a> tags */
  int depth = 0;
  for (const char *p = markup; *p; p++) {
    if (g_str_has_prefix(p, "<a ")) {
      depth++;
      g_assert_cmpint(depth, ==, 1);
    } else if (g_str_has_prefix(p, "</a>")) {
      depth--;
      g_assert_cmpint(depth, >=, 0);
    }
  }
  g_assert_cmpint(depth, ==, 0);
  g_autofree char *s1 = g_regex_replace_literal(re_open, markup, -1, 0, "", 0, NULL);
  g_autofree char *s2 = g_regex_replace_literal(re_close, s1, -1, 0, "", 0, NULL);
  g_autoptr(GError) err = NULL;
  gboolean ok = pango_parse_markup(s2, -1, 0, NULL, NULL, NULL, &err);
  g_assert_no_error(err);
  g_assert_true(ok);
}

static void test_markdown_rich_embeds_and_inline_pango(void) {
  /* 1. Markdown ![alt](src "title") extraction */
  char *alt = NULL, *src = NULL, *link_url = NULL;
  g_assert_true(ide_markdown_extract_embed(
      "![Kajo Banner](docs/kajo-banner.svg \"Refracted Prism\")",
      &alt,
      &src,
      &link_url));
  g_assert_cmpstr(alt, ==, "Kajo Banner");
  g_assert_cmpstr(src, ==, "docs/kajo-banner.svg");
  g_assert_null(link_url);
  g_clear_pointer(&alt, g_free);
  g_clear_pointer(&src, g_free);
  g_clear_pointer(&link_url, g_free);

  /* 2. Linked image badge [![alt](src)](href) extraction */
  g_assert_true(ide_markdown_extract_embed(
      "[![Build Status](assets/badge.svg)](https://example.org)",
      &alt,
      &src,
      &link_url));
  g_assert_cmpstr(alt, ==, "Build Status");
  g_assert_cmpstr(src, ==, "assets/badge.svg");
  g_assert_cmpstr(link_url, ==, "https://example.org");
  g_clear_pointer(&alt, g_free);
  g_clear_pointer(&src, g_free);
  g_clear_pointer(&link_url, g_free);

  /* 3. HTML <img src="..." alt="..."> extraction (including indented lines) */
  g_assert_true(ide_markdown_extract_embed(
      "  <img src=\"docs/kajo-banner.svg\" alt=\"Kajo Desktop Banner\" width=\"100%\">",
      &alt,
      &src,
      &link_url));
  g_assert_cmpstr(alt, ==, "Kajo Desktop Banner");
  g_assert_cmpstr(src, ==, "docs/kajo-banner.svg");
  g_assert_null(link_url);
  g_clear_pointer(&alt, g_free);
  g_clear_pointer(&src, g_free);
  g_clear_pointer(&link_url, g_free);

  /* 4. Asset classification */
  g_assert_cmpint(ide_markdown_classify_asset("docs/kajo-banner.svg"),
                  ==,
                  IDE_MD_EMBED_IMAGE);
  g_assert_cmpint(ide_markdown_classify_asset("manual.pdf"),
                  ==,
                  IDE_MD_EMBED_PDF);
  g_assert_cmpint(ide_markdown_classify_asset("release.tar.gz"),
                  ==,
                  IDE_MD_EMBED_ARCHIVE);
  g_assert_cmpint(ide_markdown_classify_asset("demo.mp4"),
                  ==,
                  IDE_MD_EMBED_MEDIA);
  g_assert_cmpint(ide_markdown_classify_asset("theme.flac"),
                  ==,
                  IDE_MD_EMBED_MEDIA);
  g_assert_cmpint(ide_markdown_classify_asset("src/main.c"),
                  ==,
                  IDE_MD_EMBED_DOCUMENT);

  /* 5. Relative path & file:// URI resolution with #L42 line anchor */
  g_autofree char *tmp_ws = g_dir_make_tmp("kajo_md_ws_XXXXXX", NULL);
  g_autofree char *docs_dir = g_build_filename(tmp_ws, "docs", NULL);
  g_mkdir_with_parents(docs_dir, 0755);
  g_autofree char *banner_file =
      g_build_filename(docs_dir, "kajo-banner.svg", NULL);
  g_file_set_contents(banner_file, "<svg></svg>", -1, NULL);

  guint line_1based = 0;
  g_autofree char *resolved1 = ide_markdown_resolve_asset_path(
      "docs/kajo-banner.svg", tmp_ws, tmp_ws, &line_1based);
  g_assert_cmpstr(resolved1, ==, banner_file);
  g_assert_cmpuint(line_1based, ==, 0);

  g_autofree char *uri_with_line =
      g_strdup_printf("file://%s#L128", banner_file);
  g_autofree char *resolved2 = ide_markdown_resolve_asset_path(
      uri_with_line, docs_dir, tmp_ws, &line_1based);
  g_assert_cmpstr(resolved2, ==, banner_file);
  g_assert_cmpuint(line_1based, ==, 128);

  unlink(banner_file);
  rmdir(docs_dir);
  rmdir(tmp_ws);

  /* 6. Inline Pango markup formatting */
  g_autofree char *pango = ide_markdown_format_inline_pango(
      "Use **bold**, *italic*, ~~strike~~, `code`, <kbd>Ctrl+P</kbd>, and [Kajo](https://kajo.org).");
  g_assert_nonnull(strstr(pango, "<b>bold</b>"));
  g_assert_nonnull(strstr(pango, "<i>italic</i>"));
  g_assert_nonnull(strstr(pango, "<s>strike</s>"));
  g_assert_nonnull(strstr(pango, " code "));
  g_assert_nonnull(strstr(pango, "Ctrl+P"));
  g_assert_nonnull(strstr(pango, "<a href=\"https://kajo.org\">"));
  assert_valid_gtk_label_markup(pango);

  /* 7. Auto-link tag must only check inside <...> and not swallow trailing text */
  g_autofree char *auto_pango = ide_markdown_format_inline_pango(
      "See <https://kajo.org> followed by <not-a-link> and plain text.");
  assert_valid_gtk_label_markup(auto_pango);
  g_assert_nonnull(strstr(auto_pango, "<a href=\"https://kajo.org\">"));
  g_assert_nonnull(strstr(auto_pango, "followed by"));
  g_assert_null(strstr(auto_pango, "<a href=\"not-a-link\">"));

  /* 8. Nested link/badge inside [label](url) must produce valid Pango (no nested <a>) */
  g_autofree char *nested_pango = ide_markdown_format_inline_pango(
      "[![Build](https://ci.example/b.svg)](https://kajo.org)");
  assert_valid_gtk_label_markup(nested_pango);
  const char *first_a = strstr(nested_pango, "<a ");
  g_assert_nonnull(first_a);
  g_assert_null(strstr(first_a + 3, "<a "));

  /* 9. Table separator row detection and markdown path classification */
  g_assert_true(ide_markdown_is_table_separator_row("| :-- | :---: | ---: |"));
  g_assert_true(ide_markdown_is_table_separator_row("|---|---|"));
  g_assert_false(ide_markdown_is_table_separator_row("| feature | ---flag |"));
  g_assert_false(ide_markdown_is_table_separator_row("| a | b |"));
  g_assert_false(ide_markdown_is_table_separator_row("plain text --- here"));

  g_assert_true(ide_markdown_is_markdown_path("README.md"));
  g_assert_true(ide_markdown_is_markdown_path("/tmp/docs/spec.markdown"));
  g_assert_true(ide_markdown_is_markdown_path("guide.MDX"));
  g_assert_false(ide_markdown_is_markdown_path("src/ide/ide_window.c"));
  g_assert_false(ide_markdown_is_markdown_path("Makefile"));
  g_assert_false(ide_markdown_is_markdown_path(NULL));
}

static void test_mermaid_extended_syntax(void) {
  /* Chained arrows, & multi-target fan-out, mid-arrow labels, and <br/> line breaks */
  const char *flow =
      "flowchart TD\n"
      "  A[\"Stage 1<br/>Init\"] --> B[\"Stage 2\"] --> C[\"Done\"]\n"
      "  B & C -- \"writes\" --> D[\"Output\"]\n";
  IdeMermaidGraph *g = ide_mermaid_parse_graph(flow, 1);
  g_assert_nonnull(g);
  g_assert_cmpint(g->kind, ==, IDE_MERMAID_FLOWCHART_TD);
  g_assert_cmpuint(g->nodes->len, ==, 4);
  g_assert_cmpuint(g->edges->len, ==, 4);

  IdeMermaidNode *na = g_ptr_array_index(g->nodes, 0);
  g_assert_cmpstr(na->label, ==, "Stage 1\nInit");
  g_assert_cmpfloat(na->height, >, 42.0);

  int writes_edges = 0;
  for (guint i = 0; i < g->edges->len; i++) {
    IdeMermaidEdge *e = g_ptr_array_index(g->edges, i);
    if (g_strcmp0(e->label, "writes") == 0) writes_edges++;
  }
  g_assert_cmpint(writes_edges, ==, 2);
  ide_mermaid_graph_free(g);

  /* Fan-out / fan-in TD layout: A and E centered over B..D span, zero sibling overlap */
  const char *fan_flow =
      "flowchart TD\n"
      "  A[\"Start\"] --> B[\"Branch Left\"] & C[\"Branch Mid\"] & D[\"Branch Right\"]\n"
      "  B & C & D --> E[\"Merge\"]\n";
  IdeMermaidGraph *fg = ide_mermaid_parse_graph(fan_flow, 1);
  g_assert_nonnull(fg);
  g_assert_cmpuint(fg->nodes->len, ==, 5);
  const IdeMermaidNode *fa = g_ptr_array_index(fg->nodes, 0);
  const IdeMermaidNode *fb = g_ptr_array_index(fg->nodes, 1);
  const IdeMermaidNode *fc = g_ptr_array_index(fg->nodes, 2);
  const IdeMermaidNode *fd = g_ptr_array_index(fg->nodes, 3);
  const IdeMermaidNode *fe = g_ptr_array_index(fg->nodes, 4);
  double min_bx = MIN(fb->x, MIN(fc->x, fd->x));
  double max_bx = MAX(fb->x + fb->width, MAX(fc->x + fc->width, fd->x + fd->width));
  double mid_bcd = (min_bx + max_bx) * 0.5;
  g_assert_cmpfloat(fabs((fa->x + fa->width * 0.5) - mid_bcd), <, 1.0);
  g_assert_cmpfloat(fabs((fe->x + fe->width * 0.5) - mid_bcd), <, 1.0);
  g_assert_cmpfloat(fc->x, >=, fb->x + fb->width);
  g_assert_cmpfloat(fd->x, >=, fc->x + fc->width);
  ide_mermaid_graph_free(fg);

  /* Shared dark style scheme resolution works without creating an IdeBuffer */
  g_assert_nonnull(ide_resolve_style_scheme("kajo-metro-dark"));

  /* Sequence diagram participant aliases & Note skipping */
  const char *seq =
      "sequenceDiagram\n"
      "  participant K as Kajo IDE\n"
      "  participant L as Language Server\n"
      "  Note over K,L: handshake\n"
      "  K->>L: initialize\n"
      "  L-->>K: capabilities\n";
  IdeMermaidGraph *sg = ide_mermaid_parse_graph(seq, 10);
  g_assert_nonnull(sg);
  g_assert_cmpint(sg->kind, ==, IDE_MERMAID_SEQUENCE);
  g_assert_cmpuint(sg->nodes->len, ==, 2);
  g_assert_cmpuint(sg->edges->len, ==, 2);
  IdeMermaidNode *sk = g_ptr_array_index(sg->nodes, 0);
  g_assert_cmpstr(sk->id, ==, "K");
  g_assert_cmpstr(sk->label, ==, "Kajo IDE");
  ide_mermaid_graph_free(sg);
}


/* ───────────── LSP: pure helpers ───────────── */

static void test_lsp_text_edits_and_snippets(void) {
  g_autoptr(GPtrArray) edits = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);
  /* out of order on purpose; second edit touches a surrogate pair line */
  g_ptr_array_add(edits, ide_lsp_text_edit_new(1, 3, 1, 3, "!"));
  g_ptr_array_add(edits, ide_lsp_text_edit_new(0, 0, 0, 5, "HELLO"));
  g_autofree char *out = ide_lsp_apply_edits_to_string("hello\nab\xF0\x9F\x9A\x80" "cd\n", edits);
  /* "ab🚀cd": 🚀 is 2 UTF-16 units, so character 3 sits *inside* the pair's end → after 🚀? */
  g_assert_nonnull(out);
  g_assert_true(g_str_has_prefix(out, "HELLO\n"));

  /* character 4 is right after the emoji */
  g_autoptr(GPtrArray) e2 = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);
  g_ptr_array_add(e2, ide_lsp_text_edit_new(0, 4, 0, 4, "_"));
  g_autofree char *o2 = ide_lsp_apply_edits_to_string("ab\xF0\x9F\x9A\x80" "cd", e2);
  g_assert_cmpstr(o2, ==, "ab\xF0\x9F\x9A\x80_cd");

  /* multi-line replace and deletion */
  g_autoptr(GPtrArray) e3 = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);
  g_ptr_array_add(e3, ide_lsp_text_edit_new(0, 2, 1, 1, "X"));
  g_autofree char *o3 = ide_lsp_apply_edits_to_string("abc\ndef\n", e3);
  g_assert_cmpstr(o3, ==, "abXef\n");

  /* out of range / overlapping edits are rejected */
  g_autoptr(GPtrArray) bad = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);
  g_ptr_array_add(bad, ide_lsp_text_edit_new(9, 0, 9, 1, "x"));
  g_assert_null(ide_lsp_apply_edits_to_string("a\n", bad));
  g_autoptr(GPtrArray) ov = g_ptr_array_new_with_free_func(ide_lsp_text_edit_free);
  g_ptr_array_add(ov, ide_lsp_text_edit_new(0, 0, 0, 3, "x"));
  g_ptr_array_add(ov, ide_lsp_text_edit_new(0, 1, 0, 2, "y"));
  g_assert_null(ide_lsp_apply_edits_to_string("abcd", ov));

  /* snippets */
  g_autofree char *s1 = ide_lsp_strip_snippet("Printf(${1:format}, ${2:args...})$0");
  g_assert_cmpstr(s1, ==, "Printf(format, args...)");
  g_autofree char *s2 = ide_lsp_strip_snippet("for ${1:i} := range ${2|a,b|} {\n\t$0\n}");
  g_assert_cmpstr(s2, ==, "for i := range  {\n\t\n}");
  g_autofree char *s3 = ide_lsp_strip_snippet("cost \\$5 and ${3} $1");
  g_assert_cmpstr(s3, ==, "cost $5 and  ");

  /* hover contents in all three LSP shapes */
  JsonNode *a = json_from_string("{\"kind\":\"markdown\",\"value\":\"**x**\"}", NULL);
  g_autofree char *ha = ide_lsp_hover_to_markdown(a);
  g_assert_cmpstr(ha, ==, "**x**");
  JsonNode *b = json_from_string("{\"language\":\"go\",\"value\":\"func f()\"}", NULL);
  g_autofree char *hb = ide_lsp_hover_to_markdown(b);
  g_assert_cmpstr(hb, ==, "```go\nfunc f()\n```");
  JsonNode *c = json_from_string("[\"one\", {\"language\":\"c\",\"value\":\"int x\"}]", NULL);
  g_autofree char *hc = ide_lsp_hover_to_markdown(c);
  g_assert_nonnull(strstr(hc, "one\n\n```c\nint x\n```"));
  json_node_unref(a);
  json_node_unref(b);
  json_node_unref(c);

  /* TextEdit and InsertReplaceEdit parsing */
  JsonNode *en = json_from_string(
      "[{\"range\":{\"start\":{\"line\":1,\"character\":2},\"end\":{\"line\":1,\"character\":4}},\"newText\":\"a\"},"
      "{\"insert\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}},"
      "\"replace\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":3}},\"newText\":\"b\"}]", NULL);
  g_autoptr(GPtrArray) parsed = ide_lsp_parse_text_edits(en);
  g_assert_cmpuint(parsed->len, ==, 2);
  const IdeLspTextEdit *p1 = g_ptr_array_index(parsed, 1);
  g_assert_cmpuint(p1->char_end, ==, 3); /* replace range wins over insert */
  json_node_unref(en);
}

static void test_lsp_references_signature_call_hierarchy_parsers(void) {
  /* 1. ide_lsp_parse_locations: Location + LocationLink */
  const char *locs_json =
      "["
      "  {\"uri\":\"file:///tmp/a.c\",\"range\":{\"start\":{\"line\":4,\"character\":2},\"end\":{\"line\":4,\"character\":5}}},"
      "  {\"targetUri\":\"file:///tmp/b.c\",\"targetRange\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":9,\"character\":1}},"
      "   \"targetSelectionRange\":{\"start\":{\"line\":2,\"character\":7},\"end\":{\"line\":2,\"character\":11}}},"
      "  {\"targetUri\":\"file:///tmp/c.c\",\"targetRange\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":3}}}"
      "]";
  g_autoptr(JsonNode) locs_node = json_from_string(locs_json, NULL);
  g_autoptr(GPtrArray) locs = ide_lsp_parse_locations(locs_node);
  g_assert_cmpuint(locs->len, ==, 3);

  const IdeLspLocation *l0 = g_ptr_array_index(locs, 0);
  g_assert_cmpstr(l0->path, ==, "/tmp/a.c");
  g_assert_cmpuint(l0->line_1based, ==, 5);
  g_assert_cmpuint(l0->col_1based, ==, 3);

  const IdeLspLocation *l1 = g_ptr_array_index(locs, 1);
  g_assert_cmpstr(l1->path, ==, "/tmp/b.c");
  g_assert_cmpuint(l1->line_1based, ==, 3);
  g_assert_cmpuint(l1->col_1based, ==, 8);

  const IdeLspLocation *l2 = g_ptr_array_index(locs, 2);
  g_assert_cmpstr(l2->path, ==, "/tmp/c.c");
  g_assert_cmpuint(l2->line_1based, ==, 1);
  g_assert_cmpuint(l2->col_1based, ==, 1);

  g_autoptr(GPtrArray) empty_locs = ide_lsp_parse_locations(NULL);
  g_assert_cmpuint(empty_locs->len, ==, 0);

  /* 2. ide_lsp_parse_signature_help + ide_lsp_format_signature_pango */
  const char *sig_json =
      "{"
      "  \"activeSignature\": 0,"
      "  \"activeParameter\": 1,"
      "  \"signatures\": ["
      "    {"
      "      \"label\": \"void fill(std::vector<int> &out, const T *ptr)\","
      "      \"documentation\": {\"kind\":\"markdown\",\"value\":\"Fills out.\"},"
      "      \"parameters\": ["
      "        {\"label\": \"std::vector<int> &out\"},"
      "        {\"label\": \"const T *ptr\"}"
      "      ]"
      "    },"
      "    {"
      "      \"label\": \"int add(int, int)\","
      "      \"documentation\": \"Adds two ints.\","
      "      \"parameters\": [{\"label\": [8, 11]}, {\"label\": [13, 16]}]"
      "    }"
      "  ]"
      "}";
  g_autoptr(JsonNode) sig_node = json_from_string(sig_json, NULL);
  guint active_sig = 99;
  gint active_param = -99;
  g_autoptr(GPtrArray) sigs = ide_lsp_parse_signature_help(sig_node, &active_sig, &active_param);
  g_assert_cmpuint(sigs->len, ==, 2);
  g_assert_cmpuint(active_sig, ==, 0);
  g_assert_cmpint(active_param, ==, 1);

  const IdeLspSignatureInfo *s0 = g_ptr_array_index(sigs, 0);
  g_assert_cmpstr(s0->documentation, ==, "Fills out.");
  g_assert_cmpuint(s0->param_labels->len, ==, 2);
  g_autofree char *p0 = ide_lsp_format_signature_pango(s0, 0);
  g_autoptr(GError) perr = NULL;
  g_assert_true(pango_parse_markup(p0, -1, 0, NULL, NULL, NULL, &perr));
  g_assert_no_error(perr);
  g_assert_nonnull(strstr(p0, "<span foreground=\"#00d8f0\" weight=\"bold\">std::vector&lt;int&gt; &amp;out</span>"));

  g_autofree char *p1 = ide_lsp_format_signature_pango(s0, 1);
  g_assert_true(pango_parse_markup(p1, -1, 0, NULL, NULL, NULL, &perr));
  g_assert_no_error(perr);
  g_assert_nonnull(strstr(p1, "<span foreground=\"#00d8f0\" weight=\"bold\">const T *ptr</span>"));

  /* Offset-tuple parameter labels + duplicate type highlighting (2nd "int") */
  const IdeLspSignatureInfo *s1 = g_ptr_array_index(sigs, 1);
  g_assert_cmpuint(s1->param_labels->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(s1->param_labels, 0), ==, "int");
  g_assert_cmpstr(g_ptr_array_index(s1->param_labels, 1), ==, "int");
  g_autofree char *p_dup = ide_lsp_format_signature_pango(s1, 1);
  g_assert_true(pango_parse_markup(p_dup, -1, 0, NULL, NULL, NULL, &perr));
  g_assert_no_error(perr);
  g_assert_cmpstr(p_dup, ==, "int add(int, <span foreground=\"#00d8f0\" weight=\"bold\">int</span>)");

  /* 3. ide_lsp_parse_call_hierarchy: incoming & outgoing */
  const char *in_json =
      "["
      "  {\"from\":{\"name\":\"dispatch\",\"kind\":12,\"detail\":\"core.c\",\"uri\":\"file:///tmp/core.c\","
      "            \"range\":{\"start\":{\"line\":10,\"character\":0},\"end\":{\"line\":20,\"character\":1}},"
      "            \"selectionRange\":{\"start\":{\"line\":10,\"character\":4},\"end\":{\"line\":10,\"character\":12}}},"
      "   \"fromRanges\":[{\"start\":{\"line\":14,\"character\":6},\"end\":{\"line\":14,\"character\":10}}]},"
      "  {\"from\":{\"name\":\"init_all\",\"kind\":6,\"uri\":\"file:///tmp/init.c\","
      "            \"selectionRange\":{\"start\":{\"line\":3,\"character\":2},\"end\":{\"line\":3,\"character\":10}}},"
      "   \"fromRanges\":[]}"
      "]";
  g_autoptr(JsonNode) in_node = json_from_string(in_json, NULL);
  g_autoptr(GPtrArray) inc = ide_lsp_parse_call_hierarchy(in_node, FALSE);
  g_assert_cmpuint(inc->len, ==, 2);
  const IdeLspCallEntry *ci0 = g_ptr_array_index(inc, 0);
  g_assert_cmpstr(ci0->name, ==, "dispatch");
  g_assert_cmpstr(ci0->detail, ==, "core.c");
  g_assert_cmpstr(ci0->path, ==, "/tmp/core.c");
  g_assert_cmpuint(ci0->line_1based, ==, 15);
  g_assert_cmpuint(ci0->col_1based, ==, 7);
  g_assert_cmpstr(ci0->kind_badge, ==, "ƒ");
  g_assert_false(ci0->outgoing);
  const IdeLspCallEntry *ci1 = g_ptr_array_index(inc, 1);
  g_assert_cmpuint(ci1->line_1based, ==, 4);
  g_assert_cmpuint(ci1->col_1based, ==, 3);

  const char *out_json =
      "["
      "  {\"to\":{\"name\":\"helper\",\"kind\":5,\"detail\":\"pkg.Type\",\"uri\":\"file:///tmp/helper.c\","
      "          \"range\":{\"start\":{\"line\":20,\"character\":0},\"end\":{\"line\":30,\"character\":1}},"
      "          \"selectionRange\":{\"start\":{\"line\":21,\"character\":5},\"end\":{\"line\":21,\"character\":11}}},"
      "   \"fromRanges\":[{\"start\":{\"line\":5,\"character\":2},\"end\":{\"line\":5,\"character\":8}}]}"
      "]";
  g_autoptr(JsonNode) out_node = json_from_string(out_json, NULL);
  g_autoptr(GPtrArray) outc = ide_lsp_parse_call_hierarchy(out_node, TRUE);
  g_assert_cmpuint(outc->len, ==, 1);
  const IdeLspCallEntry *co0 = g_ptr_array_index(outc, 0);
  g_assert_cmpstr(co0->name, ==, "helper");
  g_assert_cmpstr(co0->detail, ==, "pkg.Type");
  g_assert_cmpstr(co0->path, ==, "/tmp/helper.c");
  g_assert_cmpuint(co0->line_1based, ==, 22);
  g_assert_cmpuint(co0->col_1based, ==, 6);
  g_assert_cmpstr(co0->kind_badge, ==, "T");
  g_assert_true(co0->outgoing);
}

static void test_lsp_server_registry(void) {
  const IdeLspServerSpec *go = ide_lsp_server_spec_for_file("/w/pkg/main.go");
  g_assert_nonnull(go);
  g_assert_cmpstr(go->server_id, ==, "gopls");
  g_assert_cmpstr(ide_lsp_language_id_for_file("/w/A.java"), ==, "java");
  g_assert_cmpstr(ide_lsp_language_id_for_file("/w/a.tsx"), ==, "typescriptreact");
  g_assert_cmpstr(ide_lsp_language_id_for_file("/w/readme.txt"), ==, "plaintext");
  {
    /* .h is C unless C++ sources sit next to it */
    g_autofree char *hd = g_dir_make_tmp("kajo-hdr-XXXXXX", NULL);
    g_autofree char *h = g_build_filename(hd, "a.h", NULL);
    g_file_set_contents(h, "", -1, NULL);
    g_assert_cmpstr(ide_lsp_language_id_for_file(h), ==, "c");
    g_autofree char *cc = g_build_filename(hd, "a.cpp", NULL);
    g_file_set_contents(cc, "", -1, NULL);
    g_assert_cmpstr(ide_lsp_language_id_for_file(h), ==, "cpp");
  }
  g_assert_null(ide_lsp_server_spec_for_file("/w/readme.txt"));

  /* project root = nearest ancestor with a marker, bounded by the workspace */
  g_autofree char *ws = g_dir_make_tmp("kajo-lsp-root-XXXXXX", NULL);
  g_autofree char *mod = g_build_filename(ws, "svc", NULL);
  g_autofree char *deep = g_build_filename(mod, "internal", "x", NULL);
  g_mkdir_with_parents(deep, 0755);
  g_autofree char *gomod = g_build_filename(mod, "go.mod", NULL);
  g_file_set_contents(gomod, "module m\n", -1, NULL);
  g_autofree char *file = g_build_filename(deep, "a.go", NULL);
  g_autofree char *root = ide_lsp_find_root(go, file, ws);
  g_assert_cmpstr(root, ==, mod);
  g_autofree char *other = g_build_filename(ws, "loose.go", NULL);
  g_autofree char *fallback = ide_lsp_find_root(go, other, ws);
  g_assert_cmpstr(fallback, ==, ws);

  /* jdtls gets an isolated data dir; clangd finds builddir/compile_commands.json */
  g_setenv("KAJO_JDTLS_JAVA", "/opt/jdk-test/bin/java", TRUE);
  g_auto(GStrv) jv = ide_lsp_build_argv(ide_lsp_server_spec_for_file("A.java"), ws);
  g_assert_cmpstr(jv[0], ==, "jdtls");
  g_assert_cmpstr(jv[1], ==, "-data");
  g_assert_nonnull(strstr(jv[2], "kajo/jdtls"));
  g_assert_cmpstr(jv[3], ==, "--java-executable");
  g_assert_cmpstr(jv[4], ==, "/opt/jdk-test/bin/java");
  g_unsetenv("KAJO_JDTLS_JAVA");
  g_autofree char *bd = g_build_filename(ws, "builddir", NULL);
  g_mkdir_with_parents(bd, 0755);
  g_autofree char *cc = g_build_filename(bd, "compile_commands.json", NULL);
  g_file_set_contents(cc, "[]", -1, NULL);
  g_auto(GStrv) cv = ide_lsp_build_argv(ide_lsp_server_spec_for_file("a.c"), ws);
  gboolean has_dir = FALSE;
  for (guint i = 0; cv[i]; i++) {
    has_dir |= g_str_has_prefix(cv[i], "--compile-commands-dir=");
  }
  g_assert_true(has_dir);
}

/* ───────────── LSP: manager against the mock server ───────────── */

typedef struct {
  int           diag_calls;
  int           msg_calls;
  int           ui_calls;
  IdeLspUiState last_ui_state;
  char         *last_msg;
  char         *jump_path;
  guint         jump_line, jump_col;
} MgrProbe;

static void probe_diag(IdeBuffer *b, gpointer ud) { (void)b; ((MgrProbe *)ud)->diag_calls++; }
static void probe_msg(const char *m, gpointer ud) {
  MgrProbe *p = ud;
  p->msg_calls++;
  g_free(p->last_msg);
  p->last_msg = g_strdup(m);
}
static void probe_ui(IdeLspUiState s, gpointer ud) {
  MgrProbe *p = ud;
  p->ui_calls++;
  p->last_ui_state = s;
}
static void probe_status(const char *n, gboolean a, gpointer ud) { (void)n; (void)a; (void)ud; }
static void probe_jump(const char *path, guint l, guint c, gpointer ud) {
  MgrProbe *p = ud;
  g_free(p->jump_path);
  p->jump_path = g_strdup(path);
  p->jump_line = l;
  p->jump_col = c;
}

static gboolean probe_timeout(gpointer d) { *(gboolean *)d = TRUE; return G_SOURCE_REMOVE; }

#define SPIN_UNTIL(cond) \
  do { \
    gboolean to_ = FALSE; \
    guint id_ = g_timeout_add_seconds(10, probe_timeout, &to_); \
    while (!(cond) && !to_) g_main_context_iteration(NULL, TRUE); \
    if (!to_) g_source_remove(id_); \
    g_assert_true(cond); \
  } while (0)

static void test_lsp_manager_with_mock_server(void) {
  g_autofree char *script = g_build_filename(KAJO_SOURCE_DIR, "tests", "mock_lsp_server.py", NULL);
  g_autofree char *cmd = g_strdup_printf("python3 %s", script);
  g_setenv("KAJO_LSP_MOCK_CMD", cmd, TRUE);

  g_autofree char *ws = g_dir_make_tmp("kajo-lsp-mgr-XXXXXX", NULL);
  g_autofree char *path = g_build_filename(ws, "sample.kajo-mock", NULL);
  const char *src = "alpha := foo(1)\nfoo(2) // BUG\nvalue  \nTODO\n";
  g_file_set_contents(path, src, -1, NULL);

  MgrProbe probe = { 0 };
  IdeLspManager *lsp = ide_lsp_manager_new(ws, probe_jump, probe_status, &probe);
  ide_lsp_manager_set_callbacks(lsp, probe_diag, probe_msg, probe_ui, &probe);

  IdeBuffer *buf = ide_buffer_new(path);
  g_assert_true(ide_buffer_load_from_disk(buf, NULL));
  ide_lsp_manager_notify_open(lsp, buf);
  g_assert_true(ide_lsp_manager_has_server(lsp, buf));

  /* 1. diagnostics arrive from the server and land on the buffer */
  SPIN_UNTIL(probe.diag_calls > 0);
  const GPtrArray *diags = ide_buffer_get_diagnostics(buf);
  g_assert_cmpuint(diags->len, ==, 2);
  const IdeDiagnostic *d0 = g_ptr_array_index(diags, 0);
  g_assert_cmpuint(d0->line, ==, 2);
  g_assert_cmpuint(d0->col, ==, 11);       /* 1-based column of "BUG" */
  g_assert_cmpint(d0->severity, ==, IDE_DIAG_ERROR);
  g_assert_cmpstr(d0->source, ==, "mock");
  g_assert_nonnull(strstr(d0->message, "BUG"));
  const IdeDiagnostic *d1 = g_ptr_array_index(diags, 1);
  g_assert_cmpint(d1->severity, ==, IDE_DIAG_WARNING);
  g_assert_true(ide_lsp_manager_is_ready(lsp, buf));

  /* 2. server showMessage reaches the message callback */
  SPIN_UNTIL(probe.msg_calls > 0);
  g_assert_nonnull(strstr(probe.last_msg, "mock ready"));

  /* 3. edits invalidate diagnostics: removing the BUG marker clears the error */
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter s, e;
  gtk_text_buffer_get_iter_at_line_offset(tb, &s, 1, 10);
  gtk_text_buffer_get_iter_at_line_offset(tb, &e, 1, 13);
  int before = probe.diag_calls;
  gtk_text_buffer_delete(tb, &s, &e);
  ide_lsp_manager_notify_change(lsp, buf);
  SPIN_UNTIL(probe.diag_calls > before);
  g_assert_cmpuint(ide_buffer_get_diagnostics(buf)->len, ==, 1);

  /* 4. rename applies the server's WorkspaceEdit to the open buffer (undoable) */
  GtkTextIter at;
  gtk_text_buffer_get_iter_at_line_offset(tb, &at, 0, 10); /* inside "foo" */
  gtk_text_buffer_place_cursor(tb, &at);
  probe.msg_calls = 0;
  ide_lsp_manager_request_rename(lsp, buf, 1, 11, "bar");
  SPIN_UNTIL(probe.msg_calls > 0);
  g_autofree char *after = ide_buffer_get_text(buf);
  g_assert_true(g_str_has_prefix(after, "alpha := bar(1)\nbar(2) // \n"));
  g_assert_nonnull(strstr(probe.last_msg, "2 edits in 1 file"));

  /* 5. formatting removes trailing whitespace through the server's edits */
  probe.msg_calls = 0;
  ide_lsp_manager_request_format(lsp, buf);
  SPIN_UNTIL(probe.msg_calls > 0);
  g_autofree char *fmt = ide_buffer_get_text(buf);
  g_assert_nonnull(strstr(fmt, "value\nTODO"));
  g_assert_null(strstr(fmt, "value  "));

  /* 6. go to definition jumps to the server's location (UTF-16 -> char col) */
  ide_lsp_manager_request_definition(lsp, buf, 1, 1);
  SPIN_UNTIL(probe.jump_path != NULL);
  g_assert_cmpstr(probe.jump_path, ==, path);
  g_assert_cmpuint(probe.jump_line, ==, 3);
  g_assert_cmpuint(probe.jump_col, ==, 5);

  /* 7. references: opens IDE_LSP_UI_REFERENCES, select_next + confirm jumps to 2nd ref */
  g_clear_pointer(&probe.jump_path, g_free);
  probe.jump_line = 0;
  probe.jump_col = 0;
  ide_lsp_manager_request_references(lsp, NULL, buf, 1, 10);
  SPIN_UNTIL(probe.last_ui_state == IDE_LSP_UI_REFERENCES);
  ide_lsp_manager_select_next(lsp);
  ide_lsp_manager_confirm_selection(lsp, buf);
  g_assert_cmpint(probe.last_ui_state, ==, IDE_LSP_UI_CLOSED);
  g_assert_cmpstr(probe.jump_path, ==, path);
  g_assert_cmpuint(probe.jump_line, ==, 2);
  g_assert_cmpuint(probe.jump_col, ==, 1);

  /* 8. signature help: opens IDE_LSP_UI_SIGNATURE, cycles overloads, cancels cleanly */
  ide_lsp_manager_request_signature_help(lsp, NULL, buf, 1, 13);
  SPIN_UNTIL(probe.last_ui_state == IDE_LSP_UI_SIGNATURE);
  ide_lsp_manager_select_next(lsp);
  ide_lsp_manager_select_prev(lsp);
  g_assert_true(ide_lsp_manager_select_index(lsp, 1));
  g_assert_false(ide_lsp_manager_select_index(lsp, 5));
  ide_lsp_manager_cancel_popover(lsp);
  g_assert_cmpint(probe.last_ui_state, ==, IDE_LSP_UI_CLOSED);

  /* 9. call hierarchy: incoming -> toggle to outgoing -> confirm jumps to callee */
  g_clear_pointer(&probe.jump_path, g_free);
  probe.jump_line = 0;
  probe.jump_col = 0;
  gtk_text_buffer_get_iter_at_line_offset(tb, &at, 0, 9);
  gtk_text_buffer_place_cursor(tb, &at);
  ide_lsp_manager_request_call_hierarchy(lsp, NULL, buf, FALSE);
  SPIN_UNTIL(probe.last_ui_state == IDE_LSP_UI_CALL_HIERARCHY);
  int ui_before_toggle = probe.ui_calls;
  ide_lsp_manager_toggle_call_hierarchy_dir(lsp);
  SPIN_UNTIL(probe.ui_calls > ui_before_toggle && probe.last_ui_state == IDE_LSP_UI_CALL_HIERARCHY);
  ide_lsp_manager_confirm_selection(lsp, buf);
  g_assert_cmpint(probe.last_ui_state, ==, IDE_LSP_UI_CLOSED);
  g_assert_cmpstr(probe.jump_path, ==, path);
  g_assert_cmpuint(probe.jump_line, ==, 3);
  g_assert_cmpuint(probe.jump_col, ==, 1);

  /* 10. closing the buffer sends didClose and detaches it */
  ide_buffer_free(buf);
  ide_lsp_manager_free(lsp);

  g_free(probe.last_msg);
  g_free(probe.jump_path);
  g_unsetenv("KAJO_LSP_MOCK_CMD");
}

static void test_lsp_manager_without_server(void) {
  /* unknown extension: native mode, no server, requests fall back gracefully */
  MgrProbe probe = { 0 };
  g_autofree char *ws = g_dir_make_tmp("kajo-lsp-none-XXXXXX", NULL);
  g_autofree char *path = g_build_filename(ws, "notes.txt", NULL);
  g_file_set_contents(path, "hello world\n", -1, NULL);
  IdeLspManager *lsp = ide_lsp_manager_new(ws, probe_jump, probe_status, &probe);
  ide_lsp_manager_set_callbacks(lsp, probe_diag, probe_msg, probe_ui, &probe);
  IdeBuffer *buf = ide_buffer_new(path);
  ide_buffer_load_from_disk(buf, NULL);
  ide_lsp_manager_notify_open(lsp, buf);
  g_assert_false(ide_lsp_manager_has_server(lsp, buf));
  ide_lsp_manager_request_format(lsp, buf);
  g_assert_nonnull(probe.last_msg);
  g_assert_nonnull(strstr(probe.last_msg, "No language server"));
  ide_lsp_manager_request_references(lsp, NULL, buf, 1, 1);
  g_assert_nonnull(strstr(probe.last_msg, "Find references needs"));
  ide_lsp_manager_request_signature_help(lsp, NULL, buf, 1, 1);
  g_assert_nonnull(strstr(probe.last_msg, "Signature help needs"));
  ide_lsp_manager_request_call_hierarchy(lsp, NULL, buf, FALSE);
  g_assert_nonnull(strstr(probe.last_msg, "Call hierarchy needs"));
  /* Out-of-range selection (e.g. digit 5 with two code actions) must be rejected,
   * never clamped onto a different item. */
  g_assert_false(ide_lsp_manager_select_index(lsp, 0));
  g_assert_false(ide_lsp_manager_select_index(lsp, 4));
  g_assert_false(ide_lsp_manager_select_index(lsp, -1));
  /* buffers freed before the manager must not leave dangling notifications */
  ide_buffer_free(buf);
  ide_lsp_manager_free(lsp);
  g_free(probe.last_msg);
}

typedef struct {
  gboolean  done;
  JsonNode *result_copy;
  char     *error;
} RawProbe;

static void on_raw_probe(JsonNode *result, const char *error, gpointer ud) {
  RawProbe *p = ud;
  p->done = TRUE;
  g_clear_pointer(&p->result_copy, json_node_unref);
  g_clear_pointer(&p->error, g_free);
  if (result != NULL) {
    p->result_copy = json_node_copy(result);
  }
  if (error != NULL) {
    p->error = g_strdup(error);
  }
}

static void test_lsp_manager_request_raw(void) {
  g_autofree char *script = g_build_filename(KAJO_SOURCE_DIR, "tests", "mock_lsp_server.py", NULL);
  g_autofree char *cmd = g_strdup_printf("python3 %s", script);
  g_setenv("KAJO_LSP_MOCK_CMD", cmd, TRUE);

  g_autofree char *ws = g_dir_make_tmp("kajo-lsp-raw-XXXXXX", NULL);
  g_autofree char *path = g_build_filename(ws, "raw.kajo-mock", NULL);
  g_autofree char *txt_path = g_build_filename(ws, "plain.txt", NULL);
  const char *src = "alpha := foo(1)\nfoo(2)\nvalue\n";
  g_file_set_contents(path, src, -1, NULL);
  g_file_set_contents(txt_path, "no server\n", -1, NULL);

  MgrProbe probe = { 0 };
  IdeLspManager *lsp = ide_lsp_manager_new(ws, probe_jump, probe_status, &probe);
  ide_lsp_manager_set_callbacks(lsp, probe_diag, probe_msg, probe_ui, &probe);

  IdeBuffer *buf = ide_buffer_new(path);
  g_assert_true(ide_buffer_load_from_disk(buf, NULL));
  ide_lsp_manager_notify_open(lsp, buf);

  IdeBuffer *no_srv_buf = ide_buffer_new(txt_path);
  g_assert_true(ide_buffer_load_from_disk(no_srv_buf, NULL));
  ide_lsp_manager_notify_open(lsp, no_srv_buf);

  /* Wait until the mock server finishes initialize */
  SPIN_UNTIL(ide_lsp_manager_is_ready(lsp, buf));

  /* 1. No-server buffer returns FALSE without invoking cb and frees params cleanly */
  RawProbe rp_none = { 0 };
  g_autofree char *txt_uri = g_filename_to_uri(txt_path, NULL, NULL);
  g_assert_false(ide_lsp_manager_request_raw(
      lsp, no_srv_buf, "textDocument/references",
      ide_lsp_position_params(txt_uri, 0, 0), on_raw_probe, &rp_none));
  g_assert_false(rp_none.done);

  /* 2. Ready server answers textDocument/references via raw bridge */
  RawProbe rp_refs = { 0 };
  g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
  JsonNode *ref_params = ide_lsp_position_params(uri, 0, 10);
  JsonObject *cobj = json_object_new();
  json_object_set_boolean_member(cobj, "includeDeclaration", FALSE);
  json_object_set_object_member(json_node_get_object(ref_params), "context", cobj);
  g_assert_true(ide_lsp_manager_request_raw(
      lsp, buf, "textDocument/references", ref_params, on_raw_probe, &rp_refs));
  SPIN_UNTIL(rp_refs.done);
  g_assert_null(rp_refs.error);
  g_assert_nonnull(rp_refs.result_copy);
  g_autoptr(GPtrArray) locs = ide_lsp_parse_locations(rp_refs.result_copy);
  g_assert_cmpuint(locs->len, ==, 2);
  g_clear_pointer(&rp_refs.result_copy, json_node_unref);

  /* 3. Unknown method delivers server error cleanly */
  RawProbe rp_err = { 0 };
  g_assert_true(ide_lsp_manager_request_raw(
      lsp, buf, "custom/nonexistentMethod", NULL, on_raw_probe, &rp_err));
  SPIN_UNTIL(rp_err.done);
  g_assert_null(rp_err.result_copy);
  g_assert_nonnull(rp_err.error);
  g_assert_nonnull(strstr(rp_err.error, "method not found"));
  g_clear_pointer(&rp_err.error, g_free);

  ide_buffer_free(no_srv_buf);
  ide_buffer_free(buf);
  ide_lsp_manager_free(lsp);
  g_free(probe.last_msg);
  g_free(probe.jump_path);
  unlink(path);
  unlink(txt_path);
  rmdir(ws);
  g_unsetenv("KAJO_LSP_MOCK_CMD");
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide/ipc/jsonrpc-framing",
                  test_jsonrpc_framing_and_extraction);
  g_test_add_func("/ide/ipc/lsp-and-dap-detection",
                  test_lsp_and_dap_detection);
  g_test_add_func("/ide/ipc/agent-transactions-and-rollback",
                  test_agent_ipc_transactions_and_rollback);
  g_test_add_func("/ide/ipc/agent-unix-socket",
                  test_agent_unix_socket_server);
  g_test_add_func("/ide/ipc/agent-socket-roundtrip-and-stalled-client",
                  test_agent_socket_roundtrip_and_stalled_client);
  g_test_add_func("/ide/markdown/mermaid-vector-and-checklist",
                  test_mermaid_vector_parser_and_markdown_checklist);
  g_test_add_func("/ide/markdown/mermaid-extended-syntax",
                  test_mermaid_extended_syntax);
  g_test_add_func("/ide/markdown/rich-embeds-and-inline-pango",
                  test_markdown_rich_embeds_and_inline_pango);
  g_test_add_func("/ide/lsp/text-edits-snippets-hover", test_lsp_text_edits_and_snippets);
  g_test_add_func("/ide/lsp/references-signature-call-hierarchy-parsers",
                  test_lsp_references_signature_call_hierarchy_parsers);
  g_test_add_func("/ide/lsp/server-registry", test_lsp_server_registry);
  g_test_add_func("/ide/lsp/manager-mock-server", test_lsp_manager_with_mock_server);
  g_test_add_func("/ide/lsp/manager-without-server", test_lsp_manager_without_server);
  g_test_add_func("/ide/lsp/manager-request-raw", test_lsp_manager_request_raw);
  return g_test_run();
}
