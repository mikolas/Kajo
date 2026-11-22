#ifndef KAJO_IDE_FSM_PRIVATE_H
#define KAJO_IDE_FSM_PRIVATE_H

#include "ide_fsm.h"

G_BEGIN_DECLS

typedef struct {
  const char   *key;
  IdeActionType action;
} IdeFsmKeyBinding;

gboolean ide_fsm_is_leader_prefix(const char *prefix);
gboolean ide_fsm_lookup_key_table(const char             *key,
                                  const IdeFsmKeyBinding *table,
                                  gsize                   count,
                                  IdeActionType          *out_action);
gboolean ide_fsm_lookup_normal_key(const char *key, IdeActionType *out_action);
gboolean ide_fsm_lookup_insert_key(const char *key, IdeActionType *out_action);
gboolean ide_fsm_lookup_select_key(const char *key, IdeActionType *out_action);
gboolean ide_fsm_lookup_drawer_edit_key(const char *key, IdeActionType *out_action);
gboolean ide_fsm_lookup_drawer_key(const char *key, IdeActionType *out_action);
gboolean ide_fsm_lookup_tree_key(const char *key, IdeActionType *out_action);

G_END_DECLS

#endif /* KAJO_IDE_FSM_PRIVATE_H */
