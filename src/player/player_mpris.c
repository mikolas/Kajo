#include "player_mpris.h"
#include <string.h>
#include <unistd.h>

static const char mpris_introspection_xml[] =
    "<node>"
    "  <interface name='org.mpris.MediaPlayer2'>"
    "    <method name='Raise'/>"
    "    <method name='Quit'/>"
    "    <property name='CanQuit' type='b' access='read'/>"
    "    <property name='CanRaise' type='b' access='read'/>"
    "    <property name='HasTrackList' type='b' access='read'/>"
    "    <property name='Identity' type='s' access='read'/>"
    "    <property name='DesktopEntry' type='s' access='read'/>"
    "    <property name='SupportedUriSchemes' type='as' access='read'/>"
    "    <property name='SupportedMimeTypes' type='as' access='read'/>"
    "  </interface>"
    "  <interface name='org.mpris.MediaPlayer2.Player'>"
    "    <method name='Next'/>"
    "    <method name='Previous'/>"
    "    <method name='Pause'/>"
    "    <method name='PlayPause'/>"
    "    <method name='Stop'/>"
    "    <method name='Play'/>"
    "    <method name='Seek'>"
    "      <arg direction='in' name='Offset' type='x'/>"
    "    </method>"
    "    <method name='SetPosition'>"
    "      <arg direction='in' name='TrackId' type='o'/>"
    "      <arg direction='in' name='Position' type='x'/>"
    "    </method>"
    "    <property name='PlaybackStatus' type='s' access='read'/>"
    "    <property name='Rate' type='d' access='readwrite'/>"
    "    <property name='Metadata' type='a{sv}' access='read'/>"
    "    <property name='Volume' type='d' access='readwrite'/>"
    "    <property name='Position' type='x' access='read'/>"
    "    <property name='MinimumRate' type='d' access='read'/>"
    "    <property name='MaximumRate' type='d' access='read'/>"
    "    <property name='CanGoNext' type='b' access='read'/>"
    "    <property name='CanGoPrevious' type='b' access='read'/>"
    "    <property name='CanPlay' type='b' access='read'/>"
    "    <property name='CanPause' type='b' access='read'/>"
    "    <property name='CanSeek' type='b' access='read'/>"
    "    <property name='CanControl' type='b' access='read'/>"
    "  </interface>"
    "</node>";

struct _KajoPlayerMpris {
    GDBusConnection *conn;
    GDBusNodeInfo *node_info;
    guint reg_root_id;
    guint reg_player_id;
    guint owner_id;

    KajoMprisCommandFunc cmd_cb;
    gpointer user_data;

    gboolean has_media;
    gboolean playing;
    gint64 position_usec;
    gint64 duration_usec;
    double volume;

    char *title;
    char *uri;
    char *content_type;
};

static GVariant *
build_metadata_variant(const KajoPlayerMpris *self)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

    g_variant_builder_add(&b, "{sv}", "mpris:trackid",
                          g_variant_new_object_path("/org/kajo/Player/Track/0"));

    if (self->duration_usec > 0) {
        g_variant_builder_add(&b, "{sv}", "mpris:length",
                              g_variant_new_int64(self->duration_usec));
    }

    g_variant_builder_add(&b, "{sv}", "xesam:title",
                          g_variant_new_string(self->title ? self->title : "Kajo Player"));

    GVariantBuilder artist_b;
    g_variant_builder_init(&artist_b, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&artist_b, "s", self->content_type ? self->content_type : "Kajo Media");
    g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_builder_end(&artist_b));

    if (self->uri) {
        g_variant_builder_add(&b, "{sv}", "xesam:url",
                              g_variant_new_string(self->uri));
    }

    return g_variant_builder_end(&b);
}

static const char *
get_playback_status_str(const KajoPlayerMpris *self)
{
    if (!self->has_media)
        return "Stopped";
    return self->playing ? "Playing" : "Paused";
}

static void
emit_properties_changed(KajoPlayerMpris *self)
{
    if (!self || !self->conn) return;

    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "PlaybackStatus",
                          g_variant_new_string(get_playback_status_str(self)));
    g_variant_builder_add(&props, "{sv}", "Metadata",
                          build_metadata_variant(self));
    g_variant_builder_add(&props, "{sv}", "Volume",
                          g_variant_new_double(self->volume));

    GVariantBuilder inv;
    g_variant_builder_init(&inv, G_VARIANT_TYPE("as"));

    g_dbus_connection_emit_signal(
        self->conn,
        NULL,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player", &props, &inv),
        NULL
    );
}

