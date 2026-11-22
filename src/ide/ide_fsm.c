#include "ide_fsm_private.h"

#include <string.h>

#define IDE_LEADER_TIMEOUT_USEC (2 * G_USEC_PER_SEC)
#define IDE_JK_EXIT_MAX_USEC    (200 * 1000)

struct _IdeFsm {
  IdeState    state;
  IdeState    previous_state;
  char        leader_buf[16];
  char        match_buf[8];
  char        last_char[16];
  gint64      last_input_usec;
  gint64      leader_timeout_usec;
  gboolean    drawer_editable;
  gboolean    tree_editable;
  GHashTable *remaps[IDE_STATE_COUNT];
};

typedef struct {
  const char   *chord;
  IdeState      next_state;
  IdeActionType action;
} LeaderBinding;

static gboolean
is_space_key(const char *key)
{
  return (g_strcmp0(key, "Space") == 0 ||
          g_strcmp0(key, "space") == 0 ||
          g_strcmp0(key, " ") == 0);
}

static gboolean
is_pgup_key(const char *key)
{
  return (g_strcmp0(key, "pgup") == 0 || g_strcmp0(key, "pageup") == 0);
}

static gboolean
is_pgdown_key(const char *key)
{
  return (g_strcmp0(key, "pgdown") == 0 ||
          g_strcmp0(key, "pgdn") == 0 ||
          g_strcmp0(key, "pagedown") == 0);
}

static IdeAction
ide_action_make(IdeActionType type)
{
  IdeAction act;
  memset(&act, 0, sizeof(act));
  act.type = type;
  return act;
}

static IdeAction
ide_action_make_str(IdeActionType type, const char *s)
{
  IdeAction act = ide_action_make(type);
  if (s != NULL) {
    g_strlcpy(act.str, s, sizeof(act.str));
  }
  return act;
}

static IdeAction
ide_action_make_int(IdeActionType type, int v)
{
  IdeAction act = ide_action_make(type);
  act.int_val = v;
  return act;
}

static void
ide_fsm_clear_buffers(IdeFsm *fsm)
{
  fsm->leader_buf[0] = '\0';
  fsm->match_buf[0] = '\0';
  fsm->last_char[0] = '\0';
}

static gboolean
is_printable_single_glyph(const char *s)
{
  if (s == NULL || s[0] == '\0') {
    return FALSE;
  }
  if (!g_utf8_validate(s, -1, NULL)) {
    return FALSE;
  }
  if (g_utf8_strlen(s, -1) != 1) {
    return FALSE;
  }
  gunichar ch = g_utf8_get_char(s);
  if (ch < 32 && ch != '\t' && ch != '\n') {
    return FALSE;
  }
  return TRUE;
}

static gboolean
match_key_table(const char *key, const IdeFsmKeyBinding *table, gsize count, IdeAction *out)
{
  IdeActionType act = IDE_ACTION_NONE;
  if (ide_fsm_lookup_key_table(key, table, count, &act)) {
    *out = ide_action_make(act);
    return TRUE;
  }
  return FALSE;
}

