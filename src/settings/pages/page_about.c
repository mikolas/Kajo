#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../compositor/compositor.h"
#include <sys/utsname.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _SettingsPageAbout {
    GtkBox  parent_instance;

    gchar  *kernel_summary;
    gchar  *cpu_summary;
    gchar  *mem_summary;
    gchar  *compositor_summary;
    gchar  *gtk_summary;
    guint   copy_reset_timer_id;
} SettingsPageAbout;

typedef struct _SettingsPageAboutClass {
    GtkBoxClass parent_class;
} SettingsPageAboutClass;

G_DEFINE_TYPE(SettingsPageAbout, settings_page_about, GTK_TYPE_BOX)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SettingsPageAbout, g_object_unref)

static GtkWidget *
create_about_info_row(const char *glyph,
                      const char *title,
                      const char *subtitle,
                      const char *badge_text,
                      const char *badge_style)
{
    GtkWidget *badge = gtk_label_new(badge_text ? badge_text : "");
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    if (badge_style && badge_style[0] != '\0')
        gtk_widget_add_css_class(badge, badge_style);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

    return create_boxed_row(glyph, title, subtitle, badge);
}

static gboolean
on_reset_copied_badge_timeout(gpointer user_data)
{
    SettingsPageAbout *self = (SettingsPageAbout *)user_data;
    self->copy_reset_timer_id = 0;
    settings_page_set_live_status(GTK_WIDGET(self),
                                  "✓ SYSTEM TELEMETRY",
                                  "Live /proc, uname() & GTK4 Wayland runtime inventory");
    return G_SOURCE_REMOVE;
}

static void
action_copy_system_info(gpointer user_data)
{
    SettingsPageAbout *self = (SettingsPageAbout *)user_data;
    if (!self)
        return;

    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;

    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (!clipboard)
        return;

    g_autofree gchar *report = g_strdup_printf(
        "Kajo Desktop Suite (v0.1.0)\n"
        "Kernel:     %s\n"
        "Processor:  %s\n"
        "Memory:     %s\n"
        "Compositor: %s\n"
        "Toolkit:    %s\n"
        "Binaries:   kajo, kajo-settings, kajo-ide, kajo-files, kajo-player, kajo-mod, kajo-greeter\n",
        self->kernel_summary ? self->kernel_summary : "Unknown",
        self->cpu_summary ? self->cpu_summary : "Unknown",
        self->mem_summary ? self->mem_summary : "Unknown",
        self->compositor_summary ? self->compositor_summary : "Wayland",
        self->gtk_summary ? self->gtk_summary : "GTK4");

    gdk_clipboard_set_text(clipboard, report);

    settings_page_set_live_status(GTK_WIDGET(self),
                                  "✓ COPIED TO CLIPBOARD",
                                  "System summary copied to Wayland clipboard");
    g_clear_handle_id(&self->copy_reset_timer_id, g_source_remove);
    self->copy_reset_timer_id = g_timeout_add_seconds(3, on_reset_copied_badge_timeout, self);
}

