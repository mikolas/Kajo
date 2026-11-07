#define _DEFAULT_SOURCE
#include "files_diskimage.h"
#include "files_ops.h"

#include <gio/gunixfdlist.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UDISKS_NAME      "org.freedesktop.UDisks2"
#define UDISKS_ROOT      "/org/freedesktop/UDisks2"
#define UDISKS_MANAGER   "/org/freedesktop/UDisks2/Manager"
#define IFACE_LOOP       "org.freedesktop.UDisks2.Loop"
#define IFACE_FS         "org.freedesktop.UDisks2.Filesystem"
#define IFACE_PART       "org.freedesktop.UDisks2.Partition"

#define POLL_INTERVAL_MS 200
#define POLL_MAX         25   /* ~5 s for the filesystem to appear */
#define CALL_TIMEOUT_MS  60000 /* leaves room for a polkit prompt */

/* ─── Pure helpers ─── */

static char *
bytes_to_string(GVariant *ay)
{
    gsize n = 0;
    const char *d = g_variant_get_fixed_array(ay, &n, 1);
    return d ? g_strndup(d, strnlen(d, n)) : NULL; /* tolerate a missing NUL terminator */
}

static GVariant *
lookup_iface(GVariant *ifaces, const char *iface)
{
    return g_variant_lookup_value(ifaces, iface, G_VARIANT_TYPE("a{sv}"));
}

char *
kajo_diskimage_find_loop(GVariant *managed, const char *backing_path, guint32 uid)
{
    if (!managed || !backing_path)
        return NULL;
    for (gsize i = 0; i < g_variant_n_children(managed); i++) {
        g_autoptr(GVariant) entry = g_variant_get_child_value(managed, i);
        g_autoptr(GVariant) obj = g_variant_get_child_value(entry, 0);
        g_autoptr(GVariant) ifaces = g_variant_get_child_value(entry, 1);
        g_autoptr(GVariant) loop = lookup_iface(ifaces, IFACE_LOOP);
        if (!loop)
            continue;
        g_autoptr(GVariant) bf = g_variant_lookup_value(loop, "BackingFile", G_VARIANT_TYPE_BYTESTRING);
        guint32 setup_uid = G_MAXUINT32;
        g_variant_lookup(loop, "SetupByUID", "u", &setup_uid);
        g_autofree char *backing = bf ? bytes_to_string(bf) : NULL;
        if (backing && strcmp(backing, backing_path) == 0 && setup_uid == uid)
            return g_strdup(g_variant_get_string(obj, NULL));
    }
    return NULL;
}

char *
kajo_diskimage_find_filesystem(GVariant *managed, const char *loop_path)
{
    if (!managed || !loop_path)
        return NULL;

    g_autoptr(GVariant) own = g_variant_lookup_value(managed, loop_path, G_VARIANT_TYPE("a{sa{sv}}"));
    if (own) {
        g_autoptr(GVariant) fs = lookup_iface(own, IFACE_FS);
        if (fs)
            return g_strdup(loop_path);
    }

    char *best = NULL;
    guint32 best_number = G_MAXUINT32;
    for (gsize i = 0; i < g_variant_n_children(managed); i++) {
        g_autoptr(GVariant) entry = g_variant_get_child_value(managed, i);
        g_autoptr(GVariant) obj = g_variant_get_child_value(entry, 0);
        g_autoptr(GVariant) ifaces = g_variant_get_child_value(entry, 1);
        g_autoptr(GVariant) part = lookup_iface(ifaces, IFACE_PART);
        g_autoptr(GVariant) fs = lookup_iface(ifaces, IFACE_FS);
        if (!part || !fs)
            continue;
        const char *table = NULL;
        guint32 number = 0;
        g_variant_lookup(part, "Table", "&o", &table);
        g_variant_lookup(part, "Number", "u", &number);
        if (g_strcmp0(table, loop_path) == 0 && number < best_number) {
            g_free(best);
            best = g_strdup(g_variant_get_string(obj, NULL));
            best_number = number;
        }
    }
    return best;
}

char *
kajo_diskimage_first_mount_point(GVariant *managed, const char *fs_path)
{
    if (!managed || !fs_path)
        return NULL;
    g_autoptr(GVariant) ifaces = g_variant_lookup_value(managed, fs_path, G_VARIANT_TYPE("a{sa{sv}}"));
    g_autoptr(GVariant) fs = ifaces ? lookup_iface(ifaces, IFACE_FS) : NULL;
    g_autoptr(GVariant) mps = fs ? g_variant_lookup_value(fs, "MountPoints", G_VARIANT_TYPE("aay")) : NULL;
    if (!mps)
        return NULL;
    for (gsize i = 0; i < g_variant_n_children(mps); i++) {
        g_autoptr(GVariant) mp = g_variant_get_child_value(mps, i);
        char *s = bytes_to_string(mp);
        if (s && s[0] != '\0')
            return s;
        g_free(s);
    }
    return NULL;
}

