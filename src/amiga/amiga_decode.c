#include "amiga_decode.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AmigaSrFlags
amiga_decode_sr(uint16_t sr)
{
  AmigaSrFlags f = {
    .trace      = (sr & 0x8000u) != 0,
    .supervisor = (sr & 0x2000u) != 0,
    .ipl        = (uint8_t)((sr >> 8) & 0x7u),
    .extend     = (sr & 0x0010u) != 0,
    .negative   = (sr & 0x0008u) != 0,
    .zero       = (sr & 0x0004u) != 0,
    .overflow   = (sr & 0x0002u) != 0,
    .carry      = (sr & 0x0001u) != 0,
  };
  return f;
}

/* ─── Line-by-Line 68000 Static Cycle LUT (no call/loop unrolling) ─── */

typedef enum {
  SZ_NONE = 0,
  SZ_B,
  SZ_W,
  SZ_L,
} OpSize;

typedef enum {
  EA_NONE = 0,
  EA_DN,
  EA_AN,
  EA_IND,     /* (An) */
  EA_POST,    /* (An)+ */
  EA_PRE,     /* -(An) */
  EA_DISP,    /* d16(An) or (d16,An) or d16(PC) */
  EA_INDEX,   /* d8(An,Xn) or (d8,An,Xn) */
  EA_ABSW,    /* xxxx.w or label */
  EA_ABSL,    /* xxxxxxxx.l or $123456 */
  EA_IMM,     /* #... */
} EaKind;

static const char *
skip_ws(const char *p)
{
  while (*p && (*p == ' ' || *p == '\t')) {
    p++;
  }
  return p;
}

static void
strip_comment_and_trim(const char *line, char *out, size_t cap)
{
  if (!line || !out || cap == 0) {
    return;
  }
  bool in_sq = false, in_dq = false;
  size_t i = 0;
  for (const char *p = line; *p && i + 1 < cap; p++) {
    if (*p == '\'' && !in_dq) {
      in_sq = !in_sq;
    } else if (*p == '"' && !in_sq) {
      in_dq = !in_dq;
    } else if (!in_sq && !in_dq && (*p == ';' || (*p == '*' && p == line))) {
      break;
    } else if (*p == '\r' || *p == '\n') {
      break;
    }
    out[i++] = *p;
  }
  out[i] = '\0';
  while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) {
    out[--i] = '\0';
  }
}

static bool
is_directive(const char *mnem)
{
  static const char *const dirs[] = {
    "section", "code", "data", "bss", "dc", "dcb", "ds", "dx",
    "equ", "equr", "fequ", "set", "reg", "macro", "endm", "end",
    "include", "incbin", "incdir", "xdef", "xref", "public",
    "extern", "even", "cnop", "align", "opt", "machine", "cpu",
    "fpu", "if", "ifeq", "ifne", "ifgt", "ifge", "iflt", "ifle",
    "ifdef", "ifndef", "ifd", "ifnd", "else", "elseif", "endc",
    "endif", "rept", "endr", "printt", "printv", "fail", "rs",
    "rsreset", "rsset", "org", "offset", "comment",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(dirs); i++) {
    if (strcmp(mnem, dirs[i]) == 0) {
      return true;
    }
  }
  return false;
}

static EaKind
classify_ea(const char *raw)
{
  if (!raw || !raw[0]) {
    return EA_NONE;
  }
  char buf[128];
  g_strlcpy(buf, raw, sizeof(buf));
  g_strstrip(buf);
  if (!buf[0]) {
    return EA_NONE;
  }
  if (buf[0] == '#') {
    return EA_IMM;
  }
  for (size_t i = 0; buf[i]; i++) {
    buf[i] = (char)tolower((unsigned char)buf[i]);
  }
  if (strcmp(buf, "sp") == 0 || (buf[0] == 'a' && buf[1] >= '0' && buf[1] <= '7' && buf[2] == '\0')) {
    return EA_AN;
  }
  if (buf[0] == 'd' && buf[1] >= '0' && buf[1] <= '7' && buf[2] == '\0') {
    return EA_DN;
  }
  if (strcmp(buf, "ccr") == 0 || strcmp(buf, "sr") == 0 || strcmp(buf, "usp") == 0) {
    return EA_DN;
  }
  size_t len = strlen(buf);
  if (buf[0] == '-' && buf[1] == '(' && buf[len - 1] == ')') {
    return EA_PRE;
  }
  if (buf[0] == '(' && len >= 2 && buf[len - 1] == '+' && buf[len - 2] == ')') {
    return EA_POST;
  }
  const char *lp = strchr(buf, '(');
  if (lp != NULL) {
    const char *comma = strchr(lp, ',');
    if (comma != NULL) {
      /* Distinguish (disp,An) [2 components, 2nd is An/pc] vs (An,Xn) / (d8,An,Xn) */
      const char *comma2 = strchr(comma + 1, ',');
      if (comma2 != NULL) {
        return EA_INDEX;
      }
      if (lp == buf) {
        const char *after = comma + 1;
        while (*after == ' ') after++;
        if ((after[0] == 'a' || strcmp(after, "pc)") == 0 || strncmp(after, "sp)", 3) == 0) &&
            strlen(after) <= 4 && strchr(after, ')') != NULL) {
          return EA_DISP;
        }
        return EA_INDEX;
      }
      return EA_INDEX;
    }
    if (lp == buf) {
      return EA_IND;
    }
    return EA_DISP;
  }
  if (strstr(buf, ".w") != NULL) {
    return EA_ABSW;
  }
  if (strstr(buf, ".l") != NULL) {
    return EA_ABSL;
  }
  if (buf[0] == '$' && strlen(buf) <= 5) {
    return EA_ABSW;
  }
  return EA_ABSL;
}

static void
split_top_level_operands(const char *s, char *op1, size_t cap1, char *op2, size_t cap2)
{
  op1[0] = '\0';
  op2[0] = '\0';
  if (!s || !s[0]) {
    return;
  }
  int depth = 0;
  bool in_sq = false, in_dq = false;
  const char *split = NULL;
  for (const char *p = s; *p; p++) {
    if (*p == '\'' && !in_dq) in_sq = !in_sq;
    else if (*p == '"' && !in_sq) in_dq = !in_dq;
    else if (!in_sq && !in_dq) {
      if (*p == '(') depth++;
      else if (*p == ')' && depth > 0) depth--;
      else if (*p == ',' && depth == 0) {
        split = p;
        break;
      }
    }
  }
  if (split != NULL) {
    size_t n1 = MIN((size_t)(split - s), cap1 - 1);
    memcpy(op1, s, n1);
    op1[n1] = '\0';
    g_strlcpy(op2, split + 1, cap2);
  } else {
    g_strlcpy(op1, s, cap1);
  }
  g_strstrip(op1);
  g_strstrip(op2);
}