static void
query_live_system_specs(SettingsPageAbout *self,
                        gchar            **out_kernel_short,
                        gchar            **out_threads_badge,
                        gchar            **out_gtk_badge,
                        gboolean          *out_niri_active)
{
    struct utsname uts;
    if (uname(&uts) == 0) {
        self->kernel_summary = g_strdup_printf("%s %s (%s)", uts.sysname, uts.release, uts.machine);
        *out_kernel_short = g_strdup_printf("%s %s", uts.sysname, uts.release);
    } else {
        self->kernel_summary = g_strdup("Linux (uname unavailable)");
        *out_kernel_short = g_strdup("Linux");
    }

    guint threads = g_get_num_processors();
    *out_threads_badge = g_strdup_printf("%u THREADS", threads);

    g_autofree gchar *cpu_model = NULL;
    g_autofree gchar *cpu_contents = NULL;
    if (g_file_get_contents("/proc/cpuinfo", &cpu_contents, NULL, NULL)) {
        g_auto(GStrv) lines = g_strsplit(cpu_contents, "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            if (g_str_has_prefix(lines[i], "model name") ||
                g_str_has_prefix(lines[i], "Model") ||
                g_str_has_prefix(lines[i], "Hardware")) {
                gchar *colon = strchr(lines[i], ':');
                if (colon) {
                    cpu_model = g_strdup(colon + 1);
                    g_strstrip(cpu_model);
                    break;
                }
            }
        }
    }
    if (cpu_model && cpu_model[0] != '\0')
        self->cpu_summary = g_strdup_printf("%s • %u logical CPUs", cpu_model, threads);
    else
        self->cpu_summary = g_strdup_printf("%u logical CPUs", threads);

    guint64 mem_total_kb = 0;
    guint64 mem_avail_kb = 0;
    g_autofree gchar *mem_contents = NULL;
    if (g_file_get_contents("/proc/meminfo", &mem_contents, NULL, NULL)) {
        g_auto(GStrv) lines = g_strsplit(mem_contents, "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            if (g_str_has_prefix(lines[i], "MemTotal:")) {
                (void)sscanf(lines[i], "MemTotal: %" G_GUINT64_FORMAT " kB", &mem_total_kb);
            } else if (g_str_has_prefix(lines[i], "MemAvailable:")) {
                (void)sscanf(lines[i], "MemAvailable: %" G_GUINT64_FORMAT " kB", &mem_avail_kb);
            }
        }
    }
    if (mem_total_kb > 0) {
        double total_gb = (double)mem_total_kb / (1024.0 * 1024.0);
        double avail_gb = (double)mem_avail_kb / (1024.0 * 1024.0);
        if (mem_avail_kb > 0)
            self->mem_summary = g_strdup_printf("%.1f GiB Total • %.1f GiB Available", total_gb, avail_gb);
        else
            self->mem_summary = g_strdup_printf("%.1f GiB Total System RAM", total_gb);
    } else {
        self->mem_summary = g_strdup("Memory telemetry unavailable");
    }

    const gchar *wl_disp = g_getenv("WAYLAND_DISPLAY");
    const gchar *niri_sock = g_getenv("NIRI_SOCKET");
    gboolean has_kc = shell_compositor_is_kajo_comp();
    gboolean has_niri = (niri_sock && niri_sock[0] != '\0');
    *out_niri_active = (has_kc || has_niri);
    self->compositor_summary = g_strdup_printf(
        "%s • %s",
        (wl_disp && wl_disp[0] != '\0') ? wl_disp : "Wayland",
        has_kc ? "kajo-comp Scrollable-Tiling IPC Active"
               : (has_niri ? "Niri Scrollable-Tiling IPC Active" : "Standard Wayland Session"));

    guint maj = gtk_get_major_version();
    guint min = gtk_get_minor_version();
    guint mic = gtk_get_micro_version();
    *out_gtk_badge = g_strdup_printf("GTK %u.%u.%u", maj, min, mic);
    self->gtk_summary = g_strdup_printf(
        "GTK %u.%u.%u • gtk4-layer-shell • Pango • Cairo", maj, min, mic);
}

static void
settings_page_about_dispose(GObject *object)
{
    SettingsPageAbout *self = (SettingsPageAbout *)object;
    g_clear_handle_id(&self->copy_reset_timer_id, g_source_remove);
    g_clear_pointer(&self->kernel_summary, g_free);
    g_clear_pointer(&self->cpu_summary, g_free);
    g_clear_pointer(&self->mem_summary, g_free);
    g_clear_pointer(&self->compositor_summary, g_free);
    g_clear_pointer(&self->gtk_summary, g_free);
    G_OBJECT_CLASS(settings_page_about_parent_class)->dispose(object);
}

