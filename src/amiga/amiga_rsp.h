#ifndef KAJO_AMIGA_RSP_H
#define KAJO_AMIGA_RSP_H

/* Pure GTK-free C17 async GDB Remote Serial Protocol (RSP) client for Amiberry >= 8.5.0:
 * - Single-socket TCP transport over GSocketClient (proxy disabled for loopback safety)
 * - QStartNoAckMode, g/G, m/M (including 512B $DFF000..$DFF1FF shadow custom bank),
 *   Z0/z0 (len=2 even), Z2-Z4/z2-z4, vCont;c/s/r, two-stage qRcmd,step-over, 0x03 break, D detach
 * - qRcmd single-packet $<hex>#cs framing + parsers for capabilities, execution-status,
 *   tasks, segments, process-break, disasm, watch events, history, guest-output, memory-map */

#include <gio/gio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_RSP_CLIENT (amiga_rsp_client_get_type())
G_DECLARE_FINAL_TYPE(AmigaRspClient, amiga_rsp_client, AMIGA, RSP_CLIENT, GObject)

typedef enum {
  AMIGA_RSP_DISCONNECTED = 0,
  AMIGA_RSP_CONNECTING,
  AMIGA_RSP_STOPPED,
  AMIGA_RSP_RUNNING,
  AMIGA_RSP_DETACHING,
} AmigaRspState;

typedef struct {
  uint32_t d[8];
  uint32_t a[8];
  uint32_t sr;
  uint32_t pc;
} AmigaCpuRegs;

typedef enum {
  AMIGA_STOP_SIGNAL = 0,
  AMIGA_STOP_WINUAE_ENTRY,
  AMIGA_STOP_WATCH,
  AMIGA_STOP_EXIT,
} AmigaStopKind;

typedef struct {
  AmigaStopKind kind;
  uint8_t       signal_num;
  bool          has_pc;
  uint32_t      pc;
  char         *entry_proc;
  char         *watch_kind; /* "watch" | "rwatch" | "awatch" */
  uint32_t      watch_addr;
  int           exit_code;
  char         *raw;
} AmigaStopEvent;

typedef struct {
  char     execution[24]; /* "running" | "stopped" | "host-paused" */
  bool     debugger_stopped;
  int      host_pause;
  uint32_t frame;
  uint32_t vpos;
  uint32_t hpos;
  uint64_t cycles;
} AmigaExecStatus;

typedef struct {
  uint32_t index;
  uint32_t address;
  uint32_t size;
} AmigaSegmentInfo;

typedef struct {
  uint32_t   process;
  char      *name;
  char      *command;
  GPtrArray *segments; /* AmigaSegmentInfo* */
} AmigaSegmentList;

typedef struct {
  uint32_t address;
  char    *name;
  uint32_t type;
  uint32_t state;
  char    *list_name; /* "current" | "ready" | "waiting" */
  uint32_t signals_wait;
  uint32_t saved_sp;
  uint32_t stack_lower;
  uint32_t stack_upper;
} AmigaTaskInfo;

typedef struct {
  uint32_t pc;
  uint32_t addr;
  uint32_t size;
  uint32_t rwi;
  uint32_t src;
  uint32_t old_val;
  uint32_t new_val;
  uint32_t frame;
  uint32_t vpos;
  uint32_t hpos;
} AmigaWatchEvent;

typedef struct {
  uint32_t pc;
  uint32_t frame;
  uint32_t vpos;
  uint32_t hpos;
  uint32_t regs[16]; /* D0..D7, A0..A7 */
} AmigaHistoryEntry;

typedef struct {
  char    *type;
  uint32_t start;
  uint32_t length;
  char    *name;
} AmigaMemRegion;

typedef struct {
  uint8_t  raw[512];
  uint16_t dmaconr;
  uint16_t vposr;
  uint16_t vhposr;
  uint16_t clxdat;
  uint16_t adkconr;
  uint16_t intenar;
  uint16_t intreqr;
  uint16_t copcon;
  uint32_t cop1lc;
  uint32_t cop2lc;
  uint16_t bltcon0;
  uint16_t bltcon1;
  uint16_t bltafwm;
  uint16_t bltalwm;
  uint32_t bltpt[4];   /* 0=C, 1=B, 2=A, 3=D */
  int16_t  bltmod[4];  /* 0=C, 1=B, 2=A, 3=D */
  uint16_t bltsize;
  uint16_t diwstrt;
  uint16_t diwstop;
  uint16_t ddfstrt;
  uint16_t ddfstop;
  uint16_t dmacon;
  uint16_t intena;
  uint16_t intreq;
  uint32_t bplpt[8];
  uint16_t bplcon[5];  /* BPLCON0..BPLCON4 */
  int16_t  bpl1mod;
  int16_t  bpl2mod;
  uint32_t sprpt[8];
  uint16_t color[32];  /* 12-bit OCS/ECS or AGA high nibbles */
  uint16_t fmode;
  bool     ntsc;
} AmigaCustomSnapshot;

typedef void (*AmigaRspStopCb)(AmigaRspClient       *client,
                               const AmigaStopEvent *ev,
                               gpointer              user_data);

void amiga_stop_event_free(AmigaStopEvent *ev);
void amiga_segment_list_free(AmigaSegmentList *sl);
void amiga_task_info_free(AmigaTaskInfo *ti);
void amiga_mem_region_free(AmigaMemRegion *mr);

/* ─── Pure Protocol & Parser Helpers ─── */
uint8_t amiga_rsp_checksum(const char *payload, size_t len);
char   *amiga_rsp_frame_packet(const char *payload);
char   *amiga_rsp_hex_encode(const uint8_t *data, size_t len);
bool    amiga_rsp_hex_decode(const char *hex, uint8_t **out_bytes, size_t *out_len);
char   *amiga_rsp_expand_rle(const char *payload, size_t len);

