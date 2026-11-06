#include "ide_jsonrpc.h"

#include <string.h>

char *
ide_jsonrpc_frame_message(const char *json_body)
{
  if (json_body == NULL) {
    json_body = "{}";
  }
  gsize len = strlen(json_body);
  return g_strdup_printf("Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n%s", len, json_body);
}

char *
ide_jsonrpc_extract_body(const char *stream_buf, gsize stream_len, gsize *out_consumed_bytes)
{
  if (out_consumed_bytes != NULL) {
    *out_consumed_bytes = 0;
  }
  if (stream_buf == NULL || stream_len == 0) {
    return NULL;
  }
  const char *hdr_end = g_strstr_len(stream_buf, (gssize)stream_len, "\r\n\r\n");
  if (hdr_end == NULL) {
    return NULL;
  }
  gsize hdr_len = (gsize)(hdr_end - stream_buf);
  g_autofree char *hdr_str = g_strndup(stream_buf, hdr_len);
  const char *cl = g_strstr_len(hdr_str, (gssize)hdr_len, "Content-Length:");
  if (cl == NULL) {
    cl = g_strstr_len(hdr_str, (gssize)hdr_len, "content-length:");
  }
  if (cl == NULL) {
    return NULL;
  }
  cl += strlen("Content-Length:");
  while (*cl == ' ' || *cl == '\t') {
    cl++;
  }
  gint64 body_len = g_ascii_strtoll(cl, NULL, 10);
  if (body_len < 0) {
    return NULL;
  }
  gsize total = hdr_len + 4 + (gsize)body_len;
  if (stream_len < total) {
    return NULL;
  }
  if (out_consumed_bytes != NULL) {
    *out_consumed_bytes = total;
  }
  return g_strndup(hdr_end + 4, (gsize)body_len);
}