static void
settings_page_about_init(SettingsPageAbout *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_about_class_init(SettingsPageAboutClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_about_dispose;
}

GtkWidget *
build_page_about(void)
{
    SettingsPageAbout *self = g_object_new(settings_page_about_get_type(), NULL);

    g_autofree gchar *kernel_short  = NULL;
    g_autofree gchar *threads_badge = NULL;
    g_autofree gchar *gtk_badge     = NULL;
    gboolean niri_active = FALSE;
    query_live_system_specs(self, &kernel_short, &threads_badge, &gtk_badge, &niri_active);

    /* ─── Card 1: System Kernel, CPU & Memory Telemetry ─── */
    GtkWidget *card_hw = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. KERNEL, CPU & MEMORY",
        "LIVE /PROC",
        "accent",
        NULL);

    gtk_box_append(GTK_BOX(card_hw), create_about_info_row(
        "🐧", "Kernel & Architecture", self->kernel_summary, "UNAME", NULL));
    gtk_box_append(GTK_BOX(card_hw), create_about_info_row(
        "⚡", "Processor & Threads", self->cpu_summary, threads_badge, "accent"));
    gtk_box_append(GTK_BOX(card_hw), create_about_info_row(
        "🧠", "System Memory (RAM)", self->mem_summary, "MEMINFO", NULL));
    gtk_box_append(GTK_BOX(card_hw), create_about_info_row(
        "❖", "Wayland & Compositor", self->compositor_summary,
        shell_compositor_is_kajo_comp() ? "KAJO-COMP IPC"
                                        : (niri_active ? "NIRI IPC" : "WAYLAND"),
        niri_active ? "success" : NULL));

    /* ─── Card 2: Runtime Toolkit & Subsystem Stack ─── */
    GtkWidget *card_stack = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. TOOLKIT & SUBSYSTEM STACK",
        "C17 • C++17",
        "peak-warn",
        NULL);

    gtk_box_append(GTK_BOX(card_stack), create_about_info_row(
        "🪟", "UI Toolkit & Layer-Shell", self->gtk_summary, gtk_badge, "accent"));
    gtk_box_append(GTK_BOX(card_stack), create_about_info_row(
        "🔊", "Audio & Media Stack",
        "PipeWire / libpulse • libmpv + GtkGLArea • MPRIS2 D-Bus",
        "AV / PW", NULL));
    gtk_box_append(GTK_BOX(card_stack), create_about_info_row(
        "🎹", "Retro & Tracker Synthesis",
        "libuade (68k/Paula) • libsidplayfp/reSIDfp • libgme • libopenmpt",
        "4 CORES", NULL));
    gtk_box_append(GTK_BOX(card_stack), create_about_info_row(
        "🔒", "Storage, Docs & Session Auth",
        "GIO / GVFS • UDisks2 • libarchive • poppler-glib • Linux-PAM",
        "SYSTEM", NULL));

    settings_bento_add_pair(GTK_WIDGET(self), card_hw, card_stack);

    /* ─── Card 3: Kajo Desktop Application Suite (8 Native Binaries) ─── */
    GtkWidget *card_suite = create_section_card(
        SETTINGS_CARD_RIGHT,
        "3. KAJO DESKTOP APPLICATION SUITE (8 NATIVE BINARIES)",
        "8 BINARIES",
        "success",
        NULL);

    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "❖", "kajo-comp — Scrollable-Tiling Wayland Compositor (org.kajo.comp)",
        "wlroots 0.20 + Vulkan scrolling strip, analytical springs, MRU switcher & screenshot UI",
        "COMPOSITOR", "accent"));
    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "▬", "kajo — Desktop Shell & Layer-Shell Panel (org.kajo.shell)",
        "Wayland panel, 22 widgets, FFT visualizer, notifications, launcher, ShellOSK & PAM lock",
        "SHELL", "accent"));
    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "⚙", "kajo-settings — System & Suite Configurator (org.kajo.Settings)",
        "12-page scalable Niri column UI with live D-Bus, PipeWire, TOML config & GLSL shaders",
        "SETTINGS", "success"));
    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "🮖", "kajo-ide — Selection-First Modal Code Editor (org.kajo.IDE)",
        "GtkSourceView-5, Nordic text objects, Tree-sitter, async LSP, DAP, Mermaid & Cairo gutter",
        "IDE", NULL));
    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "📁", "kajo-files — URI-First File Manager (org.kajo.Files)",
        "GIO/GVFS/UDisks2 browser, LUKS/SMB inline auth, libarchive extract & Space Quick Look",
        "FILES", NULL));
    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "▶", "kajo-player — Hardware-Accelerated Media Player (org.kajo.Player)",
        "libmpv + GtkGLArea video/audio playback, subtitle/stream tracks, chapters & MPRIS2",
        "PLAYER", NULL));
    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "🎹", "kajo-mod — Retro Amiga, C64 SID & Tracker Player (org.kajo.Mod)",
        "4-tier router (libuade, libsidplayfp/reSIDfp, libgme, libopenmpt), HVSC STIL & archives",
        "MOD", NULL));
    gtk_box_append(GTK_BOX(card_suite), create_about_info_row(
        "🔑", "kajo-greeter — Native Wayland Login Greeter (org.kajo.greeter)",
        "greetd IPC Wayland session greeter with PAM --test mode & multi-monitor Niri/Cage support",
        "GREETER", NULL));

    settings_bento_add_full(GTK_WIDGET(self), card_suite);

    GtkWidget *root = GTK_WIDGET(self);
    settings_page_bind_actions(root,
                               "📋 COPY SYSTEM INFO",
                               action_copy_system_info,
                               NULL,
                               NULL,
                               self);
    settings_page_set_live_status(root,
                                  "✓ SYSTEM TELEMETRY",
                                  "Live /proc, uname() & GTK4 Wayland runtime inventory");
    settings_page_set_deck_badges(root, "KAJO SUITE 0.1", gtk_badge);
    settings_page_set_counter(root, 0, "KERNEL", kernel_short, "accent");
    settings_page_set_counter(root, 1, "TOOLKIT", gtk_badge, NULL);
    settings_page_set_counter(root, 2, "COMPOSITOR",
                              shell_compositor_is_kajo_comp() ? "kajo-comp IPC Active"
                                                              : (niri_active ? "Niri IPC Active" : "Wayland"),
                              niri_active ? "success" : NULL);
    settings_page_set_counter(root, 3, "SUITE", "8 Binaries", NULL);

    return root;
}