static gboolean
match_shift_direction(IdeFsm *fsm, const char *key, IdeAction *out)
{
  static const struct {
    const char *key;
    const char *dir;
  } kDirs[] = {
    { "shift+left",  "left" },
    { "shift+right", "right" },
    { "shift+up",    "up" },
    { "shift+down",  "down" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(kDirs); i++) {
    if (g_strcmp0(key, kDirs[i].key) == 0) {
      fsm->state = IDE_STATE_SELECT;
      *out = ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, kDirs[i].dir);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
match_leader_table(IdeFsm *fsm, const char *b, const LeaderBinding *table, gsize count, IdeAction *out)
{
  for (gsize i = 0; i < count; i++) {
    if (g_strcmp0(b, table[i].chord) == 0) {
      fsm->state = table[i].next_state;
      fsm->leader_buf[0] = '\0';
      *out = ide_action_make(table[i].action);
      return TRUE;
    }
  }
  return FALSE;
}

/* ═══════════════════════════ lifecycle & accessors ═══════════════════════════ */

IdeFsm *
ide_fsm_new(void)
{
  IdeFsm *fsm = g_new0(IdeFsm, 1);
  fsm->state = IDE_STATE_NORMAL;
  fsm->previous_state = IDE_STATE_NORMAL;
  fsm->leader_timeout_usec = IDE_LEADER_TIMEOUT_USEC;
  return fsm;
}

void
ide_fsm_free(IdeFsm *fsm)
{
  if (fsm == NULL) {
    return;
  }
  ide_fsm_clear_remaps(fsm);
  g_free(fsm);
}

IdeState
ide_fsm_get_state(const IdeFsm *fsm)
{
  g_return_val_if_fail(fsm != NULL, IDE_STATE_NORMAL);
  return fsm->state;
}

IdeState
ide_fsm_get_previous_state(const IdeFsm *fsm)
{
  g_return_val_if_fail(fsm != NULL, IDE_STATE_NORMAL);
  return fsm->previous_state;
}

void
ide_fsm_set_state(IdeFsm *fsm, IdeState state)
{
  g_return_if_fail(fsm != NULL);
  if (fsm->state != state) {
    fsm->previous_state = fsm->state;
  }
  fsm->state = state;
  if (state != IDE_STATE_DRAWER) {
    fsm->drawer_editable = FALSE;
  }
  if (state != IDE_STATE_TREE) {
    fsm->tree_editable = FALSE;
  }
  ide_fsm_clear_buffers(fsm);
}

const char *
ide_fsm_get_pending_chord(const IdeFsm *fsm)
{
  g_return_val_if_fail(fsm != NULL, "");
  if (fsm->state == IDE_STATE_MATCH_PENDING) {
    return fsm->match_buf;
  }
  if (fsm->state == IDE_STATE_LEADER || fsm->leader_buf[0] != '\0') {
    return fsm->leader_buf;
  }
  if ((fsm->state == IDE_STATE_NORMAL ||
       fsm->state == IDE_STATE_SELECT ||
       fsm->state == IDE_STATE_TREE) &&
      g_strcmp0(fsm->last_char, "g") == 0) {
    return "g";
  }
  return "";
}

guint
ide_fsm_get_leader_timeout_ms(const IdeFsm *fsm)
{
  g_return_val_if_fail(fsm != NULL, 2000);
  return (guint)(fsm->leader_timeout_usec / 1000);
}

gboolean
ide_fsm_expire_leader(IdeFsm *fsm)
{
  g_return_val_if_fail(fsm != NULL, FALSE);
  if (fsm->state != IDE_STATE_LEADER) {
    return FALSE;
  }
  fsm->state = IDE_STATE_NORMAL;
  ide_fsm_clear_buffers(fsm);
  return TRUE;
}

gboolean
ide_fsm_tree_prompt_active(const IdeFsm *fsm, gboolean prompt_visible)
{
  g_return_val_if_fail(fsm != NULL, FALSE);
  return fsm->tree_editable || prompt_visible;
}

gboolean
ide_fsm_get_drawer_editable(const IdeFsm *fsm)
{
  g_return_val_if_fail(fsm != NULL, FALSE);
  return fsm->drawer_editable;
}

void
ide_fsm_set_drawer_editable(IdeFsm *fsm, gboolean editable)
{
  g_return_if_fail(fsm != NULL);
  fsm->drawer_editable = editable;
}

gboolean
ide_fsm_get_tree_editable(const IdeFsm *fsm)
{
  g_return_val_if_fail(fsm != NULL, FALSE);
  return fsm->tree_editable;
}

void
ide_fsm_set_tree_editable(IdeFsm *fsm, gboolean editable)
{
  g_return_if_fail(fsm != NULL);
  fsm->tree_editable = editable;
}

void
ide_fsm_set_remap(IdeFsm        *fsm,
                  IdeState       state,
                  const char    *key,
                  IdeActionType  action)
{
  g_return_if_fail(fsm != NULL);
  g_return_if_fail(state >= 0 && state < IDE_STATE_COUNT);
  g_return_if_fail(key != NULL && key[0] != '\0');

  if (action == IDE_ACTION_NONE) {
    return;
  }
  if (fsm->remaps[state] == NULL) {
    fsm->remaps[state] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }
  g_hash_table_replace(fsm->remaps[state], g_strdup(key), GINT_TO_POINTER((int)action));
}

void
ide_fsm_clear_remaps(IdeFsm *fsm)
{
  g_return_if_fail(fsm != NULL);
  for (int i = 0; i < IDE_STATE_COUNT; i++) {
    g_clear_pointer(&fsm->remaps[i], g_hash_table_destroy);
  }
}

static gboolean
ide_fsm_lookup_remap(IdeFsm *fsm, IdeState state, const char *key, IdeActionType *out_act)
{
  if (state < 0 || state >= IDE_STATE_COUNT || fsm->remaps[state] == NULL) {
    return FALSE;
  }
  gpointer val = g_hash_table_lookup(fsm->remaps[state], key);
  if (val != NULL) {
    *out_act = (IdeActionType)GPOINTER_TO_INT(val);
    return TRUE;
  }
  g_autofree char *lower = g_ascii_strdown(key, -1);
  val = g_hash_table_lookup(fsm->remaps[state], lower);
  if (val != NULL) {
    *out_act = (IdeActionType)GPOINTER_TO_INT(val);
    return TRUE;
  }
  return FALSE;
}

/* ═══════════════════════════ NORMAL / INSERT / SELECT ═══════════════════════════ */

static gboolean
handle_normal_mode_transition(IdeFsm *fsm, const char *key, IdeAction *out)
{
  if (g_strcmp0(key, "o") == 0) {
    fsm->state = IDE_STATE_INSERT;
    *out = ide_action_make_str(IDE_ACTION_INSERT_NEWLINE, "below");
    return TRUE;
  }
  if (g_strcmp0(key, "O") == 0) {
    fsm->state = IDE_STATE_INSERT;
    *out = ide_action_make_str(IDE_ACTION_INSERT_NEWLINE, "above");
    return TRUE;
  }
  if (match_shift_direction(fsm, key, out)) {
    return TRUE;
  }
  if (is_space_key(key)) {
    fsm->previous_state = IDE_STATE_NORMAL;
    fsm->state = IDE_STATE_LEADER;
    fsm->leader_buf[0] = '\0';
    *out = ide_action_make(IDE_ACTION_NONE);
    return TRUE;
  }
  if (g_strcmp0(key, "m") == 0) {
    fsm->state = IDE_STATE_MATCH_PENDING;
    fsm->match_buf[0] = '\0';
    *out = ide_action_make(IDE_ACTION_NONE);
    return TRUE;
  }

  static const struct {
    const char   *key;
    IdeState      next_state;
    IdeActionType action;
  } kTransitions[] = {
    { "i",  IDE_STATE_INSERT, IDE_ACTION_NONE },
    { "I",  IDE_STATE_INSERT, IDE_ACTION_MOVE_LINE_START },
    { "a",  IDE_STATE_INSERT, IDE_ACTION_MOVE_RIGHT },
    { "A",  IDE_STATE_INSERT, IDE_ACTION_MOVE_LINE_END },
    { "c",  IDE_STATE_INSERT, IDE_ACTION_CHANGE_SELECTION },
    { "w",  IDE_STATE_SELECT, IDE_ACTION_SELECT_WORD },
    { "b",  IDE_STATE_SELECT, IDE_ACTION_SELECT_WORD_BACKWARD },
    { "x",  IDE_STATE_SELECT, IDE_ACTION_SELECT_LINE },
    { "v",  IDE_STATE_SELECT, IDE_ACTION_NONE },
    { "/",  IDE_STATE_SEARCH, IDE_ACTION_SEARCH_OPEN },
    { "?",  IDE_STATE_HELP,   IDE_ACTION_HELP_TOGGLE },
    { "f1", IDE_STATE_HELP,   IDE_ACTION_HELP_TOGGLE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(kTransitions); i++) {
    if (g_strcmp0(key, kTransitions[i].key) == 0) {
      fsm->state = kTransitions[i].next_state;
      *out = ide_action_make(kTransitions[i].action);
      return TRUE;
    }
  }
  return FALSE;
}

static IdeAction
handle_normal(IdeFsm *fsm, const char *key)
{
  IdeAction act;

  /* Two-key 'g' navigation chords (gg, gh, gl, gd, gr) */
  if (g_strcmp0(fsm->last_char, "g") == 0) {
    static const IdeFsmKeyBinding kGChords[] = {
      { "g", IDE_ACTION_MOVE_BUFFER_START },
      { "h", IDE_ACTION_MOVE_LINE_START },
      { "l", IDE_ACTION_MOVE_LINE_END },
      { "d", IDE_ACTION_LSP_DEFINITION },
      { "r", IDE_ACTION_LSP_REFERENCES },
    };
    if (match_key_table(key, kGChords, G_N_ELEMENTS(kGChords), &act)) {
      return act;
    }
  }

  if (handle_normal_mode_transition(fsm, key, &act)) {
    return act;
  }

  IdeActionType mapped = IDE_ACTION_NONE;
  if (ide_fsm_lookup_normal_key(key, &mapped)) {
    return ide_action_make(mapped);
  }

  /* Alt+1 .. Alt+9 direct tab switching */
  if (g_str_has_prefix(key, "alt+") && strlen(key) == 5 &&
      key[4] >= '1' && key[4] <= '9') {
    return ide_action_make_int(IDE_ACTION_TAB_SWITCH, key[4] - '0');
  }

  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_insert(IdeFsm *fsm, const char *key, gint64 now_usec)
{
  /* Fast home-row exit: "j" followed by "k" within < 200ms */
  if (g_strcmp0(key, "k") == 0 && g_strcmp0(fsm->last_char, "j") == 0 &&
      fsm->last_input_usec > 0 &&
      (now_usec - fsm->last_input_usec) < IDE_JK_EXIT_MAX_USEC) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_BACKSPACE);
  }

  if (g_strcmp0(key, "esc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_MOVE_LEFT);
  }

  IdeAction act;
  if (match_shift_direction(fsm, key, &act)) {
    return act;
  }

  IdeActionType mapped = IDE_ACTION_NONE;
  if (ide_fsm_lookup_insert_key(key, &mapped)) {
    return ide_action_make(mapped);
  }

  if (is_space_key(key)) {
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, " ");
  }

  /* Alt+1 .. Alt+9 direct tab switching */
  if (g_str_has_prefix(key, "alt+") && strlen(key) == 5 &&
      key[4] >= '1' && key[4] <= '9') {
    return ide_action_make_int(IDE_ACTION_TAB_SWITCH, key[4] - '0');
  }

  if (is_printable_single_glyph(key)) {
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, key);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_select(IdeFsm *fsm, const char *key)
{
  IdeAction act;

  /* Two-key 'g' chords in SELECT mode extend selection to target */
  if (g_strcmp0(fsm->last_char, "g") == 0) {
    static const IdeFsmKeyBinding kSelectG[] = {
      { "g", IDE_ACTION_MOVE_BUFFER_START },
      { "h", IDE_ACTION_MOVE_LINE_START },
      { "l", IDE_ACTION_MOVE_LINE_END },
    };
    if (match_key_table(key, kSelectG, G_N_ELEMENTS(kSelectG), &act)) {
      return act;
    }
  }

  if (is_space_key(key)) {
    fsm->previous_state = IDE_STATE_SELECT;
    fsm->state = IDE_STATE_LEADER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }
  if (g_strcmp0(key, "m") == 0) {
    fsm->state = IDE_STATE_MATCH_PENDING;
    fsm->match_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }

  static const struct {
    const char   *key;
    IdeState      next_state;
    IdeActionType action;
  } kExitKeys[] = {
    { "esc",          IDE_STATE_NORMAL, IDE_ACTION_COLLAPSE_SELECTION },
    { "v",            IDE_STATE_NORMAL, IDE_ACTION_COLLAPSE_SELECTION },
    { "d",            IDE_STATE_NORMAL, IDE_ACTION_DELETE_SELECTION },
    { "delete",       IDE_STATE_NORMAL, IDE_ACTION_DELETE_SELECTION },
    { "backspace",    IDE_STATE_NORMAL, IDE_ACTION_DELETE_SELECTION },
    { "ctrl+x",       IDE_STATE_NORMAL, IDE_ACTION_DELETE_SELECTION },
    { "c",            IDE_STATE_INSERT, IDE_ACTION_CHANGE_SELECTION },
    { "i",            IDE_STATE_INSERT, IDE_ACTION_CHANGE_SELECTION },
    { "y",            IDE_STATE_NORMAL, IDE_ACTION_YANK },
    { "ctrl+c",       IDE_STATE_NORMAL, IDE_ACTION_YANK },
    { "p",            IDE_STATE_NORMAL, IDE_ACTION_PASTE_AFTER },
    { "ctrl+v",       IDE_STATE_NORMAL, IDE_ACTION_PASTE_AFTER },
    { "u",            IDE_STATE_NORMAL, IDE_ACTION_UNDO },
    { "ctrl+z",       IDE_STATE_NORMAL, IDE_ACTION_UNDO },
    { "U",            IDE_STATE_NORMAL, IDE_ACTION_REDO },
    { "ctrl+y",       IDE_STATE_NORMAL, IDE_ACTION_REDO },
    { "ctrl+r",       IDE_STATE_NORMAL, IDE_ACTION_REDO },
    { "ctrl+shift+z", IDE_STATE_NORMAL, IDE_ACTION_REDO },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(kExitKeys); i++) {
    if (g_strcmp0(key, kExitKeys[i].key) == 0) {
      fsm->state = kExitKeys[i].next_state;
      return ide_action_make(kExitKeys[i].action);
    }
  }

  if (g_strcmp0(key, "h") == 0 || g_strcmp0(key, "left") == 0 ||
      g_strcmp0(key, "shift+left") == 0) {
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "left");
  }
  if (g_strcmp0(key, "l") == 0 || g_strcmp0(key, "right") == 0 ||
      g_strcmp0(key, "shift+right") == 0) {
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "right");
  }
  if (g_strcmp0(key, "j") == 0 || g_strcmp0(key, "down") == 0 ||
      g_strcmp0(key, "shift+down") == 0) {
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "down");
  }
  if (g_strcmp0(key, "k") == 0 || g_strcmp0(key, "up") == 0 ||
      g_strcmp0(key, "shift+up") == 0) {
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "up");
  }

  IdeActionType mapped = IDE_ACTION_NONE;
  if (ide_fsm_lookup_select_key(key, &mapped)) {
    return ide_action_make(mapped);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_match(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0 || strlen(key) != 1) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->match_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }

  gsize len = strlen(fsm->match_buf);
  if (len == 0) {
    if (key[0] != 'i' && key[0] != 'a') {
      fsm->state = IDE_STATE_NORMAL;
      fsm->match_buf[0] = '\0';
      return ide_action_make(IDE_ACTION_NONE);
    }
    fsm->match_buf[0] = key[0];
    fsm->match_buf[1] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }

  if (len == 1) {
    IdeAction act = ide_action_make(IDE_ACTION_SELECT_TEXT_OBJECT);
    act.arg1[0] = fsm->match_buf[0];
    act.arg1[1] = '\0';
    act.arg2[0] = key[0];
    act.arg2[1] = '\0';
    fsm->state = IDE_STATE_SELECT;
    fsm->match_buf[0] = '\0';
    return act;
  }

  fsm->state = IDE_STATE_NORMAL;
  fsm->match_buf[0] = '\0';
  return ide_action_make(IDE_ACTION_NONE);
}

