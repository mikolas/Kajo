#include "amiga_symbols.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_ELFUTILS
#include <dwarf.h>
#include <elfutils/libdw.h>
#include <gelf.h>
#include <libelf.h>
#endif

struct _AmigaSymbolTable {
  GPtrArray *symbols; /* AmigaSymbol* */
  GPtrArray *lines;   /* AmigaLineEntry* */
  GArray    *seg_bases; /* uint32_t indexed by hunk_index */
};

static void
amiga_symbol_free(AmigaSymbol *s)
{
  if (!s) return;
  g_free(s->name);
  g_free(s);
}

static void
amiga_line_entry_free(AmigaLineEntry *l)
{
  if (!l) return;
  g_free(l->file);
  g_free(l);
}

void
amiga_outline_item_free(AmigaOutlineItem *it)
{
  if (!it) return;
  g_free(it->name);
  g_free(it);
}

AmigaSymbolTable *
amiga_symbols_new(void)
{
  AmigaSymbolTable *tbl = g_new0(AmigaSymbolTable, 1);
  tbl->symbols = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_symbol_free);
  tbl->lines = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_line_entry_free);
  tbl->seg_bases = g_array_new(FALSE, TRUE, sizeof(uint32_t));
  return tbl;
}

void
amiga_symbols_free(AmigaSymbolTable *tbl)
{
  if (!tbl) return;
  if (tbl->symbols) g_ptr_array_unref(tbl->symbols);
  if (tbl->lines) g_ptr_array_unref(tbl->lines);
  if (tbl->seg_bases) g_array_unref(tbl->seg_bases);
  g_free(tbl);
}

void
amiga_symbols_add_symbol(AmigaSymbolTable *tbl,
                         const char       *name,
                         uint32_t          hunk_index,
                         uint32_t          offset,
                         uint32_t          size)
{
  if (!tbl || !name || !name[0]) return;
  AmigaSymbol *s = g_new0(AmigaSymbol, 1);
  s->name = g_strdup(name);
  s->hunk_index = hunk_index;
  s->offset = offset;
  s->size = size;
  g_ptr_array_add(tbl->symbols, s);
}

void
amiga_symbols_add_line(AmigaSymbolTable *tbl,
                       const char       *file,
                       uint32_t          line,
                       uint32_t          hunk_index,
                       uint32_t          offset)
{
  if (!tbl || !file || !file[0] || line == 0) return;
  AmigaLineEntry *l = g_new0(AmigaLineEntry, 1);
  l->file = g_strdup(file);
  l->line = line;
  l->hunk_index = hunk_index;
  l->offset = offset;
  l->end_offset = offset + 2u;
  g_ptr_array_add(tbl->lines, l);
}

static int
cmp_line_by_hunk_offset(gconstpointer a, gconstpointer b)
{
  const AmigaLineEntry *la = *(const AmigaLineEntry *const *)a;
  const AmigaLineEntry *lb = *(const AmigaLineEntry *const *)b;
  if (la->hunk_index != lb->hunk_index) {
    return (la->hunk_index < lb->hunk_index) ? -1 : 1;
  }
  if (la->offset != lb->offset) {
    return (la->offset < lb->offset) ? -1 : 1;
  }
  return (la->line < lb->line) ? -1 : (la->line > lb->line ? 1 : 0);
}

void
amiga_symbols_finalize_line_ranges(AmigaSymbolTable *tbl)
{
  if (!tbl || !tbl->lines || tbl->lines->len == 0) return;
  g_ptr_array_sort(tbl->lines, cmp_line_by_hunk_offset);
  for (guint i = 0; i < tbl->lines->len; i++) {
    AmigaLineEntry *cur = g_ptr_array_index(tbl->lines, i);
    if (i + 1 < tbl->lines->len) {
      const AmigaLineEntry *nxt = g_ptr_array_index(tbl->lines, i + 1);
      if (nxt->hunk_index == cur->hunk_index && nxt->offset > cur->offset) {
        cur->end_offset = nxt->offset;
        continue;
      }
    }
    if (cur->end_offset <= cur->offset) {
      cur->end_offset = cur->offset + 4u;
    }
  }
}

