#include "../src/ide/ide_fsm.h"

#include <glib.h>

static void
test_fsm_selection_first_workflow(void)
{
  IdeFsm *fsm = ide_fsm_new();
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);
  g_assert_cmpstr(ide_state_to_string(ide_fsm_get_state(fsm)), ==, "NORMAL");

  /* 'w' selects word and enters SELECT mode */
  IdeAction act = ide_fsm_handle_key(fsm, "w");
  g_assert_cmpint(act.type, ==, IDE_ACTION_SELECT_WORD);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_SELECT);

  /* 'l' in SELECT mode extends selection right */
  act = ide_fsm_handle_key(fsm, "l");
  g_assert_cmpint(act.type, ==, IDE_ACTION_EXTEND_SELECTION);
  g_assert_cmpstr(act.str, ==, "right");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_SELECT);

  /* 'd' in SELECT mode deletes selection and returns to NORMAL */
  act = ide_fsm_handle_key(fsm, "d");
  g_assert_cmpint(act.type, ==, IDE_ACTION_DELETE_SELECTION);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* 'x' selects line -> 'c' changes selection and enters INSERT mode */
  act = ide_fsm_handle_key(fsm, "x");
  g_assert_cmpint(act.type, ==, IDE_ACTION_SELECT_LINE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_SELECT);

  act = ide_fsm_handle_key(fsm, "c");
  g_assert_cmpint(act.type, ==, IDE_ACTION_CHANGE_SELECTION);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_INSERT);

  ide_fsm_free(fsm);
}

static void
test_fsm_insert_and_jk_fast_exit(void)
{
  IdeFsm *fsm = ide_fsm_new();
  gint64 t0 = 1000000;

  /* Enter INSERT mode via 'i' */
  IdeAction act = ide_fsm_handle_key_at(fsm, "i", t0);
  g_assert_cmpint(act.type, ==, IDE_ACTION_NONE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_INSERT);

  /* Type 'j' followed by 'k' after 80ms (< 200ms threshold) -> exits to NORMAL */
  act = ide_fsm_handle_key_at(fsm, "j", t0 + 10000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_INSERT_CHAR);
  g_assert_cmpstr(act.str, ==, "j");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_INSERT);

  act = ide_fsm_handle_key_at(fsm, "k", t0 + 90000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_BACKSPACE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Re-enter INSERT mode; 'j' followed by 'k' after 350ms (>= 200ms) stays in INSERT */
  ide_fsm_handle_key_at(fsm, "i", t0 + 200000);
  act = ide_fsm_handle_key_at(fsm, "j", t0 + 300000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_INSERT_CHAR);
  act = ide_fsm_handle_key_at(fsm, "k", t0 + 650000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_INSERT_CHAR);
  g_assert_cmpstr(act.str, ==, "k");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_INSERT);

  ide_fsm_free(fsm);
}

static void
test_fsm_nordic_text_objects_and_stepping(void)
{
  IdeFsm *fsm = ide_fsm_new();

  /* 'm i f' -> Select inside function */
  IdeAction act = ide_fsm_handle_key(fsm, "m");
  g_assert_cmpint(act.type, ==, IDE_ACTION_NONE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_MATCH_PENDING);

  act = ide_fsm_handle_key(fsm, "i");
  g_assert_cmpint(act.type, ==, IDE_ACTION_NONE);
  g_assert_cmpstr(ide_fsm_get_pending_chord(fsm), ==, "i");

  act = ide_fsm_handle_key(fsm, "f");
  g_assert_cmpint(act.type, ==, IDE_ACTION_SELECT_TEXT_OBJECT);
  g_assert_cmpstr(act.arg1, ==, "i");
  g_assert_cmpstr(act.arg2, ==, "f");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_SELECT);

  /* Collapse selection and test 'm a b' (around block {}) */
  ide_fsm_handle_key(fsm, "esc");
  ide_fsm_handle_key(fsm, "m");
  ide_fsm_handle_key(fsm, "a");
  act = ide_fsm_handle_key(fsm, "b");
  g_assert_cmpint(act.type, ==, IDE_ACTION_SELECT_TEXT_OBJECT);
  g_assert_cmpstr(act.arg1, ==, "a");
  g_assert_cmpstr(act.arg2, ==, "b");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_SELECT);

  /* Collapse selection and test Nordic 'ö' / 'ä' quick stepping */
  ide_fsm_handle_key(fsm, "esc");
  act = ide_fsm_handle_key(fsm, "ö");
  g_assert_cmpint(act.type, ==, IDE_ACTION_STEP_BACKWARD);
  act = ide_fsm_handle_key(fsm, "ä");
  g_assert_cmpint(act.type, ==, IDE_ACTION_STEP_FORWARD);

  /* Test Alt+1 .. Alt+9 direct tab switching */
  act = ide_fsm_handle_key(fsm, "alt+4");
  g_assert_cmpint(act.type, ==, IDE_ACTION_TAB_SWITCH);
  g_assert_cmpint(act.int_val, ==, 4);

  ide_fsm_free(fsm);
}

