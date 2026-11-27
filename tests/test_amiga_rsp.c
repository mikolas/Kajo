#include "../src/amiga/amiga_rsp.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void
test_protocol_framing_rle_and_parsers(void)
{
  /* Checksum & frame */
  g_autofree char *pkt = amiga_rsp_frame_packet("QStartNoAckMode");
  g_assert_cmpstr(pkt, ==, "$QStartNoAckMode#b0");

  /* Hex roundtrip */
  const uint8_t sample[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  g_autofree char *hex = amiga_rsp_hex_encode(sample, sizeof(sample));
  g_assert_cmpstr(hex, ==, "deadbeef");
  g_autofree uint8_t *dec = NULL;
  size_t dlen = 0;
  g_assert_true(amiga_rsp_hex_decode(hex, &dec, &dlen));
  g_assert_cmpuint(dlen, ==, sizeof(sample));
  g_assert_cmpint(memcmp(dec, sample, sizeof(sample)), ==, 0);

  /* RLE expansion: '0* ' repeats '0' 3 times (32 - 29 = 3) -> total 4 zeroes */
  g_autofree char *rle = amiga_rsp_expand_rle("0* ab", 5);
  g_assert_cmpstr(rle, ==, "0000ab");

  /* Regs g/G roundtrip */
  AmigaCpuRegs in = {0};
  for (int i = 0; i < 8; i++) {
    in.d[i] = 0x10000000u + (uint32_t)i;
    in.a[i] = 0x20000000u + (uint32_t)i;
  }
  in.sr = 0x00002704u;
  in.pc = 0x00020124u;
  g_autofree char *g_cmd = amiga_rsp_format_regs_G(&in);
  g_assert_nonnull(g_cmd);
  g_assert_cmpint(g_cmd[0], ==, 'G');

  AmigaCpuRegs out = {0};
  g_assert_true(amiga_rsp_parse_regs_g(g_cmd + 1, &out));
  g_assert_cmpuint(out.d[3], ==, 0x10000003u);
  g_assert_cmpuint(out.a[7], ==, 0x20000007u);
  g_assert_cmpuint(out.sr, ==, 0x00002704u);
  g_assert_cmpuint(out.pc, ==, 0x00020124u);

  /* Stop reply parsers */
  AmigaStopEvent ev = {0};
  g_assert_true(amiga_rsp_parse_stop_reply("T05winuae-entry:demo.exe;11:00020004;", &ev));
  g_assert_cmpint(ev.kind, ==, AMIGA_STOP_WINUAE_ENTRY);
  g_assert_cmpstr(ev.entry_proc, ==, "demo.exe");
  g_assert_true(ev.has_pc);
  g_assert_cmpuint(ev.pc, ==, 0x20004u);
  g_free(ev.entry_proc);
  g_free(ev.watch_kind);
  g_free(ev.raw);
  memset(&ev, 0, sizeof(ev));

  g_assert_true(amiga_rsp_parse_stop_reply("T05watch:dff058;11:00020010;", &ev));
  g_assert_cmpint(ev.kind, ==, AMIGA_STOP_WATCH);
  g_assert_cmpstr(ev.watch_kind, ==, "watch");
  g_assert_cmpuint(ev.watch_addr, ==, 0xDFF058u);
  g_free(ev.entry_proc);
  g_free(ev.watch_kind);
  g_free(ev.raw);

  /* execution-status cycles as string */
  AmigaExecStatus st = {0};
  g_assert_true(amiga_rsp_parse_execution_status(
      "{\"execution\":\"stopped\",\"debugger_stopped\":true,\"host_pause\":0,"
      "\"frame\":12,\"vpos\":140,\"hpos\":88,\"cycles\":\"987654321\"}", &st));
  g_assert_true(st.debugger_stopped);
  g_assert_cmpuint(st.frame, ==, 12);
  g_assert_cmpuint((guint)st.cycles, ==, 987654321u);

  /* 512B custom snapshot decoder */
  uint8_t custom[512];
  memset(custom, 0, sizeof(custom));
  custom[0x080] = 0x00; custom[0x081] = 0x02; custom[0x082] = 0x04; custom[0x083] = 0x00; /* COP1LC */
  custom[0x180] = 0x0F; custom[0x181] = 0xA0; /* COLOR00 = $FA0 */
  custom[0x1FA] = 0x80; custom[0x1FB] = 0x01; /* NTSC */
  AmigaCustomSnapshot snap = {0};
  amiga_rsp_decode_custom_snapshot(custom, &snap);
  g_assert_cmpuint(snap.cop1lc, ==, 0x00020400u);
  g_assert_cmpuint(snap.color[0], ==, 0x0FA0u);
  g_assert_true(snap.ntsc);
}

typedef struct {
  GAsyncResult *res;
  int           stops;
  AmigaStopEvent last_stop;
} SyncWaitCtx;

static void
on_async_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer ud)
{
  SyncWaitCtx *ctx = ud;
  ctx->res = g_object_ref(res);
}