/* ═══════════════════════════ LEADER chords ═══════════════════════════ */

static gboolean
handle_leader_file(IdeFsm *fsm, const char *b, IdeAction *out)
{
  static const LeaderBinding kFileChords[] = {
    { "ff", IDE_STATE_PICKER, IDE_ACTION_PICKER_OPEN_FILES },
    { "fg", IDE_STATE_PICKER, IDE_ACTION_PICKER_OPEN_GREP },
    { "/",  IDE_STATE_PICKER, IDE_ACTION_PICKER_OPEN_GREP },
    { "bb", IDE_STATE_PICKER, IDE_ACTION_PICKER_OPEN_BUFFERS },
    { ",",  IDE_STATE_PICKER, IDE_ACTION_PICKER_OPEN_BUFFERS },
    { "bd", IDE_STATE_NORMAL, IDE_ACTION_BUFFER_CLOSE },
    { "q",  IDE_STATE_NORMAL, IDE_ACTION_BUFFER_CLOSE },
    { "bD", IDE_STATE_NORMAL, IDE_ACTION_BUFFER_FORCE_CLOSE },
    { "Q",  IDE_STATE_NORMAL, IDE_ACTION_BUFFER_FORCE_CLOSE },
    { "fs", IDE_STATE_NORMAL, IDE_ACTION_SAVE },
    { "fa", IDE_STATE_NORMAL, IDE_ACTION_SAVE_ALL },
  };
  return match_leader_table(fsm, b, kFileChords, G_N_ELEMENTS(kFileChords), out);
}

