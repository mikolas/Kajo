#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include "widget.h"
#include "../compositor/compositor.h"

#define CPU_MEM_INTERVAL_SECONDS 2
#define MAX_CORES 128
#define TOP_PROCS 5

typedef struct {
    guint64 total;
    guint64 idle;
} CpuSample;

typedef struct {
    gchar   name[64];
    double  cpu_pct;
    guint64 mem_kb;
} ProcInfo;

typedef struct {
    GtkWidget *box;
    GtkWidget *name_label;
    GtkWidget *bar;
    GtkWidget *pct_label;
} CoreItem;

typedef struct {
    ShellWidget base;
    GtkWidget  *button;
    GtkWidget  *popover;
    GtkWidget  *panel_label;
    GtkWidget  *icon_img;
    /* Popover widgets */
    GtkWidget  *cpu_overall_label;
    GtkWidget  *cpu_power_sublabel;
    GtkWidget  *cores_grid;
    GtkWidget  *mem_label;
    GtkWidget  *mem_bar;
    GtkWidget  *disks_box;
    GtkWidget  *net_label;
    GtkWidget  *top_procs_box;
    GtkWidget  *temp_label;
    GtkWidget  *fan_label;
    GtkWidget  *power_label;
    GtkWidget  *status_badge;
    GtkWidget  *power_badge;
    /* State */
    guint       timer_id;
    CpuSample   prev_overall;
    CpuSample   prev_cores[MAX_CORES];
    gint        num_cores;
    gboolean    has_prev;
    guint64     last_dt;
    guint64     prev_energy_uj;
    gint64      prev_power_time;
    guint64     prev_rx_bytes;
    guint64     prev_tx_bytes;
    gint64      prev_net_time;
    GHashTable *proc_table;
    CoreItem    core_items[MAX_CORES];
} CpuMemWidget;

/* --- CPU reading --- */

static gint cpu_mem_read_all(CpuMemWidget *cm, int *out_overall_pct,
                             double *core_pcts, gint *out_num_cores)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    char line[256];
    gint core_idx = 0;
    gboolean first = TRUE;

    CpuSample cur_overall = {0};
    CpuSample cur_cores[MAX_CORES] = {{0}};

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0)
            break;

        guint64 user, nice, system, idle, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (first) {
            /* Overall "cpu " line */
            sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
            cur_overall.idle = idle + iowait;
            cur_overall.total = user + nice + system + idle + iowait + irq + softirq + steal;
            first = FALSE;
        } else {
            /* Per-core "cpuN " lines */
            if (core_idx >= MAX_CORES) continue;
            sscanf(line, "cpu%*d %lu %lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
            cur_cores[core_idx].idle = idle + iowait;
            cur_cores[core_idx].total = user + nice + system + idle + iowait + irq + softirq + steal;
            core_idx++;
        }
    }
    fclose(fp);

    *out_num_cores = core_idx;

    if (!cm->has_prev) {
        cm->prev_overall = cur_overall;
        memcpy(cm->prev_cores, cur_cores, sizeof(cur_cores));
        cm->num_cores = core_idx;
        cm->has_prev = TRUE;
        *out_overall_pct = 0;
        for (gint i = 0; i < core_idx; i++)
            core_pcts[i] = 0.0;
        return 0;
    }

    /* Compute overall */
    guint64 dt = cur_overall.total - cm->prev_overall.total;
    guint64 di = cur_overall.idle - cm->prev_overall.idle;
    *out_overall_pct = dt > 0 ? (int)(100 * (dt - di) / dt) : 0;
    cm->last_dt = dt;

    /* Compute per-core */
    for (gint i = 0; i < core_idx; i++) {
        guint64 cdt = cur_cores[i].total - cm->prev_cores[i].total;
        guint64 cdi = cur_cores[i].idle - cm->prev_cores[i].idle;
        core_pcts[i] = cdt > 0 ? (100.0 * (cdt - cdi) / cdt) : 0.0;
    }

    cm->prev_overall = cur_overall;
    memcpy(cm->prev_cores, cur_cores, sizeof(cur_cores));
    cm->num_cores = core_idx;

    return 0;
}