static uint16_t
ea_cycles(EaKind k, OpSize sz, uint8_t *r, uint8_t *w)
{
  bool is_long = (sz == SZ_L);
  uint8_t rw = is_long ? 2 : 1;
  switch (k) {
    case EA_NONE:
    case EA_DN:
    case EA_AN:
      return 0;
    case EA_IND:
    case EA_POST:
      *r += rw;
      return is_long ? 8 : 4;
    case EA_PRE:
      *r += rw;
      return is_long ? 10 : 6;
    case EA_DISP:
    case EA_ABSW:
      *r += (uint8_t)(1 + rw);
      return is_long ? 12 : 8;
    case EA_ABSL:
      *r += (uint8_t)(2 + rw);
      return is_long ? 16 : 12;
    case EA_INDEX:
      *r += (uint8_t)(1 + rw);
      return is_long ? 14 : 10;
    case EA_IMM:
      *r += is_long ? 2 : 1;
      return is_long ? 8 : 4;
    default:
      return 0;
  }
}

/* Destination EA calculation cost for MOVE (addressing mode only; MOVE adds write cycles separately) */
static uint16_t
ea_dest_move_cycles(EaKind k, OpSize sz, uint8_t *r, uint8_t *w)
{
  bool is_long = (sz == SZ_L);
  uint8_t ww = is_long ? 2 : 1;
  switch (k) {
    case EA_DN:
    case EA_AN:
      return 0;
    case EA_IND:
    case EA_POST:
    case EA_PRE:
      *w += ww;
      return is_long ? 8 : 4;
    case EA_DISP:
    case EA_ABSW:
      *r += 1;
      *w += ww;
      return is_long ? 12 : 8;
    case EA_ABSL:
      *r += 2;
      *w += ww;
      return is_long ? 16 : 12;
    case EA_INDEX:
      *r += 1;
      *w += ww;
      return is_long ? 14 : 10;
    default:
      return 0;
  }
}

static int
count_movem_regs(const char *spec)
{
  /* Heuristic count of registers in mask like d0-d7/a0-a6 or #imm */
  if (!spec || !spec[0]) {
    return 4;
  }
  int count = 0;
  g_auto(GStrv) parts = g_strsplit(spec, "/", -1);
  for (int i = 0; parts[i] != NULL; i++) {
    char seg[32];
    g_strlcpy(seg, parts[i], sizeof(seg));
    g_strstrip(seg);
    char *dash = strchr(seg, '-');
    if (dash != NULL && strlen(seg) >= 5) {
      int r1 = dash[-1] - '0';
      int r2 = dash[2] - '0';
      if (r1 >= 0 && r1 <= 7 && r2 >= r1 && r2 <= 7) {
        count += (r2 - r1 + 1);
        continue;
      }
    }
    if ((seg[0] == 'd' || seg[0] == 'D' || seg[0] == 'a' || seg[0] == 'A' ||
         strcmp(seg, "sp") == 0 || strcmp(seg, "SP") == 0) &&
        strlen(seg) <= 3) {
      count += 1;
    }
  }
  return count > 0 ? count : 4;
}

static bool
parse_immediate_shift(const char *op, int *out_n)
{
  if (!op || op[0] != '#') {
    return false;
  }
  const char *p = op + 1;
  char *end = NULL;
  long v = 0;
  if (*p == '$') {
    v = strtol(p + 1, &end, 16);
  } else {
    v = strtol(p, &end, 10);
  }
  if (end == p) {
    return false;
  }
  *out_n = (int)CLAMP(v, 1, 8);
  return true;
}

