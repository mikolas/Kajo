#include "../src/ide/ide_agent_ipc.h"
#include "../src/ide/ide_dap.h"
#include "../src/ide/ide_lsp.h"
#include "../src/ide/ide_markdown.h"
#include <gio/gunixsocketaddress.h>
#include <glib.h>
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
  g_assert_cmpstr(ide_dap_detect_adapter_for_file("/src/main.py"),
                  ==,
                  "python3 -m debugpy.adapter");
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
  g_test_add_func("/ide/markdown/mermaid-vector-and-checklist",
                  test_mermaid_vector_parser_and_markdown_checklist);
  return g_test_run();
}
