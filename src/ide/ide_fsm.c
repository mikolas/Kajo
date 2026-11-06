#include "ide_fsm.h"

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

static const char *const kValidLeaderChords[] = {
  "ff", "fg", "fs", "fa", "bb", "bd", "bD", "e",  "o",  "a",  "k",  "?",  "h",
  "/",  "mp", "mt", "md", "mf", "ne", "pe", "nd", "pd", "nh", "ph", "ns", "ps",
  "ds", "da", "dr", "db", "dd", "dc", "dn", "di", "do", "dp", "dq", "dv", "ca",
  "cr", "cf", "rb", "rt", "rr", "rc", "gd", "gi", "gc", "ch", "sr", "sp", "sf", "s/", "ws", "w>", "w<", "w+",
  "w-", "ww", "wh", "wl", "wj", "wk", NULL
};

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
is_leader_prefix(const char *prefix)
{
  if (prefix == NULL || prefix[0] == '\0') {
    return FALSE;
  }
  for (gsize i = 0; kValidLeaderChords[i] != NULL; i++) {
    if (g_str_has_prefix(kValidLeaderChords[i], prefix)) {
      return TRUE;
    }
  }
  return FALSE;
}

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

void
ide_fsm_set_state(IdeFsm *fsm, IdeState state)
{
  g_return_if_fail(fsm != NULL);
  fsm->previous_state = fsm->state;
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
ide_state_to_string(IdeState state)
{
  switch (state) {
    case IDE_STATE_NORMAL:
      return "NORMAL";
    case IDE_STATE_INSERT:
      return "INSERT";
    case IDE_STATE_SELECT:
      return "SELECT";
    case IDE_STATE_LEADER:
      return "LEADER";
    case IDE_STATE_MATCH_PENDING:
      return "MATCH";
    case IDE_STATE_PICKER:
      return "PICKER";
    case IDE_STATE_DRAWER:
      return "DRAWER";
    case IDE_STATE_DIFF_REVIEW:
      return "DIFF_REVIEW";
    case IDE_STATE_TREE:
      return "TREE";
    case IDE_STATE_HOVER:
      return "HOVER";
    case IDE_STATE_COMPLETION:
      return "COMPLETION";
    case IDE_STATE_CODE_ACTION:
      return "ACTION";
    case IDE_STATE_RENAME:
      return "RENAME";
    case IDE_STATE_SEARCH:
      return "SEARCH";
    case IDE_STATE_HELP:
      return "HELP";
    default:
      return "UNKNOWN";
  }
}

const char *
ide_state_get_css_class(IdeState state)
{
  switch (state) {
    case IDE_STATE_SELECT:
      return "mode-select";
    case IDE_STATE_INSERT:
      return "mode-insert";
    case IDE_STATE_MATCH_PENDING:
    case IDE_STATE_SEARCH:
    case IDE_STATE_PICKER:
    case IDE_STATE_HELP:
      return "mode-search";
    case IDE_STATE_LEADER:
      return "mode-leader";
    default:
      return "mode-normal";
  }
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

IdeActionType
ide_action_type_from_name(const char *name)
{
  if (name == NULL || name[0] == '\0') {
    return IDE_ACTION_NONE;
  }
  g_autofree char *lower = g_ascii_strdown(name, -1);
  g_strstrip(lower);
  g_strdelimit(lower, "-", '_');

  static const struct {
    const char   *key;
    IdeActionType type;
  } kMap[] = {
    { "move_left", IDE_ACTION_MOVE_LEFT },
    { "left", IDE_ACTION_MOVE_LEFT },
    { "move_right", IDE_ACTION_MOVE_RIGHT },
    { "right", IDE_ACTION_MOVE_RIGHT },
    { "move_up", IDE_ACTION_MOVE_UP },
    { "up", IDE_ACTION_MOVE_UP },
    { "move_down", IDE_ACTION_MOVE_DOWN },
    { "down", IDE_ACTION_MOVE_DOWN },
    { "move_word_forward", IDE_ACTION_MOVE_WORD_FORWARD },
    { "word_forward", IDE_ACTION_MOVE_WORD_FORWARD },
    { "move_word_backward", IDE_ACTION_MOVE_WORD_BACKWARD },
    { "word_backward", IDE_ACTION_MOVE_WORD_BACKWARD },
    { "page_up", IDE_ACTION_PAGE_UP },
    { "pgup", IDE_ACTION_PAGE_UP },
    { "page_down", IDE_ACTION_PAGE_DOWN },
    { "pgdn", IDE_ACTION_PAGE_DOWN },
    { "pgdown", IDE_ACTION_PAGE_DOWN },
    { "move_line_start", IDE_ACTION_MOVE_LINE_START },
    { "line_start", IDE_ACTION_MOVE_LINE_START },
    { "home", IDE_ACTION_MOVE_LINE_START },
    { "move_line_end", IDE_ACTION_MOVE_LINE_END },
    { "line_end", IDE_ACTION_MOVE_LINE_END },
    { "end", IDE_ACTION_MOVE_LINE_END },
    { "move_buffer_start", IDE_ACTION_MOVE_BUFFER_START },
    { "buffer_start", IDE_ACTION_MOVE_BUFFER_START },
    { "move_buffer_end", IDE_ACTION_MOVE_BUFFER_END },
    { "buffer_end", IDE_ACTION_MOVE_BUFFER_END },
    { "select_word", IDE_ACTION_SELECT_WORD },
    { "select_word_backward", IDE_ACTION_SELECT_WORD_BACKWARD },
    { "select_line", IDE_ACTION_SELECT_LINE },
    { "extend_selection", IDE_ACTION_EXTEND_SELECTION },
    { "collapse_selection", IDE_ACTION_COLLAPSE_SELECTION },
    { "select_text_object", IDE_ACTION_SELECT_TEXT_OBJECT },
    { "insert_char", IDE_ACTION_INSERT_CHAR },
    { "insert_newline", IDE_ACTION_INSERT_NEWLINE },
    { "newline", IDE_ACTION_INSERT_NEWLINE },
    { "insert_tab", IDE_ACTION_INSERT_TAB },
    { "tab", IDE_ACTION_INSERT_TAB },
    { "backspace", IDE_ACTION_BACKSPACE },
    { "delete_char", IDE_ACTION_DELETE_CHAR },
    { "delete", IDE_ACTION_DELETE_CHAR },
    { "delete_selection", IDE_ACTION_DELETE_SELECTION },
    { "change_selection", IDE_ACTION_CHANGE_SELECTION },
    { "indent", IDE_ACTION_INDENT },
    { "outdent", IDE_ACTION_OUTDENT },
    { "yank", IDE_ACTION_YANK },
    { "copy", IDE_ACTION_YANK },
    { "paste_after", IDE_ACTION_PASTE_AFTER },
    { "paste", IDE_ACTION_PASTE_AFTER },
    { "paste_before", IDE_ACTION_PASTE_BEFORE },
    { "undo", IDE_ACTION_UNDO },
    { "redo", IDE_ACTION_REDO },
    { "pipe_selection", IDE_ACTION_PIPE_SELECTION },
    { "tab_next", IDE_ACTION_TAB_NEXT },
    { "next_tab", IDE_ACTION_TAB_NEXT },
    { "buffer_next", IDE_ACTION_TAB_NEXT },
    { "tab_prev", IDE_ACTION_TAB_PREV },
    { "prev_tab", IDE_ACTION_TAB_PREV },
    { "buffer_prev", IDE_ACTION_TAB_PREV },
    { "tab_switch", IDE_ACTION_TAB_SWITCH },
    { "buffer_close", IDE_ACTION_BUFFER_CLOSE },
    { "close_buffer", IDE_ACTION_BUFFER_CLOSE },
    { "buffer_force_close", IDE_ACTION_BUFFER_FORCE_CLOSE },
    { "force_close_buffer", IDE_ACTION_BUFFER_FORCE_CLOSE },
    { "save", IDE_ACTION_SAVE },
    { "save_all", IDE_ACTION_SAVE_ALL },
    { "toggle_file_tree", IDE_ACTION_TOGGLE_FILE_TREE },
    { "toggle_tree", IDE_ACTION_TOGGLE_FILE_TREE },
    { "toggle_drawer", IDE_ACTION_TOGGLE_DRAWER },
    { "toggle_markdown_preview", IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW },
    { "drawer_close", IDE_ACTION_DRAWER_CLOSE },
    { "drawer_jump", IDE_ACTION_DRAWER_JUMP },
    { "drawer_maximize", IDE_ACTION_DRAWER_MAXIMIZE },
    { "next_error", IDE_ACTION_NEXT_ERROR },
    { "prev_error", IDE_ACTION_PREV_ERROR },
    { "next_diagnostic", IDE_ACTION_NEXT_DIAGNOSTIC },
    { "prev_diagnostic", IDE_ACTION_PREV_DIAGNOSTIC },
    { "next_hunk", IDE_ACTION_NEXT_HUNK },
    { "prev_hunk", IDE_ACTION_PREV_HUNK },
    { "step_forward", IDE_ACTION_STEP_FORWARD },
    { "step_backward", IDE_ACTION_STEP_BACKWARD },
    { "agent_review_open", IDE_ACTION_AGENT_REVIEW_OPEN },
    { "agent_review", IDE_ACTION_AGENT_REVIEW_OPEN },
    { "agent_accept_all", IDE_ACTION_AGENT_ACCEPT_ALL },
    { "agent_revert_all", IDE_ACTION_AGENT_REVERT_ALL },
    { "alternate_file", IDE_ACTION_ALTERNATE_FILE },
    { "lsp_definition", IDE_ACTION_LSP_DEFINITION },
    { "definition", IDE_ACTION_LSP_DEFINITION },
    { "lsp_implementation", IDE_ACTION_LSP_IMPLEMENTATION },
    { "implementation", IDE_ACTION_LSP_IMPLEMENTATION },
    { "lsp_call_hierarchy", IDE_ACTION_LSP_CALL_HIERARCHY },
    { "call_hierarchy", IDE_ACTION_LSP_CALL_HIERARCHY },
    { "lsp_code_action", IDE_ACTION_LSP_CODE_ACTION },
    { "code_action", IDE_ACTION_LSP_CODE_ACTION },
    { "task_build", IDE_ACTION_TASK_BUILD },
    { "build", IDE_ACTION_TASK_BUILD },
    { "task_test", IDE_ACTION_TASK_TEST },
    { "test", IDE_ACTION_TASK_TEST },
    { "task_run", IDE_ACTION_TASK_RUN },
    { "run", IDE_ACTION_TASK_RUN },
    { "task_clean", IDE_ACTION_TASK_CLEAN },
    { "lsp_format", IDE_ACTION_LSP_FORMAT },
    { "format", IDE_ACTION_LSP_FORMAT },
    { "lsp_rename", IDE_ACTION_LSP_RENAME },
    { "rename", IDE_ACTION_LSP_RENAME },
    { "lsp_hover", IDE_ACTION_LSP_HOVER },
    { "hover", IDE_ACTION_LSP_HOVER },
    { "lsp_complete", IDE_ACTION_LSP_COMPLETE },
    { "completion", IDE_ACTION_LSP_COMPLETE },
    { "lsp_cancel", IDE_ACTION_LSP_CANCEL },
    { "jumplist_back", IDE_ACTION_JUMPLIST_BACK },
    { "jumplist_forward", IDE_ACTION_JUMPLIST_FORWARD },
    { "toggle_checklist", IDE_ACTION_TOGGLE_CHECKLIST },
    { "toggle_mermaid_mode", IDE_ACTION_TOGGLE_MERMAID_MODE },
    { "toggle_markdown_fold", IDE_ACTION_TOGGLE_MARKDOWN_FOLD },
    { "picker_open_files", IDE_ACTION_PICKER_OPEN_FILES },
    { "open_files", IDE_ACTION_PICKER_OPEN_FILES },
    { "picker_open_grep", IDE_ACTION_PICKER_OPEN_GREP },
    { "live_grep", IDE_ACTION_PICKER_OPEN_GREP },
    { "picker_open_buffers", IDE_ACTION_PICKER_OPEN_BUFFERS },
    { "open_buffers", IDE_ACTION_PICKER_OPEN_BUFFERS },
    { "picker_select", IDE_ACTION_PICKER_SELECT },
    { "picker_cancel", IDE_ACTION_PICKER_CANCEL },
    { "picker_next", IDE_ACTION_PICKER_NEXT },
    { "picker_prev", IDE_ACTION_PICKER_PREV },
    { "pane_focus_tree", IDE_ACTION_PANE_FOCUS_TREE },
    { "pane_focus_buffer", IDE_ACTION_PANE_FOCUS_BUFFER },
    { "pane_focus_drawer", IDE_ACTION_PANE_FOCUS_DRAWER },
    { "pane_cycle", IDE_ACTION_PANE_CYCLE },
    { "resize_tree_grow", IDE_ACTION_RESIZE_TREE_GROW },
    { "resize_tree_shrink", IDE_ACTION_RESIZE_TREE_SHRINK },
    { "resize_drawer_grow", IDE_ACTION_RESIZE_DRAWER_GROW },
    { "resize_drawer_shrink", IDE_ACTION_RESIZE_DRAWER_SHRINK },
    { "tree_up", IDE_ACTION_TREE_UP },
    { "tree_down", IDE_ACTION_TREE_DOWN },
    { "tree_toggle_or_open", IDE_ACTION_TREE_TOGGLE_OR_OPEN },
    { "tree_collapse_or_parent", IDE_ACTION_TREE_COLLAPSE_OR_PARENT },
    { "tree_refresh", IDE_ACTION_TREE_REFRESH },
    { "tree_create_file", IDE_ACTION_TREE_CREATE_FILE },
    { "tree_create_dir", IDE_ACTION_TREE_CREATE_DIR },
    { "tree_delete", IDE_ACTION_TREE_DELETE },
    { "search_open", IDE_ACTION_SEARCH_OPEN },
    { "search", IDE_ACTION_SEARCH_OPEN },
    { "search_cancel", IDE_ACTION_SEARCH_CANCEL },
    { "search_next", IDE_ACTION_SEARCH_NEXT },
    { "search_prev", IDE_ACTION_SEARCH_PREV },
    { "search_clear_highlight", IDE_ACTION_SEARCH_CLEAR_HIGHLIGHT },
    { "replace_open", IDE_ACTION_REPLACE_OPEN },
    { "replace", IDE_ACTION_REPLACE_OPEN },
    { "project_replace_open", IDE_ACTION_PROJECT_REPLACE_OPEN },
    { "dap_toggle_breakpoint", IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
    { "toggle_breakpoint", IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
    { "dap_start", IDE_ACTION_DAP_START },
    { "dap_continue", IDE_ACTION_DAP_CONTINUE },
    { "debug_continue", IDE_ACTION_DAP_CONTINUE },
    { "dap_step_over", IDE_ACTION_DAP_STEP_OVER },
    { "debug_step_over", IDE_ACTION_DAP_STEP_OVER },
    { "dap_step_into", IDE_ACTION_DAP_STEP_INTO },
    { "debug_step_into", IDE_ACTION_DAP_STEP_INTO },
    { "dap_step_out", IDE_ACTION_DAP_STEP_OUT },
    { "debug_step_out", IDE_ACTION_DAP_STEP_OUT },
    { "dap_pause", IDE_ACTION_DAP_PAUSE },
    { "debug_pause", IDE_ACTION_DAP_PAUSE },
    { "dap_stop", IDE_ACTION_DAP_STOP },
    { "debug_stop", IDE_ACTION_DAP_STOP },
    { "dap_toggle_view", IDE_ACTION_DAP_TOGGLE_VIEW },
    { "debug_toggle_view", IDE_ACTION_DAP_TOGGLE_VIEW },
    { "help_toggle", IDE_ACTION_HELP_TOGGLE },
    { "help", IDE_ACTION_HELP_TOGGLE },
    { "help_open", IDE_ACTION_HELP_OPEN },
    { "help_cancel", IDE_ACTION_HELP_CANCEL },
    { NULL, IDE_ACTION_NONE }
  };

  for (gsize i = 0; kMap[i].key != NULL; i++) {
    if (g_strcmp0(lower, kMap[i].key) == 0) {
      return kMap[i].type;
    }
  }
  return IDE_ACTION_NONE;
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

static IdeAction
handle_normal(IdeFsm *fsm, const char *key)
{
  /* Two-key 'g' navigation chords (gg, gh, gl, gd) */
  if (g_strcmp0(fsm->last_char, "g") == 0) {
    if (g_strcmp0(key, "g") == 0) {
      return ide_action_make(IDE_ACTION_MOVE_BUFFER_START);
    }
    if (g_strcmp0(key, "h") == 0) {
      return ide_action_make(IDE_ACTION_MOVE_LINE_START);
    }
    if (g_strcmp0(key, "l") == 0) {
      return ide_action_make(IDE_ACTION_MOVE_LINE_END);
    }
    if (g_strcmp0(key, "d") == 0) {
      return ide_action_make(IDE_ACTION_LSP_DEFINITION);
    }
  }

  /* Mode transitions */
  if (g_strcmp0(key, "i") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_NONE);
  }
  if (g_strcmp0(key, "I") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_MOVE_LINE_START);
  }
  if (g_strcmp0(key, "a") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_MOVE_RIGHT);
  }
  if (g_strcmp0(key, "A") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_MOVE_LINE_END);
  }
  if (g_strcmp0(key, "o") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make_str(IDE_ACTION_INSERT_NEWLINE, "below");
  }
  if (g_strcmp0(key, "O") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make_str(IDE_ACTION_INSERT_NEWLINE, "above");
  }
  if (g_strcmp0(key, "c") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_CHANGE_SELECTION);
  }
  if (g_strcmp0(key, "w") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make(IDE_ACTION_SELECT_WORD);
  }
  if (g_strcmp0(key, "b") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make(IDE_ACTION_SELECT_WORD_BACKWARD);
  }
  if (g_strcmp0(key, "x") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make(IDE_ACTION_SELECT_LINE);
  }
  if (g_strcmp0(key, "v") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make(IDE_ACTION_NONE);
  }
  if (g_strcmp0(key, "shift+left") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "left");
  }
  if (g_strcmp0(key, "shift+right") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "right");
  }
  if (g_strcmp0(key, "shift+up") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "up");
  }
  if (g_strcmp0(key, "shift+down") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "down");
  }
  if (g_strcmp0(key, "/") == 0) {
    fsm->state = IDE_STATE_SEARCH;
    return ide_action_make(IDE_ACTION_SEARCH_OPEN);
  }
  if (g_strcmp0(key, "n") == 0) {
    return ide_action_make(IDE_ACTION_SEARCH_NEXT);
  }
  if (g_strcmp0(key, "N") == 0) {
    return ide_action_make(IDE_ACTION_SEARCH_PREV);
  }
  if (is_space_key(key)) {
    fsm->previous_state = IDE_STATE_NORMAL;
    fsm->state = IDE_STATE_LEADER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }
  if (g_strcmp0(key, "m") == 0) {
    fsm->state = IDE_STATE_MATCH_PENDING;
    fsm->match_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
  }

  /* Basic Navigation */
  if (g_strcmp0(key, "h") == 0 || g_strcmp0(key, "left") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LEFT);
  }
  if (g_strcmp0(key, "l") == 0 || g_strcmp0(key, "right") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_RIGHT);
  }
  if (g_strcmp0(key, "j") == 0 || g_strcmp0(key, "down") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_DOWN);
  }
  if (g_strcmp0(key, "k") == 0 || g_strcmp0(key, "up") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_UP);
  }
  if (is_pgup_key(key) || g_strcmp0(key, "ctrl+b") == 0 ||
      g_strcmp0(key, "ctrl+u") == 0) {
    return ide_action_make(IDE_ACTION_PAGE_UP);
  }
  if (is_pgdown_key(key) || g_strcmp0(key, "ctrl+f") == 0 ||
      g_strcmp0(key, "ctrl+d") == 0) {
    return ide_action_make(IDE_ACTION_PAGE_DOWN);
  }
  if (g_strcmp0(key, "home") == 0 || g_strcmp0(key, "0") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LINE_START);
  }
  if (g_strcmp0(key, "end") == 0 || g_strcmp0(key, "$") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LINE_END);
  }
  if (g_strcmp0(key, "ctrl+home") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_START);
  }
  if (g_strcmp0(key, "ctrl+end") == 0 || g_strcmp0(key, "G") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_END);
  }
  if (g_strcmp0(key, "ctrl+right") == 0 || g_strcmp0(key, "alt+right") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_WORD_FORWARD);
  }
  if (g_strcmp0(key, "ctrl+left") == 0 || g_strcmp0(key, "alt+left") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_WORD_BACKWARD);
  }

  /* Mutations & Standard Shortcuts */
  if (g_strcmp0(key, "d") == 0 || g_strcmp0(key, "delete") == 0 ||
      g_strcmp0(key, "ctrl+x") == 0) {
    return ide_action_make(IDE_ACTION_DELETE_CHAR);
  }
  if (g_strcmp0(key, "backspace") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LEFT);
  }
  if (g_strcmp0(key, "y") == 0 || g_strcmp0(key, "ctrl+c") == 0) {
    return ide_action_make(IDE_ACTION_YANK);
  }
  if (g_strcmp0(key, "p") == 0 || g_strcmp0(key, "ctrl+v") == 0) {
    return ide_action_make(IDE_ACTION_PASTE_AFTER);
  }
  if (g_strcmp0(key, "P") == 0) {
    return ide_action_make(IDE_ACTION_PASTE_BEFORE);
  }
  if (g_strcmp0(key, "u") == 0 || g_strcmp0(key, "ctrl+z") == 0) {
    return ide_action_make(IDE_ACTION_UNDO);
  }
  if (g_strcmp0(key, "U") == 0 || g_strcmp0(key, "ctrl+y") == 0 ||
      g_strcmp0(key, "ctrl+r") == 0 || g_strcmp0(key, "ctrl+shift+z") == 0) {
    return ide_action_make(IDE_ACTION_REDO);
  }
  if (g_strcmp0(key, ">") == 0) {
    return ide_action_make(IDE_ACTION_INDENT);
  }
  if (g_strcmp0(key, "<") == 0) {
    return ide_action_make(IDE_ACTION_OUTDENT);
  }

  /* Buffer Tabs */
  if (g_strcmp0(key, "H") == 0) {
    return ide_action_make(IDE_ACTION_TAB_PREV);
  }
  if (g_strcmp0(key, "L") == 0) {
    return ide_action_make(IDE_ACTION_TAB_NEXT);
  }

  /* Nordic Quick Step Keys (ö / ä) */
  if (g_strcmp0(key, "ö") == 0 || g_strcmp0(key, "odiaeresis") == 0) {
    return ide_action_make(IDE_ACTION_STEP_BACKWARD);
  }
  if (g_strcmp0(key, "ä") == 0 || g_strcmp0(key, "adiaeresis") == 0) {
    return ide_action_make(IDE_ACTION_STEP_FORWARD);
  }

  /* Jumplist & Panes */
  if (g_strcmp0(key, "ctrl+o") == 0) {
    return ide_action_make(IDE_ACTION_JUMPLIST_BACK);
  }
  if (g_strcmp0(key, "ctrl+i") == 0) {
    return ide_action_make(IDE_ACTION_JUMPLIST_FORWARD);
  }
  if (g_strcmp0(key, "ctrl+w") == 0) {
    return ide_action_make(IDE_ACTION_PANE_CYCLE);
  }

  /* LSP & Save */
  if (g_strcmp0(key, "K") == 0) {
    return ide_action_make(IDE_ACTION_LSP_HOVER);
  }
  if (g_strcmp0(key, "ctrl+s") == 0) {
    return ide_action_make(IDE_ACTION_SAVE);
  }

  /* DAP Debugger Function Keys */
  if (g_strcmp0(key, "f5") == 0) {
    return ide_action_make(IDE_ACTION_DAP_CONTINUE);
  }
  if (g_strcmp0(key, "f9") == 0) {
    return ide_action_make(IDE_ACTION_DAP_TOGGLE_BREAKPOINT);
  }
  if (g_strcmp0(key, "f10") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STEP_OVER);
  }
  if (g_strcmp0(key, "f11") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STEP_INTO);
  }
  if (g_strcmp0(key, "shift+f11") == 0 || g_strcmp0(key, "f12") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STEP_OUT);
  }
  if (g_strcmp0(key, "shift+f5") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STOP);
  }

  /* Help / Command Palette */
  if (g_strcmp0(key, "?") == 0 || g_strcmp0(key, "f1") == 0) {
    fsm->state = IDE_STATE_HELP;
    return ide_action_make(IDE_ACTION_HELP_TOGGLE);
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
  if (g_strcmp0(key, "enter") == 0) {
    return ide_action_make(IDE_ACTION_INSERT_NEWLINE);
  }
  if (g_strcmp0(key, "tab") == 0) {
    return ide_action_make(IDE_ACTION_INSERT_TAB);
  }
  if (g_strcmp0(key, "shift+tab") == 0 || g_strcmp0(key, "btab") == 0) {
    return ide_action_make(IDE_ACTION_OUTDENT);
  }
  if (g_strcmp0(key, "ctrl+space") == 0 || g_strcmp0(key, "ctrl+@") == 0) {
    return ide_action_make(IDE_ACTION_LSP_COMPLETE);
  }
  if (g_strcmp0(key, "ctrl+s") == 0) {
    return ide_action_make(IDE_ACTION_SAVE);
  }
  if (g_strcmp0(key, "ctrl+z") == 0) {
    return ide_action_make(IDE_ACTION_UNDO);
  }
  if (g_strcmp0(key, "ctrl+y") == 0 || g_strcmp0(key, "ctrl+r") == 0 ||
      g_strcmp0(key, "ctrl+shift+z") == 0) {
    return ide_action_make(IDE_ACTION_REDO);
  }
  if (g_strcmp0(key, "ctrl+c") == 0) {
    return ide_action_make(IDE_ACTION_YANK);
  }
  if (g_strcmp0(key, "ctrl+v") == 0) {
    return ide_action_make(IDE_ACTION_PASTE_AFTER);
  }
  if (g_strcmp0(key, "ctrl+x") == 0) {
    return ide_action_make(IDE_ACTION_DELETE_SELECTION);
  }
  if (g_strcmp0(key, "ctrl+w") == 0) {
    return ide_action_make(IDE_ACTION_PANE_CYCLE);
  }
  if (g_strcmp0(key, "backspace") == 0) {
    return ide_action_make(IDE_ACTION_BACKSPACE);
  }
  if (g_strcmp0(key, "delete") == 0) {
    return ide_action_make(IDE_ACTION_DELETE_CHAR);
  }
  if (g_strcmp0(key, "left") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LEFT);
  }
  if (g_strcmp0(key, "right") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_RIGHT);
  }
  if (g_strcmp0(key, "up") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_UP);
  }
  if (g_strcmp0(key, "down") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_DOWN);
  }
  if (g_strcmp0(key, "shift+left") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "left");
  }
  if (g_strcmp0(key, "shift+right") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "right");
  }
  if (g_strcmp0(key, "shift+up") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "up");
  }
  if (g_strcmp0(key, "shift+down") == 0) {
    fsm->state = IDE_STATE_SELECT;
    return ide_action_make_str(IDE_ACTION_EXTEND_SELECTION, "down");
  }
  if (is_pgup_key(key)) {
    return ide_action_make(IDE_ACTION_PAGE_UP);
  }
  if (is_pgdown_key(key)) {
    return ide_action_make(IDE_ACTION_PAGE_DOWN);
  }
  if (g_strcmp0(key, "home") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LINE_START);
  }
  if (g_strcmp0(key, "end") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LINE_END);
  }
  if (g_strcmp0(key, "ctrl+home") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_START);
  }
  if (g_strcmp0(key, "ctrl+end") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_END);
  }
  if (g_strcmp0(key, "ctrl+right") == 0 || g_strcmp0(key, "alt+right") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_WORD_FORWARD);
  }
  if (g_strcmp0(key, "ctrl+left") == 0 || g_strcmp0(key, "alt+left") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_WORD_BACKWARD);
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
  /* Two-key 'g' chords in SELECT mode extend selection to target */
  if (g_strcmp0(fsm->last_char, "g") == 0) {
    if (g_strcmp0(key, "g") == 0) {
      return ide_action_make(IDE_ACTION_MOVE_BUFFER_START);
    }
    if (g_strcmp0(key, "h") == 0) {
      return ide_action_make(IDE_ACTION_MOVE_LINE_START);
    }
    if (g_strcmp0(key, "l") == 0) {
      return ide_action_make(IDE_ACTION_MOVE_LINE_END);
    }
  }

  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "v") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_COLLAPSE_SELECTION);
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
  if (g_strcmp0(key, "d") == 0 || g_strcmp0(key, "delete") == 0 ||
      g_strcmp0(key, "backspace") == 0 || g_strcmp0(key, "ctrl+x") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_DELETE_SELECTION);
  }
  if (g_strcmp0(key, "c") == 0 || g_strcmp0(key, "i") == 0) {
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_CHANGE_SELECTION);
  }
  if (g_strcmp0(key, "y") == 0 || g_strcmp0(key, "ctrl+c") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_YANK);
  }
  if (g_strcmp0(key, "p") == 0 || g_strcmp0(key, "ctrl+v") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_PASTE_AFTER);
  }
  if (g_strcmp0(key, "u") == 0 || g_strcmp0(key, "ctrl+z") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_UNDO);
  }
  if (g_strcmp0(key, "U") == 0 || g_strcmp0(key, "ctrl+y") == 0 ||
      g_strcmp0(key, "ctrl+r") == 0 || g_strcmp0(key, "ctrl+shift+z") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_REDO);
  }
  if (g_strcmp0(key, "ctrl+s") == 0) {
    return ide_action_make(IDE_ACTION_SAVE);
  }
  if (g_strcmp0(key, ">") == 0 || g_strcmp0(key, "tab") == 0) {
    return ide_action_make(IDE_ACTION_INDENT);
  }
  if (g_strcmp0(key, "<") == 0 || g_strcmp0(key, "shift+tab") == 0) {
    return ide_action_make(IDE_ACTION_OUTDENT);
  }
  if (g_strcmp0(key, "!") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_PIPE_SELECTION);
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
  if (is_pgup_key(key) || g_strcmp0(key, "ctrl+u") == 0) {
    return ide_action_make(IDE_ACTION_PAGE_UP);
  }
  if (is_pgdown_key(key) || g_strcmp0(key, "ctrl+d") == 0) {
    return ide_action_make(IDE_ACTION_PAGE_DOWN);
  }
  if (g_strcmp0(key, "home") == 0 || g_strcmp0(key, "0") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LINE_START);
  }
  if (g_strcmp0(key, "end") == 0 || g_strcmp0(key, "$") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_LINE_END);
  }
  if (g_strcmp0(key, "ctrl+home") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_START);
  }
  if (g_strcmp0(key, "ctrl+end") == 0 || g_strcmp0(key, "G") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_END);
  }
  if (g_strcmp0(key, "w") == 0 || g_strcmp0(key, "ctrl+right") == 0) {
    return ide_action_make(IDE_ACTION_SELECT_WORD);
  }
  if (g_strcmp0(key, "b") == 0 || g_strcmp0(key, "ctrl+left") == 0) {
    return ide_action_make(IDE_ACTION_SELECT_WORD_BACKWARD);
  }
  if (g_strcmp0(key, "x") == 0) {
    return ide_action_make(IDE_ACTION_SELECT_LINE);
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

  /* Fuzzy Finders & Buffers */
  if (g_strcmp0(b, "ff") == 0) {
    fsm->state = IDE_STATE_PICKER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PICKER_OPEN_FILES);
  }
  if (g_strcmp0(b, "fg") == 0 || g_strcmp0(b, "/") == 0) {
    fsm->state = IDE_STATE_PICKER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PICKER_OPEN_GREP);
  }
  if (g_strcmp0(b, "bb") == 0) {
    fsm->state = IDE_STATE_PICKER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PICKER_OPEN_BUFFERS);
  }
  if (g_strcmp0(b, "bd") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_BUFFER_CLOSE);
  }
  if (g_strcmp0(b, "bD") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_BUFFER_FORCE_CLOSE);
  }
  if (g_strcmp0(b, "fs") == 0 || g_strcmp0(b, "ws") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_SAVE);
  }
  if (g_strcmp0(b, "fa") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_SAVE_ALL);
  }

  /* Layout & Drawers */
  if (g_strcmp0(b, "e") == 0) {
    fsm->state = (fsm->previous_state == IDE_STATE_TREE) ? IDE_STATE_NORMAL : IDE_STATE_TREE;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_TOGGLE_FILE_TREE);
  }
  if (g_strcmp0(b, "o") == 0) {
    fsm->state = IDE_STATE_DRAWER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_TOGGLE_DRAWER);
  }
  if (g_strcmp0(b, "?") == 0 || g_strcmp0(b, "h") == 0) {
    fsm->state = IDE_STATE_HELP;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_HELP_TOGGLE);
  }
  if (g_strcmp0(b, "mp") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW);
  }
  if (g_strcmp0(b, "mt") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_TOGGLE_CHECKLIST);
  }
  if (g_strcmp0(b, "md") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_TOGGLE_MERMAID_MODE);
  }
  if (g_strcmp0(b, "mf") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_TOGGLE_MARKDOWN_FOLD);
  }

  /* Tooling */
  if (g_strcmp0(b, "a") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_ALTERNATE_FILE);
  }

  /* Diagnostics & Errors */
  if (g_strcmp0(b, "ne") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NEXT_ERROR);
  }
  if (g_strcmp0(b, "pe") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PREV_ERROR);
  }
  if (g_strcmp0(b, "nd") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NEXT_DIAGNOSTIC);
  }
  if (g_strcmp0(b, "pd") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PREV_DIAGNOSTIC);
  }
  if (g_strcmp0(b, "nh") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NEXT_HUNK);
  }
  if (g_strcmp0(b, "ph") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PREV_HUNK);
  }

  /* Agent & Review */
  if (g_strcmp0(b, "ds") == 0) {
    fsm->state = IDE_STATE_DIFF_REVIEW;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_AGENT_REVIEW_OPEN);
  }
  if (g_strcmp0(b, "da") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_AGENT_ACCEPT_ALL);
  }
  if (g_strcmp0(b, "dr") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_AGENT_REVERT_ALL);
  }

  /* DAP Debugger */
  if (g_strcmp0(b, "db") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_TOGGLE_BREAKPOINT);
  }
  if (g_strcmp0(b, "dd") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_START);
  }
  if (g_strcmp0(b, "dc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_CONTINUE);
  }
  if (g_strcmp0(b, "dn") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_STEP_OVER);
  }
  if (g_strcmp0(b, "di") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_STEP_INTO);
  }
  if (g_strcmp0(b, "do") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_STEP_OUT);
  }
  if (g_strcmp0(b, "dp") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_PAUSE);
  }
  if (g_strcmp0(b, "dq") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_STOP);
  }
  if (g_strcmp0(b, "dv") == 0) {
    fsm->state = IDE_STATE_DRAWER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_DAP_TOGGLE_VIEW);
  }

  /* LSP */
  if (g_strcmp0(b, "ca") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_LSP_CODE_ACTION);
  }
  if (g_strcmp0(b, "rb") == 0 || g_strcmp0(b, "rt") == 0 || g_strcmp0(b, "rr") == 0 ||
      g_strcmp0(b, "rc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(b[1] == 'b' ? IDE_ACTION_TASK_BUILD
                           : b[1] == 't' ? IDE_ACTION_TASK_TEST
                           : b[1] == 'r' ? IDE_ACTION_TASK_RUN
                                         : IDE_ACTION_TASK_CLEAN);
  }
  if (g_strcmp0(b, "cf") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_LSP_FORMAT);
  }
  if (g_strcmp0(b, "cr") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_LSP_RENAME);
  }
  if (g_strcmp0(b, "gd") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_LSP_DEFINITION);
  }
  if (g_strcmp0(b, "gi") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_LSP_IMPLEMENTATION);
  }
  if (g_strcmp0(b, "gc") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_LSP_CALL_HIERARCHY);
  }
  if (g_strcmp0(b, "k") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_LSP_HOVER);
  }

  /* Resizing */
  if (g_strcmp0(b, "w>") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_RESIZE_TREE_GROW);
  }
  if (g_strcmp0(b, "w<") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_RESIZE_TREE_SHRINK);
  }
  if (g_strcmp0(b, "w+") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_RESIZE_DRAWER_GROW);
  }
  if (g_strcmp0(b, "w-") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_RESIZE_DRAWER_SHRINK);
  }

  /* Window & Pane Navigation */
  if (g_strcmp0(b, "ww") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PANE_CYCLE);
  }
  if (g_strcmp0(b, "wh") == 0) {
    fsm->state = IDE_STATE_TREE;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PANE_FOCUS_TREE);
  }
  if (g_strcmp0(b, "wl") == 0 || g_strcmp0(b, "wk") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PANE_FOCUS_BUFFER);
  }
  if (g_strcmp0(b, "wj") == 0) {
    fsm->state = IDE_STATE_DRAWER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PANE_FOCUS_DRAWER);
  }

  /* Search & Replace */
  if (g_strcmp0(b, "sr") == 0) {
    fsm->state = IDE_STATE_DRAWER;
    fsm->drawer_editable = TRUE;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_REPLACE_OPEN);
  }
  if (g_strcmp0(b, "sp") == 0) {
    fsm->state = IDE_STATE_DRAWER;
    fsm->drawer_editable = TRUE;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_PROJECT_REPLACE_OPEN);
  }
  if (g_strcmp0(b, "sf") == 0 || g_strcmp0(b, "s/") == 0) {
    fsm->state = IDE_STATE_SEARCH;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_SEARCH_OPEN);
  }
  if (g_strcmp0(b, "ch") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_SEARCH_CLEAR_HIGHLIGHT);
  }
  if (g_strcmp0(b, "ns") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_SEARCH_NEXT);
  }
  if (g_strcmp0(b, "ps") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_SEARCH_PREV);
  }

  if (is_leader_prefix(b)) {
    return ide_action_make(IDE_ACTION_NONE);
  }

  fsm->state = IDE_STATE_NORMAL;
  fsm->leader_buf[0] = '\0';
  return ide_action_make(IDE_ACTION_NONE);
}