bool    amiga_rsp_parse_regs_g(const char *hex_reply, AmigaCpuRegs *out_regs);
char   *amiga_rsp_format_regs_G(const AmigaCpuRegs *regs);
bool    amiga_rsp_parse_stop_reply(const char *pkt, AmigaStopEvent *out_stop);

bool    amiga_rsp_parse_execution_status(const char *json_text, AmigaExecStatus *out_st);
bool    amiga_rsp_parse_segments(const char *json_text, AmigaSegmentList **out_list);
bool    amiga_rsp_parse_tasks(const char *json_text, GPtrArray **out_tasks);
bool    amiga_rsp_parse_guest_output(const char *json_text, GPtrArray **out_lines);
bool    amiga_rsp_parse_watch_events(const char *json_text, GPtrArray **out_events);
bool    amiga_rsp_parse_history(const char *json_text, GPtrArray **out_entries);
bool    amiga_rsp_parse_memory_map(const char *text, GPtrArray **out_regions);
void    amiga_rsp_decode_custom_snapshot(const uint8_t buf[512], AmigaCustomSnapshot *out);

/* ─── Async TCP RSP Client ─── */
AmigaRspClient *amiga_rsp_client_new(void);
AmigaRspState   amiga_rsp_client_get_state(AmigaRspClient *client);
void            amiga_rsp_client_set_stop_callback(AmigaRspClient *client,
                                                   AmigaRspStopCb  cb,
                                                   gpointer        user_data);

void   amiga_rsp_client_connect_async(AmigaRspClient      *client,
                                      const char          *host,
                                      uint16_t             port,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
bool   amiga_rsp_client_connect_finish(AmigaRspClient *client,
                                       GAsyncResult   *result,
                                       GError        **error);

void   amiga_rsp_client_raw_command_async(AmigaRspClient      *client,
                                          const char          *payload,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data);
char  *amiga_rsp_client_raw_command_finish(AmigaRspClient *client,
                                           GAsyncResult   *result,
                                           GError        **error);

void   amiga_rsp_client_rcmd_async(AmigaRspClient      *client,
                                   const char          *cmd_utf8,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
char  *amiga_rsp_client_rcmd_finish(AmigaRspClient *client,
                                    GAsyncResult   *result,
                                    GError        **error);

void   amiga_rsp_client_read_regs_async(AmigaRspClient      *client,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
bool   amiga_rsp_client_read_regs_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         AmigaCpuRegs   *out_regs,
                                         GError        **error);

void   amiga_rsp_client_write_regs_async(AmigaRspClient      *client,
                                         const AmigaCpuRegs  *regs,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data);
bool   amiga_rsp_client_write_regs_finish(AmigaRspClient *client,
                                          GAsyncResult   *result,
                                          GError        **error);

void    amiga_rsp_client_read_mem_async(AmigaRspClient      *client,
                                        uint32_t             addr,
                                        size_t               len,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
GBytes *amiga_rsp_client_read_mem_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         GError        **error);

void   amiga_rsp_client_write_mem_async(AmigaRspClient      *client,
                                        uint32_t             addr,
                                        const uint8_t       *bytes,
                                        size_t               len,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
bool   amiga_rsp_client_write_mem_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         GError        **error);

void   amiga_rsp_client_bp_set_async(AmigaRspClient      *client,
                                     uint32_t             addr,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);
bool   amiga_rsp_client_bp_set_finish(AmigaRspClient *client,
                                      GAsyncResult   *result,
                                      GError        **error);

void   amiga_rsp_client_bp_remove_async(AmigaRspClient      *client,
                                        uint32_t             addr,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
bool   amiga_rsp_client_bp_remove_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         GError        **error);

void   amiga_rsp_client_watch_set_async(AmigaRspClient      *client,
                                        uint8_t              z_type, /* 2=write,3=read,4=access */
                                        uint32_t             addr,
                                        uint32_t             len,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
bool   amiga_rsp_client_watch_set_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         GError        **error);

void   amiga_rsp_client_watch_remove_async(AmigaRspClient      *client,
                                           uint8_t              z_type,
                                           uint32_t             addr,
                                           uint32_t             len,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
bool   amiga_rsp_client_watch_remove_finish(AmigaRspClient *client,
                                            GAsyncResult   *result,
                                            GError        **error);

void   amiga_rsp_client_continue_async(AmigaRspClient      *client,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);
bool   amiga_rsp_client_continue_finish(AmigaRspClient *client,
                                        GAsyncResult   *result,
                                        GError        **error);

void   amiga_rsp_client_step_into_async(AmigaRspClient      *client,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
bool   amiga_rsp_client_step_into_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         GError        **error);

void   amiga_rsp_client_step_range_async(AmigaRspClient      *client,
                                         uint32_t             start_addr,
                                         uint32_t             end_addr,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data);
bool   amiga_rsp_client_step_range_finish(AmigaRspClient *client,
                                          GAsyncResult   *result,
                                          GError        **error);

void   amiga_rsp_client_step_over_async(AmigaRspClient      *client,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
bool   amiga_rsp_client_step_over_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         GError        **error);

void   amiga_rsp_client_interrupt_async(AmigaRspClient      *client,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
bool   amiga_rsp_client_interrupt_finish(AmigaRspClient *client,
                                         GAsyncResult   *result,
                                         GError        **error);

void   amiga_rsp_client_detach_async(AmigaRspClient      *client,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);
bool   amiga_rsp_client_detach_finish(AmigaRspClient *client,
                                      GAsyncResult   *result,
                                      GError        **error);

G_END_DECLS

#endif /* KAJO_AMIGA_RSP_H */