/* --- Memory reading --- */

static gboolean cpu_mem_read_mem(double *out_used_gb, double *out_total_gb, int *out_pct)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return FALSE;

    char line[256];
    guint64 mem_total = 0, mem_available = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) continue;
    }
    fclose(fp);

    if (mem_total == 0) return FALSE;

    guint64 used = mem_total > mem_available ? (mem_total - mem_available) : 0;
    *out_used_gb = (double)used / (1024.0 * 1024.0);
    *out_total_gb = (double)mem_total / (1024.0 * 1024.0);
    *out_pct = (int)(100 * used / mem_total);
    return TRUE;
}

/* --- Real Physical Disks List (dysk style device deduplication) --- */

static void cpu_mem_update_disks(CpuMemWidget *cm)
{
    if (!cm->disks_box) return;

    /* Remove old children */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(cm->disks_box)) != NULL)
        gtk_box_remove(GTK_BOX(cm->disks_box), child);

    FILE *fp = setmntent("/proc/mounts", "r");
    if (!fp) return;

    struct mntent *entry;
    GHashTable *seen_devs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    while ((entry = getmntent(fp)) != NULL) {
        if (!g_str_has_prefix(entry->mnt_fsname, "/dev/")) continue;
        if (g_strcmp0(entry->mnt_type, "tmpfs") == 0 ||
            g_strcmp0(entry->mnt_type, "devtmpfs") == 0 ||
            g_strcmp0(entry->mnt_type, "squashfs") == 0 ||
            g_strcmp0(entry->mnt_type, "overlay") == 0) continue;

        /* Deduplicate physical device nodes (/dev/nvme0n1p2, /dev/sda1) */
        if (g_hash_table_contains(seen_devs, entry->mnt_fsname)) continue;
        g_hash_table_add(seen_devs, g_strdup(entry->mnt_fsname));

        struct statvfs stat;
        if (statvfs(entry->mnt_dir, &stat) == 0 && stat.f_blocks > 0) {
            uint64_t total = (uint64_t)stat.f_blocks * stat.f_frsize;
            uint64_t free = (uint64_t)stat.f_bavail * stat.f_frsize;
            uint64_t used = total > free ? (total - free) : 0;
            double used_gb = (double)used / (1024.0 * 1024.0 * 1024.0);
            double total_gb = (double)total / (1024.0 * 1024.0 * 1024.0);
            int pct = total > 0 ? (int)(100 * used / total) : 0;

            const char *dev_name = strrchr(entry->mnt_fsname, '/');
            dev_name = dev_name ? dev_name + 1 : entry->mnt_fsname;

            gchar *label_str = g_strdup_printf("%s (%s): %.1f / %.1f GB (%d%%)",
                entry->mnt_dir, dev_name, used_gb, total_gb, pct);

            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget *lbl = gtk_label_new(label_str);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            g_free(label_str);

            GtkWidget *bar = gtk_level_bar_new_for_interval(0.0, 1.0);
            gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), (double)pct / 100.0);

            gtk_box_append(GTK_BOX(box), lbl);
            gtk_box_append(GTK_BOX(box), bar);
            gtk_box_append(GTK_BOX(cm->disks_box), box);
        }
    }
    endmntent(fp);
    g_hash_table_destroy(seen_devs);
}

/* --- Real Network Throughput Reading (/proc/net/dev) --- */