static void
on_stop_event(AmigaRspClient *client G_GNUC_UNUSED,
              const AmigaStopEvent *ev,
              gpointer ud)
{
  SyncWaitCtx *ctx = ud;
  ctx->stops++;
  g_free(ctx->last_stop.entry_proc);
  g_free(ctx->last_stop.watch_kind);
  g_free(ctx->last_stop.raw);
  ctx->last_stop = *ev;
  ctx->last_stop.entry_proc = g_strdup(ev->entry_proc);
  ctx->last_stop.watch_kind = g_strdup(ev->watch_kind);
  ctx->last_stop.raw = g_strdup(ev->raw);
}

static GAsyncResult *
pump_until_done(SyncWaitCtx *ctx)
{
  while (ctx->res == NULL) {
    g_main_context_iteration(NULL, TRUE);
  }
  return g_steal_pointer(&ctx->res);
}

static void
test_async_mock_server_session(void)
{
  const char *src_root = g_getenv("KAJO_SOURCE_ROOT");
  if (!src_root) src_root = ".";
  g_autofree char *mock_py = g_build_filename(src_root, "tests", "mock_amiberry_rsp.py", NULL);
  g_assert_true(g_file_test(mock_py, G_FILE_TEST_EXISTS));

  g_autoptr(GError) err = NULL;
  const char *argv[] = { "python3", mock_py, NULL };
  g_autoptr(GSubprocessLauncher) launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_autoptr(GSubprocess) srv = g_subprocess_launcher_spawnv(launcher, argv, &err);
  g_assert_no_error(err);
  g_assert_nonnull(srv);

  g_autoptr(GDataInputStream) dis =
      g_data_input_stream_new(g_subprocess_get_stdout_pipe(srv));
  g_autofree char *line = g_data_input_stream_read_line_utf8(dis, NULL, NULL, &err);
  g_assert_no_error(err);
  g_assert_nonnull(line);
  g_assert_true(g_str_has_prefix(line, "PORT="));
  uint16_t port = (uint16_t)g_ascii_strtoull(line + 5, NULL, 10);
  g_assert_cmpuint(port, >, 0);

  SyncWaitCtx ctx = {0};
  g_autoptr(AmigaRspClient) cli = amiga_rsp_client_new();
  amiga_rsp_client_set_stop_callback(cli, on_stop_event, &ctx);

  /* Connect + QStartNoAckMode */
  amiga_rsp_client_connect_async(cli, "127.0.0.1", port, NULL, on_async_done, &ctx);
  g_autoptr(GAsyncResult) r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_connect_finish(cli, r, &err));
  g_assert_no_error(err);
  g_assert_cmpint(amiga_rsp_client_get_state(cli), ==, AMIGA_RSP_STOPPED);

  /* process-break + continue -> winuae-entry stop */
  amiga_rsp_client_rcmd_async(cli, "process-break name demo.exe", NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_autofree char *pb_rep = amiga_rsp_client_rcmd_finish(cli, r, &err);
  g_assert_cmpstr(pb_rep, ==, "OK");

  int stops_before = ctx.stops;
  amiga_rsp_client_continue_async(cli, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_continue_finish(cli, r, &err));
  while (ctx.stops == stops_before) {
    g_main_context_iteration(NULL, TRUE);
  }
  g_assert_cmpint(ctx.last_stop.kind, ==, AMIGA_STOP_WINUAE_ENTRY);
  g_assert_cmpuint(ctx.last_stop.pc, ==, 0x20004u);

  /* segments + tasks + memory-map */
  amiga_rsp_client_rcmd_async(cli, "segments", NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_autofree char *seg_json = amiga_rsp_client_rcmd_finish(cli, r, &err);
  AmigaSegmentList *segs = NULL;
  g_assert_true(amiga_rsp_parse_segments(seg_json, &segs));
  g_assert_cmpuint(segs->segments->len, ==, 2);
  amiga_segment_list_free(segs);

  amiga_rsp_client_rcmd_async(cli, "tasks", NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_autofree char *t_json = amiga_rsp_client_rcmd_finish(cli, r, &err);
  g_autoptr(GPtrArray) tasks = NULL;
  g_assert_true(amiga_rsp_parse_tasks(t_json, &tasks));
  g_assert_cmpuint(tasks->len, ==, 1);

  /* read_mem 512B $DFF000 snapshot */
  amiga_rsp_client_read_mem_async(cli, 0xDFF000u, 0x200u, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_autoptr(GBytes) custom_bytes = amiga_rsp_client_read_mem_finish(cli, r, &err);
  g_assert_no_error(err);
  g_assert_cmpuint(g_bytes_get_size(custom_bytes), ==, 512);
  AmigaCustomSnapshot snap = {0};
  amiga_rsp_decode_custom_snapshot(g_bytes_get_data(custom_bytes, NULL), &snap);
  g_assert_cmpuint(snap.cop1lc, ==, 0x00020100u);
  g_assert_cmpuint(snap.color[0], ==, 0x0F00u);

  /* Odd BP address rejected */
  amiga_rsp_client_bp_set_async(cli, 0x20005u, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_false(amiga_rsp_client_bp_set_finish(cli, r, &err));
  g_clear_error(&err);

  /* Even BP succeeds */
  amiga_rsp_client_bp_set_async(cli, 0x20010u, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_bp_set_finish(cli, r, &err));

  /* Two-stage step-over */
  stops_before = ctx.stops;
  amiga_rsp_client_step_over_async(cli, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_step_over_finish(cli, r, &err));
  while (ctx.stops == stops_before) {
    g_main_context_iteration(NULL, TRUE);
  }
  g_assert_cmpint(amiga_rsp_client_get_state(cli), ==, AMIGA_RSP_STOPPED);

  /* Range step vCont;r */
  stops_before = ctx.stops;
  amiga_rsp_client_step_range_async(cli, 0x20004u, 0x20010u, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_step_range_finish(cli, r, &err));
  while (ctx.stops == stops_before) {
    g_main_context_iteration(NULL, TRUE);
  }

  /* While RUNNING: halted rcmd fails E16/BUSY, execution-status succeeds, interrupt_async stops */
  amiga_rsp_client_continue_async(cli, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_continue_finish(cli, r, &err));
  g_assert_cmpint(amiga_rsp_client_get_state(cli), ==, AMIGA_RSP_RUNNING);

  amiga_rsp_client_rcmd_async(cli, "segments", NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_null(amiga_rsp_client_rcmd_finish(cli, r, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&err);

  amiga_rsp_client_rcmd_async(cli, "execution-status", NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_autofree char *es_json = amiga_rsp_client_rcmd_finish(cli, r, &err);
  g_assert_nonnull(es_json);
  AmigaExecStatus st = {0};
  g_assert_true(amiga_rsp_parse_execution_status(es_json, &st));
  g_assert_false(st.debugger_stopped);

  amiga_rsp_client_interrupt_async(cli, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_interrupt_finish(cli, r, &err));
  g_assert_cmpint(amiga_rsp_client_get_state(cli), ==, AMIGA_RSP_STOPPED);

  /* watch events + guest-output + detach */
  amiga_rsp_client_rcmd_async(cli, "watch events", NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_autofree char *w_json = amiga_rsp_client_rcmd_finish(cli, r, &err);
  g_autoptr(GPtrArray) wevs = NULL;
  g_assert_true(amiga_rsp_parse_watch_events(w_json, &wevs));
  g_assert_cmpuint(wevs->len, ==, 1);

  amiga_rsp_client_rcmd_async(cli, "guest-output read", NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_autofree char *g_json = amiga_rsp_client_rcmd_finish(cli, r, &err);
  g_autoptr(GPtrArray) glines = NULL;
  g_assert_true(amiga_rsp_parse_guest_output(g_json, &glines));
  g_assert_cmpuint(glines->len, ==, 1);

  amiga_rsp_client_detach_async(cli, NULL, on_async_done, &ctx);
  g_clear_object(&r); r = pump_until_done(&ctx);
  g_assert_true(amiga_rsp_client_detach_finish(cli, r, &err));
  g_assert_cmpint(amiga_rsp_client_get_state(cli), ==, AMIGA_RSP_DISCONNECTED);

  g_free(ctx.last_stop.entry_proc);
  g_free(ctx.last_stop.watch_kind);
  g_free(ctx.last_stop.raw);
  g_subprocess_wait_check(srv, NULL, NULL);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/rsp/framing-rle-and-parsers", test_protocol_framing_rle_and_parsers);
  g_test_add_func("/amiga/rsp/async-mock-server-session", test_async_mock_server_session);
  return g_test_run();
}