AmigaCycleInfo
amiga_m68k_line_cycles(const char *line)
{
  AmigaCycleInfo out;
  memset(&out, 0, sizeof(out));

  char buf[512];
  strip_comment_and_trim(line, buf, sizeof(buf));
  const char *p = skip_ws(buf);
  if (!*p) {
    return out;
  }

  /* Strip leading local/global label `foo:` or `.loop` at column 0 */
  bool starts_at_col0 = (p == buf);
  const char *colon = strchr(p, ':');
  if (colon != NULL) {
    const char *ws = p;
    while (ws < colon && !isspace((unsigned char)*ws)) {
      ws++;
    }
    if (ws == colon) {
      p = skip_ws(colon + 1);
      if (!*p) {
        return out;
      }
    }
  } else if (starts_at_col0) {
    /* First token might be a label without ':' before EQU or directive, or standalone label */
    char tok1[64] = {0};
    size_t k = 0;
    while (p[k] && !isspace((unsigned char)p[k]) && k + 1 < sizeof(tok1)) {
      tok1[k] = (char)tolower((unsigned char)p[k]);
      k++;
    }
    const char *after = skip_ws(p + k);
    if (!*after) {
      /* Standalone col0 token: could be bare `rts`/`nop` OR bare label. Check common 0-arg ops */
      if (strcmp(tok1, "rts") != 0 && strcmp(tok1, "rte") != 0 &&
          strcmp(tok1, "rtr") != 0 && strcmp(tok1, "nop") != 0 &&
          strcmp(tok1, "reset") != 0 && strcmp(tok1, "illegal") != 0 &&
          strcmp(tok1, "trapv") != 0) {
        return out;
      }
    } else {
      char tok2[64] = {0};
      size_t k2 = 0;
      while (after[k2] && !isspace((unsigned char)after[k2]) && k2 + 1 < sizeof(tok2)) {
        tok2[k2] = (char)tolower((unsigned char)after[k2]);
        k2++;
      }
      if (strcmp(tok2, "equ") == 0 || strcmp(tok2, "=") == 0 ||
          strcmp(tok2, "set") == 0 || is_directive(tok2)) {
        return out;
      }
    }
  }

  /* Extract mnemonic and optional .b/.w/.l/.s suffix */
  char raw_mnem[64] = {0};
  size_t mi = 0;
  while (*p && !isspace((unsigned char)*p) && mi + 1 < sizeof(raw_mnem)) {
    raw_mnem[mi++] = (char)tolower((unsigned char)*p++);
  }
  raw_mnem[mi] = '\0';
  p = skip_ws(p);

  char mnem[64] = {0};
  OpSize sz = SZ_W;
  const char *dot = strchr(raw_mnem, '.');
  if (dot != NULL) {
    size_t base_n = MIN((size_t)(dot - raw_mnem), sizeof(mnem) - 1);
    memcpy(mnem, raw_mnem, base_n);
    mnem[base_n] = '\0';
    if (dot[1] == 'b' || dot[1] == 's') sz = SZ_B;
    else if (dot[1] == 'w') sz = SZ_W;
    else if (dot[1] == 'l') sz = SZ_L;
  } else {
    g_strlcpy(mnem, raw_mnem, sizeof(mnem));
  }

  if (!mnem[0] || mnem[0] == '.' || is_directive(mnem)) {
    return out;
  }

  char op1[128], op2[128];
  split_top_level_operands(p, op1, sizeof(op1), op2, sizeof(op2));
  EaKind k1 = classify_ea(op1);
  EaKind k2 = classify_ea(op2);

  uint16_t min_c = 0, max_c = 0;
  uint8_t rd = 0, wr = 0;
  bool var_shift = false, branch = false;

  if (strcmp(mnem, "nop") == 0) {
    min_c = max_c = 4; rd = 1;
  } else if (strcmp(mnem, "rts") == 0) {
    min_c = max_c = 16; rd = 4;
  } else if (strcmp(mnem, "rte") == 0 || strcmp(mnem, "rtr") == 0) {
    min_c = max_c = 20; rd = 5;
  } else if (strcmp(mnem, "moveq") == 0) {
    min_c = max_c = 4; rd = 1;
  } else if (strcmp(mnem, "move") == 0 || strcmp(mnem, "movea") == 0) {
    rd = 1;
    uint16_t c1 = ea_cycles(k1, sz, &rd, &wr);
    uint16_t c2 = ea_dest_move_cycles(k2, sz, &rd, &wr);
    min_c = max_c = (uint16_t)(4 + c1 + c2);
  } else if (strcmp(mnem, "movem") == 0) {
    bool is_long = (sz == SZ_L);
    if (k1 == EA_AN || k1 == EA_DN || strchr(op1, '-') != NULL || strchr(op1, '/') != NULL) {
      /* Regs -> Mem */
      int n = count_movem_regs(op1);
      uint16_t base = (k2 == EA_PRE || k2 == EA_IND) ? 8 : 12;
      wr = (uint8_t)(n * (is_long ? 2 : 1));
      rd = (k2 == EA_DISP || k2 == EA_ABSW) ? 3 : 2;
      min_c = max_c = (uint16_t)(base + n * (is_long ? 8 : 4));
    } else {
      /* Mem -> Regs */
      int n = count_movem_regs(op2);
      uint16_t base = (k1 == EA_POST || k1 == EA_IND) ? 12 : 16;
      rd = (uint8_t)(2 + n * (is_long ? 2 : 1));
      min_c = max_c = (uint16_t)(base + n * (is_long ? 8 : 4));
    }
  } else if (strcmp(mnem, "lea") == 0) {
    rd = 1;
    uint16_t c = (k1 == EA_IND) ? 4 : (k1 == EA_DISP || k1 == EA_ABSW) ? 8 : 12;
    if (k1 == EA_DISP || k1 == EA_ABSW || k1 == EA_INDEX) rd += 1;
    else if (k1 == EA_ABSL) rd += 2;
    min_c = max_c = c;
  } else if (strcmp(mnem, "pea") == 0) {
    rd = 1; wr = 2;
    uint16_t c = (k1 == EA_IND) ? 12 : (k1 == EA_DISP || k1 == EA_ABSW) ? 16 : 20;
    if (k1 == EA_DISP || k1 == EA_ABSW || k1 == EA_INDEX) rd += 1;
    else if (k1 == EA_ABSL) rd += 2;
    min_c = max_c = c;
  } else if (strcmp(mnem, "addq") == 0 || strcmp(mnem, "subq") == 0) {
    bool is_long = (sz == SZ_L);
    rd = 1;
    if (k2 == EA_DN || k2 == EA_AN) {
      min_c = max_c = (is_long || k2 == EA_AN) ? 8 : 4;
    } else {
      min_c = max_c = (uint16_t)((is_long ? 12 : 8) + ea_cycles(k2, sz, &rd, &wr));
      wr += is_long ? 2 : 1;
    }
  } else if (strcmp(mnem, "add") == 0 || strcmp(mnem, "adda") == 0 ||
             strcmp(mnem, "sub") == 0 || strcmp(mnem, "suba") == 0 ||
             strcmp(mnem, "and") == 0 || strcmp(mnem, "or") == 0 ||
             strcmp(mnem, "eor") == 0 || strcmp(mnem, "cmp") == 0 ||
             strcmp(mnem, "cmpa") == 0) {
    bool is_long = (sz == SZ_L);
    bool is_cmp = (mnem[0] == 'c');
    rd = 1;
    if (k2 == EA_DN || k2 == EA_AN) {
      uint16_t base = is_long ? ((k1 == EA_DN || k1 == EA_AN || k1 == EA_IMM) && !is_cmp ? 8 : 6) : 4;
      if (is_long && is_cmp) base = 6;
      min_c = max_c = (uint16_t)(base + ea_cycles(k1, sz, &rd, &wr));
    } else {
      uint16_t base = is_long ? 12 : 8;
      if (is_cmp) base = is_long ? 14 : 8;
      min_c = max_c = (uint16_t)(base + ea_cycles(k1, sz, &rd, &wr) + ea_cycles(k2, sz, &rd, &wr));
      if (!is_cmp) wr += is_long ? 2 : 1;
    }
  } else if (strcmp(mnem, "addi") == 0 || strcmp(mnem, "subi") == 0 ||
             strcmp(mnem, "andi") == 0 || strcmp(mnem, "ori") == 0 ||
             strcmp(mnem, "eori") == 0 || strcmp(mnem, "cmpi") == 0) {
    bool is_long = (sz == SZ_L);
    bool is_cmp = (mnem[0] == 'c');
    rd = is_long ? 3 : 2;
    if (k2 == EA_DN) {
      min_c = max_c = (is_long && !is_cmp) ? 16 : (is_long ? 14 : 8);
    } else {
      min_c = max_c = (uint16_t)((is_long ? 20 : 12) + ea_cycles(k2, sz, &rd, &wr));
      if (!is_cmp) wr += is_long ? 2 : 1;
    }
  } else if (strcmp(mnem, "tst") == 0 || strcmp(mnem, "clr") == 0 ||
             strcmp(mnem, "neg") == 0 || strcmp(mnem, "not") == 0) {
    bool is_long = (sz == SZ_L);
    bool is_tst = (mnem[0] == 't');
    rd = 1;
    if (k1 == EA_DN) {
      min_c = max_c = is_long ? (is_tst ? 4 : 6) : 4;
    } else {
      uint16_t base = is_tst ? 4 : (is_long ? 12 : 8);
      min_c = max_c = (uint16_t)(base + ea_cycles(k1, sz, &rd, &wr));
      if (!is_tst) wr += is_long ? 2 : 1;
    }
  } else if (strcmp(mnem, "swap") == 0 || strcmp(mnem, "ext") == 0) {
    min_c = max_c = 4; rd = 1;
  } else if (strcmp(mnem, "exg") == 0) {
    min_c = max_c = 6; rd = 1;
  } else if (strcmp(mnem, "lsl") == 0 || strcmp(mnem, "lsr") == 0 ||
             strcmp(mnem, "asl") == 0 || strcmp(mnem, "asr") == 0 ||
             strcmp(mnem, "rol") == 0 || strcmp(mnem, "ror") == 0 ||
             strcmp(mnem, "roxl") == 0 || strcmp(mnem, "roxr") == 0) {
    bool is_long = (sz == SZ_L);
    rd = 1;
    if (!op2[0]) {
      /* Memory shift by 1 */
      min_c = max_c = (uint16_t)(8 + ea_cycles(k1, SZ_W, &rd, &wr));
      wr += 1;
    } else {
      int imm_n = 0;
      uint16_t base = is_long ? 8 : 6;
      if (parse_immediate_shift(op1, &imm_n)) {
        min_c = max_c = (uint16_t)(base + 2 * imm_n);
      } else {
        var_shift = true;
        min_c = base;
        max_c = (uint16_t)(base + 2 * 16);
      }
    }
  } else if (strcmp(mnem, "btst") == 0 || strcmp(mnem, "bset") == 0 ||
             strcmp(mnem, "bclr") == 0 || strcmp(mnem, "bchg") == 0) {
    bool is_btst = (strcmp(mnem, "btst") == 0);
    bool is_imm = (k1 == EA_IMM);
    rd = is_imm ? 2 : 1;
    uint16_t base = is_imm ? (is_btst ? 10 : 12) : (is_btst ? 6 : 8);
    if (k2 != EA_DN) {
      min_c = max_c = (uint16_t)(base - 2 + ea_cycles(k2, SZ_B, &rd, &wr));
      if (!is_btst) wr += 1;
    } else {
      min_c = max_c = base;
    }
  } else if (strcmp(mnem, "mulu") == 0 || strcmp(mnem, "muls") == 0) {
    rd = 1;
    uint16_t ea = ea_cycles(k1, SZ_W, &rd, &wr);
    min_c = (uint16_t)(38 + ea);
    max_c = (uint16_t)(70 + ea);
  } else if (strcmp(mnem, "divu") == 0 || strcmp(mnem, "divs") == 0) {
    rd = 1;
    uint16_t ea = ea_cycles(k1, SZ_W, &rd, &wr);
    bool is_signed = (mnem[3] == 's');
    min_c = (uint16_t)((is_signed ? 122 : 76) + ea);
    max_c = (uint16_t)((is_signed ? 158 : 140) + ea);
  } else if (strcmp(mnem, "bra") == 0) {
    min_c = max_c = 10; rd = 2;
  } else if (strcmp(mnem, "bsr") == 0) {
    min_c = max_c = 18; rd = 2; wr = 2;
  } else if (strcmp(mnem, "jsr") == 0) {
    rd = (k1 == EA_IND) ? 2 : (k1 == EA_ABSL) ? 3 : 2;
    wr = 2;
    min_c = max_c = (k1 == EA_IND) ? 16 : (k1 == EA_ABSL) ? 20 : 18;
  } else if (strcmp(mnem, "jmp") == 0) {
    rd = (k1 == EA_IND) ? 2 : (k1 == EA_ABSL) ? 3 : 2;
    min_c = max_c = (k1 == EA_IND) ? 8 : (k1 == EA_ABSL) ? 12 : 10;
  } else if (strcmp(mnem, "link") == 0) {
    min_c = max_c = 16; rd = 2; wr = 2;
  } else if (strcmp(mnem, "unlk") == 0) {
    min_c = max_c = 12; rd = 3;
  } else if (strncmp(mnem, "db", 2) == 0 && strlen(mnem) >= 3) {
    branch = true;
    min_c = 10; max_c = 14; rd = 2;
  } else if (mnem[0] == 'b' && strlen(mnem) >= 3 && strlen(mnem) <= 4) {
    branch = true;
    min_c = (sz == SZ_B) ? 8 : 8;
    max_c = (sz == SZ_B) ? 10 : 12;
    rd = 2;
  } else {
    return out;
  }

  out.has_cycles = true;
  out.min_cycles = min_c;
  out.max_cycles = max_c;
  out.reads = rd;
  out.writes = wr;
  out.variable_shift = var_shift;
  out.is_branch = branch;

  if (var_shift) {
    g_snprintf(out.display, sizeof(out.display), "%u+2n(%u/%u)", min_c, rd, wr);
  } else if (branch) {
    g_snprintf(out.display, sizeof(out.display), "%u/%u", min_c, max_c);
  } else if (min_c != max_c) {
    g_snprintf(out.display, sizeof(out.display), "%u-%u(%u/%u)", min_c, max_c, rd, wr);
  } else {
    g_snprintf(out.display, sizeof(out.display), "%u(%u/%u)", min_c, rd, wr);
  }
  return out;
}