static void cpu_mem_update_network(CpuMemWidget *cm)
{
    if (!cm->net_label) return;

    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return;

    char line[256];
    guint64 total_rx = 0, total_tx = 0;

    if (fgets(line, sizeof(line), fp)) {}
    if (fgets(line, sizeof(line), fp)) {}

    while (fgets(line, sizeof(line), fp)) {
        gchar *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        gchar *iface = g_strstrip(line);
        if (g_strcmp0(iface, "lo") == 0) continue;

        guint64 rx = 0, tx = 0;
        guint64 dummy;
        if (sscanf(colon + 1, "%lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx) >= 9) {
            total_rx += rx;
            total_tx += tx;
        }
    }
    fclose(fp);

    gint64 now = g_get_monotonic_time();
    if (cm->prev_net_time > 0 && cm->prev_rx_bytes <= total_rx) {
        double dt = (double)(now - cm->prev_net_time) / 1000000.0;
        if (dt > 0.1) {
            double rx_speed = (double)(total_rx - cm->prev_rx_bytes) / dt;
            double tx_speed = (double)(total_tx - cm->prev_tx_bytes) / dt;

            gchar *rx_str = rx_speed >= 1024 * 1024 ? g_strdup_printf("%.1f MB/s", rx_speed / (1024.0 * 1024.0)) : g_strdup_printf("%.0f KB/s", rx_speed / 1024.0);
            gchar *tx_str = tx_speed >= 1024 * 1024 ? g_strdup_printf("%.1f MB/s", tx_speed / (1024.0 * 1024.0)) : g_strdup_printf("%.0f KB/s", tx_speed / 1024.0);

            gchar *label_str = g_strdup_printf("Network: ⬇ %s   ⬆ %s", rx_str, tx_str);
            gtk_label_set_text(GTK_LABEL(cm->net_label), label_str);

            g_free(rx_str);
            g_free(tx_str);
            g_free(label_str);
        }
    }

    cm->prev_rx_bytes = total_rx;
    cm->prev_tx_bytes = total_tx;
    cm->prev_net_time = now;
}

/* --- Temperature --- */

static gboolean cpu_mem_read_temp(double *out_temp)
{
    gchar *contents = NULL;
    if (!g_file_get_contents("/sys/class/thermal/thermal_zone0/temp", &contents, NULL, NULL))
        return FALSE;
    *out_temp = atoi(contents) / 1000.0;
    g_free(contents);
    return TRUE;
}

/* --- Fan Speed (RPM) --- */

static gboolean cpu_mem_read_fan_rpm(int *out_rpm)
{
    for (int h = 0; h < 12; h++) {
        for (int f = 1; f <= 4; f++) {
            gchar fan_path[128];
            snprintf(fan_path, sizeof(fan_path), "/sys/class/hwmon/hwmon%d/fan%d_input", h, f);
            gchar *contents = NULL;
            if (g_file_get_contents(fan_path, &contents, NULL, NULL)) {
                int rpm = atoi(contents);
                g_free(contents);
                if (rpm > 0) {
                    *out_rpm = rpm;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/* --- CPU Power Draw (Watts) --- */

static gboolean cpu_mem_read_power(CpuMemWidget *cm, double *out_watts)
{
    /* 1. Try RAPL energy accumulators (Intel RAPL & AMD RAPL) */
    static const char *rapl_paths[] = {
        "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
        "/sys/class/powercap/intel-rapl:0/energy_uj",
        "/sys/class/powercap/amd_energy/energy1_input",
        "/sys/class/powercap/intel-rapl/intel-rapl:1/energy_uj",
        NULL
    };

    for (int i = 0; rapl_paths[i] != NULL; i++) {
        gchar *contents = NULL;
        if (g_file_get_contents(rapl_paths[i], &contents, NULL, NULL)) {
            guint64 cur_energy_uj = g_ascii_strtoull(contents, NULL, 10);
            g_free(contents);
            if (cur_energy_uj > 0) {
                gint64 now = g_get_monotonic_time();
                if (cm->prev_energy_uj > 0 && cm->prev_power_time > 0 && cur_energy_uj >= cm->prev_energy_uj) {
                    double dt_sec = (double)(now - cm->prev_power_time) / 1000000.0;
                    if (dt_sec > 0.1) {
                        guint64 d_energy_uj = cur_energy_uj - cm->prev_energy_uj;
                        *out_watts = ((double)d_energy_uj / 1000000.0) / dt_sec;
                        cm->prev_energy_uj = cur_energy_uj;
                        cm->prev_power_time = now;
                        return TRUE;
                    }
                }
                cm->prev_energy_uj = cur_energy_uj;
                cm->prev_power_time = now;
                return FALSE;
            }
        }
    }

    /* 2. Try HWMON instant power inputs (/sys/class/hwmon/hwmonN/power*_input), skipping GPU/storage sensors */
    for (int h = 0; h < 12; h++) {
        gchar name_path[128];
        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/hwmon%d/name", h);
        gchar *sensor_name = NULL;
        if (g_file_get_contents(name_path, &sensor_name, NULL, NULL)) {
            g_strstrip(sensor_name);
            if (g_strcmp0(sensor_name, "amdgpu") == 0 ||
                g_strcmp0(sensor_name, "nvidia") == 0 ||
                g_strcmp0(sensor_name, "nouveau") == 0 ||
                g_strcmp0(sensor_name, "nvme") == 0) {
                g_free(sensor_name);
                continue;
            }
            g_free(sensor_name);
        }

        guint64 total_microwatts = 0;
        int found_rails = 0;

        for (int p = 1; p <= 4; p++) {
            gchar hwmon_path[128];
            snprintf(hwmon_path, sizeof(hwmon_path), "/sys/class/hwmon/hwmon%d/power%d_input", h, p);
            gchar *contents = NULL;
            if (g_file_get_contents(hwmon_path, &contents, NULL, NULL)) {
                guint64 microwatts = g_ascii_strtoull(contents, NULL, 10);
                g_free(contents);
                if (microwatts > 0) {
                    total_microwatts += microwatts;
                    found_rails++;
                }
            }
        }

        if (found_rails > 0 && total_microwatts > 0) {
            *out_watts = (double)total_microwatts / 1000000.0;
            return TRUE;
        }
    }

    /* 3. Try laptop power supply battery fallback (/sys/class/power_supply/BATN/power_now) */
    static const char *bat_paths[] = {
        "/sys/class/power_supply/BAT0/power_now",
        "/sys/class/power_supply/BAT1/power_now",
        NULL
    };
    for (int b = 0; bat_paths[b] != NULL; b++) {
        gchar *contents = NULL;
        if (g_file_get_contents(bat_paths[b], &contents, NULL, NULL)) {
            guint64 microwatts = g_ascii_strtoull(contents, NULL, 10);
            g_free(contents);
            if (microwatts > 0) {
                *out_watts = (double)microwatts / 1000000.0;
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* --- Top processes --- */

static void cpu_mem_get_top_procs(CpuMemWidget *cm, ProcInfo *procs, gint *out_count)
{
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) { *out_count = 0; return; }

    GHashTable *new_table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    typedef struct {
        pid_t   pid;
        double  cpu_pct;
        guint64 mem_pages;
        char    name[64];
    } ProcEntry;

    ProcEntry *entries = NULL;
    gint n_entries = 0;
    gint alloc = 256;
    entries = g_malloc(alloc * sizeof(ProcEntry));

    struct dirent *de;
    while ((de = readdir(proc_dir)) != NULL) {
        if (!isdigit(de->d_name[0])) continue;
        pid_t pid = atoi(de->d_name);

        /* Read comm */
        gchar comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        gchar *comm = NULL;
        if (!g_file_get_contents(comm_path, &comm, NULL, NULL)) continue;
        g_strstrip(comm);

        /* Read stat for cpu time */
        gchar stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        gchar *stat_contents = NULL;
        if (!g_file_get_contents(stat_path, &stat_contents, NULL, NULL)) {
            g_free(comm);
            continue;
        }

        gchar *p = strrchr(stat_contents, ')');
        if (!p) { g_free(comm); g_free(stat_contents); continue; }
        p += 2;

        guint64 utime = 0, stime = 0;
        int field = 3;
        gchar *tok = strtok(p, " ");
        while (tok) {
            if (field == 14) utime = strtoull(tok, NULL, 10);
            if (field == 15) { stime = strtoull(tok, NULL, 10); break; }
            tok = strtok(NULL, " ");
            field++;
        }
        g_free(stat_contents);

        guint64 cur_proc_time = utime + stime;
        guint64 *val_copy = g_new(guint64, 1);
        *val_copy = cur_proc_time;
        g_hash_table_insert(new_table, GINT_TO_POINTER(pid), val_copy);

        double cpu_pct = 0.0;
        if (cm->last_dt > 0 && cm->proc_table != NULL) {
            gpointer prev_ptr = g_hash_table_lookup(cm->proc_table, GINT_TO_POINTER(pid));
            if (prev_ptr != NULL) {
                guint64 prev_proc_time = *(guint64 *)prev_ptr;
                guint64 dproc = cur_proc_time > prev_proc_time ? (cur_proc_time - prev_proc_time) : 0;
                /* Normalized Mode (0 - 100% total system capacity, matching btop & gnome-system-monitor) */
                cpu_pct = (100.0 * (double)dproc) / (double)cm->last_dt;
            }
        }

        /* Read statm for memory */
        gchar statm_path[64];
        snprintf(statm_path, sizeof(statm_path), "/proc/%d/statm", pid);
        gchar *statm_contents = NULL;
        guint64 rss_pages = 0;
        if (g_file_get_contents(statm_path, &statm_contents, NULL, NULL)) {
            guint64 size_pages = 0;
            sscanf(statm_contents, "%lu %lu", &size_pages, &rss_pages);
            g_free(statm_contents);
        }

        if (n_entries >= alloc) {
            alloc *= 2;
            entries = g_realloc(entries, alloc * sizeof(ProcEntry));
        }
        entries[n_entries].pid = pid;
        entries[n_entries].cpu_pct = cpu_pct;
        entries[n_entries].mem_pages = rss_pages;
        strncpy(entries[n_entries].name, comm, 63);
        entries[n_entries].name[63] = '\0';
        n_entries++;
        g_free(comm);
    }
    closedir(proc_dir);

    /* Replace old proc table with new sample */
    if (cm->proc_table)
        g_hash_table_destroy(cm->proc_table);
    cm->proc_table = new_table;

    /* Sort by cpu_pct descending, secondary by mem_pages */
    for (gint i = 0; i < n_entries - 1; i++) {
        for (gint j = i + 1; j < n_entries; j++) {
            gboolean swap = FALSE;
            if (entries[j].cpu_pct > entries[i].cpu_pct + 0.01) {
                swap = TRUE;
            } else if (ABS(entries[j].cpu_pct - entries[i].cpu_pct) <= 0.01) {
                if (entries[j].mem_pages > entries[i].mem_pages)
                    swap = TRUE;
            }
            if (swap) {
                ProcEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    gint count = n_entries < TOP_PROCS ? n_entries : TOP_PROCS;
    long page_size = sysconf(_SC_PAGESIZE);
    for (gint i = 0; i < count; i++) {
        strncpy(procs[i].name, entries[i].name, 63);
        procs[i].name[63] = '\0';
        procs[i].cpu_pct = entries[i].cpu_pct;
        procs[i].mem_kb = (entries[i].mem_pages * page_size) / 1024;
    }
    *out_count = count;
    g_free(entries);
}

/* --- CPU Frequency (GHz) --- */

static gboolean cpu_mem_read_freq(double *out_max_ghz, double *out_avg_ghz)
{
    double max_khz = 0;
    double total_khz = 0;
    int count = 0;

    for (int p = 0; p < 128; p++) {
        gchar path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", p);
        gchar *contents = NULL;
        if (g_file_get_contents(path, &contents, NULL, NULL)) {
            double khz = g_ascii_strtod(contents, NULL);
            g_free(contents);
            if (khz > 0) {
                if (khz > max_khz) max_khz = khz;
                total_khz += khz;
                count++;
            }
        }
    }

    if (count > 0 && max_khz > 0) {
        *out_max_ghz = (max_khz / 1000000.0);
        *out_avg_ghz = ((total_khz / (double)count) / 1000000.0);
        return TRUE;
    }
    return FALSE;
}

/* --- Update Tick --- */

static void cpu_mem_update(CpuMemWidget *cm)
{
    int overall_pct = 0;
    double core_pcts[MAX_CORES] = {0};
    gint num_cores = 0;

    cpu_mem_read_all(cm, &overall_pct, core_pcts, &num_cores);

    /* Panel label */
    double used_gb = 0, total_gb = 0;
    int mem_pct = 0;
    gboolean mem_ok = cpu_mem_read_mem(&used_gb, &total_gb, &mem_pct);

    gchar *panel_text;
    if (mem_ok)
        panel_text = g_strdup_printf("CPU %d%% MEM %d%%", overall_pct, mem_pct);
    else
        panel_text = g_strdup_printf("CPU %d%% MEM --", overall_pct);
    gtk_label_set_text(GTK_LABEL(cm->panel_label), panel_text);
    g_free(panel_text);
    shell_widget_apply_mode_visibility(cm->base.mode, cm->icon_img, cm->panel_label);

    /* Popover: CPU overall with Frequency */
    double max_ghz = 0, avg_ghz = 0;
    gchar *cpu_text;
    if (cpu_mem_read_freq(&max_ghz, &avg_ghz)) {
        cpu_text = g_strdup_printf("CPU: %d%% @ %.2f GHz (%d Threads)", overall_pct, max_ghz, num_cores);
    } else {
        cpu_text = g_strdup_printf("CPU: %d%% (%d Threads)", overall_pct, num_cores);
    }
    gtk_label_set_text(GTK_LABEL(cm->cpu_overall_label), cpu_text);
    g_free(cpu_text);

    /* Per-core 2-column paired layout grid */
    for (gint i = 0; i < num_cores && i < MAX_CORES; i++) {
        if (!cm->core_items[i].box) {
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

            gchar *name_str = g_strdup_printf("C%02d", i + 1);
            GtkWidget *name_lbl = gtk_label_new(name_str);
            g_free(name_str);
            gtk_widget_add_css_class(name_lbl, "monospace");
            gtk_widget_set_size_request(name_lbl, 32, -1);
            gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);

            GtkWidget *bar = gtk_level_bar_new_for_interval(0.0, 1.0);
            gtk_widget_set_size_request(bar, 90, 6);

            GtkWidget *pct_lbl = gtk_label_new("0%");
            gtk_widget_add_css_class(pct_lbl, "monospace");
            gtk_widget_set_size_request(pct_lbl, 36, -1);
            gtk_label_set_xalign(GTK_LABEL(pct_lbl), 1.0);

            gtk_box_append(GTK_BOX(box), name_lbl);
            gtk_box_append(GTK_BOX(box), bar);
            gtk_box_append(GTK_BOX(box), pct_lbl);

            cm->core_items[i].box = box;
            cm->core_items[i].name_label = name_lbl;
            cm->core_items[i].bar = bar;
            cm->core_items[i].pct_label = pct_lbl;

            gint col = i % 2;
            gint row = i / 2;
            gtk_grid_attach(GTK_GRID(cm->cores_grid), box, col, row, 1, 1);
        }

        gtk_level_bar_set_value(GTK_LEVEL_BAR(cm->core_items[i].bar), core_pcts[i] / 100.0);
        gchar *pct_str = g_strdup_printf("%.0f%%", core_pcts[i]);
        gtk_label_set_text(GTK_LABEL(cm->core_items[i].pct_label), pct_str);
        g_free(pct_str);
        gtk_widget_set_visible(cm->core_items[i].box, TRUE);
    }
    for (gint i = num_cores; i < MAX_CORES; i++) {
        if (cm->core_items[i].box)
            gtk_widget_set_visible(cm->core_items[i].box, FALSE);
    }

    /* Memory */
    if (mem_ok) {
        gchar *mem_text = g_strdup_printf("Memory: %.1f / %.1f GB (%d%%)", used_gb, total_gb, mem_pct);
        gtk_label_set_text(GTK_LABEL(cm->mem_label), mem_text);
        g_free(mem_text);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(cm->mem_bar), (double)mem_pct / 100.0);
    }

    /* Storage (Physical Disks List with Deduplication) */
    cpu_mem_update_disks(cm);

    /* Network Throughput (/proc/net/dev) */
    cpu_mem_update_network(cm);

    /* Top processes */
    /* Remove old children */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(cm->top_procs_box)) != NULL)
        gtk_box_remove(GTK_BOX(cm->top_procs_box), child);

    ProcInfo procs[TOP_PROCS] = {0};
    gint proc_count = 0;
    cpu_mem_get_top_procs(cm, procs, &proc_count);

    for (gint i = 0; i < proc_count; i++) {
        gchar *row;
        if (procs[i].mem_kb > 1024 * 1024)
            row = g_strdup_printf("%-16s  CPU: %.1f%%  MEM: %.1f GB",
                                  procs[i].name, procs[i].cpu_pct,
                                  procs[i].mem_kb / (1024.0 * 1024.0));
        else if (procs[i].mem_kb > 1024)
            row = g_strdup_printf("%-16s  CPU: %.1f%%  MEM: %.0f MB",
                                  procs[i].name, procs[i].cpu_pct,
                                  procs[i].mem_kb / 1024.0);
        else
            row = g_strdup_printf("%-16s  CPU: %.1f%%  MEM: %lu KB",
                                  procs[i].name, procs[i].cpu_pct,
                                  (unsigned long)procs[i].mem_kb);

        GtkWidget *lbl = gtk_label_new(row);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_widget_add_css_class(lbl, "monospace");
        gtk_box_append(GTK_BOX(cm->top_procs_box), lbl);
        g_free(row);
    }

    /* Temperature */
    double temp = 0;
    if (cpu_mem_read_temp(&temp)) {
        gchar *temp_text = g_strdup_printf("Temp: %.1f °C", temp);
        gtk_label_set_text(GTK_LABEL(cm->temp_label), temp_text);
        g_free(temp_text);
        gtk_widget_set_visible(cm->temp_label, TRUE);
    } else {
        gtk_widget_set_visible(cm->temp_label, FALSE);
    }

    /* CPU Fan Speed */
    int rpm = 0;
    if (cpu_mem_read_fan_rpm(&rpm)) {
        gchar *fan_text = g_strdup_printf("Fan: %d RPM", rpm);
        gtk_label_set_text(GTK_LABEL(cm->fan_label), fan_text);
        g_free(fan_text);
        gtk_widget_set_visible(cm->fan_label, TRUE);
    } else {
        gtk_widget_set_visible(cm->fan_label, FALSE);
    }

    /* Power Draw */
    double watts = 0;
    if (cpu_mem_read_power(cm, &watts)) {
        gchar *power_text = g_strdup_printf("Power Draw: %.1f W", watts);
        gtk_label_set_text(GTK_LABEL(cm->power_label), power_text);
        g_free(power_text);

        gchar *sub_power = g_strdup_printf("Power: %.1f W", watts);
        gtk_label_set_text(GTK_LABEL(cm->cpu_power_sublabel), sub_power);
        g_free(sub_power);

        gchar *badge_text = g_strdup_printf("[ %.1f W ]", watts);
        gtk_label_set_text(GTK_LABEL(cm->power_badge), badge_text);
        g_free(badge_text);
    } else {
        gtk_label_set_text(GTK_LABEL(cm->power_label), "Power Draw: 0.0 W (AC Power)");
        gtk_label_set_text(GTK_LABEL(cm->cpu_power_sublabel), "Power: 0.0 W");
        gtk_label_set_text(GTK_LABEL(cm->power_badge), "[ 0.0 W ]");
    }
    gtk_label_set_text(GTK_LABEL(cm->status_badge), "[ LIVE ]");
    gtk_widget_set_visible(cm->power_label, TRUE);
}

static gboolean cpu_mem_tick(gpointer user_data)
{
    cpu_mem_update((CpuMemWidget *)user_data);
    return G_SOURCE_CONTINUE;
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellCpuMemPopover {
    GtkPopover parent_instance;

    GtkWidget *cpu_overall_label;
    GtkWidget *cpu_power_sublabel;
    GtkWidget *cores_grid;
    GtkWidget *mem_label;
    GtkWidget *mem_bar;
    GtkWidget *disks_box;
    GtkWidget *net_label;
    GtkWidget *top_procs_box;
    GtkWidget *temp_label;
    GtkWidget *fan_label;
    GtkWidget *power_label;
    GtkWidget *status_badge;
    GtkWidget *power_badge;
} ShellCpuMemPopover;

typedef struct _ShellCpuMemPopoverClass {
    GtkPopoverClass parent_class;
} ShellCpuMemPopoverClass;

G_DEFINE_TYPE(ShellCpuMemPopover, shell_cpu_mem_popover, GTK_TYPE_POPOVER)

static void
shell_cpu_mem_popover_init(ShellCpuMemPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_cpu_mem_popover_class_init(ShellCpuMemPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/cpu_mem_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, cpu_overall_label);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, cpu_power_sublabel);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, cores_grid);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, mem_label);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, mem_bar);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, disks_box);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, net_label);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, top_procs_box);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, temp_label);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, fan_label);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, power_label);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, status_badge);
    gtk_widget_class_bind_template_child(widget_class, ShellCpuMemPopover, power_badge);
}