/* ─── Async flow ─── */

typedef struct {
    char            *real_path;
    GDBusConnection *bus;
    char            *loop_path;
    gboolean         created_loop;  /* we ran LoopSetup: we own the cleanup */
    guint            polls;
    gboolean         listing_for_reuse; /* first listing: look for an existing loop */
} MountOp;

static void
mount_op_free(gpointer data)
{
    MountOp *op = data;
    g_free(op->real_path);
    g_free(op->loop_path);
    g_clear_object(&op->bus);
    g_free(op);
}

static void list_objects(GTask *task);

static void
on_loop_deleted(GObject *src, GAsyncResult *res, gpointer user_data)
{
    g_autofree char *loop_path = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (err)
        g_warning("Could not remove unused loop device %s: %s", loop_path, err->message);
}

/* Deletes a loop device we created but could not use (autoclear is only set after a mount).
 * A reply callback is required: a call without one is sent as NO_REPLY_EXPECTED, which
 * UDisks2 silently drops for this polkit-checked method (verified on udisks 2.x). */
static void
discard_created_loop(MountOp *op)
{
    if (!op->created_loop || !op->loop_path || !op->bus)
        return;
    g_dbus_connection_call(op->bus, UDISKS_NAME, op->loop_path, IFACE_LOOP, "Delete",
                           g_variant_new_parsed("(@a{sv} {},)"), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_loop_deleted, g_strdup(op->loop_path));
    op->created_loop = FALSE;
}

static void
fail(GTask *task, GError *error)
{
    discard_created_loop(g_task_get_task_data(task));
    g_task_return_error(task, error);
    g_object_unref(task);
}

static void
fail_msg(GTask *task, const char *msg)
{
    fail(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, msg));
}

static void
succeed(GTask *task, const char *mount_path)
{
    g_task_return_pointer(task, g_strdup(mount_path), g_free);
    g_object_unref(task);
}

static void
on_autoclear_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GTask *task = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (err)
        g_warning("SetAutoclear failed (loop device will need manual removal): %s", err->message);
    succeed(task, g_object_get_data(G_OBJECT(task), "mount-path"));
}

/* Every success path ends here: a loop device we created must get autoclear, however its
 * filesystem got mounted (our Mount call, an AlreadyMounted race, an automounter). */
static void
finish_mounted(GTask *task, const char *mount_path)
{
    MountOp *op = g_task_get_task_data(task);
    if (!op->created_loop) {
        succeed(task, mount_path);
        return;
    }
    op->created_loop = FALSE; /* ownership passes to the kernel's autoclear */
    g_object_set_data_full(G_OBJECT(task), "mount-path", g_strdup(mount_path), g_free);
    g_dbus_connection_call(op->bus, UDISKS_NAME, op->loop_path, IFACE_LOOP, "SetAutoclear",
                           g_variant_new_parsed("(true, @a{sv} {})"), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_autoclear_done, task);
}

static void
on_mounted(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GTask *task = user_data;
    MountOp *op = g_task_get_task_data(task);
    GError *err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!ret) {
        g_autofree char *remote = g_dbus_error_get_remote_error(err);
        if (g_strcmp0(remote, "org.freedesktop.UDisks2.Error.AlreadyMounted") == 0 &&
            op->polls++ < POLL_MAX) {
            g_clear_error(&err);
            list_objects(task); /* picks the existing mount point up */
            return;
        }
        g_dbus_error_strip_remote_error(err);
        fail(task, err);
        return;
    }

    const char *mp = NULL;
    g_variant_get(ret, "(&s)", &mp);
    finish_mounted(task, mp);
}

static gboolean
on_poll_timeout(gpointer user_data)
{
    GTask *task = user_data;
    GCancellable *c = g_task_get_cancellable(task);
    GError *err = NULL;
    if (c && g_cancellable_set_error_if_cancelled(c, &err)) {
        fail(task, err);
        return G_SOURCE_REMOVE;
    }
    list_objects(task);
    return G_SOURCE_REMOVE;
}

static void
on_loop_setup(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GTask *task = user_data;
    MountOp *op = g_task_get_task_data(task);
    GError *err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_with_unix_fd_list_finish(
        G_DBUS_CONNECTION(src), NULL, res, &err);
    if (!ret) {
        g_dbus_error_strip_remote_error(err);
        fail(task, err);
        return;
    }
    const char *obj = NULL;
    g_variant_get(ret, "(&o)", &obj);
    op->loop_path = g_strdup(obj);
    op->created_loop = TRUE;
    GCancellable *c = g_task_get_cancellable(task);
    if (c && g_cancellable_set_error_if_cancelled(c, &err)) {
        fail(task, err); /* deletes the loop device we just created */
        return;
    }
    list_objects(task);
}