AmigaCycleSum
amiga_m68k_sum_cycles(const char *const *lines, size_t n_lines)
{
  AmigaCycleSum sum;
  memset(&sum, 0, sizeof(sum));
  if (!lines) {
    return sum;
  }
  for (size_t i = 0; i < n_lines; i++) {
    AmigaCycleInfo ci = amiga_m68k_line_cycles(lines[i]);
    if (!ci.has_cycles) {
      continue;
    }
    sum.min_cycles += ci.min_cycles;
    sum.max_cycles += ci.max_cycles;
    sum.reads += ci.reads;
    sum.writes += ci.writes;
    sum.instr_count += 1;
    if (ci.variable_shift) {
      sum.has_variable_shift = true;
    }
  }
  return sum;
}

/* ─── Hardware Register Table ($DFF000..$1FE) ─── */

static const AmigaHwRegInfo k_hw_regs[] = {
  { 0x000, "BLTDDAT", AMIGA_REG_R,  "Blitter dest dummy data" },
  { 0x002, "DMACONR", AMIGA_REG_R,  "DMA control & Blitter busy status" },
  { 0x004, "VPOSR",   AMIGA_REG_R,  "Light pen / Agnus ID / vertical MSB (V8)" },
  { 0x006, "VHPOSR",  AMIGA_REG_R,  "Beam vertical V7..V0 & horizontal H8..H1" },
  { 0x00E, "CLXDAT",  AMIGA_REG_R,  "Sprite/playfield collision latch" },
  { 0x010, "ADKCONR", AMIGA_REG_R,  "Audio, disk & UART control read" },
  { 0x01C, "INTENAR", AMIGA_REG_R,  "Interrupt enable status read" },
  { 0x01E, "INTREQR", AMIGA_REG_R,  "Interrupt request status read" },
  { 0x02E, "COPCON",  AMIGA_REG_W,  "Copper control (CDANG=bit 1 allows Blitter moves)" },
  { 0x040, "BLTCON0", AMIGA_REG_W,  "Blitter control 0: ASH, USEA..D, LF minterm" },
  { 0x042, "BLTCON1", AMIGA_REG_W,  "Blitter control 1: BSH, line/fill/desc flags" },
  { 0x044, "BLTAFWM", AMIGA_REG_W,  "Blitter first-word mask for source A" },
  { 0x046, "BLTALWM", AMIGA_REG_W,  "Blitter last-word mask for source A" },
  { 0x048, "BLTCPTH", AMIGA_REG_W,  "Blitter source C pointer high" },
  { 0x04A, "BLTCPTL", AMIGA_REG_W,  "Blitter source C pointer low" },
  { 0x04C, "BLTBPTH", AMIGA_REG_W,  "Blitter source B pointer high" },
  { 0x04E, "BLTBPTL", AMIGA_REG_W,  "Blitter source B pointer low" },
  { 0x050, "BLTAPTH", AMIGA_REG_W,  "Blitter source A pointer high" },
  { 0x052, "BLTAPTL", AMIGA_REG_W,  "Blitter source A pointer low" },
  { 0x054, "BLTDPTH", AMIGA_REG_W,  "Blitter dest D pointer high" },
  { 0x056, "BLTDPTL", AMIGA_REG_W,  "Blitter dest D pointer low" },
  { 0x058, "BLTSIZE", AMIGA_REG_W,  "Blitter start + height[15:6] / width[5:0]" },
  { 0x05A, "BLTCON0L",AMIGA_REG_W,  "ECS Blitter LF minterm low byte" },
  { 0x05C, "BLTSIZV", AMIGA_REG_W,  "ECS vertical blit size" },
  { 0x05E, "BLTSIZH", AMIGA_REG_W,  "ECS horizontal blit size + start" },
  { 0x060, "BLTCMOD", AMIGA_REG_W,  "Blitter modulo C" },
  { 0x062, "BLTBMOD", AMIGA_REG_W,  "Blitter modulo B" },
  { 0x064, "BLTAMOD", AMIGA_REG_W,  "Blitter modulo A" },
  { 0x066, "BLTDMOD", AMIGA_REG_W,  "Blitter modulo D" },
  { 0x070, "BLTCDAT", AMIGA_REG_W,  "Blitter source C preload data" },
  { 0x072, "BLTBDAT", AMIGA_REG_W,  "Blitter source B preload data" },
  { 0x074, "BLTADAT", AMIGA_REG_W,  "Blitter source A preload data" },
  { 0x07C, "DENISEID",AMIGA_REG_R,  "Denise/Lisa chip revision ID" },
  { 0x080, "COP1LCH", AMIGA_REG_W,  "Copper list 1 pointer high" },
  { 0x082, "COP1LCL", AMIGA_REG_W,  "Copper list 1 pointer low" },
  { 0x084, "COP2LCH", AMIGA_REG_W,  "Copper list 2 pointer high" },
  { 0x086, "COP2LCL", AMIGA_REG_W,  "Copper list 2 pointer low" },
  { 0x088, "COPJMP1", AMIGA_REG_W,  "Strobe restart Copper list 1" },
  { 0x08A, "COPJMP2", AMIGA_REG_W,  "Strobe restart Copper list 2" },
  { 0x08E, "DIWSTRT", AMIGA_REG_W,  "Display window start (VSTRT/HSTRT)" },
  { 0x090, "DIWSTOP", AMIGA_REG_W,  "Display window stop (VSTOP/HSTOP)" },
  { 0x092, "DDFSTRT", AMIGA_REG_W,  "Bitplane DMA data-fetch start" },
  { 0x094, "DDFSTOP", AMIGA_REG_W,  "Bitplane DMA data-fetch stop" },
  { 0x096, "DMACON",  AMIGA_REG_W,  "DMA control write (SET/CLR bit 15)" },
  { 0x098, "CLXCON",  AMIGA_REG_W,  "Collision control" },
  { 0x09A, "INTENA",  AMIGA_REG_W,  "Interrupt enable write (SET/CLR bit 15)" },
  { 0x09C, "INTREQ",  AMIGA_REG_W,  "Interrupt request write (SET/CLR bit 15)" },
  { 0x09E, "ADKCON",  AMIGA_REG_W,  "Audio/disk/UART control write" },
  { 0x0E0, "BPL1PTH", AMIGA_REG_W,  "Bitplane 1 pointer high" },
  { 0x0E2, "BPL1PTL", AMIGA_REG_W,  "Bitplane 1 pointer low" },
  { 0x0E4, "BPL2PTH", AMIGA_REG_W,  "Bitplane 2 pointer high" },
  { 0x0E6, "BPL2PTL", AMIGA_REG_W,  "Bitplane 2 pointer low" },
  { 0x0E8, "BPL3PTH", AMIGA_REG_W,  "Bitplane 3 pointer high" },
  { 0x0EA, "BPL3PTL", AMIGA_REG_W,  "Bitplane 3 pointer low" },
  { 0x0EC, "BPL4PTH", AMIGA_REG_W,  "Bitplane 4 pointer high" },
  { 0x0EE, "BPL4PTL", AMIGA_REG_W,  "Bitplane 4 pointer low" },
  { 0x0F0, "BPL5PTH", AMIGA_REG_W,  "Bitplane 5 pointer high" },
  { 0x0F2, "BPL5PTL", AMIGA_REG_W,  "Bitplane 5 pointer low" },
  { 0x0F4, "BPL6PTH", AMIGA_REG_W,  "Bitplane 6 pointer high" },
  { 0x0F6, "BPL6PTL", AMIGA_REG_W,  "Bitplane 6 pointer low" },
  { 0x100, "BPLCON0", AMIGA_REG_W,  "Bitplane control 0: HIRES, BPU, HAM, DPF, COLOR" },
  { 0x102, "BPLCON1", AMIGA_REG_W,  "Horizontal playfield fine scroll" },
  { 0x104, "BPLCON2", AMIGA_REG_W,  "Playfield vs sprite priority" },
  { 0x106, "BPLCON3", AMIGA_REG_W,  "ECS/AGA palette bank & border control" },
  { 0x108, "BPL1MOD", AMIGA_REG_W,  "Odd bitplanes modulo" },
  { 0x10A, "BPL2MOD", AMIGA_REG_W,  "Even bitplanes modulo" },
  { 0x10C, "BPLCON4", AMIGA_REG_W,  "AGA bitplane/sprite color XOR & bank" },
  { 0x120, "SPR0PTH", AMIGA_REG_W,  "Sprite 0 pointer high" },
  { 0x180, "COLOR00", AMIGA_REG_W,  "Palette register 00 (background)" },
  { 0x182, "COLOR01", AMIGA_REG_W,  "Palette register 01" },
  { 0x1FC, "FMODE",   AMIGA_REG_W,  "AGA fetch mode (1x/2x/4x)" },
};

