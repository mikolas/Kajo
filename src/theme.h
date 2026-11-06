#ifndef SHELL_THEME_H
#define SHELL_THEME_H

#include <gtk/gtk.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ShellTheme ShellTheme;

/* Accent palette shared by the panel theme picker and kajo-settings. */
typedef struct {
    const char *id;
    const char *name;
    const char *accent;
    const char *hover;
} ShellAccentPreset;

#define SHELL_N_ACCENT_PRESETS 8
extern const ShellAccentPreset shell_accent_presets[SHELL_N_ACCENT_PRESETS];

struct _ShellTheme {
    GtkCssProvider *provider;
    GFileMonitor   *monitor;
    gchar          *css_path;

    GtkCssProvider *override_provider;
    GFileMonitor   *override_monitor;
    gchar          *override_css_path;
};

ShellTheme *shell_theme_new(const gchar *css_path);
void        shell_theme_apply(ShellTheme *theme, GdkDisplay *display);
void        shell_theme_reload(ShellTheme *theme);
void        shell_theme_destroy(ShellTheme *theme);
void        shell_theme_write_override_css(const gchar *accent_hex,
                                           const gchar *hover_hex,
                                           gint         body_px,
                                           gint         heading_px,
                                           gint         subtitle_px,
                                           gint         badge_px);
void        shell_theme_sync_from_config(const ShellConfig *config);

/* Pango font-family list for the kajo-ide editor font: the user's choice first,
 * then the same fallbacks the generated CSS uses ("Fira Code", "FiraCode Nerd
 * Font", monospace). Anything that draws editor text outside CSS (settings
 * preview, Cairo gutter) must use this so it renders the same face as the
 * editor even when the chosen family is not installed. Caller frees. */
gchar      *shell_ide_font_stack(const gchar *family);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_THEME_H */