static void
handle_method_call(GDBusConnection *connection G_GNUC_UNUSED,
                   const gchar *sender G_GNUC_UNUSED,
                   const gchar *object_path G_GNUC_UNUSED,
                   const gchar *interface_name,
                   const gchar *method_name,
                   GVariant *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer user_data)
{
    KajoPlayerMpris *self = user_data;

    if (g_strcmp0(interface_name, "org.mpris.MediaPlayer2") == 0) {
        if (g_strcmp0(method_name, "Raise") == 0 && self->cmd_cb)
            self->cmd_cb(KAJO_MPRIS_CMD_RAISE, 0, self->user_data);
        else if (g_strcmp0(method_name, "Quit") == 0 && self->cmd_cb)
            self->cmd_cb(KAJO_MPRIS_CMD_QUIT, 0, self->user_data);

        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(interface_name, "org.mpris.MediaPlayer2.Player") == 0 && self->cmd_cb) {
        if (g_strcmp0(method_name, "Play") == 0)
            self->cmd_cb(KAJO_MPRIS_CMD_PLAY, 0, self->user_data);
        else if (g_strcmp0(method_name, "Pause") == 0)
            self->cmd_cb(KAJO_MPRIS_CMD_PAUSE, 0, self->user_data);
        else if (g_strcmp0(method_name, "PlayPause") == 0)
            self->cmd_cb(KAJO_MPRIS_CMD_PLAY_PAUSE, 0, self->user_data);
        else if (g_strcmp0(method_name, "Stop") == 0)
            self->cmd_cb(KAJO_MPRIS_CMD_STOP, 0, self->user_data);
        else if (g_strcmp0(method_name, "Next") == 0)
            self->cmd_cb(KAJO_MPRIS_CMD_NEXT, 0, self->user_data);
        else if (g_strcmp0(method_name, "Previous") == 0)
            self->cmd_cb(KAJO_MPRIS_CMD_PREV, 0, self->user_data);
        else if (g_strcmp0(method_name, "Seek") == 0) {
            gint64 offset = 0;
            g_variant_get(parameters, "(x)", &offset);
            self->cmd_cb(KAJO_MPRIS_CMD_PLAY, offset, self->user_data);
        }
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *
handle_get_property(GDBusConnection *connection G_GNUC_UNUSED,
                    const gchar *sender G_GNUC_UNUSED,
                    const gchar *object_path G_GNUC_UNUSED,
                    const gchar *interface_name,
                    const gchar *property_name,
                    GError **error G_GNUC_UNUSED,
                    gpointer user_data)
{
    KajoPlayerMpris *self = user_data;

    if (g_strcmp0(interface_name, "org.mpris.MediaPlayer2") == 0) {
        if (g_strcmp0(property_name, "CanQuit") == 0 ||
            g_strcmp0(property_name, "CanRaise") == 0)
            return g_variant_new_boolean(TRUE);
        if (g_strcmp0(property_name, "HasTrackList") == 0)
            return g_variant_new_boolean(FALSE);
        if (g_strcmp0(property_name, "Identity") == 0)
            return g_variant_new_string("Kajo Player");
        if (g_strcmp0(property_name, "DesktopEntry") == 0)
            return g_variant_new_string("org.kajo.Player");
        if (g_strcmp0(property_name, "SupportedUriSchemes") == 0) {
            const char *const schemes[] = { "file", "http", "https", NULL };
            return g_variant_new_strv(schemes, -1);
        }
        if (g_strcmp0(property_name, "SupportedMimeTypes") == 0) {
            const char *const mimes[] = {
                "video/x-matroska", "video/mp4", "video/webm", "audio/mpeg", "audio/flac", "audio/ogg", NULL
            };
            return g_variant_new_strv(mimes, -1);
        }
    }

    if (g_strcmp0(interface_name, "org.mpris.MediaPlayer2.Player") == 0) {
        if (g_strcmp0(property_name, "PlaybackStatus") == 0)
            return g_variant_new_string(get_playback_status_str(self));
        if (g_strcmp0(property_name, "Rate") == 0 ||
            g_strcmp0(property_name, "MinimumRate") == 0 ||
            g_strcmp0(property_name, "MaximumRate") == 0)
            return g_variant_new_double(1.0);
        if (g_strcmp0(property_name, "Metadata") == 0)
            return build_metadata_variant(self);
        if (g_strcmp0(property_name, "Volume") == 0)
            return g_variant_new_double(self->volume);
        if (g_strcmp0(property_name, "Position") == 0)
            return g_variant_new_int64(self->position_usec);
        if (g_strcmp0(property_name, "CanGoNext") == 0 ||
            g_strcmp0(property_name, "CanGoPrevious") == 0 ||
            g_strcmp0(property_name, "CanPlay") == 0 ||
            g_strcmp0(property_name, "CanPause") == 0 ||
            g_strcmp0(property_name, "CanSeek") == 0 ||
            g_strcmp0(property_name, "CanControl") == 0)
            return g_variant_new_boolean(TRUE);
    }

    return NULL;
}

static const GDBusInterfaceVTable mpris_vtable = {
    .method_call = handle_method_call,
    .get_property = handle_get_property,
    .set_property = NULL,
};

static void
on_bus_acquired(GDBusConnection *connection, const gchar *name G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerMpris *self = user_data;
    self->conn = g_object_ref(connection);

    if (self->node_info && self->node_info->interfaces) {
        self->reg_root_id = g_dbus_connection_register_object(
            connection,
            "/org/mpris/MediaPlayer2",
            self->node_info->interfaces[0],
            &mpris_vtable,
            self,
            NULL,
            NULL
        );
        self->reg_player_id = g_dbus_connection_register_object(
            connection,
            "/org/mpris/MediaPlayer2",
            self->node_info->interfaces[1],
            &mpris_vtable,
            self,
            NULL,
            NULL
        );
    }
}

KajoPlayerMpris *
kajo_player_mpris_new(KajoMprisCommandFunc cmd_cb, gpointer user_data)
{
    KajoPlayerMpris *self = g_new0(KajoPlayerMpris, 1);
    self->cmd_cb = cmd_cb;
    self->user_data = user_data;
    self->volume = 1.0;

    self->node_info = g_dbus_node_info_new_for_xml(mpris_introspection_xml, NULL);

    g_autofree char *bus_name = g_strdup_printf("org.mpris.MediaPlayer2.kajo_player.instance%d", (int)getpid());
    self->owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        bus_name,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        NULL,
        NULL,
        self,
        NULL
    );

    return self;
}

void
kajo_player_mpris_free(KajoPlayerMpris *self)
{
    if (!self) return;
    if (self->owner_id > 0)
        g_bus_unown_name(self->owner_id);
    if (self->conn) {
        if (self->reg_root_id > 0)
            g_dbus_connection_unregister_object(self->conn, self->reg_root_id);
        if (self->reg_player_id > 0)
            g_dbus_connection_unregister_object(self->conn, self->reg_player_id);
        g_clear_object(&self->conn);
    }
    if (self->node_info)
        g_dbus_node_info_unref(self->node_info);
    g_free(self->title);
    g_free(self->uri);
    g_free(self->content_type);
    g_free(self);
}

void
kajo_player_mpris_update_state(KajoPlayerMpris *self,
                               gboolean has_media,
                               gboolean playing,
                               gint64 position_usec,
                               gint64 duration_usec,
                               double volume)
{
    if (!self) return;
    gboolean state_changed = (self->has_media != has_media) ||
                             (self->playing != playing) ||
                             (self->duration_usec != duration_usec);

    self->has_media = has_media;
    self->playing = playing;
    self->position_usec = position_usec;
    self->duration_usec = duration_usec;
    self->volume = volume;

    if (state_changed)
        emit_properties_changed(self);
}

void
kajo_player_mpris_update_track(KajoPlayerMpris *self,
                               const KajoMediaTrack *track,
                               gint64 duration_usec)
{
    if (!self) return;
    g_clear_pointer(&self->title, g_free);
    g_clear_pointer(&self->uri, g_free);
    g_clear_pointer(&self->content_type, g_free);

    if (track) {
        self->title = g_strdup(track->display_name);
        self->uri = g_strdup(track->uri);
        self->content_type = g_strdup(track->content_type);
        self->has_media = TRUE;
    } else {
        self->has_media = FALSE;
    }
    self->duration_usec = duration_usec;
    emit_properties_changed(self);
}