static IdeAction
handle_drawer(IdeFsm *fsm, const char *key)
{
  if (fsm->drawer_editable) {
    if (g_strcmp0(key, "esc") == 0) {
      fsm->state = IDE_STATE_NORMAL;
      fsm->drawer_editable = FALSE;
      return ide_action_make(IDE_ACTION_DRAWER_CLOSE);
    }
    if (g_strcmp0(key, "enter") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_APPLY_ALL);
    }
    if (g_strcmp0(key, "tab") == 0 || g_strcmp0(key, "down") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_NEXT_FIELD);
    }
    if (g_strcmp0(key, "shift+tab") == 0 || g_strcmp0(key, "btab") == 0 ||
        g_strcmp0(key, "up") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_PREV_FIELD);
    }
    if (g_strcmp0(key, "alt+c") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_TOGGLE_CASE);
    }
    if (g_strcmp0(key, "alt+w") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_TOGGLE_WORD);
    }
    if (g_strcmp0(key, "alt+r") == 0 || g_strcmp0(key, "ctrl+r") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_TOGGLE_REGEX);
    }
    if (g_strcmp0(key, "alt+a") == 0 || g_strcmp0(key, "ctrl+a") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_APPLY_ALL);
    }
    if (g_strcmp0(key, "alt+y") == 0 || g_strcmp0(key, "ctrl+y") == 0) {
      return ide_action_make(IDE_ACTION_REPLACE_APPLY_CURRENT);
    }
    if (is_pgup_key(key)) {
      return ide_action_make(IDE_ACTION_PAGE_UP);
    }
    if (is_pgdown_key(key)) {
      return ide_action_make(IDE_ACTION_PAGE_DOWN);
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

  /* Read-only Drawer navigation */
  if (g_strcmp0(key, "esc") == 0 || g_strcmp0(key, "q") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_DRAWER_CLOSE);
  }
  if (g_strcmp0(key, "enter") == 0) {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make(IDE_ACTION_DRAWER_JUMP);
  }
  if (g_strcmp0(key, "Z") == 0) {
    return ide_action_make(IDE_ACTION_DRAWER_MAXIMIZE);
  }
  if (g_strcmp0(key, "j") == 0 || g_strcmp0(key, "down") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_DOWN);
  }
  if (g_strcmp0(key, "k") == 0 || g_strcmp0(key, "up") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_UP);
  }
  if (g_strcmp0(key, "d") == 0 || g_strcmp0(key, "ctrl+d") == 0 ||
      is_pgdown_key(key)) {
    return ide_action_make(IDE_ACTION_PAGE_DOWN);
  }
  if (g_strcmp0(key, "u") == 0 || g_strcmp0(key, "ctrl+u") == 0 ||
      is_pgup_key(key)) {
    return ide_action_make(IDE_ACTION_PAGE_UP);
  }
  if (g_strcmp0(key, "G") == 0 || g_strcmp0(key, "ctrl+end") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_END);
  }
  if (g_strcmp0(key, "home") == 0 || g_strcmp0(key, "ctrl+home") == 0) {
    return ide_action_make(IDE_ACTION_MOVE_BUFFER_START);
  }
  if (g_strcmp0(key, "f5") == 0) {
    return ide_action_make(IDE_ACTION_DAP_CONTINUE);
  }
  if (g_strcmp0(key, "f9") == 0) {
    return ide_action_make(IDE_ACTION_DAP_TOGGLE_BREAKPOINT);
  }
  if (g_strcmp0(key, "f10") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STEP_OVER);
  }
  if (g_strcmp0(key, "f11") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STEP_INTO);
  }
  if (g_strcmp0(key, "shift+f11") == 0 || g_strcmp0(key, "f12") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STEP_OUT);
  }
  if (g_strcmp0(key, "shift+f5") == 0) {
    return ide_action_make(IDE_ACTION_DAP_STOP);
  }
  if (g_strcmp0(key, "ctrl+w") == 0) {
    return ide_action_make(IDE_ACTION_PANE_CYCLE);
  }
  if (is_space_key(key)) {
    fsm->previous_state = IDE_STATE_DRAWER;
    fsm->state = IDE_STATE_LEADER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
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
  if (g_strcmp0(key, "j") == 0 || g_strcmp0(key, "down") == 0) {
    return ide_action_make(IDE_ACTION_TREE_DOWN);
  }
  if (g_strcmp0(key, "k") == 0 || g_strcmp0(key, "up") == 0) {
    return ide_action_make(IDE_ACTION_TREE_UP);
  }
  if (g_strcmp0(key, "enter") == 0 || g_strcmp0(key, "l") == 0 || g_strcmp0(key, "right") == 0) {
    return ide_action_make(IDE_ACTION_TREE_TOGGLE_OR_OPEN);
  }
  if (g_strcmp0(key, "h") == 0 || g_strcmp0(key, "left") == 0) {
    return ide_action_make(IDE_ACTION_TREE_COLLAPSE_OR_PARENT);
  }
  if (is_pgdown_key(key) || g_strcmp0(key, "ctrl+d") == 0) {
    return ide_action_make(IDE_ACTION_TREE_PAGE_DOWN);
  }
  if (is_pgup_key(key) || g_strcmp0(key, "ctrl+u") == 0) {
    return ide_action_make(IDE_ACTION_TREE_PAGE_UP);
  }
  if (g_strcmp0(key, "home") == 0) {
    return ide_action_make(IDE_ACTION_TREE_TOP);
  }
  if (g_strcmp0(key, "end") == 0 || g_strcmp0(key, "G") == 0) {
    return ide_action_make(IDE_ACTION_TREE_BOTTOM);
  }
  if (g_strcmp0(key, "g") == 0) {
    if (g_strcmp0(fsm->last_char, "g") == 0) {
      return ide_action_make(IDE_ACTION_TREE_TOP);
    }
    return ide_action_make(IDE_ACTION_NONE);
  }
  if (g_strcmp0(key, "r") == 0) {
    return ide_action_make(IDE_ACTION_TREE_REFRESH);
  }
  if (g_strcmp0(key, "a") == 0) {
    return ide_action_make(IDE_ACTION_TREE_CREATE_FILE);
  }
  if (g_strcmp0(key, "A") == 0) {
    return ide_action_make(IDE_ACTION_TREE_CREATE_DIR);
  }
  if (g_strcmp0(key, "d") == 0) {
    return ide_action_make(IDE_ACTION_TREE_DELETE);
  }
  if (g_strcmp0(key, "ctrl+w") == 0) {
    return ide_action_make(IDE_ACTION_PANE_CYCLE);
  }
  if (is_space_key(key)) {
    fsm->previous_state = IDE_STATE_TREE;
    fsm->state = IDE_STATE_LEADER;
    fsm->leader_buf[0] = '\0';
    return ide_action_make(IDE_ACTION_NONE);
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
    fsm->state = IDE_STATE_INSERT;
    return ide_action_make(IDE_ACTION_LSP_CANCEL);
  }
  if (g_strcmp0(key, "enter") == 0 || g_strcmp0(key, "tab") == 0) {
    fsm->state = IDE_STATE_INSERT;
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
  if (strlen(key) == 1 && key[0] >= '1' && key[0] <= '9') {
    fsm->state = IDE_STATE_NORMAL;
    return ide_action_make_int(IDE_ACTION_LSP_SELECT_CONFIRM, key[0] - '1');
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

IdeAction
ide_fsm_handle_key_at(IdeFsm *fsm, const char *key, gint64 now_usec)
{
  g_return_val_if_fail(fsm != NULL, ide_action_make(IDE_ACTION_NONE));
  if (key == NULL || key[0] == '\0') {
    return ide_action_make(IDE_ACTION_NONE);
  }

  /* Check 2-second leader timeout */
  if (fsm->state == IDE_STATE_LEADER && fsm->last_input_usec > 0 &&
      (now_usec - fsm->last_input_usec) > fsm->leader_timeout_usec) {
    fsm->state = IDE_STATE_NORMAL;
    ide_fsm_clear_buffers(fsm);
  }

  IdeAction result = ide_action_make(IDE_ACTION_NONE);
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
  if (g_strcmp0(key, "g") == 0 &&
      (result.type == IDE_ACTION_MOVE_BUFFER_START ||
       result.type == IDE_ACTION_TREE_TOP)) {
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
