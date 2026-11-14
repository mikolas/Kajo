/* fx_registry.c — descriptor table of Kajo compositor effects. */
#include "fx_internal.h"

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

const CompFxEffectDesc *
fx_registry_get(FxSlotKind kind)
{
    switch (kind) {
    case FX_SLOT_SHADOW: return &k_fx_shadow_desc;
    case FX_SLOT_DIM:    return &k_fx_dim_desc;
    case FX_SLOT_COUNT:  break;
    }
    return NULL;
}

#endif /* WLR_HAS_KFX */
