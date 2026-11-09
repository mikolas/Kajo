#ifndef KAJO_MOD_VIS_H
#define KAJO_MOD_VIS_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KAJO_MOD_ENGINE_KIND_DEFINED
#define KAJO_MOD_ENGINE_KIND_DEFINED

#define KAJO_MOD_SCOPE_SAMPLES 256
#define KAJO_MOD_FFT_BANDS     16

typedef enum {
    KAJO_MOD_ENGINE_UADE = 0,
    KAJO_MOD_ENGINE_SIDPLAYFP,
    KAJO_MOD_ENGINE_GME,
    KAJO_MOD_ENGINE_OPENMPT,
    KAJO_MOD_ENGINE_XMP,
    KAJO_MOD_ENGINE_NONE
} KajoModEngineKind;

#endif /* KAJO_MOD_ENGINE_KIND_DEFINED */

GtkWidget *kajo_mod_scope_new(void);
void       kajo_mod_scope_clear(GtkWidget *scope);
void       kajo_mod_scope_set_pcm(GtkWidget *scope, const float *left, const float *right, int n_samples);

GtkWidget *kajo_mod_fft_new(void);
void       kajo_mod_fft_clear(GtkWidget *fft);
void       kajo_mod_fft_set_bands(GtkWidget *fft, const float bands[KAJO_MOD_FFT_BANDS], const float peaks[KAJO_MOD_FFT_BANDS]);

#ifdef __cplusplus
}
#endif

#endif /* KAJO_MOD_VIS_H */