static gboolean
handle_leader_layout(IdeFsm *fsm, const char *b, IdeAction *out)
{
  if (g_strcmp0(b, "e") == 0) {
    fsm->state = (fsm->previous_state == IDE_STATE_TREE) ? IDE_STATE_NORMAL : IDE_STATE_TREE;
    fsm->leader_buf[0] = '\0';
    *out = ide_action_make(IDE_ACTION_TOGGLE_FILE_TREE);
    return TRUE;
  }
  static const LeaderBinding kLayoutChords[] = {
    { "o",  IDE_STATE_DRAWER, IDE_ACTION_TOGGLE_DRAWER },
    { "?",  IDE_STATE_HELP,   IDE_ACTION_HELP_TOGGLE },
    { "h",  IDE_STATE_HELP,   IDE_ACTION_HELP_TOGGLE },
    { "mp", IDE_STATE_NORMAL, IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW },
    { "mt", IDE_STATE_NORMAL, IDE_ACTION_TOGGLE_CHECKLIST },
    { "md", IDE_STATE_NORMAL, IDE_ACTION_TOGGLE_MERMAID_MODE },
    { "mm", IDE_STATE_NORMAL, IDE_ACTION_TOGGLE_MERMAID_MODE },
    { "a",  IDE_STATE_NORMAL, IDE_ACTION_ALTERNATE_FILE },
    { "vr", IDE_STATE_NORMAL, IDE_ACTION_TOGGLE_RELATIVE_LINES },
    { "k",  IDE_STATE_NORMAL, IDE_ACTION_LSP_HOVER },
  };
  return match_leader_table(fsm, b, kLayoutChords, G_N_ELEMENTS(kLayoutChords), out);
}

static gboolean
handle_leader_nav(IdeFsm *fsm, const char *b, IdeAction *out)
{
  static const LeaderBinding kNavChords[] = {
    { "ne", IDE_STATE_NORMAL, IDE_ACTION_NEXT_ERROR },
    { "pe", IDE_STATE_NORMAL, IDE_ACTION_PREV_ERROR },
    { "nd", IDE_STATE_NORMAL, IDE_ACTION_NEXT_DIAGNOSTIC },
    { "pd", IDE_STATE_NORMAL, IDE_ACTION_PREV_DIAGNOSTIC },
    { "nh", IDE_STATE_NORMAL, IDE_ACTION_NEXT_HUNK },
    { "ph", IDE_STATE_NORMAL, IDE_ACTION_PREV_HUNK },
    { "ns", IDE_STATE_NORMAL, IDE_ACTION_SEARCH_NEXT },
    { "ps", IDE_STATE_NORMAL, IDE_ACTION_SEARCH_PREV },
  };
  return match_leader_table(fsm, b, kNavChords, G_N_ELEMENTS(kNavChords), out);
}