const AmigaHwRegInfo *
amiga_hw_reg_lookup(uint16_t offset)
{
  uint16_t off = (uint16_t)(offset & 0x1FEu);
  for (size_t i = 0; i < G_N_ELEMENTS(k_hw_regs); i++) {
    if (k_hw_regs[i].offset == off) {
      return &k_hw_regs[i];
    }
  }
  static AmigaHwRegInfo dyn_color_regs[32];
  if (off >= 0x180 && off <= 0x1BE) {
    uint16_t idx = (off - 0x180u) / 2u;
    if (idx < 32 && dyn_color_regs[idx].name == NULL) {
      static char col_names[32][12];
      g_snprintf(col_names[idx], sizeof(col_names[idx]), "COLOR%02u", idx);
      dyn_color_regs[idx].offset = off;
      dyn_color_regs[idx].name = col_names[idx];
      dyn_color_regs[idx].access = AMIGA_REG_W;
      dyn_color_regs[idx].summary = "OCS/ECS/AGA color palette entry";
    }
    if (idx < 32) {
      return &dyn_color_regs[idx];
    }
  }
  return NULL;
}

const char *
amiga_lvo_lookup(const char *lib_hint, int16_t lvo_offset)
{
  static const struct {
    const char *lib;
    int16_t     off;
    const char *name;
  } k_lvos[] = {
    { "exec", -30,  "_LVOSupervisor" },
    { "exec", -132, "_LVOForbid" },
    { "exec", -138, "_LVOPermit" },
    { "exec", -144, "_LVODisable" },
    { "exec", -150, "_LVOEnable" },
    { "exec", -198, "_LVOAllocMem" },
    { "exec", -210, "_LVOFreeMem" },
    { "exec", -318, "_LVOWait" },
    { "exec", -324, "_LVOSignal" },
    { "exec", -408, "_LVOCloseLibrary" },
    { "exec", -444, "_LVOOpenDevice" },
    { "exec", -450, "_LVOCloseDevice" },
    { "exec", -552, "_LVOOpenLibrary" },
    { "dos",  -30,  "_LVOOpen" },
    { "dos",  -36,  "_LVOClose" },
    { "dos",  -42,  "_LVORead" },
    { "dos",  -48,  "_LVOWrite" },
    { "dos",  -150, "_LVOLoadSeg" },
    { "dos",  -198, "_LVODelay" },
    { "gfx",  -228, "_LVOWaitBlit" },
    { "gfx",  -270, "_LVOWaitTOF" },
    { "gfx",  -456, "_LVOOwnBlitter" },
    { "gfx",  -462, "_LVODisownBlitter" },
  };
  for (size_t i = 0; i < G_N_ELEMENTS(k_lvos); i++) {
    if (k_lvos[i].off == lvo_offset &&
        (!lib_hint || g_ascii_strcasecmp(lib_hint, k_lvos[i].lib) == 0)) {
      return k_lvos[i].name;
    }
  }
  return NULL;
}

