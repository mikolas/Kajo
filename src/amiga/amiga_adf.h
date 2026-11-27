#ifndef KAJO_AMIGA_ADF_H
#define KAJO_AMIGA_ADF_H

/* In-process Amiga .adf disk image builder and inspector using ADFlib (OFS/FFS)
 * plus raw 901,120-byte bootblock + trackdisk image writer (adf_raw). */

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct {
  const char *guest_path; /* e.g. "s/startup-sequence" or "demo.exe" */
  const char *host_path;  /* XOR with memory bytes */
  const uint8_t *bytes;
  size_t      bytes_len;
} AmigaAdfFileEntry;

typedef struct {
  const char               *adf_path;
  const char               *volume_name;
  bool                      ffs;        /* false = OFS (Kick 1.3 boot safe), true = FFS */
  bool                      intl;
  bool                      hd;         /* false = 880KB DD, true = 1760KB HD */
  bool                      bootable;   /* installs standard DOS bootblock if bootblock == NULL */
  const uint8_t            *bootblock;  /* optional 1024-byte custom bootblock */
  const AmigaAdfFileEntry  *files;
  size_t                    n_files;
} AmigaAdfDosSpec;

const char *amiga_adf_lib_version(void);

bool amiga_adf_create_dos(const AmigaAdfDosSpec *spec, GError **error);
bool amiga_adf_list_files(const char  *adf_path,
                          GPtrArray  **out_paths,
                          GError     **error);

/* Writes a raw DD 901,120-byte ADF with 68000 bootblock checksum at offset 4. */
bool amiga_adf_create_raw(const char    *adf_path,
                          const uint8_t *bootblock,
                          size_t         boot_len,
                          const uint8_t *payload,
                          size_t         payload_len,
                          GError       **error);

uint32_t amiga_adf_bootblock_checksum(const uint8_t block[1024]);

G_END_DECLS

#endif /* KAJO_AMIGA_ADF_H */