/* --- Widget interface --- */

static ShellWidget *cpu_mem_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    CpuMemWidget *cm = g_new0(CpuMemWidget, 1);

    /* Panel button child hbox */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    cm->icon_img = gtk_image_new_from_icon_name("fl-cpu-symbolic");
    cm->panel_label = gtk_label_new("CPU --% MEM --%");
    gtk_box_append(GTK_BOX(hbox), cm->icon_img);
    gtk_box_append(GTK_BOX(hbox), cm->panel_label);

    /* Menu button */
    cm->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(cm->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(cm->button), hbox);
    gtk_widget_add_css_class(cm->button, "flat");
    gtk_widget_add_css_class(cm->button, "shell-widget");
    gtk_widget_add_css_class(cm->button, "shell-widget-cpu-mem");

    /* Instantiate Declarative Popover */
    ShellCpuMemPopover *popover = g_object_new(shell_cpu_mem_popover_get_type(), NULL);
    cm->popover = GTK_WIDGET(popover);

    cm->cpu_overall_label = popover->cpu_overall_label;
    cm->cpu_power_sublabel = popover->cpu_power_sublabel;
    cm->cores_grid = popover->cores_grid;
    cm->mem_label = popover->mem_label;
    cm->mem_bar = popover->mem_bar;
    cm->disks_box = popover->disks_box;
    cm->net_label = popover->net_label;
    cm->top_procs_box = popover->top_procs_box;
    cm->temp_label = popover->temp_label;
    cm->fan_label = popover->fan_label;
    cm->power_label = popover->power_label;
    cm->status_badge = popover->status_badge;
    cm->power_badge = popover->power_badge;

    gtk_menu_button_set_popover(GTK_MENU_BUTTON(cm->button), cm->popover);

    return (ShellWidget *)cm;
}