AmigaBltcon0Info
amiga_decode_bltcon0(uint16_t bltcon0)
{
  AmigaBltcon0Info info;
  memset(&info, 0, sizeof(info));
  info.lf_byte = (uint16_t)(bltcon0 & 0xFFu);
  info.ash = (uint8_t)((bltcon0 >> 12) & 0xFu);
  info.use_a = (bltcon0 & 0x0800u) != 0;
  info.use_b = (bltcon0 & 0x0400u) != 0;
  info.use_c = (bltcon0 & 0x0200u) != 0;
  info.use_d = (bltcon0 & 0x0100u) != 0;

  switch (info.lf_byte) {
    case 0x00: g_strlcpy(info.equation, "D = 0 (clear)", sizeof(info.equation)); break;
    case 0xF0: g_strlcpy(info.equation, "D = A (copy A)", sizeof(info.equation)); break;
    case 0xCC: g_strlcpy(info.equation, "D = B (copy B)", sizeof(info.equation)); break;
    case 0xAA: g_strlcpy(info.equation, "D = C (copy C)", sizeof(info.equation)); break;
    case 0xCA: g_strlcpy(info.equation, "D = (A&B)|(C&~A) cookie-cut", sizeof(info.equation)); break;
    case 0xEA: g_strlcpy(info.equation, "D = A|C merge", sizeof(info.equation)); break;
    case 0x4A: g_strlcpy(info.equation, "D = A^C XOR/line", sizeof(info.equation)); break;
    case 0xFF: g_strlcpy(info.equation, "D = $FF (fill)", sizeof(info.equation)); break;
    default:
      g_snprintf(info.equation, sizeof(info.equation), "LF=$%02X", info.lf_byte);
      break;
  }
  return info;
}

void
amiga_decode_ocs_rgb12(uint16_t rgb12, uint8_t *r, uint8_t *g, uint8_t *b)
{
  uint8_t r4 = (uint8_t)((rgb12 >> 8) & 0xFu);
  uint8_t g4 = (uint8_t)((rgb12 >> 4) & 0xFu);
  uint8_t b4 = (uint8_t)(rgb12 & 0xFu);
  if (r) *r = (uint8_t)((r4 << 4) | r4);
  if (g) *g = (uint8_t)((g4 << 4) | g4);
  if (b) *b = (uint8_t)((b4 << 4) | b4);
}