static void
test_fsm_leader_chords_and_timeout(void)
{
  IdeFsm *fsm = ide_fsm_new();
  gint64 t0 = 5000000;

  /* Space f f -> Fuzzy File Picker */
  ide_fsm_handle_key_at(fsm, "Space", t0);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_LEADER);
  ide_fsm_handle_key_at(fsm, "f", t0 + 50000);
  g_assert_cmpstr(ide_fsm_get_pending_chord(fsm), ==, "f");
  IdeAction act = ide_fsm_handle_key_at(fsm, "f", t0 + 100000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_PICKER_OPEN_FILES);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_PICKER);

  /* Esc closes picker -> back to NORMAL */
  act = ide_fsm_handle_key_at(fsm, "esc", t0 + 150000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_PICKER_CANCEL);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Space d s -> Agent Review Open */
  ide_fsm_handle_key_at(fsm, "Space", t0 + 200000);
  ide_fsm_handle_key_at(fsm, "d", t0 + 250000);
  act = ide_fsm_handle_key_at(fsm, "s", t0 + 300000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_AGENT_REVIEW_OPEN);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_DIFF_REVIEW);
  ide_fsm_handle_key_at(fsm, "q", t0 + 350000);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Space s p -> Project Search & Replace (editable drawer) */
  ide_fsm_handle_key_at(fsm, "Space", t0 + 400000);
  ide_fsm_handle_key_at(fsm, "s", t0 + 420000);
  act = ide_fsm_handle_key_at(fsm, "p", t0 + 440000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_PROJECT_REPLACE_OPEN);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_DRAWER);
  g_assert_true(ide_fsm_get_drawer_editable(fsm));

  act = ide_fsm_handle_key_at(fsm, "alt+r", t0 + 460000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_REPLACE_TOGGLE_REGEX);
  act = ide_fsm_handle_key_at(fsm, "esc", t0 + 480000);
  g_assert_cmpint(act.type, ==, IDE_ACTION_DRAWER_CLOSE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Leader timeout (> 2.0s) resets stale LEADER back to NORMAL and swallows the
   * expired key so keys like 'a', 'i', 'c', or 'j' never trigger NORMAL actions
   * or accidentally enter INSERT mode. */
  ide_fsm_handle_key_at(fsm, "Space", t0 + 500000);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_LEADER);
  act = ide_fsm_handle_key_at(fsm, "j", t0 + 3000000); /* +2.5s elapsed */
  g_assert_cmpint(act.type, ==, IDE_ACTION_NONE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Specifically test Space -> c -> >2.0s pause -> 'a' (must stay NORMAL, not INSERT) */
  ide_fsm_handle_key_at(fsm, "Space", t0 + 4000000);
  ide_fsm_handle_key_at(fsm, "c", t0 + 4100000);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_LEADER);
  g_assert_cmpstr(ide_fsm_get_pending_chord(fsm), ==, "c");
  act = ide_fsm_handle_key_at(fsm, "a", t0 + 6500000); /* +2.4s elapsed */
  g_assert_cmpint(act.type, ==, IDE_ACTION_NONE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Explicit timer expiration via ide_fsm_expire_leader() */
  ide_fsm_handle_key_at(fsm, "Space", t0 + 7000000);
  ide_fsm_handle_key_at(fsm, "w", t0 + 7050000);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_LEADER);
  g_assert_true(ide_fsm_expire_leader(fsm));
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);
  g_assert_cmpstr(ide_fsm_get_pending_chord(fsm), ==, "");
  g_assert_false(ide_fsm_expire_leader(fsm));

  /* Verify stray 'S' after Space is NOT treated as a prefix for "Space" */
  ide_fsm_set_state(fsm, IDE_STATE_NORMAL);
  ide_fsm_handle_key(fsm, "Space");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_LEADER);
  act = ide_fsm_handle_key(fsm, "S");
  g_assert_cmpint(act.type, ==, IDE_ACTION_NONE);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Verify Which-Key HUD text accuracy */
  const char *g_hud = ide_fsm_get_chord_hud_text(IDE_STATE_NORMAL, "g");
  g_assert_nonnull(strstr(g_hud, "g g"));
  g_assert_nonnull(strstr(g_hud, "g d"));
  g_assert_null(strstr(g_hud, "g e"));
  g_assert_null(strstr(g_hud, "g n"));
  g_assert_null(strstr(g_hud, "g p"));
  const char *n_hud = ide_fsm_get_chord_hud_text(IDE_STATE_LEADER, "n");
  g_assert_nonnull(strstr(n_hud, "Next Search Match"));
  g_assert_null(strstr(n_hud, "Next Buffer"));
  const char *s_hud = ide_fsm_get_chord_hud_text(IDE_STATE_LEADER, "s");
  g_assert_nonnull(strstr(s_hud, "Buffer Search"));
  g_assert_null(strstr(s_hud, "Find Files"));

  /* Verify every chord in kValidLeaderChords dispatches a non-NONE action */
  const char *const *chords = ide_fsm_get_valid_leader_chords();
  g_assert_nonnull(chords);
  gsize n_chords = 0;
  for (gsize i = 0; chords[i] != NULL; i++) {
    n_chords++;
    ide_fsm_set_state(fsm, IDE_STATE_NORMAL);
    ide_fsm_handle_key(fsm, "Space");
    const char *c = chords[i];
    if (g_strcmp0(c, " ") == 0) {
      act = ide_fsm_handle_key(fsm, "Space");
    } else {
      for (gsize k = 0; c[k] != '\0'; k++) {
        char kstr[2] = { c[k], '\0' };
        act = ide_fsm_handle_key(fsm, kstr);
      }
    }
    g_assert_cmpint(act.type, !=, IDE_ACTION_NONE);
  }
  g_assert_cmpuint(n_chords, >, 30);

  ide_fsm_free(fsm);
}

