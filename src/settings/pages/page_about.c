#include "../settings_common.h"
#include "../settings_pages.h"
#include <sys/utsname.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageAbout {
    GtkBox parent_instance;

    GtkWidget *kernel_label;
    GtkWidget *cpu_label;
    GtkWidget *mem_label;
    GtkWidget *gtk_label;
} SettingsPageAbout;

typedef struct _SettingsPageAboutClass {
    GtkBoxClass parent_class;
} SettingsPageAboutClass;

G_DEFINE_TYPE(SettingsPageAbout, settings_page_about, GTK_TYPE_BOX)

static void
settings_page_about_init(SettingsPageAbout *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_about_class_init(SettingsPageAboutClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_about.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageAbout, kernel_label);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAbout, cpu_label);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAbout, mem_label);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAbout, gtk_label);
}

GtkWidget *
build_page_about(void)
{
    SettingsPageAbout *page = g_object_new(settings_page_about_get_type(), NULL);

    /* Real System Kernel Query via C uname() */
    struct utsname uts;
    if (uname(&uts) == 0) {
        gchar *kernel_info = g_strdup_printf("Kernel: %s %s (%s)", uts.sysname, uts.release, uts.machine);
        gtk_label_set_text(GTK_LABEL(page->kernel_label), kernel_info);
        g_free(kernel_info);
    }

    /* Real CPU Processor Query from /proc/cpuinfo */
    gchar *cpu_contents = NULL;
    if (g_file_get_contents("/proc/cpuinfo", &cpu_contents, NULL, NULL)) {
        gchar **lines = g_strsplit(cpu_contents, "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            if (g_str_has_prefix(lines[i], "model name")) {
                gchar *colon = strchr(lines[i], ':');
                if (colon) {
                    gchar *cpu_info = g_strdup_printf("Processor:%s", colon + 1);
                    gtk_label_set_text(GTK_LABEL(page->cpu_label), cpu_info);
                    g_free(cpu_info);
                    break;
                }
            }
        }
        g_strfreev(lines);
        g_free(cpu_contents);
    }

    /* Real RAM Memory Query from /proc/meminfo */
    gchar *mem_contents = NULL;
    if (g_file_get_contents("/proc/meminfo", &mem_contents, NULL, NULL)) {
        gchar **lines = g_strsplit(mem_contents, "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            if (g_str_has_prefix(lines[i], "MemTotal:")) {
                guint64 kb = 0;
                if (sscanf(lines[i], "MemTotal: %" G_GUINT64_FORMAT " kB", &kb) == 1) {
                    double gb = (double)kb / (1024.0 * 1024.0);
                    gchar *mem_info = g_strdup_printf("Memory: %.1f GB Total System RAM", gb);
                    gtk_label_set_text(GTK_LABEL(page->mem_label), mem_info);
                    g_free(mem_info);
                    break;
                }
            }
        }
        g_strfreev(lines);
        g_free(mem_contents);
    }

    gchar *gtk_ver_str = g_strdup_printf("Toolkit: GTK %u.%u.%u + gtk4-layer-shell + PipeWire + BlueZ5",
                                         gtk_get_major_version(),
                                         gtk_get_minor_version(),
                                         gtk_get_micro_version());
    gtk_label_set_text(GTK_LABEL(page->gtk_label), gtk_ver_str);
    g_free(gtk_ver_str);

    return create_settings_page_card("ABOUT SYSTEM", "[ GTK4 WAYLAND ]", GTK_WIDGET(page), NULL);
}