static gboolean
handle_leader_debug(IdeFsm *fsm, const char *b, IdeAction *out)
{
  static const LeaderBinding kDebugChords[] = {
    { "ds", IDE_STATE_DIFF_REVIEW, IDE_ACTION_AGENT_REVIEW_OPEN },
    { "da", IDE_STATE_NORMAL,      IDE_ACTION_AGENT_ACCEPT_ALL },
    { "dr", IDE_STATE_NORMAL,      IDE_ACTION_AGENT_REVERT_ALL },
    { "db", IDE_STATE_NORMAL,      IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
    { "dB", IDE_STATE_NORMAL,      IDE_ACTION_DAP_CONDITIONAL_BREAKPOINT },
    { "dd", IDE_STATE_NORMAL,      IDE_ACTION_DAP_START },
    { "dc", IDE_STATE_NORMAL,      IDE_ACTION_DAP_CONTINUE },
    { "dn", IDE_STATE_NORMAL,      IDE_ACTION_DAP_STEP_OVER },
    { "di", IDE_STATE_NORMAL,      IDE_ACTION_DAP_STEP_INTO },
    { "do", IDE_STATE_NORMAL,      IDE_ACTION_DAP_STEP_OUT },
    { "dp", IDE_STATE_NORMAL,      IDE_ACTION_DAP_PAUSE },
    { "dq", IDE_STATE_NORMAL,      IDE_ACTION_DAP_STOP },
    { "dv", IDE_STATE_DRAWER,      IDE_ACTION_DAP_TOGGLE_VIEW },
  };
  return match_leader_table(fsm, b, kDebugChords, G_N_ELEMENTS(kDebugChords), out);
}

static gboolean
handle_leader_lsp(IdeFsm *fsm, const char *b, IdeAction *out)
{
  static const LeaderBinding kLspChords[] = {
    { "ca", IDE_STATE_NORMAL, IDE_ACTION_LSP_CODE_ACTION },
    { "cf", IDE_STATE_NORMAL, IDE_ACTION_LSP_FORMAT },
    { "cr", IDE_STATE_NORMAL, IDE_ACTION_LSP_RENAME },
    { "ch", IDE_STATE_NORMAL, IDE_ACTION_SEARCH_CLEAR_HIGHLIGHT },
    { "rb", IDE_STATE_NORMAL, IDE_ACTION_TASK_BUILD },
    { "rt", IDE_STATE_NORMAL, IDE_ACTION_TASK_TEST },
    { "rr", IDE_STATE_NORMAL, IDE_ACTION_TASK_RUN },
    { "rc", IDE_STATE_NORMAL, IDE_ACTION_TASK_CLEAN },
    { "gd", IDE_STATE_NORMAL, IDE_ACTION_LSP_DEFINITION },
    { "gi", IDE_STATE_NORMAL, IDE_ACTION_LSP_IMPLEMENTATION },
    { "gr", IDE_STATE_NORMAL, IDE_ACTION_LSP_REFERENCES },
    { "gs", IDE_STATE_NORMAL, IDE_ACTION_LSP_SIGNATURE_HELP },
    { "gc", IDE_STATE_NORMAL, IDE_ACTION_LSP_CALL_HIERARCHY_IN },
    { "go", IDE_STATE_NORMAL, IDE_ACTION_LSP_CALL_HIERARCHY_OUT },
  };
  return match_leader_table(fsm, b, kLspChords, G_N_ELEMENTS(kLspChords), out);
}

static gboolean
handle_leader_strip(IdeFsm *fsm, const char *b, IdeAction *out)
{
  if (b[0] != 't' || b[1] == '\0' || b[2] != '\0') {
    return FALSE;
  }
  IdeActionType strip_act = IDE_ACTION_NONE;
  switch (b[1]) {
    case 's': strip_act = IDE_ACTION_STRIP_TOGGLE; break;
    case 'h': strip_act = IDE_ACTION_STRIP_FOCUS_LEFT; break;
    case 'l': strip_act = IDE_ACTION_STRIP_FOCUS_RIGHT; break;
    case 'j': strip_act = IDE_ACTION_STRIP_STEP_DOWN; break;
    case 'k': strip_act = IDE_ACTION_STRIP_STEP_UP; break;
    case 'e': strip_act = IDE_ACTION_STRIP_PROMOTE; break;
    case 'u': strip_act = IDE_ACTION_STRIP_SURFACE; break;
    case 'f': strip_act = IDE_ACTION_STRIP_TOGGLE_FULL_FILE; break;
    case 'z': strip_act = IDE_ACTION_STRIP_TOGGLE_ZEN; break;
    case 'o': strip_act = IDE_ACTION_STRIP_TOGGLE_OVERVIEW; break;
    case 'p': strip_act = IDE_ACTION_STRIP_PIN_TOGGLE; break;
    case 'r': strip_act = IDE_ACTION_STRIP_RESYNC; break;
    case '1': strip_act = IDE_ACTION_STRIP_LENS_CALLS; break;
    case '2': strip_act = IDE_ACTION_STRIP_LENS_TYPES; break;
    case '3': strip_act = IDE_ACTION_STRIP_LENS_IMPACT; break;
    case '4': strip_act = IDE_ACTION_STRIP_LENS_DIFF; break;
    case 'b': strip_act = IDE_ACTION_STRIP_BRANCH_TRAIL; break;
    case '[': strip_act = IDE_ACTION_STRIP_PREV_TRAIL; break;
    case ']': strip_act = IDE_ACTION_STRIP_NEXT_TRAIL; break;
    case '/': strip_act = IDE_ACTION_STRIP_FILTER; break;
    default: break;
  }
  if (strip_act == IDE_ACTION_NONE) {
    return FALSE;
  }
  fsm->state = IDE_STATE_NORMAL;
  fsm->leader_buf[0] = '\0';
  *out = ide_action_make(strip_act);
  return TRUE;
}

static gboolean
handle_leader_window(IdeFsm *fsm, const char *b, IdeAction *out)
{
  static const LeaderBinding kWindowChords[] = {
    { "ws", IDE_STATE_NORMAL, IDE_ACTION_SAVE },
    { "wa", IDE_STATE_NORMAL, IDE_ACTION_SAVE_ALL },
    { "wq", IDE_STATE_NORMAL, IDE_ACTION_BUFFER_FORCE_CLOSE },
    { "w>", IDE_STATE_NORMAL, IDE_ACTION_RESIZE_TREE_GROW },
    { "w<", IDE_STATE_NORMAL, IDE_ACTION_RESIZE_TREE_SHRINK },
    { "w+", IDE_STATE_NORMAL, IDE_ACTION_RESIZE_DRAWER_GROW },
    { "w-", IDE_STATE_NORMAL, IDE_ACTION_RESIZE_DRAWER_SHRINK },
    { "ww", IDE_STATE_NORMAL, IDE_ACTION_PANE_CYCLE },
    { "wh", IDE_STATE_TREE,   IDE_ACTION_PANE_FOCUS_TREE },
    { "wl", IDE_STATE_NORMAL, IDE_ACTION_PANE_FOCUS_BUFFER },
    { "wk", IDE_STATE_NORMAL, IDE_ACTION_PANE_FOCUS_BUFFER },
    { "wj", IDE_STATE_DRAWER, IDE_ACTION_PANE_FOCUS_DRAWER },
  };
  return match_leader_table(fsm, b, kWindowChords, G_N_ELEMENTS(kWindowChords), out);
}

static gboolean
handle_leader_search(IdeFsm *fsm, const char *b, IdeAction *out)
{
  if (g_strcmp0(b, "sr") == 0) {
    fsm->state = IDE_STATE_DRAWER;
    fsm->drawer_editable = TRUE;
    fsm->leader_buf[0] = '\0';
    *out = ide_action_make(IDE_ACTION_REPLACE_OPEN);
    return TRUE;
  }
  if (g_strcmp0(b, "sp") == 0) {
    fsm->state = IDE_STATE_DRAWER;
    fsm->drawer_editable = TRUE;
    fsm->leader_buf[0] = '\0';
    *out = ide_action_make(IDE_ACTION_PROJECT_REPLACE_OPEN);
    return TRUE;
  }
  if (g_strcmp0(b, "sf") == 0 || g_strcmp0(b, "s/") == 0) {
    fsm->state = IDE_STATE_SEARCH;
    fsm->leader_buf[0] = '\0';
    *out = ide_action_make(IDE_ACTION_SEARCH_OPEN);
    return TRUE;
  }
  return FALSE;
}

static IdeAction
handle_leader(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }

  /* Space + Space opens Fuzzy File Picker directly */
  if (fsm->leader_buf[0] == '\0' && is_space_key(key)) {
    fsm->state = IDE_STATE_PICKER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PICKER_OPEN_FILES);
  }

  gsize cur_len = strlen(fsm->leader_buf);
  gsize key_len = strlen(key);
  if (cur_len + key_len >= sizeof(fsm->leader_buf)) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }
  memcpy(fsm->leader_buf + cur_len, key, key_len + 1);

  const char *b = fsm->leader_buf;
  IdeAction act;
  if (handle_leader_file(fsm, b, &act) ||
      handle_leader_layout(fsm, b, &act) ||
      handle_leader_nav(fsm, b, &act) ||
      handle_leader_debug(fsm, b, &act) ||
      handle_leader_lsp(fsm, b, &act) ||
      handle_leader_strip(fsm, b, &act) ||
      handle_leader_window(fsm, b, &act) ||
      handle_leader_search(fsm, b, &act)) {
    return act;
  }

  if (ide_fsm_is_leader_prefix(b)) {
    return ide_action_make(IDE_ACTION_NONE);
  }

  fsm->state = IDE_STATE_NORMAL;
  fsm->leader_buf[0] = '\0';
  return ide_action_make(IDE_ACTION_NONE);
}