static void
start_loop_setup(GTask *task)
{
    MountOp *op = g_task_get_task_data(task);
    int fd = open(op->real_path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        fail(task, g_error_new(G_IO_ERROR, fd < 0 ? g_io_error_from_errno(e) : G_IO_ERROR_NOT_REGULAR_FILE,
                               "Cannot open disk image: %s", fd < 0 ? g_strerror(e) : "not a regular file"));
        return;
    }
    g_autoptr(GUnixFDList) fds = g_unix_fd_list_new_from_array(&fd, 1); /* takes ownership of fd */
    g_dbus_connection_call_with_unix_fd_list(
        op->bus, UDISKS_NAME, UDISKS_MANAGER, "org.freedesktop.UDisks2.Manager", "LoopSetup",
        g_variant_new_parsed("(handle 0, {'read-only': <true>})"), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, CALL_TIMEOUT_MS, fds,
        NULL /* never cancel in flight: the reply names the device we must clean up */,
        on_loop_setup, task);
}

static void
on_listed(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GTask *task = user_data;
    MountOp *op = g_task_get_task_data(task);
    GError *err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!ret) {
        g_dbus_error_strip_remote_error(err);
        fail(task, err);
        return;
    }
    g_autoptr(GVariant) managed = g_variant_get_child_value(ret, 0);

    if (op->listing_for_reuse) {
        op->listing_for_reuse = FALSE;
        op->loop_path = kajo_diskimage_find_loop(managed, op->real_path, (guint32)getuid());
        if (!op->loop_path) {
            start_loop_setup(task);
            return;
        }
    }

    g_autofree char *fs_path = kajo_diskimage_find_filesystem(managed, op->loop_path);
    if (fs_path) {
        g_autofree char *mp = kajo_diskimage_first_mount_point(managed, fs_path);
        if (mp) {
            finish_mounted(task, mp);
            return;
        }
        g_dbus_connection_call(op->bus, UDISKS_NAME, fs_path, IFACE_FS, "Mount",
                               g_variant_new_parsed("(@a{sv} {},)"), G_VARIANT_TYPE("(s)"),
                               G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, CALL_TIMEOUT_MS,
                               NULL /* a mount that completes must still get autoclear */,
                               on_mounted, task);
        return;
    }

    if (++op->polls > POLL_MAX) {
        fail_msg(task, "No mountable filesystem found in the disk image");
        return;
    }
    g_timeout_add(POLL_INTERVAL_MS, on_poll_timeout, task);
}

static void
list_objects(GTask *task)
{
    MountOp *op = g_task_get_task_data(task);
    g_dbus_connection_call(op->bus, UDISKS_NAME, UDISKS_ROOT, "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                           G_DBUS_CALL_FLAGS_NONE, -1, g_task_get_cancellable(task), on_listed, task);
}

static void
on_bus(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    GTask *task = user_data;
    MountOp *op = g_task_get_task_data(task);
    GError *err = NULL;
    op->bus = g_bus_get_finish(res, &err);
    if (!op->bus) {
        fail(task, err);
        return;
    }
    op->listing_for_reuse = TRUE;
    list_objects(task);
}

typedef struct {
    KajoDiskImageCallback callback;
    gpointer              user_data;
} CallbackHolder;

static void
mount_task_done(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    CallbackHolder *h = user_data;
    GError *err = NULL;
    g_autofree char *mp = g_task_propagate_pointer(G_TASK(res), &err);
    h->callback(mp, err, h->user_data);
    g_clear_error(&err);
    g_free(h);
}

void
kajo_files_diskimage_mount_async(GFile *image,
                                 GCancellable *cancellable,
                                 KajoDiskImageCallback callback,
                                 gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(image) && callback != NULL);

    CallbackHolder *holder = g_new0(CallbackHolder, 1);
    holder->callback = callback;
    holder->user_data = user_data;
    GTask *task = g_task_new(NULL, cancellable, mount_task_done, holder);

    g_autofree char *path = g_file_get_path(image);
    g_autofree char *real = path ? kajo_files_resolve_real_path(path) : NULL;
    if (!real) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Disk images can only be mounted from a local path");
        g_object_unref(task);
        return;
    }

    MountOp *op = g_new0(MountOp, 1);
    op->real_path = g_steal_pointer(&real);
    g_task_set_task_data(task, op, mount_op_free);
    g_bus_get(G_BUS_TYPE_SYSTEM, cancellable, on_bus, task);
}
