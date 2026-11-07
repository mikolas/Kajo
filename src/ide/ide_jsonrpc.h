#ifndef KAJO_IDE_JSONRPC_H
#define KAJO_IDE_JSONRPC_H

/* Content-Length message framing shared by the LSP and DAP clients. */

#include <glib.h>

G_BEGIN_DECLS

/* "Content-Length: N\r\n\r\n<body>" */
char *ide_jsonrpc_frame_message(const char *json_body);

/* Extracts the first complete framed body from the stream, or NULL when more
 * bytes are needed. *out_consumed_bytes receives the bytes to drop. */
char *ide_jsonrpc_extract_body(const char *stream_buf,
                               gsize       stream_len,
                               gsize      *out_consumed_bytes);

G_END_DECLS

#endif