/* ═══════════════════════════ DRAWER / TREE / OVERLAYS ═══════════════════════════ */

static IdeAction
handle_drawer_editable(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->drawer_editable = FALSE;
    return ide_action_make(IDE_ACTION_DRAWER_CLOSE);
  }

  IdeActionType mapped = IDE_ACTION_NONE;
  if (ide_fsm_lookup_drawer_edit_key(key, &mapped)) {
    return ide_action_make(mapped);
  }
  if (is_space_key(key)) {
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, " ");
  }
  if (is_printable_single_glyph(key)) {
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, key);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_drawer(IdeFsm *fsm, const char *key)
{
  if (fsm->drawer_editable) {
    return handle_drawer_editable(fsm, key);
  }

  /* Read-only Drawer navigation */
  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "q") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_DRAWER_CLOSE);
  }
  if (g_strcmp0(key, "enter") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_DRAWER_JUMP);
  }
  if (is_space_key(key)) {
    fsm->previous_state = IDE_STATE_DRAWER;
    fsm->state = IDE_STATE_LEADER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }

  IdeActionType mapped = IDE_ACTION_NONE;
  if (ide_fsm_lookup_drawer_key(key, &mapped)) {
    return ide_action_make(mapped);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_tree(IdeFsm *fsm, const char *key)
{
  if (fsm->tree_editable) {
    if (g_strcmp0(key, "esc") == 0) {
      fsm->tree_editable = FALSE;
      return ide_action_make(IDE_ACTION_LSP_CANCEL);
    }
    if (g_strcmp0(key, "enter") == 0) {
      fsm->tree_editable = FALSE;
      return ide_action_make(IDE_ACTION_LSP_SELECT_CONFIRM);
    }
    if (g_strcmp0(key, "backspace") == 0) {
      return ide_action_make(IDE_ACTION_BACKSPACE);
    }
    if (is_space_key(key)) {
      return ide_action_make_str(IDE_ACTION_INSERT_CHAR, " ");
    }
    if (is_printable_single_glyph(key)) {
      return ide_action_make_str(IDE_ACTION_INSERT_CHAR, key);
    }
    return ide_action_make(IDE_ACTION_NONE);
  }

  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "q") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_PANE_FOCUS_BUFFER);
  }
  if (g_strcmp0(key, "g") == 0) {
    if (g_strcmp0(fsm->last_char, "g") == 0) {
      return ide_action_make(IDE_ACTION_TREE_TOP);
    }
    return ide_action_make(IDE_ACTION_NONE);
  }
  if (g_strcmp0(key, "a") == 0) {
    fsm->tree_editable = TRUE;
    return ide_action_make(IDE_ACTION_TREE_CREATE_FILE);
  }
  if (g_strcmp0(key, "A") == 0) {
    fsm->tree_editable = TRUE;
    return ide_action_make(IDE_ACTION_TREE_CREATE_DIR);
  }
  if (is_space_key(key)) {
    fsm->previous_state = IDE_STATE_TREE;
    fsm->state = IDE_STATE_LEADER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }

  IdeActionType mapped = IDE_ACTION_NONE;
  if (ide_fsm_lookup_tree_key(key, &mapped)) {
    return ide_action_make(mapped);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_picker(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_PICKER_CANCEL);
  }
  if (g_strcmp0(key, "enter") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_PICKER_SELECT);
  }
  if (g_strcmp0(key, "down") == 0 || g_strcmp0(key, "ctrl+n") == 0 ||
      g_strcmp0(key, "tab") == 0 || g_strcmp0(key, "ctrl+j") == 0) {
    return ide_action_make(IDE_ACTION_PICKER_NEXT);
  }
  if (g_strcmp0(key, "up") == 0 || g_strcmp0(key, "ctrl+p") == 0 ||
      g_strcmp0(key, "shift+tab") == 0 || g_strcmp0(key, "ctrl+k") == 0) {
    return ide_action_make(IDE_ACTION_PICKER_PREV);
  }
  if (is_pgdown_key(key) || g_strcmp0(key, "ctrl+d") == 0) {
    return ide_action_make(IDE_ACTION_HELP_PAGE_DOWN);
  }
  if (is_pgup_key(key) || g_strcmp0(key, "ctrl+u") == 0) {
    return ide_action_make(IDE_ACTION_HELP_PAGE_UP);
  }
  if (g_strcmp0(key, "backspace") == 0) {
    return ide_action_make(IDE_ACTION_PICKER_BACKSPACE);
  }
  if (is_space_key(key)) {
    return ide_action_make_str(IDE_ACTION_PICKER_INPUT, " ");
  }
  if (is_printable_single_glyph(key)) {
    return ide_action_make_str(IDE_ACTION_PICKER_INPUT, key);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_help(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "q") == 0 ||
      g_strcmp0(key, "?") == 0 || g_strcmp0(key, "f1") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_HELP_CANCEL);
  }
  if (g_strcmp0(key, "enter") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_HELP_SELECT);
  }
  if (g_strcmp0(key, "down") == 0 || g_strcmp0(key, "ctrl+n") == 0 || g_strcmp0(key, "tab") == 0) {
    return ide_action_make(IDE_ACTION_HELP_NEXT);
  }
  if (g_strcmp0(key, "up") == 0 || g_strcmp0(key, "ctrl+p") == 0 ||
      g_strcmp0(key, "shift+tab") == 0 || g_strcmp0(key, "btab") == 0) {
    return ide_action_make(IDE_ACTION_HELP_PREV);
  }
  if (is_pgdown_key(key) || g_strcmp0(key, "ctrl+d") == 0) {
    return ide_action_make(IDE_ACTION_HELP_PAGE_DOWN);
  }
  if (is_pgup_key(key) || g_strcmp0(key, "ctrl+u") == 0) {
    return ide_action_make(IDE_ACTION_HELP_PAGE_UP);
  }
  if (g_strcmp0(key, "backspace") == 0) {
    return ide_action_make(IDE_ACTION_HELP_BACKSPACE);
  }
  if (is_space_key(key)) {
    return ide_action_make_str(IDE_ACTION_HELP_INPUT, " ");
  }
  if (is_printable_single_glyph(key)) {
    return ide_action_make_str(IDE_ACTION_HELP_INPUT, key);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_diff_review(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "q") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_DRAWER_CLOSE);
  }
  if (g_strcmp0(key, "j") == 0 || g_strcmp0(key, "down") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_DOWN);
  }
  if (g_strcmp0(key, "k") == 0 || g_strcmp0(key, "up") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_UP);
  }
  if (g_strcmp0(key, "enter") == 0) {
    return ide_action_make(IDE_ACTION_DRAWER_JUMP);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_hover(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "q") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_LSP_CANCEL);
  }
  if (g_strcmp0(key, "j") == 0 || g_strcmp0(key, "down") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_DOWN);
  }
  if (g_strcmp0(key, "k") == 0 || g_strcmp0(key, "up") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_UP);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_completion(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0) {
    fsm->state = (fsm->previous_state == IDE_STATE_NORMAL) ? IDE_STATE_NORMAL : IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_LSP_CANCEL);
  }
  if (g_strcmp0(key, "enter") == 0 || g_strcmp0(key, "tab") == 0) {
    fsm->state = (fsm->previous_state == IDE_STATE_NORMAL) ? IDE_STATE_NORMAL : IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_LSP_SELECT_CONFIRM);
  }
  if (g_strcmp0(key, "down") == 0 || g_strcmp0(key, "ctrl+n") == 0) {
    return ide_action_make(IDE_ACTION_LSP_SELECT_NEXT);
  }
  if (g_strcmp0(key, "up") == 0 || g_strcmp0(key, "ctrl+p") == 0) {
    return ide_action_make(IDE_ACTION_LSP_SELECT_PREV);
  }
  if (g_strcmp0(key, "backspace") == 0) {
    return ide_action_make(IDE_ACTION_BACKSPACE);
  }
  if (g_strcmp0(key, "ctrl+space") == 0 || g_strcmp0(key, "ctrl+@") == 0) {
    return ide_action_make(IDE_ACTION_NONE);
  }
  if (is_space_key(key)) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, " ");
  }
  if (is_printable_single_glyph(key)) {
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, key);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_code_action(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "q") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_LSP_CANCEL);
  }
  if (g_strcmp0(key, "enter") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_LSP_SELECT_CONFIRM);
  }
  if (g_strcmp0(key, "j") == 0 || g_strcmp0(key, "down") == 0) {
    return ide_action_make(IDE_ACTION_LSP_SELECT_NEXT);
  }
  if (g_strcmp0(key, "k") == 0 || g_strcmp0(key, "up") == 0) {
    return ide_action_make(IDE_ACTION_LSP_SELECT_PREV);
  }
  if (g_strcmp0(key, "tab") == 0) {
    return ide_action_make(IDE_ACTION_LSP_CALL_HIERARCHY_TOGGLE);
  }
  if (strlen(key) == 1 && key[0] >= '1' && key[0] <= '9') {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make_int(IDE_ACTION_LSP_SELECT_CONFIRM, (key[0] - '1') + 1);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_rename(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_LSP_CANCEL);
  }
  if (g_strcmp0(key, "enter") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_LSP_SELECT_CONFIRM);
  }
  if (g_strcmp0(key, "backspace") == 0) {
    return ide_action_make(IDE_ACTION_BACKSPACE);
  }
  if (is_space_key(key)) {
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, " ");
  }
  if (is_printable_single_glyph(key)) {
    return ide_action_make_str(IDE_ACTION_INSERT_CHAR, key);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_search(IdeFsm *fsm, const char *key)
{
  if (g_strcmp0(key, "esc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_SEARCH_CANCEL);
  }
  if (g_strcmp0(key, "enter") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_SEARCH_CONFIRM);
  }
  if (g_strcmp0(key, "backspace") == 0) {
    return ide_action_make(IDE_ACTION_SEARCH_BACKSPACE);
  }
  if (is_space_key(key)) {
    return ide_action_make_str(IDE_ACTION_SEARCH_INPUT, " ");
  }
  if (is_printable_single_glyph(key)) {
    return ide_action_make_str(IDE_ACTION_SEARCH_INPUT, key);
  }
  return ide_action_make(IDE_ACTION_NONE);
}

/* ═══════════════════════════ main entry points ═══════════════════════════ */

IdeAction
ide_fsm_handle_key_at(IdeFsm *fsm, const char *key, gint64 now_usec)
{
  g_return_val_if_fail(fsm != NULL, ide_action_make(IDE_ACTION_NONE));
  if (key == NULL || key[0] == '\0') {
    return ide_action_make(IDE_ACTION_NONE);
  }

  /* Check 2-second leader timeout: expire state and swallow expired key */
  if (fsm->state == IDE_STATE_LEADER && fsm->last_input_usec > 0 &&
      (now_usec - fsm->last_input_usec) > fsm->leader_timeout_usec) {
    ide_fsm_expire_leader(fsm);
    fsm->last_input_usec = now_usec;
    return ide_action_make(IDE_ACTION_NONE);
  }

  IdeState pre_state = fsm->state;
  IdeAction result;
  gboolean in_editable_prompt =
      (fsm->state == IDE_STATE_DRAWER && fsm->drawer_editable) ||
      (fsm->state == IDE_STATE_TREE && fsm->tree_editable);

  IdeActionType remapped = IDE_ACTION_NONE;
  if (!in_editable_prompt && ide_fsm_lookup_remap(fsm, fsm->state, key, &remapped)) {
    switch (remapped) {
      case IDE_ACTION_SELECT_WORD:
      case IDE_ACTION_SELECT_WORD_BACKWARD:
      case IDE_ACTION_SELECT_LINE:
      case IDE_ACTION_EXTEND_SELECTION:
        fsm->state = IDE_STATE_SELECT;
        break;
      case IDE_ACTION_INSERT_NEWLINE:
        if (fsm->state == IDE_STATE_NORMAL) {
          fsm->state = IDE_STATE_INSERT;
        }
        break;
      case IDE_ACTION_CHANGE_SELECTION:
        fsm->state = IDE_STATE_INSERT;
        break;
      case IDE_ACTION_SEARCH_OPEN:
        fsm->state = IDE_STATE_SEARCH;
        break;
      case IDE_ACTION_DRAWER_CLOSE:
      case IDE_ACTION_PICKER_CANCEL:
      case IDE_ACTION_HELP_CANCEL:
      case IDE_ACTION_SEARCH_CANCEL:
        fsm->state = IDE_STATE_NORMAL;
        break;
      default:
        break;
    }
    result = ide_action_make(remapped);
    goto done;
  }

  switch (fsm->state) {
    case IDE_STATE_NORMAL:
      result = handle_normal(fsm, key);
      break;
    case IDE_STATE_INSERT:
      result = handle_insert(fsm, key, now_usec);
      break;
    case IDE_STATE_SELECT:
      result = handle_select(fsm, key);
      break;
    case IDE_STATE_LEADER:
      result = handle_leader(fsm, key);
      break;
    case IDE_STATE_MATCH_PENDING:
      result = handle_match(fsm, key);
      break;
    case IDE_STATE_DRAWER:
      result = handle_drawer(fsm, key);
      break;
    case IDE_STATE_TREE:
      result = handle_tree(fsm, key);
      break;
    case IDE_STATE_PICKER:
      result = handle_picker(fsm, key);
      break;
    case IDE_STATE_HELP:
      result = handle_help(fsm, key);
      break;
    case IDE_STATE_DIFF_REVIEW:
      result = handle_diff_review(fsm, key);
      break;
    case IDE_STATE_HOVER:
      result = handle_hover(fsm, key);
      break;
    case IDE_STATE_COMPLETION:
      result = handle_completion(fsm, key);
      break;
    case IDE_STATE_CODE_ACTION:
      result = handle_code_action(fsm, key);
      break;
    case IDE_STATE_RENAME:
      result = handle_rename(fsm, key);
      break;
    case IDE_STATE_SEARCH:
      result = handle_search(fsm, key);
      break;
    default:
      fsm->state = IDE_STATE_NORMAL;
      result = ide_action_make(IDE_ACTION_NONE);
      break;
  }

done:
  fsm->last_input_usec = now_usec;
  if (pre_state == IDE_STATE_LEADER ||
      pre_state == IDE_STATE_MATCH_PENDING ||
      fsm->state == IDE_STATE_LEADER ||
      fsm->state == IDE_STATE_MATCH_PENDING ||
      (g_strcmp0(key, "g") == 0 &&
       (result.type == IDE_ACTION_MOVE_BUFFER_START ||
        result.type == IDE_ACTION_TREE_TOP))) {
    fsm->last_char[0] = '\0';
  } else {
    g_strlcpy(fsm->last_char, key, sizeof(fsm->last_char));
  }
  return result;
}

IdeAction
ide_fsm_handle_key(IdeFsm *fsm, const char *key)
{
  return ide_fsm_handle_key_at(fsm, key, g_get_monotonic_time());
}