static uint32_t
segment_base_for_hunk(const AmigaSymbolTable *tbl, uint32_t hunk_index)
{
  if (!tbl || !tbl->seg_bases || hunk_index >= tbl->seg_bases->len) {
    return 0;
  }
  return g_array_index(tbl->seg_bases, uint32_t, hunk_index);
}

bool
amiga_symbols_load_elf(AmigaSymbolTable *tbl,
                       const char       *elf_path,
                       GError          **error)
{
  if (!tbl || !elf_path || !elf_path[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid ELF args");
    return false;
  }
#ifdef HAVE_ELFUTILS
  if (elf_version(EV_CURRENT) == EV_NONE) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "libelf version init failed");
    return false;
  }
  int fd = open(elf_path, O_RDONLY);
  if (fd < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Cannot open ELF '%s': %s", elf_path, g_strerror(errno));
    return false;
  }
  Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
  if (!elf || elf_kind(elf) != ELF_K_ELF) {
    if (elf) elf_end(elf);
    close(fd);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "'%s' is not a valid ELF file", elf_path);
    return false;
  }

  /* 1. Read .symtab symbols */
  Elf_Scn *scn = NULL;
  while ((scn = elf_nextscn(elf, scn)) != NULL) {
    GElf_Shdr shdr;
    if (!gelf_getshdr(scn, &shdr) || shdr.sh_type != SHT_SYMTAB) {
      continue;
    }
    Elf_Data *data = elf_getdata(scn, NULL);
    if (!data || shdr.sh_entsize == 0) continue;
    size_t count = shdr.sh_size / shdr.sh_entsize;
    for (size_t i = 0; i < count; i++) {
      GElf_Sym sym;
      if (!gelf_getsym(data, (int)i, &sym)) continue;
      unsigned char st_type = GELF_ST_TYPE(sym.st_info);
      if (st_type == STT_SECTION || st_type == STT_FILE) continue;
      const char *name = elf_strptr(elf, shdr.sh_link, sym.st_name);
      if (name && name[0]) {
        amiga_symbols_add_symbol(tbl, name, 0, (uint32_t)sym.st_value, (uint32_t)sym.st_size);
      }
    }
  }

  /* 2. Read DWARF .debug_line ranges */
  Dwarf *dw = dwarf_begin_elf(elf, DWARF_C_READ, NULL);
  if (dw != NULL) {
    Dwarf_Off off = 0;
    Dwarf_Off next_off = 0;
    size_t hdr_size = 0;
    while (dwarf_nextcu(dw, off, &next_off, &hdr_size, NULL, NULL, NULL) == 0) {
      Dwarf_Die cu_die;
      if (dwarf_offdie(dw, off + hdr_size, &cu_die) != NULL) {
        Dwarf_Lines *lines = NULL;
        size_t nlines = 0;
        if (dwarf_getsrclines(&cu_die, &lines, &nlines) == 0 && lines != NULL) {
          for (size_t i = 0; i < nlines; i++) {
            Dwarf_Line *ln = dwarf_onesrcline(lines, i);
            Dwarf_Addr addr = 0;
            int lineno = 0;
            bool end_seq = false;
            if (!ln) continue;
            if (dwarf_lineaddr(ln, &addr) != 0) continue;
            (void)dwarf_lineendsequence(ln, &end_seq);
            if (end_seq) {
              if (tbl->lines->len > 0) {
                AmigaLineEntry *prev = g_ptr_array_index(tbl->lines, tbl->lines->len - 1);
                if ((uint32_t)addr > prev->offset) {
                  prev->end_offset = (uint32_t)addr;
                }
              }
              continue;
            }
            if (dwarf_lineno(ln, &lineno) != 0 || lineno <= 0) continue;
            const char *src = dwarf_linesrc(ln, NULL, NULL);
            if (src && src[0]) {
              amiga_symbols_add_line(tbl, src, (uint32_t)lineno, 0, (uint32_t)addr);
            }
          }
        }
      }
      off = next_off;
    }
    dwarf_end(dw);
  }

  amiga_symbols_finalize_line_ranges(tbl);
  elf_end(elf);
  close(fd);
  return true;
