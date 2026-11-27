#include "amiga_disasm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
amiga_disasm_row_free(AmigaDisasmRow *row)
{
  if (!row) return;
  g_free(row->header_text);
  g_free(row->opcode_hex);
  g_free(row->mnemonic);
  g_free(row->operands);
  g_free(row->symbol_name);
  g_free(row->hw_reg_name);
  g_free(row->lvo_name);
  g_free(row->comment);
  g_free(row);
}

static bool
is_4hex_word(const char *s, size_t len)
{
  if (len != 4) return false;
  for (size_t i = 0; i < 4; i++) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  /* "abcd" is a valid 68000 mnemonic (Add Binary Coded Decimal) */
  if (g_ascii_strncasecmp(s, "abcd", 4) == 0) {
    return false;
  }
  return true;
}

static bool
parse_hex_u32(const char *s, uint32_t *out)
{
  if (!s || !out) return false;
  if (s[0] == '$') s++;
  else if (g_ascii_strncasecmp(s, "0x", 2) == 0) s += 2;
  if (!isxdigit((unsigned char)*s)) return false;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 16);
  if (!end || end == s) return false;
  *out = (uint32_t)v;
  return true;
}

static bool
is_branch_or_jump_mnem(const char *m)
{
  if (!m || !m[0]) return false;
  if (m[0] == 'b' || m[0] == 'B') {
    /* Exclude bset/bclr/bchg/btst/bf* */
    if (g_ascii_strncasecmp(m, "bset", 4) == 0 ||
        g_ascii_strncasecmp(m, "bclr", 4) == 0 ||
        g_ascii_strncasecmp(m, "bchg", 4) == 0 ||
        g_ascii_strncasecmp(m, "btst", 4) == 0 ||
        g_ascii_strncasecmp(m, "bkpt", 4) == 0) {
      return false;
    }
    return true;
  }
  return g_ascii_strncasecmp(m, "jsr", 3) == 0 ||
         g_ascii_strncasecmp(m, "jmp", 3) == 0 ||
         g_ascii_strncasecmp(m, "db", 2) == 0;
}

static void
enrich_hw_and_lvo(AmigaDisasmRow *row, const AmigaSymbolTable *syms)
{
  if (!row->operands) return;
  /* Find $dffNNN / $00dffNNN tokens for hardware register annotations */
  const char *p = row->operands;
  while ((p = strchr(p, '$')) != NULL) {
    bool is_imm = (p > row->operands && p[-1] == '#');
    p++;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 16);
    if (end > p) {
      if (v >= 0xDFF000ul && v <= 0xDFF1FEul) {
        uint16_t off = (uint16_t)(v & 0x1FEu);
        const AmigaHwRegInfo *ri = amiga_hw_reg_lookup(off);
        if (ri && !row->hw_reg_name) {
          row->hw_reg_name = g_strdup_printf("%s ($%03X)", ri->name, off);
        }
      } else if (!is_imm && !row->has_target_addr && is_branch_or_jump_mnem(row->mnemonic)) {
        row->has_target_addr = true;
        row->target_addr = (uint32_t)v;
      }
    }
  }

  /* Match LVO negative displacement like -$0084(a6) / -132(a6) */
  g_autofree char *ops_lower = g_ascii_strdown(row->operands, -1);
  const char *a6_low = strstr(ops_lower, "(a6)");
  const char *a6 = a6_low ? (row->operands + (a6_low - ops_lower)) : NULL;
  if (a6 && a6 > row->operands) {
    const char *scan = a6;
    while (scan > row->operands && scan[-1] != '-') scan--;
    if (scan > row->operands && scan[-1] == '-') {
      int32_t lvo = 0;
      if (scan[0] == '$') {
        lvo = -(int32_t)strtol(scan + 1, NULL, 16);
      } else {
        lvo = -(int32_t)strtol(scan, NULL, 10);
      }
      const char *lvo_name = amiga_lvo_lookup(NULL, (int16_t)lvo);
      if (lvo_name) {
        row->lvo_name = g_strdup(lvo_name);
      }
    }
  }

  if (row->has_ea_value && is_branch_or_jump_mnem(row->mnemonic) && !row->has_target_addr) {
    row->has_target_addr = true;
    row->target_addr = row->ea_value;
  }

  if (syms) {
    const char *sname = NULL;
    uint32_t delta = 0;
    if (amiga_symbols_resolve_addr(syms, row->addr, &sname, &delta) && delta == 0) {
      row->symbol_name = g_strdup(sname);
    }
    if (row->has_target_addr) {
      const char *tname = NULL;
      uint32_t tdelta = 0;
      if (amiga_symbols_resolve_addr(syms, row->target_addr, &tname, &tdelta) && tdelta == 0) {
        g_free(row->comment);
        row->comment = g_strdup_printf("-> %s", tname);
      }
    }
  }
}

