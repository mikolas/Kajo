#ifndef KAJO_FILES_NETWORK_H
#define KAJO_FILES_NETWORK_H

#include <gio/gio.h>
#include "files_model.h"

G_BEGIN_DECLS

/* True for remote network filesystem schemes (smb, sftp, ssh, ftp, ftps, dav, davs, nfs, afp). */
gboolean    kajo_files_network_is_remote_uri(const char *uri);

/* Short uppercase bracketed badge for Grid/List/Status views (e.g. "[ SMB ]", "[ SFTP ]"). */
const char *kajo_files_network_scheme_label(const char *uri);

/* Human-readable type description derived from a network URI scheme. */
const char *kajo_files_network_scheme_description(const char *uri);

/* Canonical deduplication key: lowercased scheme+host, root trailing '/' stripped. */
char       *kajo_files_network_canonicalize_uri(const char *uri);

/* Build a GVFS URI from a DNS-SD/Avahi resolved service record. Returns NULL for unsupported types. */
char       *kajo_files_network_build_service_uri(const char *service_type,
                                                 const char *host,
                                                 const char *address,
                                                 guint16     port,
                                                 const char *path,
                                                 const char *user);

/* Load GTK/Nautilus-compatible XBEL saved servers file into `items`, deduplicating via `seen_uris`. */
void        kajo_files_network_load_saved_servers_from_file(const char *bookmark_path,
                                                            GPtrArray  *items,
                                                            GHashTable *seen_uris);

/* Persist a remote server URI into an XBEL bookmark file (compatible with gtk-4.0/servers). */
gboolean    kajo_files_network_record_server_to_file(const char *bookmark_path,
                                                     const char *uri,
                                                     const char *title,
                                                     GError    **error);

/* Convenience wrapper persisting a connected remote URI into $XDG_CONFIG_HOME/gtk-4.0/servers. */
void        kajo_files_network_record_server(const char *uri, const char *title);

/* Main-thread only: snapshot active non-native GVFS mounts as KajoFileItem entries. */
GPtrArray  *kajo_files_network_snapshot_active_mounts(void);

/* Worker-thread aggregation for network:///: Avahi mDNS + GVFS network:/// + active mounts + saved servers. */
GPtrArray  *kajo_files_network_enumerate_sync(GFile        *network_dir,
                                              GPtrArray    *mounted_snapshot,
                                              GCancellable *cancellable,
                                              GError      **error);

G_END_DECLS

#endif /* KAJO_FILES_NETWORK_H */