#else
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
              "elfutils (libelf/libdw) not enabled in build");
  return false;
#endif
}

static bool
read_be32_at(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out_val)
{
  if (!buf || !pos || !out_val || *pos + 4 > len) {
    return false;
  }
  const uint8_t *p = buf + *pos;
  *out_val = ((uint32_t)p[0] << 24) |
             ((uint32_t)p[1] << 16) |
             ((uint32_t)p[2] << 8)  |
             (uint32_t)p[3];
  *pos += 4;
  return true;
}

bool
amiga_symbols_load_hunk(AmigaSymbolTable *tbl,
                        const char       *exe_path,
                        GError          **error)
{
  if (!tbl || !exe_path || !exe_path[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Hunk args");
    return false;
  }
  g_autofree char *raw = NULL;
  gsize len = 0;
  if (!g_file_get_contents(exe_path, &raw, &len, error)) {
    return false;
  }
  const uint8_t *buf = (const uint8_t *)raw;
  size_t pos = 0;

  uint32_t magic = 0;
  if (!read_be32_at(buf, len, &pos, &magic) || magic != 0x000003F3u) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Missing HUNK_HEADER ($000003F3) in '%s'", exe_path);
    return false;
  }

  /* Skip resident library name table until zero-length terminator */
  for (;;) {
    uint32_t nlongs = 0;
    if (!read_be32_at(buf, len, &pos, &nlongs)) goto truncated;
    if (nlongs == 0) break;
    size_t skip = (size_t)nlongs * 4u;
    if (pos + skip > len) goto truncated;
    pos += skip;
  }

  uint32_t table_size = 0, first_hunk = 0, last_hunk = 0;
  if (!read_be32_at(buf, len, &pos, &table_size) ||
      !read_be32_at(buf, len, &pos, &first_hunk) ||
      !read_be32_at(buf, len, &pos, &last_hunk) ||
      last_hunk < first_hunk) {
    goto truncated;
  }

  uint32_t nhunks = last_hunk - first_hunk + 1u;
  for (uint32_t i = 0; i < nhunks; i++) {
    uint32_t sz = 0;
    if (!read_be32_at(buf, len, &pos, &sz)) goto truncated;
    if ((sz >> 30) == 3u) {
      uint32_t ext = 0;
      if (!read_be32_at(buf, len, &pos, &ext)) goto truncated;
    }
  }

  uint32_t cur_hunk = first_hunk;
  while (pos < len) {
    uint32_t htype_raw = 0;
    if (!read_be32_at(buf, len, &pos, &htype_raw)) goto truncated;
    uint32_t htype = htype_raw & 0x0000FFFFu;

    if (htype == 0x03E9u || htype == 0x03EAu) { /* HUNK_CODE / HUNK_DATA */
      uint32_t nlongs = 0;
      if (!read_be32_at(buf, len, &pos, &nlongs)) goto truncated;
      size_t bytes = (size_t)nlongs * 4u;
      if (pos + bytes > len) goto truncated;
      pos += bytes;
    } else if (htype == 0x03EBu) { /* HUNK_BSS */
      uint32_t sz = 0;
      if (!read_be32_at(buf, len, &pos, &sz)) goto truncated;
    } else if (htype == 0x03ECu || htype == 0x03F7u) { /* HUNK_RELOC32 / HUNK_DREL32 */
      for (;;) {
        uint32_t count = 0;
        if (!read_be32_at(buf, len, &pos, &count)) goto truncated;
        if (count == 0) break;
        uint32_t target_hunk = 0;
        if (!read_be32_at(buf, len, &pos, &target_hunk)) goto truncated;
        size_t bytes = (size_t)count * 4u;
        if (pos + bytes > len) goto truncated;
        pos += bytes;
      }
    } else if (htype == 0x03F0u) { /* HUNK_SYMBOL */
      for (;;) {
        uint32_t nlongs = 0;
        if (!read_be32_at(buf, len, &pos, &nlongs)) goto truncated;
        if (nlongs == 0) break;
        size_t nbytes = (size_t)nlongs * 4u;
        if (pos + nbytes + 4u > len) goto truncated;
        g_autofree char *sym = g_strndup((const char *)(buf + pos), nbytes);
        pos += nbytes;
        uint32_t val = 0;
        if (!read_be32_at(buf, len, &pos, &val)) goto truncated;
        amiga_symbols_add_symbol(tbl, sym, cur_hunk, val, 0);
      }
    } else if (htype == 0x03F1u) { /* HUNK_DEBUG */
      uint32_t nlongs = 0;
      if (!read_be32_at(buf, len, &pos, &nlongs)) goto truncated;
      size_t dbytes = (size_t)nlongs * 4u;
      if (pos + dbytes > len) goto truncated;
      size_t end = pos + dbytes;
      if (nlongs >= 3) {
        size_t dp = pos;
        uint32_t base_off = 0, tag = 0, fn_longs = 0;
        if (read_be32_at(buf, end, &dp, &base_off) &&
            read_be32_at(buf, end, &dp, &tag) &&
            tag == 0x4C494E45u /* "LINE" */ &&
            read_be32_at(buf, end, &dp, &fn_longs)) {
          size_t fn_bytes = (size_t)fn_longs * 4u;
          if (dp + fn_bytes <= end) {
            g_autofree char *fname = g_strndup((const char *)(buf + dp), fn_bytes);
            dp += fn_bytes;
            while (dp + 8 <= end) {
              uint32_t ln = 0, loff = 0;
              if (!read_be32_at(buf, end, &dp, &ln) ||
                  !read_be32_at(buf, end, &dp, &loff)) {
                break;
              }
              amiga_symbols_add_line(tbl, fname, ln & 0x00FFFFFFu, cur_hunk, base_off + loff);
            }
          }
        }
      }
      pos = end;
    } else if (htype == 0x03F2u) { /* HUNK_END */
      cur_hunk++;
    } else {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Unsupported hunk type 0x%04X in '%s'", htype, exe_path);
      return false;
    }
  }

  amiga_symbols_finalize_line_ranges(tbl);
  return true;

truncated:
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
              "Truncated Amiga Hunk binary '%s'", exe_path);
  return false;
}

