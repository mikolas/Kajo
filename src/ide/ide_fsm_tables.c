#include "ide_fsm_private.h"

#include <string.h>

static const char *const kValidLeaderChords[] = {
  " ",  ",",  "ff", "fg", "fs", "fa", "bb", "bd", "bD", "e",  "o",  "a",  "k",  "?",  "h",
  "/",  "q",  "Q",  "mp", "mt", "md", "mm", "ne", "pe", "nd", "pd", "nh", "ph",
  "ns", "ps", "ds", "da", "dr", "db", "dB", "dd", "dc", "dn", "di", "do", "dp", "dq",
  "dv", "ca", "cr", "cf", "ch", "rb", "rt", "rr", "rc", "gd", "gi", "gr", "gs", "gc", "go",
  "vr", "sr", "sp", "sf", "s/", "ws", "wa", "wq", "w>", "w<", "w+", "w-", "ww", "wh", "wl",
  "wj", "wk", "ts", "th", "tl", "tj", "tk", "te", "tu", "tf", "tz", "to", "tp", "tr",
  "t1", "t2", "t3", "t4", "tb", "t[", "t]", "t/", NULL
};

gboolean
ide_fsm_is_leader_prefix(const char *prefix)
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

const char *const *
ide_fsm_get_valid_leader_chords(void)
{
  return kValidLeaderChords;
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

IdeState
ide_state_from_name(const char *name)
{
  if (name == NULL || name[0] == '\0') {
    return IDE_STATE_COUNT;
  }
  g_autofree char *lower = g_ascii_strdown(name, -1);
  g_strstrip(lower);
  if (g_strcmp0(lower, "normal") == 0) return IDE_STATE_NORMAL;
  if (g_strcmp0(lower, "insert") == 0) return IDE_STATE_INSERT;
  if (g_strcmp0(lower, "select") == 0) return IDE_STATE_SELECT;
  if (g_strcmp0(lower, "leader") == 0) return IDE_STATE_LEADER;
  if (g_strcmp0(lower, "match") == 0) return IDE_STATE_MATCH_PENDING;
  if (g_strcmp0(lower, "picker") == 0) return IDE_STATE_PICKER;
  if (g_strcmp0(lower, "drawer") == 0) return IDE_STATE_DRAWER;
  if (g_strcmp0(lower, "diff") == 0) return IDE_STATE_DIFF_REVIEW;
  if (g_strcmp0(lower, "tree") == 0) return IDE_STATE_TREE;
  if (g_strcmp0(lower, "hover") == 0) return IDE_STATE_HOVER;
  if (g_strcmp0(lower, "complete") == 0 || g_strcmp0(lower, "completion") == 0)
    return IDE_STATE_COMPLETION;
  if (g_strcmp0(lower, "action") == 0 || g_strcmp0(lower, "code_action") == 0)
    return IDE_STATE_CODE_ACTION;
  if (g_strcmp0(lower, "rename") == 0) return IDE_STATE_RENAME;
  if (g_strcmp0(lower, "search") == 0) return IDE_STATE_SEARCH;
  if (g_strcmp0(lower, "help") == 0) return IDE_STATE_HELP;
  return IDE_STATE_COUNT;
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
ide_fsm_get_chord_hud_text(IdeState state, const char *pending_chord)
{
  const char *chord = pending_chord ? pending_chord : "";
  if (state == IDE_STATE_LEADER) {
    if (g_strcmp0(chord, "b") == 0) {
      return "b b  Switch Buffers       b d  Close Buffer       b D  Force Close";
    }
    if (g_strcmp0(chord, "c") == 0) {
      return "c a  LSP Code Action      c r  LSP Rename         c f  LSP Format\n"
             "c h  Clear Search";
    }
    if (g_strcmp0(chord, "d") == 0) {
      return "d s  Review Agent Diffs   d a  Accept All         d r  Revert All\n"
             "d b  Toggle Breakpoint    d B  Cond Breakpoint    d d / d c  Start/Cont\n"
             "d p  Pause                d n  Step Over          d i  Step Into\n"
             "d o  Step Out             d q  Stop Session       d v  DAP Drawer";
    }
    if (g_strcmp0(chord, "f") == 0) {
      return "f f  Find Files           f g  Live Grep          f s  Save Buffer\n"
             "f a  Save All";
    }
    if (g_strcmp0(chord, "g") == 0) {
      return "g d  Go to Definition     g i  Implementation     g r  References\n"
             "g s  Signature Help       g c  Incoming Calls     g o  Outgoing Calls";
    }
    if (g_strcmp0(chord, "v") == 0) {
      return "v r  Relative Line Numbers";
    }
    if (g_strcmp0(chord, "t") == 0) {
      return "t s  Toggle Code Strip    t h / t l  Focus Col    t j / t k  Step D+1\n"
             "t e  Promote D+1 -> D0    t u  Surface Back       t f  Symbol / File\n"
             "t z  Zen 96/4             t o  Bird's-Eye Map     t p  Pin in D0\n"
             "t r  Resync Magnetic      t 1  Calls  t 2  Types  t 3  Impact  t 4  Diff\n"
             "t b  Branch Trail         t [ / t ]  Switch Trail t /  Filter D+1";
    }
    if (g_strcmp0(chord, "m") == 0) {
      return "m p  Toggle Markdown GUI  m m / m d  Mermaid Drawer\n"
             "m t  Toggle Checklist";
    }
    if (g_strcmp0(chord, "n") == 0) {
      return "n e  Next Build Error     n d  Next Diagnostic    n h  Next Diff Hunk\n"
             "n s  Next Search Match";
    }
    if (g_strcmp0(chord, "p") == 0) {
      return "p e  Prev Build Error     p d  Prev Diagnostic    p h  Prev Diff Hunk\n"
             "p s  Prev Search Match";
    }
    if (g_strcmp0(chord, "r") == 0) {
      return "r b  Build Project        r t  Run Tests          r r  Run Binary\n"
             "r c  Clean Build";
    }
    if (g_strcmp0(chord, "s") == 0) {
      return "s f / s /  Buffer Search  s r  Buffer Replace     s p  Project Replace";
    }
    if (g_strcmp0(chord, "w") == 0) {
      return "w s  Save Buffer          w a  Save All           w q  Force Close\n"
             "w w  Cycle Pane           w h  Focus Tree         w l  Focus Editor\n"
             "w j  Focus Drawer         w k  Focus Editor       w >  Grow Tree\n"
             "w <  Shrink Tree          w +  Grow Drawer        w -  Shrink Drawer";
    }
    return "f / SPC  Find Files      b / ,  Buffers     /  Live Grep\n"
           "e        File Tree       a      Alt File    w  Window / Save\n"
           "m        Markdown/Merm   s      Search/Rep  d  Agent / Debug\n"
           "c        Code / Format   r      Run / Build t  Code Strip / Lenses\n"
           "g        Goto / LSP      n / p  Next/Prev   v  View Options\n"
           "q / Q    Close Buffer    o      Drawer      k  Hover   ? / h  Help";
  }
  if (state == IDE_STATE_MATCH_PENDING) {
    return "i + ( [ { \" ' w f c b a   Select Inside Delimiter / Word / AST\n"
           "a + ( [ { \" ' w f c b a   Select Around Delimiter / Word / AST";
  }
  if (g_strcmp0(chord, "g") == 0) {
    return "g g  Buffer Start     g h  Line Start     g l  Line End\n"
           "g d  Go to Definition g r  References";
  }
  return "";
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
    { "lsp_references", IDE_ACTION_LSP_REFERENCES },
    { "references", IDE_ACTION_LSP_REFERENCES },
    { "lsp_signature_help", IDE_ACTION_LSP_SIGNATURE_HELP },
    { "signature_help", IDE_ACTION_LSP_SIGNATURE_HELP },
    { "lsp_call_hierarchy_in", IDE_ACTION_LSP_CALL_HIERARCHY_IN },
    { "call_hierarchy_in", IDE_ACTION_LSP_CALL_HIERARCHY_IN },
    { "incoming_calls", IDE_ACTION_LSP_CALL_HIERARCHY_IN },
    { "lsp_call_hierarchy_out", IDE_ACTION_LSP_CALL_HIERARCHY_OUT },
    { "call_hierarchy_out", IDE_ACTION_LSP_CALL_HIERARCHY_OUT },
    { "outgoing_calls", IDE_ACTION_LSP_CALL_HIERARCHY_OUT },
    { "lsp_call_hierarchy_toggle", IDE_ACTION_LSP_CALL_HIERARCHY_TOGGLE },
    { "toggle_relative_lines", IDE_ACTION_TOGGLE_RELATIVE_LINES },
    { "relative_lines", IDE_ACTION_TOGGLE_RELATIVE_LINES },
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
    { "dap_conditional_breakpoint", IDE_ACTION_DAP_CONDITIONAL_BREAKPOINT },
    { "conditional_breakpoint", IDE_ACTION_DAP_CONDITIONAL_BREAKPOINT },
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
    { "strip_toggle", IDE_ACTION_STRIP_TOGGLE },
    { "code_strip", IDE_ACTION_STRIP_TOGGLE },
    { "strip_focus_left", IDE_ACTION_STRIP_FOCUS_LEFT },
    { "strip_focus_right", IDE_ACTION_STRIP_FOCUS_RIGHT },
    { "strip_step_down", IDE_ACTION_STRIP_STEP_DOWN },
    { "strip_step_up", IDE_ACTION_STRIP_STEP_UP },
    { "strip_promote", IDE_ACTION_STRIP_PROMOTE },
    { "strip_surface", IDE_ACTION_STRIP_SURFACE },
    { "strip_toggle_full_file", IDE_ACTION_STRIP_TOGGLE_FULL_FILE },
    { "strip_toggle_zen", IDE_ACTION_STRIP_TOGGLE_ZEN },
    { "strip_toggle_overview", IDE_ACTION_STRIP_TOGGLE_OVERVIEW },
    { "strip_pin_toggle", IDE_ACTION_STRIP_PIN_TOGGLE },
    { "strip_resync", IDE_ACTION_STRIP_RESYNC },
    { "strip_lens_calls", IDE_ACTION_STRIP_LENS_CALLS },
    { "strip_lens_types", IDE_ACTION_STRIP_LENS_TYPES },
    { "strip_lens_impact", IDE_ACTION_STRIP_LENS_IMPACT },
    { "strip_lens_diff", IDE_ACTION_STRIP_LENS_DIFF },
    { "strip_branch_trail", IDE_ACTION_STRIP_BRANCH_TRAIL },
    { "strip_prev_trail", IDE_ACTION_STRIP_PREV_TRAIL },
    { "strip_next_trail", IDE_ACTION_STRIP_NEXT_TRAIL },
    { "strip_filter", IDE_ACTION_STRIP_FILTER },
    { NULL, IDE_ACTION_NONE }
  };

  for (gsize i = 0; kMap[i].key != NULL; i++) {
    if (g_strcmp0(lower, kMap[i].key) == 0) {
      return kMap[i].type;
    }
  }
  return IDE_ACTION_NONE;
}

/* ═══════════════════════════ Per-state key binding tables ═══════════════════════════ */

gboolean
ide_fsm_lookup_key_table(const char             *key,
                         const IdeFsmKeyBinding *table,
                         gsize                   count,
                         IdeActionType          *out_action)
{
  for (gsize i = 0; i < count; i++) {
    if (g_strcmp0(key, table[i].key) == 0) {
      *out_action = table[i].action;
      return TRUE;
    }
  }
  return FALSE;
}

gboolean
ide_fsm_lookup_normal_key(const char *key, IdeActionType *out_action)
{
  static const IdeFsmKeyBinding kNormalKeys[] = {
    { "n",                IDE_ACTION_SEARCH_NEXT },
    { "N",                IDE_ACTION_SEARCH_PREV },
    { "h",                IDE_ACTION_MOVE_LEFT },
    { "left",             IDE_ACTION_MOVE_LEFT },
    { "l",                IDE_ACTION_MOVE_RIGHT },
    { "right",            IDE_ACTION_MOVE_RIGHT },
    { "j",                IDE_ACTION_MOVE_DOWN },
    { "down",             IDE_ACTION_MOVE_DOWN },
    { "k",                IDE_ACTION_MOVE_UP },
    { "up",               IDE_ACTION_MOVE_UP },
    { "pgup",             IDE_ACTION_PAGE_UP },
    { "pageup",           IDE_ACTION_PAGE_UP },
    { "ctrl+b",           IDE_ACTION_PAGE_UP },
    { "ctrl+u",           IDE_ACTION_PAGE_UP },
    { "pgdown",           IDE_ACTION_PAGE_DOWN },
    { "pgdn",             IDE_ACTION_PAGE_DOWN },
    { "pagedown",         IDE_ACTION_PAGE_DOWN },
    { "ctrl+f",           IDE_ACTION_PAGE_DOWN },
    { "ctrl+d",           IDE_ACTION_PAGE_DOWN },
    { "home",             IDE_ACTION_MOVE_LINE_START },
    { "0",                IDE_ACTION_MOVE_LINE_START },
    { "end",              IDE_ACTION_MOVE_LINE_END },
    { "$",                IDE_ACTION_MOVE_LINE_END },
    { "ctrl+home",        IDE_ACTION_MOVE_BUFFER_START },
    { "ctrl+end",         IDE_ACTION_MOVE_BUFFER_END },
    { "G",                IDE_ACTION_MOVE_BUFFER_END },
    { "ctrl+right",       IDE_ACTION_MOVE_WORD_FORWARD },
    { "alt+right",        IDE_ACTION_MOVE_WORD_FORWARD },
    { "ctrl+left",        IDE_ACTION_MOVE_WORD_BACKWARD },
    { "alt+left",         IDE_ACTION_MOVE_WORD_BACKWARD },
    { "d",                IDE_ACTION_DELETE_CHAR },
    { "delete",           IDE_ACTION_DELETE_CHAR },
    { "ctrl+x",           IDE_ACTION_DELETE_CHAR },
    { "backspace",        IDE_ACTION_MOVE_LEFT },
    { "y",                IDE_ACTION_YANK },
    { "ctrl+c",           IDE_ACTION_YANK },
    { "p",                IDE_ACTION_PASTE_AFTER },
    { "ctrl+v",           IDE_ACTION_PASTE_AFTER },
    { "P",                IDE_ACTION_PASTE_BEFORE },
    { "u",                IDE_ACTION_UNDO },
    { "ctrl+z",           IDE_ACTION_UNDO },
    { "U",                IDE_ACTION_REDO },
    { "ctrl+y",           IDE_ACTION_REDO },
    { "ctrl+r",           IDE_ACTION_REDO },
    { "ctrl+shift+z",     IDE_ACTION_REDO },
    { ">",                IDE_ACTION_INDENT },
    { "<",                IDE_ACTION_OUTDENT },
    { "H",                IDE_ACTION_TAB_PREV },
    { "L",                IDE_ACTION_TAB_NEXT },
    { "ö",                IDE_ACTION_STEP_BACKWARD },
    { "odiaeresis",       IDE_ACTION_STEP_BACKWARD },
    { "ä",                IDE_ACTION_STEP_FORWARD },
    { "adiaeresis",       IDE_ACTION_STEP_FORWARD },
    { "ctrl+o",           IDE_ACTION_JUMPLIST_BACK },
    { "ctrl+i",           IDE_ACTION_JUMPLIST_FORWARD },
    { "ctrl+w",           IDE_ACTION_PANE_CYCLE },
    { "K",                IDE_ACTION_LSP_HOVER },
    { "ctrl+shift+space", IDE_ACTION_LSP_SIGNATURE_HELP },
    { "ctrl+s",           IDE_ACTION_SAVE },
    { "f5",               IDE_ACTION_DAP_CONTINUE },
    { "f9",               IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
    { "shift+f9",         IDE_ACTION_DAP_CONDITIONAL_BREAKPOINT },
    { "f10",              IDE_ACTION_DAP_STEP_OVER },
    { "f11",              IDE_ACTION_DAP_STEP_INTO },
    { "shift+f11",        IDE_ACTION_DAP_STEP_OUT },
    { "f12",              IDE_ACTION_DAP_STEP_OUT },
    { "shift+f5",         IDE_ACTION_DAP_STOP },
  };
  return ide_fsm_lookup_key_table(key, kNormalKeys, G_N_ELEMENTS(kNormalKeys), out_action);
}

gboolean
ide_fsm_lookup_insert_key(const char *key, IdeActionType *out_action)
{
  static const IdeFsmKeyBinding kInsertKeys[] = {
    { "enter",            IDE_ACTION_INSERT_NEWLINE },
    { "tab",              IDE_ACTION_INSERT_TAB },
    { "shift+tab",        IDE_ACTION_OUTDENT },
    { "btab",             IDE_ACTION_OUTDENT },
    { "ctrl+space",       IDE_ACTION_LSP_COMPLETE },
    { "ctrl+@",           IDE_ACTION_LSP_COMPLETE },
    { "ctrl+shift+space", IDE_ACTION_LSP_SIGNATURE_HELP },
    { "ctrl+s",           IDE_ACTION_SAVE },
    { "ctrl+z",           IDE_ACTION_UNDO },
    { "ctrl+y",           IDE_ACTION_REDO },
    { "ctrl+r",           IDE_ACTION_REDO },
    { "ctrl+shift+z",     IDE_ACTION_REDO },
    { "ctrl+c",           IDE_ACTION_YANK },
    { "ctrl+v",           IDE_ACTION_PASTE_AFTER },
    { "ctrl+x",           IDE_ACTION_DELETE_SELECTION },
    { "ctrl+w",           IDE_ACTION_PANE_CYCLE },
    { "backspace",        IDE_ACTION_BACKSPACE },
    { "delete",           IDE_ACTION_DELETE_CHAR },
    { "left",             IDE_ACTION_MOVE_LEFT },
    { "right",            IDE_ACTION_MOVE_RIGHT },
    { "up",               IDE_ACTION_MOVE_UP },
    { "down",             IDE_ACTION_MOVE_DOWN },
    { "pgup",             IDE_ACTION_PAGE_UP },
    { "pageup",           IDE_ACTION_PAGE_UP },
    { "pgdown",           IDE_ACTION_PAGE_DOWN },
    { "pgdn",             IDE_ACTION_PAGE_DOWN },
    { "pagedown",         IDE_ACTION_PAGE_DOWN },
    { "home",             IDE_ACTION_MOVE_LINE_START },
    { "end",              IDE_ACTION_MOVE_LINE_END },
    { "ctrl+home",        IDE_ACTION_MOVE_BUFFER_START },
    { "ctrl+end",         IDE_ACTION_MOVE_BUFFER_END },
    { "ctrl+right",       IDE_ACTION_MOVE_WORD_FORWARD },
    { "alt+right",        IDE_ACTION_MOVE_WORD_FORWARD },
    { "ctrl+left",        IDE_ACTION_MOVE_WORD_BACKWARD },
    { "alt+left",         IDE_ACTION_MOVE_WORD_BACKWARD },
  };
  return ide_fsm_lookup_key_table(key, kInsertKeys, G_N_ELEMENTS(kInsertKeys), out_action);
}

gboolean
ide_fsm_lookup_select_key(const char *key, IdeActionType *out_action)
{
  static const IdeFsmKeyBinding kSelectKeys[] = {
    { "ctrl+s",     IDE_ACTION_SAVE },
    { ">",          IDE_ACTION_INDENT },
    { "tab",        IDE_ACTION_INDENT },
    { "<",          IDE_ACTION_OUTDENT },
    { "shift+tab",  IDE_ACTION_OUTDENT },
    { "pgup",       IDE_ACTION_PAGE_UP },
    { "pageup",     IDE_ACTION_PAGE_UP },
    { "ctrl+u",     IDE_ACTION_PAGE_UP },
    { "pgdown",     IDE_ACTION_PAGE_DOWN },
    { "pgdn",       IDE_ACTION_PAGE_DOWN },
    { "pagedown",   IDE_ACTION_PAGE_DOWN },
    { "ctrl+d",     IDE_ACTION_PAGE_DOWN },
    { "home",       IDE_ACTION_MOVE_LINE_START },
    { "0",          IDE_ACTION_MOVE_LINE_START },
    { "end",        IDE_ACTION_MOVE_LINE_END },
    { "$",          IDE_ACTION_MOVE_LINE_END },
    { "ctrl+home",  IDE_ACTION_MOVE_BUFFER_START },
    { "ctrl+end",   IDE_ACTION_MOVE_BUFFER_END },
    { "G",          IDE_ACTION_MOVE_BUFFER_END },
    { "w",          IDE_ACTION_SELECT_WORD },
    { "ctrl+right", IDE_ACTION_SELECT_WORD },
    { "b",          IDE_ACTION_SELECT_WORD_BACKWARD },
    { "ctrl+left",  IDE_ACTION_SELECT_WORD_BACKWARD },
    { "x",          IDE_ACTION_SELECT_LINE },
  };
  return ide_fsm_lookup_key_table(key, kSelectKeys, G_N_ELEMENTS(kSelectKeys), out_action);
}

gboolean
ide_fsm_lookup_drawer_edit_key(const char *key, IdeActionType *out_action)
{
  static const IdeFsmKeyBinding kEditKeys[] = {
    { "enter",     IDE_ACTION_REPLACE_APPLY_ALL },
    { "tab",       IDE_ACTION_REPLACE_NEXT_FIELD },
    { "down",      IDE_ACTION_REPLACE_NEXT_FIELD },
    { "shift+tab", IDE_ACTION_REPLACE_PREV_FIELD },
    { "btab",      IDE_ACTION_REPLACE_PREV_FIELD },
    { "up",        IDE_ACTION_REPLACE_PREV_FIELD },
    { "alt+c",     IDE_ACTION_REPLACE_TOGGLE_CASE },
    { "alt+w",     IDE_ACTION_REPLACE_TOGGLE_WORD },
    { "alt+r",     IDE_ACTION_REPLACE_TOGGLE_REGEX },
    { "ctrl+r",    IDE_ACTION_REPLACE_TOGGLE_REGEX },
    { "alt+a",     IDE_ACTION_REPLACE_APPLY_ALL },
    { "ctrl+a",    IDE_ACTION_REPLACE_APPLY_ALL },
    { "alt+y",     IDE_ACTION_REPLACE_APPLY_CURRENT },
    { "ctrl+y",    IDE_ACTION_REPLACE_APPLY_CURRENT },
    { "pgup",      IDE_ACTION_PAGE_UP },
    { "pageup",    IDE_ACTION_PAGE_UP },
    { "pgdown",    IDE_ACTION_PAGE_DOWN },
    { "pgdn",      IDE_ACTION_PAGE_DOWN },
    { "pagedown",  IDE_ACTION_PAGE_DOWN },
    { "backspace", IDE_ACTION_BACKSPACE },
  };
  return ide_fsm_lookup_key_table(key, kEditKeys, G_N_ELEMENTS(kEditKeys), out_action);
}

gboolean
ide_fsm_lookup_drawer_key(const char *key, IdeActionType *out_action)
{
  static const IdeFsmKeyBinding kDrawerKeys[] = {
    { "Z",         IDE_ACTION_DRAWER_MAXIMIZE },
    { "j",         IDE_ACTION_MOVE_DOWN },
    { "down",      IDE_ACTION_MOVE_DOWN },
    { "k",         IDE_ACTION_MOVE_UP },
    { "up",        IDE_ACTION_MOVE_UP },
    { "d",         IDE_ACTION_PAGE_DOWN },
    { "ctrl+d",    IDE_ACTION_PAGE_DOWN },
    { "pgdown",    IDE_ACTION_PAGE_DOWN },
    { "pgdn",      IDE_ACTION_PAGE_DOWN },
    { "pagedown",  IDE_ACTION_PAGE_DOWN },
    { "u",         IDE_ACTION_PAGE_UP },
    { "ctrl+u",    IDE_ACTION_PAGE_UP },
    { "pgup",      IDE_ACTION_PAGE_UP },
    { "pageup",    IDE_ACTION_PAGE_UP },
    { "G",         IDE_ACTION_MOVE_BUFFER_END },
    { "ctrl+end",  IDE_ACTION_MOVE_BUFFER_END },
    { "home",      IDE_ACTION_MOVE_BUFFER_START },
    { "ctrl+home", IDE_ACTION_MOVE_BUFFER_START },
    { "f5",        IDE_ACTION_DAP_CONTINUE },
    { "f9",        IDE_ACTION_DAP_TOGGLE_BREAKPOINT },
    { "f10",       IDE_ACTION_DAP_STEP_OVER },
    { "f11",       IDE_ACTION_DAP_STEP_INTO },
    { "shift+f11", IDE_ACTION_DAP_STEP_OUT },
    { "f12",       IDE_ACTION_DAP_STEP_OUT },
    { "shift+f5",  IDE_ACTION_DAP_STOP },
    { "ctrl+w",    IDE_ACTION_PANE_CYCLE },
  };
  return ide_fsm_lookup_key_table(key, kDrawerKeys, G_N_ELEMENTS(kDrawerKeys), out_action);
}

gboolean
ide_fsm_lookup_tree_key(const char *key, IdeActionType *out_action)
{
  static const IdeFsmKeyBinding kTreeKeys[] = {
    { "j",        IDE_ACTION_TREE_DOWN },
    { "down",     IDE_ACTION_TREE_DOWN },
    { "k",        IDE_ACTION_TREE_UP },
    { "up",       IDE_ACTION_TREE_UP },
    { "enter",    IDE_ACTION_TREE_TOGGLE_OR_OPEN },
    { "l",        IDE_ACTION_TREE_TOGGLE_OR_OPEN },
    { "right",    IDE_ACTION_TREE_TOGGLE_OR_OPEN },
    { "h",        IDE_ACTION_TREE_COLLAPSE_OR_PARENT },
    { "left",     IDE_ACTION_TREE_COLLAPSE_OR_PARENT },
    { "pgdown",   IDE_ACTION_TREE_PAGE_DOWN },
    { "pgdn",     IDE_ACTION_TREE_PAGE_DOWN },
    { "pagedown", IDE_ACTION_TREE_PAGE_DOWN },
    { "ctrl+d",   IDE_ACTION_TREE_PAGE_DOWN },
    { "pgup",     IDE_ACTION_TREE_PAGE_UP },
    { "pageup",   IDE_ACTION_TREE_PAGE_UP },
    { "ctrl+u",   IDE_ACTION_TREE_PAGE_UP },
    { "home",     IDE_ACTION_TREE_TOP },
    { "end",      IDE_ACTION_TREE_BOTTOM },
    { "G",        IDE_ACTION_TREE_BOTTOM },
    { "r",        IDE_ACTION_TREE_REFRESH },
    { "d",        IDE_ACTION_TREE_DELETE },
    { "ctrl+w",   IDE_ACTION_PANE_CYCLE },
  };
  return ide_fsm_lookup_key_table(key, kTreeKeys, G_N_ELEMENTS(kTreeKeys), out_action);
}