/* ─── Copperlist Disassembler & Scanner ─── */

static uint16_t
read_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

size_t
amiga_copper_disasm(const uint8_t *buf,
                    size_t         len,
                    uint32_t       base_addr,
                    size_t         max_insns,
                    AmigaCopInsn  *out_insns)
{
  if (!buf || !out_insns || len < 4 || max_insns == 0) {
    return 0;
  }
  size_t count = 0;
  for (size_t off = 0; off + 3 < len && count < max_insns; off += 4) {
    uint16_t ir1 = read_be16(buf + off);
    uint16_t ir2 = read_be16(buf + off + 2);
    AmigaCopInsn *in = &out_insns[count++];
    memset(in, 0, sizeof(*in));
    in->addr = base_addr + (uint32_t)off;
    in->ir1 = ir1;
    in->ir2 = ir2;

    if (ir1 == 0xFFFFu && ir2 == 0xFFFEu) {
      in->kind = AMIGA_COP_END;
      g_strlcpy(in->text, "WAIT $FF,$FE (END OF COPPERLIST)", sizeof(in->text));
      g_strlcpy(in->note, "END", sizeof(in->note));
      break;
    } else if ((ir1 & 1u) != 0) {
      /* WAIT or SKIP */
      in->vp = (uint8_t)(ir1 >> 8);
      in->hp = (uint8_t)(ir1 & 0xFEu);
      in->bfd = (ir2 & 0x8000u) != 0;
      if ((ir2 & 1u) != 0) {
        in->kind = AMIGA_COP_SKIP;
        g_snprintf(in->text, sizeof(in->text), "SKIP V=$%02X, H=$%02X", in->vp, in->hp);
      } else {
        in->kind = AMIGA_COP_WAIT;
        g_snprintf(in->text, sizeof(in->text), "WAIT V=$%02X, H=$%02X", in->vp, in->hp);
      }
      if (ir1 == 0xFFDFu && ir2 == 0xFFFEu) {
        in->is_line255 = true;
        g_strlcpy(in->note, "LINE 255 BARRIER", sizeof(in->note));
      } else if (!in->bfd) {
        g_strlcpy(in->note, "BFD=0 BLIT SYNC", sizeof(in->note));
      }
    } else {
      /* MOVE reg, val */
      in->kind = AMIGA_COP_MOVE;
      uint16_t reg = ir1 & 0x01FEu;
      const AmigaHwRegInfo *ri = amiga_hw_reg_lookup(reg);
      const char *rname = ri ? ri->name : NULL;
      if (rname) {
        g_snprintf(in->text, sizeof(in->text), "MOVE %s ($%03X), #$%04X", rname, reg, ir2);
      } else {
        g_snprintf(in->text, sizeof(in->text), "MOVE $DFF%03X, #$%04X", reg, ir2);
      }
      if (reg >= 0x040 && reg <= 0x07E) {
        in->is_blitter_reg = true;
        g_strlcpy(in->note, "REQUIRES COPCON $0002", sizeof(in->note));
      } else if (reg >= 0x0E0 || (reg >= 0x08E && reg <= 0x096)) {
        in->is_display_reg = true;
      }
    }
  }
  return count;
}

size_t
amiga_copper_scan_chipram(const uint8_t *chip_ram,
                          size_t         chip_len,
                          uint32_t      *out_addrs,
                          size_t         max_hits)
{
  if (!chip_ram || !out_addrs || chip_len < 16 || max_hits == 0) {
    return 0;
  }
  size_t hits = 0;
  for (size_t off = 0; off + 8 <= chip_len && hits < max_hits; off += 2) {
    /* Look for plausible Copper MOVE/WAIT chains ending in $FFFFFFFE within 4KB */
    uint16_t w0 = read_be16(chip_ram + off);
    uint16_t w1 = read_be16(chip_ram + off + 2);
    if ((w0 & 1u) != 0) {
      continue; /* usually lists start with a MOVE or at least skip invalid $0000/$FFFF */
    }
    uint16_t reg0 = w0 & 0x01FEu;
    if (w0 != reg0 || (reg0 < 0x040 || reg0 > 0x1FEu)) {
      continue;
    }
    if (reg0 == 0x100 && (w1 & 0x8000u)) {
      /* good display setup start, e.g. BPLCON0 */
    }
    size_t valid_moves = 0;
    size_t scan = off;
    bool found_end = false;
    while (scan + 4 <= chip_len && (scan - off) < 4096) {
      uint16_t ir1 = read_be16(chip_ram + scan);
      uint16_t ir2 = read_be16(chip_ram + scan + 2);
      if (ir1 == 0xFFFFu && ir2 == 0xFFFEu) {
        found_end = true;
        break;
      }
      if ((ir1 & 1u) == 0) {
        if ((ir1 & 0xFE01u) != 0 || ir1 < 0x020) {
          break;
        }
        valid_moves++;
      } else {
        /* WAIT/SKIP: bit 0 of ir2 usually 0 or 1, mask bits 7..0 of ir2 valid */
        if ((ir2 & 0x7FFEu) == 0) {
          break;
        }
      }
      scan += 4;
    }
    if (found_end && valid_moves >= 2) {
      out_addrs[hits++] = (uint32_t)off;
      off = scan + 2; /* skip past this list */
    }
  }
  return hits;
}

/* ─── Bitplanes Renderer & Probe ─── */

static uint8_t
sample_pixel_raw(const uint8_t             *chip_ram,
                 size_t                     chip_len,
                 const AmigaBitplaneParams *p,
                 uint16_t                   x,
                 uint16_t                   y,
                 uint32_t                  *out_p0_addr)
{
  uint16_t eff_y = (p->layout == AMIGA_BPL_LINE_2X) ? (uint16_t)(y / 2u) : y;
  uint32_t bpr = (uint32_t)((p->width + 7u) / 8u);
  uint32_t stride = bpr;
  if (p->layout == AMIGA_BPL_INTERLEAVED) {
    stride = bpr * MAX(1u, p->planes);
  }
  stride = (uint32_t)((int32_t)stride + (int32_t)p->bpl1mod);
  if (stride == 0) {
    stride = bpr;
  }

  uint8_t bit = (uint8_t)(7u - (x & 7u));
  uint32_t byte_in_row = (uint32_t)(x >> 3);
  uint8_t idx = 0;

  for (uint8_t pl = 0; pl < p->planes && pl < 6; pl++) {
    uint32_t base = p->plane_ptrs[pl];
    if (base == 0 && pl > 0 && p->plane_ptrs[0] != 0) {
      base = p->plane_ptrs[0] + (p->layout == AMIGA_BPL_INTERLEAVED ? pl * bpr : pl * bpr * p->height);
    }
    uint32_t addr = base + (uint32_t)eff_y * stride + byte_in_row;
    if (pl == 0 && out_p0_addr) {
      *out_p0_addr = addr;
    }
    if ((p->plane_mask & (1u << pl)) == 0) {
      continue;
    }
    if (addr < chip_len) {
      uint8_t b = chip_ram[addr];
      if ((b >> bit) & 1u) {
        idx |= (uint8_t)(1u << pl);
      }
    }
  }
  return idx;
}