bool
amiga_symbols_load_map(AmigaSymbolTable *tbl,
                       const char       *map_path,
                       GError          **error)
{
  if (!tbl || !map_path || !map_path[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid map args");
    return false;
  }
  g_autofree char *txt = NULL;
  if (!g_file_get_contents(map_path, &txt, NULL, error)) {
    return false;
  }

  /* Match GNU ld -Map lines: "  0x00000120   my_symbol" (avoid lines with multiple 0x columns) */
  g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
  for (int i = 0; lines[i] != NULL; i++) {
    char *s = g_strstrip(lines[i]);
    if (!g_str_has_prefix(s, "0x") && !g_str_has_prefix(s, "0X")) continue;
    char *endptr = NULL;
    unsigned long addr = strtoul(s, &endptr, 16);
    if (!endptr || endptr == s || !isspace((unsigned char)*endptr)) continue;
    while (*endptr && isspace((unsigned char)*endptr)) endptr++;
    if (!*endptr || *endptr == '.' || *endptr == '*' || g_str_has_prefix(endptr, "0x")) continue;
    /* Only accept clean identifier tokens on the line */
    char *tok_end = endptr;
    while (*tok_end && !isspace((unsigned char)*tok_end)) tok_end++;
    g_autofree char *name = g_strndup(endptr, (gsize)(tok_end - endptr));
    if (name[0] && strchr(name, '=') == NULL && strchr(name, '(') == NULL) {
      amiga_symbols_add_symbol(tbl, name, 0, (uint32_t)addr, 0);
    }
  }
  return true;
}

void
amiga_symbols_apply_segments(AmigaSymbolTable       *tbl,
                             const AmigaSegmentList *segs)
{
  if (!tbl || !tbl->seg_bases) return;
  g_array_set_size(tbl->seg_bases, 0);
  if (!segs || !segs->segments) return;
  for (guint i = 0; i < segs->segments->len; i++) {
    const AmigaSegmentInfo *si = g_ptr_array_index(segs->segments, i);
    if (si->index >= tbl->seg_bases->len) {
      g_array_set_size(tbl->seg_bases, si->index + 1u);
    }
    g_array_index(tbl->seg_bases, uint32_t, si->index) = si->address;
  }
}

bool
amiga_symbols_resolve_addr(const AmigaSymbolTable  *tbl,
                           uint32_t                 phys_addr,
                           const char             **out_name,
                           uint32_t                *out_delta)
{
  if (!tbl || !tbl->symbols) return false;
  const AmigaSymbol *best = NULL;
  uint32_t best_delta = UINT32_MAX;

  for (guint i = 0; i < tbl->symbols->len; i++) {
    const AmigaSymbol *s = g_ptr_array_index(tbl->symbols, i);
    uint32_t abs_addr = segment_base_for_hunk(tbl, s->hunk_index) + s->offset;
    if (phys_addr < abs_addr) continue;
    uint32_t delta = phys_addr - abs_addr;
    if (delta < best_delta) {
      best = s;
      best_delta = delta;
      if (delta == 0) break;
    }
  }
  if (!best) return false;
  if (out_name) *out_name = best->name;
  if (out_delta) *out_delta = best_delta;
  return true;
}

bool
amiga_symbols_addr_to_line(const AmigaSymbolTable  *tbl,
                           uint32_t                 phys_addr,
                           const char             **out_file,
                           uint32_t                *out_line)
{
  if (!tbl || !tbl->lines) return false;
  const AmigaLineEntry *best = NULL;
  for (guint i = 0; i < tbl->lines->len; i++) {
    const AmigaLineEntry *l = g_ptr_array_index(tbl->lines, i);
    uint32_t base = segment_base_for_hunk(tbl, l->hunk_index);
    uint32_t start = base + l->offset;
    uint32_t end = base + l->end_offset;
    if (phys_addr >= start && phys_addr < end) {
      best = l;
      break;
    }
  }
  if (!best) return false;
  if (out_file) *out_file = best->file;
  if (out_line) *out_line = best->line;
  return true;
}

static bool
file_path_matches(const char *a, const char *b)
{
  if (g_strcmp0(a, b) == 0) return true;
  if (!a || !b) return false;
  g_autofree char *ba = g_path_get_basename(a);
  g_autofree char *bb = g_path_get_basename(b);
  return g_strcmp0(ba, bb) == 0;
}

bool
amiga_symbols_line_to_addr(const AmigaSymbolTable *tbl,
                           const char             *file,
                           uint32_t                line,
                           uint32_t               *out_start_addr,
                           uint32_t               *out_end_addr)
{
  if (!tbl || !tbl->lines || !file || line == 0) return false;
  for (guint i = 0; i < tbl->lines->len; i++) {
    const AmigaLineEntry *l = g_ptr_array_index(tbl->lines, i);
    if (l->line == line && file_path_matches(l->file, file)) {
      uint32_t base = segment_base_for_hunk(tbl, l->hunk_index);
      if (out_start_addr) *out_start_addr = base + l->offset;
      if (out_end_addr) *out_end_addr = base + l->end_offset;
      return true;
    }
  }
  return false;
}

bool
amiga_symbols_lookup_name(const AmigaSymbolTable *tbl,
                          const char             *name,
                          uint32_t               *out_phys_addr)
{
  if (!tbl || !tbl->symbols || !name || !out_phys_addr) return false;
  for (guint i = 0; i < tbl->symbols->len; i++) {
    const AmigaSymbol *s = g_ptr_array_index(tbl->symbols, i);
    if (g_strcmp0(s->name, name) == 0) {
      *out_phys_addr = segment_base_for_hunk(tbl, s->hunk_index) + s->offset;
      return true;
    }
  }
  return false;
}

const GPtrArray *
amiga_symbols_get_symbols(const AmigaSymbolTable *tbl)
{
  return tbl ? tbl->symbols : NULL;
}

const GPtrArray *
amiga_symbols_get_lines(const AmigaSymbolTable *tbl)
{
  return tbl ? tbl->lines : NULL;
}

GPtrArray *
amiga_symbols_extract_outline(const char *source_text, bool is_c)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_outline_item_free);
  if (!source_text) return out;

  g_auto(GStrv) lines = g_strsplit(source_text, "\n", -1);
  int brace_depth = 0;

  for (guint i = 0; lines[i] != NULL; i++) {
    const char *raw = lines[i];
    if (is_c) {
      if (brace_depth == 0) {
        g_autofree char *trim = g_strdup(raw);
        g_strstrip(trim);
        if (trim[0] && trim[0] != '#' && trim[0] != '/' && trim[0] != '*' &&
            !g_str_has_prefix(trim, "typedef ") &&
            !g_str_has_prefix(trim, "struct ") &&
            strchr(trim, ';') == NULL) {
          const char *lp = strchr(trim, '(');
          const char *rp = lp ? strchr(lp, ')') : NULL;
          bool has_brace = (strchr(trim, '{') != NULL) ||
                           (lines[i + 1] != NULL && strchr(lines[i + 1], '{') != NULL);
          if (lp && rp && has_brace && lp > trim) {
            const char *end = lp;
            while (end > trim && isspace((unsigned char)end[-1])) end--;
            const char *start = end;
            while (start > trim && (isalnum((unsigned char)start[-1]) || start[-1] == '_')) start--;
            if (end > start) {
              AmigaOutlineItem *it = g_new0(AmigaOutlineItem, 1);
              it->kind = AMIGA_OUTLINE_FUNCTION;
              it->name = g_strndup(start, (gsize)(end - start));
              it->line = i + 1u;
              g_ptr_array_add(out, it);
            }
          }
        }
      }
      for (const char *p = raw; *p; p++) {
        if (*p == '{') brace_depth++;
        else if (*p == '}' && brace_depth > 0) brace_depth--;
      }
      continue;
    }

    /* 68k ASM outline */
    if (!raw[0] || raw[0] == ';' || raw[0] == '*') continue;
    g_autofree char *buf = g_strdup(raw);
    char *cmt = strchr(buf, ';');
    if (cmt) *cmt = '\0';
    g_strchomp(buf);
    if (!buf[0]) continue;

    /* SECTION check (may be indented) */
    const char *ws = buf;
    while (*ws && isspace((unsigned char)*ws)) ws++;
    if (g_ascii_strncasecmp(ws, "section", 7) == 0 &&
        (ws[7] == '\0' || isspace((unsigned char)ws[7]))) {
      AmigaOutlineItem *it = g_new0(AmigaOutlineItem, 1);
      it->kind = AMIGA_OUTLINE_SECTION;
      it->name = g_strdup(ws);
      it->line = i + 1u;
      g_ptr_array_add(out, it);
      continue;
    }

    /* First column identifiers: Label:, NAME EQU ..., NAME = ..., NAME MACRO */
    if (!isspace((unsigned char)buf[0]) && buf[0] != '.') {
      char *p = buf;
      while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '$')) p++;
      if (p > buf) {
        g_autofree char *ident = g_strndup(buf, (gsize)(p - buf));
        const char *rest = p;
        bool colon = (*rest == ':');
        if (colon) rest++;
        while (*rest && isspace((unsigned char)*rest)) rest++;
        AmigaOutlineKind k = AMIGA_OUTLINE_LABEL;
        bool accept = colon;
        if (g_ascii_strncasecmp(rest, "equ", 3) == 0 &&
            (rest[3] == '\0' || isspace((unsigned char)rest[3]))) {
          k = AMIGA_OUTLINE_EQU;
          accept = true;
        } else if (rest[0] == '=') {
          k = AMIGA_OUTLINE_EQU;
          accept = true;
        } else if (g_ascii_strncasecmp(rest, "macro", 5) == 0 &&
                   (rest[5] == '\0' || isspace((unsigned char)rest[5]))) {
          k = AMIGA_OUTLINE_MACRO;
          accept = true;
        }
        if (accept) {
          AmigaOutlineItem *it = g_new0(AmigaOutlineItem, 1);
          it->kind = k;
          it->name = g_strdup(ident);
          it->line = i + 1u;
          g_ptr_array_add(out, it);
        }
      }
    }
  }

  return out;
}