static void
test_fsm_custom_remaps(void)
{
  IdeFsm *fsm = ide_fsm_new();
  IdeActionType act_type = ide_action_type_from_name("save_all");
  g_assert_cmpint(act_type, ==, IDE_ACTION_SAVE_ALL);

  ide_fsm_set_remap(fsm, IDE_STATE_NORMAL, "S", IDE_ACTION_SAVE_ALL);
  IdeAction act = ide_fsm_handle_key(fsm, "S");
  g_assert_cmpint(act.type, ==, IDE_ACTION_SAVE_ALL);

  ide_fsm_clear_remaps(fsm);
  act = ide_fsm_handle_key(fsm, "S");
  g_assert_cmpint(act.type, ==, IDE_ACTION_NONE);

  ide_fsm_free(fsm);
}

static void
test_fsm_standard_shortcuts_and_chords(void)
{
  IdeFsm *fsm = ide_fsm_new();

  /* Verify CSS class names match style.css */
  g_assert_cmpstr(ide_state_get_css_class(IDE_STATE_NORMAL), ==, "mode-normal");
  g_assert_cmpstr(ide_state_get_css_class(IDE_STATE_INSERT), ==, "mode-insert");
  g_assert_cmpstr(ide_state_get_css_class(IDE_STATE_SELECT), ==, "mode-select");
  g_assert_cmpstr(ide_state_get_css_class(IDE_STATE_LEADER), ==, "mode-leader");
  g_assert_cmpstr(ide_state_get_css_class(IDE_STATE_SEARCH), ==, "mode-search");

  /* Ctrl+z / Ctrl+y / Ctrl+r / Ctrl+s in NORMAL mode */
  IdeAction act = ide_fsm_handle_key(fsm, "ctrl+z");
  g_assert_cmpint(act.type, ==, IDE_ACTION_UNDO);
  act = ide_fsm_handle_key(fsm, "ctrl+y");
  g_assert_cmpint(act.type, ==, IDE_ACTION_REDO);
  act = ide_fsm_handle_key(fsm, "ctrl+r");
  g_assert_cmpint(act.type, ==, IDE_ACTION_REDO);
  act = ide_fsm_handle_key(fsm, "ctrl+s");
  g_assert_cmpint(act.type, ==, IDE_ACTION_SAVE);

  /* Lowercase "space" + "space" -> Fuzzy File Picker */
  ide_fsm_handle_key(fsm, "space");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_LEADER);
  act = ide_fsm_handle_key(fsm, "space");
  g_assert_cmpint(act.type, ==, IDE_ACTION_PICKER_OPEN_FILES);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_PICKER);
  ide_fsm_handle_key(fsm, "esc");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Space m p -> Toggle Markdown Preview and stay in NORMAL mode */
  ide_fsm_handle_key(fsm, "space");
  ide_fsm_handle_key(fsm, "m");
  act = ide_fsm_handle_key(fsm, "p");
  g_assert_cmpint(act.type, ==, IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* g chords: gg, gh, gl, gd */
  ide_fsm_handle_key(fsm, "g");
  g_assert_cmpstr(ide_fsm_get_pending_chord(fsm), ==, "g");
  act = ide_fsm_handle_key(fsm, "g");
  g_assert_cmpint(act.type, ==, IDE_ACTION_MOVE_BUFFER_START);

  ide_fsm_handle_key(fsm, "g");
  act = ide_fsm_handle_key(fsm, "l");
  g_assert_cmpint(act.type, ==, IDE_ACTION_MOVE_LINE_END);

  /* 'o' and 'O' open line and enter INSERT mode */
  act = ide_fsm_handle_key(fsm, "o");
  g_assert_cmpint(act.type, ==, IDE_ACTION_INSERT_NEWLINE);
  g_assert_cmpstr(act.str, ==, "below");
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_INSERT);

  /* Ctrl+z in INSERT mode undoes without inserting 'z' */
  act = ide_fsm_handle_key(fsm, "ctrl+z");
  g_assert_cmpint(act.type, ==, IDE_ACTION_UNDO);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_INSERT);

  /* Lowercase "pgdn" works in NORMAL mode */
  ide_fsm_handle_key(fsm, "esc");
  act = ide_fsm_handle_key(fsm, "pgdn");
  g_assert_cmpint(act.type, ==, IDE_ACTION_PAGE_DOWN);

  /* Tree 'a' and 'A' enable tree_editable so prompt receives text input */
  ide_fsm_set_state(fsm, IDE_STATE_TREE);
  g_assert_false(ide_fsm_get_tree_editable(fsm));
  act = ide_fsm_handle_key(fsm, "a");
  g_assert_cmpint(act.type, ==, IDE_ACTION_TREE_CREATE_FILE);
  g_assert_true(ide_fsm_get_tree_editable(fsm));
  act = ide_fsm_handle_key(fsm, "x");
  g_assert_cmpint(act.type, ==, IDE_ACTION_INSERT_CHAR);
  g_assert_cmpstr(act.str, ==, "x");
  ide_fsm_set_tree_editable(fsm, FALSE);

  /* Code Action numeric shortcut '1'..'9' passes 1-based index in int_val */
  ide_fsm_set_state(fsm, IDE_STATE_CODE_ACTION);
  act = ide_fsm_handle_key(fsm, "3");
  g_assert_cmpint(act.type, ==, IDE_ACTION_LSP_SELECT_CONFIRM);
  g_assert_cmpint(act.int_val, ==, 3);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  /* Completion triggered from NORMAL mode restores NORMAL on Esc */
  ide_fsm_set_state(fsm, IDE_STATE_NORMAL);
  ide_fsm_set_state(fsm, IDE_STATE_COMPLETION);
  g_assert_cmpint(ide_fsm_get_previous_state(fsm), ==, IDE_STATE_NORMAL);
  act = ide_fsm_handle_key(fsm, "esc");
  g_assert_cmpint(act.type, ==, IDE_ACTION_LSP_CANCEL);
  g_assert_cmpint(ide_fsm_get_state(fsm), ==, IDE_STATE_NORMAL);

  ide_fsm_free(fsm);
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/ide/fsm/selection-first-workflow", test_fsm_selection_first_workflow);
  g_test_add_func("/ide/fsm/insert-and-jk-fast-exit", test_fsm_insert_and_jk_fast_exit);
  g_test_add_func("/ide/fsm/nordic-text-objects-and-stepping", test_fsm_nordic_text_objects_and_stepping);
  g_test_add_func("/ide/fsm/leader-chords-and-timeout", test_fsm_leader_chords_and_timeout);
  g_test_add_func("/ide/fsm/custom-remaps", test_fsm_custom_remaps);
  g_test_add_func("/ide/fsm/standard-shortcuts-and-chords", test_fsm_standard_shortcuts_and_chords);

  return g_test_run();
}