bool
amiga_bitplane_render(const uint8_t             *chip_ram,
                      size_t                     chip_len,
                      const AmigaBitplaneParams *p,
                      uint8_t                   *out_rgba)
{
  if (!chip_ram || !p || !out_rgba || p->width == 0 || p->height == 0 || p->planes == 0 || p->planes > 6) {
    return false;
  }

  for (uint16_t y = 0; y < p->height; y++) {
    uint8_t cur_r = 0, cur_g = 0, cur_b = 0;
    amiga_decode_ocs_rgb12(p->palette_ocs[0], &cur_r, &cur_g, &cur_b);
    for (uint16_t x = 0; x < p->width; x++) {
      uint8_t raw = sample_pixel_raw(chip_ram, chip_len, p, x, y, NULL);
      uint8_t r = 0, g = 0, b = 0;
      if (p->ham6 && p->planes == 6) {
        uint8_t ctrl = (uint8_t)((raw >> 4) & 0x3u);
        uint8_t val = (uint8_t)(raw & 0xFu);
        if (ctrl == 0) {
          amiga_decode_ocs_rgb12(p->palette_ocs[val], &cur_r, &cur_g, &cur_b);
        } else if (ctrl == 1) {
          cur_b = (uint8_t)((val << 4) | val);
        } else if (ctrl == 2) {
          cur_r = (uint8_t)((val << 4) | val);
        } else {
          cur_g = (uint8_t)((val << 4) | val);
        }
        r = cur_r; g = cur_g; b = cur_b;
      } else if (p->ehb && p->planes == 6 && raw >= 32) {
        amiga_decode_ocs_rgb12(p->palette_ocs[raw - 32], &r, &g, &b);
        r >>= 1; g >>= 1; b >>= 1;
      } else {
        amiga_decode_ocs_rgb12(p->palette_ocs[raw & 31u], &r, &g, &b);
      }
      size_t off = ((size_t)y * p->width + x) * 4u;
      out_rgba[off + 0] = r;
      out_rgba[off + 1] = g;
      out_rgba[off + 2] = b;
      out_rgba[off + 3] = 255;
    }
  }
  return true;
}

bool
amiga_bitplane_probe_pixel(const uint8_t             *chip_ram,
                           size_t                     chip_len,
                           const AmigaBitplaneParams *p,
                           uint16_t                   x,
                           uint16_t                   y,
                           AmigaPixelProbe           *out_probe)
{
  if (!chip_ram || !p || !out_probe || x >= p->width || y >= p->height || p->planes == 0) {
    return false;
  }
  memset(out_probe, 0, sizeof(*out_probe));
  uint32_t addr0 = 0;
  uint8_t raw = sample_pixel_raw(chip_ram, chip_len, p, x, y, &addr0);
  out_probe->x = x;
  out_probe->y = y;
  out_probe->plane_bits = raw;
  out_probe->color_index = raw;
  out_probe->ocs_rgb = p->palette_ocs[raw & 31u];
  out_probe->chip_addr = addr0;
  out_probe->bit_index = (uint8_t)(7u - (x & 7u));
  return true;
}

/* ─── Parameterized DMA Budget Math ─── */

AmigaDmaBudget
amiga_dma_calc_budget(const AmigaDmaInput *in)
{
  AmigaDmaBudget b;
  memset(&b, 0, sizeof(b));
  if (!in) {
    return b;
  }
  const uint16_t odd_per_line = 113; /* 227 color clocks -> 113 odd DMA slots */
  const uint16_t visible_lines = 256;
  const uint16_t vblank_lines = in->ntsc ? 6 : 56; /* 262 NTSC vs 312 PAL */

  b.total_color_clocks_line = 227;
  uint8_t pl = CLAMP(in->bitplanes, 0, 6);
  uint16_t words_per_line = in->hires ? 40 : 20;
  b.bpl_slots_line = (uint16_t)(pl * words_per_line);
  uint16_t fixed_line = (uint16_t)(4 + 3 + MIN(in->audio_channels, 4) + MIN(in->sprites_active, 8) * 2);
  uint16_t used_vis = (uint16_t)(b.bpl_slots_line + MIN(odd_per_line, fixed_line));
  b.free_odd_slots_visible_line = (odd_per_line > used_vis) ? (uint16_t)(odd_per_line - used_vis) : 0;
  uint16_t free_vbl = (odd_per_line > fixed_line) ? (uint16_t)(odd_per_line - fixed_line) : 0;

  b.avail_odd_slots_frame = (uint32_t)visible_lines * b.free_odd_slots_visible_line +
                            (uint32_t)vblank_lines * free_vbl;
  uint8_t ch = CLAMP(in->blitter_ch, 1, 4);
  b.blitter_slots_frame = (uint32_t)in->blits_per_frame * in->blit_w_words * in->blit_h_lines * ch;
  b.copper_slots_frame = (uint32_t)in->copper_moves_per_frame * 2u;
  b.cop_slots_line = (uint16_t)(b.copper_slots_frame / (visible_lines + vblank_lines));
  b.used_odd_slots_frame = b.blitter_slots_frame + b.copper_slots_frame;
  b.odd_slot_margin = (int32_t)b.avail_odd_slots_frame - (int32_t)b.used_odd_slots_frame;
  b.cpu_even_cycles_frame = (uint32_t)(visible_lines + vblank_lines) * 227u; /* ~70,824 PAL cycles * 2 color clocks */
  b.odd_utilization_pct = b.avail_odd_slots_frame > 0
                              ? (100.0 * (double)b.used_odd_slots_frame / (double)b.avail_odd_slots_frame)
                              : 100.0;
  return b;
}