bool
amiga_disasm_parse(const char             *text,
                   const AmigaSymbolTable *syms,
                   GPtrArray             **out_rows)
{
  if (!text || !out_rows) return false;
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_disasm_row_free);

  g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
  for (int i = 0; lines[i] != NULL; i++) {
    g_autofree char *ln = g_strdup(lines[i]);
    g_strstrip(ln);
    if (!ln[0]) continue;

    /* Parse "<addr>[: ] ..." */
    const char *p = ln;
    if (p[0] == '$') p++;
    else if (g_ascii_strncasecmp(p, "0x", 2) == 0) p += 2;
    const char *addr_start = p;
    while (isxdigit((unsigned char)*p)) p++;
    size_t addr_len = (size_t)(p - addr_start);

    /* Non-address header/SegTracker lines */
    if (addr_len < 4 || addr_len > 8 || (*p != ':' && !isspace((unsigned char)*p))) {
      AmigaDisasmRow *hdr = g_new0(AmigaDisasmRow, 1);
      hdr->is_header = true;
      hdr->header_text = g_strdup(ln);
      g_ptr_array_add(rows, hdr);
      continue;
    }

    AmigaDisasmRow *row = g_new0(AmigaDisasmRow, 1);
    row->branch_taken = -1;
    (void)parse_hex_u32(addr_start, &row->addr);

    if (*p == ':') p++;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Consume 1..5 16-bit hex opcode words separated by spaces */
    g_autoptr(GString) ops_hex = g_string_new("");
    int nw = 0;
    while (*p && nw < 5) {
      const char *wstart = p;
      while (*p && !isspace((unsigned char)*p)) p++;
      size_t wlen = (size_t)(p - wstart);
      if (is_4hex_word(wstart, wlen)) {
        if (ops_hex->len > 0) g_string_append_c(ops_hex, ' ');
        g_string_append_len(ops_hex, wstart, (gssize)wlen);
        while (*p && isspace((unsigned char)*p)) p++;
        nw++;
      } else {
        p = wstart; /* rewind to mnemonic */
        break;
      }
    }
    row->opcode_hex = g_string_free(g_steal_pointer(&ops_hex), FALSE);

    /* Read mnemonic */
    const char *mstart = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (p == mstart) {
      /* Degenerate header-like line */
      row->is_header = true;
      row->header_text = g_strdup(ln);
      g_free(row->opcode_hex);
      row->opcode_hex = NULL;
      g_ptr_array_add(rows, row);
      continue;
    }
    row->mnemonic = g_strndup(mstart, (gsize)(p - mstart));
    while (*p && isspace((unsigned char)*p)) p++;

    /* Parse rest: operands + optional " == $<ea>" + optional " (T)"/"(F)" */
    g_autofree char *rest = g_strdup(p);
    g_strchomp(rest);

    size_t rlen = strlen(rest);
    if (rlen >= 3 && (g_strcmp0(rest + rlen - 3, "(T)") == 0 ||
                      g_strcmp0(rest + rlen - 3, "(F)") == 0)) {
      row->branch_taken = (rest[rlen - 2] == 'T') ? 1 : 0;
      rest[rlen - 3] = '\0';
      g_strchomp(rest);
    }

    char *eq = strstr(rest, "==");
    if (eq != NULL) {
      char *val_str = eq + 2;
      while (*val_str && isspace((unsigned char)*val_str)) val_str++;
      uint32_t ea = 0;
      if (parse_hex_u32(val_str, &ea)) {
        row->has_ea_value = true;
        row->ea_value = ea;
      }
      *eq = '\0';
      g_strchomp(rest);
    }

    row->operands = g_strdup(rest);

    g_autofree char *full_insn = (row->operands && row->operands[0])
                                     ? g_strdup_printf("%s %s", row->mnemonic, row->operands)
                                     : g_strdup(row->mnemonic);
    row->cycles = amiga_m68k_line_cycles(full_insn);
    enrich_hw_and_lvo(row, syms);

    g_ptr_array_add(rows, row);
  }

  *out_rows = rows;
  return true;
}