static void cpu_mem_destroy(ShellWidget *widget)
{
    CpuMemWidget *cm = (CpuMemWidget *)widget;
    if (cm->timer_id > 0)
        g_source_remove(cm->timer_id);
    if (cm->proc_table)
        g_hash_table_destroy(cm->proc_table);
    g_free(cm);
}

static GtkWidget *cpu_mem_get_widget(ShellWidget *widget)
{
    return ((CpuMemWidget *)widget)->button;
}

static void cpu_mem_enable(ShellWidget *widget)
{
    CpuMemWidget *cm = (CpuMemWidget *)widget;
    if (cm) {
        shell_widget_apply_mode_visibility(widget->mode, cm->icon_img, cm->panel_label);
    }
    if (cm->timer_id == 0) {
        cpu_mem_update(cm);
        cm->timer_id = g_timeout_add_seconds(CPU_MEM_INTERVAL_SECONDS, cpu_mem_tick, cm);
    }
}

static void cpu_mem_disable(ShellWidget *widget)
{
    CpuMemWidget *cm = (CpuMemWidget *)widget;
    if (cm->timer_id > 0) {
        g_source_remove(cm->timer_id);
        cm->timer_id = 0;
    }
}

const ShellWidgetClass cpu_mem_widget_class = {
    .id         = "cpu-mem",
    .name       = "CPU & Memory",
    .create     = cpu_mem_create,
    .destroy    = cpu_mem_destroy,
    .get_widget = cpu_mem_get_widget,
    .enable     = cpu_mem_enable,
    .disable    = cpu_mem_disable,
};
