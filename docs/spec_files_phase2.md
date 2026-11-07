# Specification: `kajo-files` Phase 2 — Virtual Archive Folders, Disk Image Mounting, Persistent State, Breadcrumb UX & Multi-Selection (`docs/spec_files_phase2.md`)

**Status:** Active Specification — tracking branch `feat/files-phase2`  
**Target Binary:** `kajo-files` (`org.kajo.Files`, `src/files/`)  
**Shared Dependencies:** `libarchive`, `gio-2.0` / `gio-unix-2.0` (UDisks2 D-Bus), `gtk4`, `ShellConfig` (`src/config.c`)

---

## 1. Objectives & Scope

`kajo-files` Phase 1 established the Aero + Metro 3-pane workspace, async worker-thread directory enumeration, natural collation sorting, context menu (`Open With / Set Default`), and `Space` Quick Look (`GtkPicture`, Poppler PDF, `KajoPlayerMpv`, `libarchive` preview).

Phase 2 resolves the core usability shortcomings identified in daily use:

1. **Archives as Virtual Read-Only Folders (`archive://`), Smart Extraction & Archive Creation (`libarchive`):**
   - Double-clicking or pressing `Enter` on an archive navigates *inside* it as a read-only virtual directory hierarchy rather than shelling out to `xdg-open`.
   - Smart extraction avoids both tarbombs (loose root files) and `foo/foo/` double-nesting when an archive already has a single root folder.
   - Creating `.zip`, `.tar.zst`, `.tar.gz`, and `.tar.xz` archives directly from single or multi-selected items.
2. **ISO & Disk Image Loop-Mounting (`UDisks2` D-Bus):**
   - Double-clicking or choosing `"💿 Mount Disk Image"` on `.iso`, `.img`, and `.raw` disk images loop-mounts them via `org.freedesktop.UDisks2.Manager.LoopSetup` (`autoclear: true`), exposes them in the sidebar with one-click `⏏` Eject/Unmount, and navigates directly into the mounted filesystem.
3. **Global Persistence of View Mode, Sort Column, Sort Direction & Chrome State (`ShellConfig`):**
   - Remembering `files_view_mode` (`grid` vs `list`), `files_sort_field`, `files_sort_ascending`, `files_sort_folders_first`, `files_show_hidden`, and `files_show_inspector` across launches in `~/.config/kajo/config.json`.
4. **Breadcrumb Bar Auto-Scroll & Rapid Up-Tree Navigation:**
   - Auto-scrolling the breadcrumb bar to the rightmost active directory segment on every navigation.
   - Keeping `/` and `/home` reachable even when inside `$HOME`.
   - Right-click sibling/subfolder jump popover on any breadcrumb segment, middle-click / `Ctrl+Click` to open any ancestor in a new window, and horizontal mouse-wheel scrolling.
5. **Core File Management Completeness:**
   - Multi-selection (`GtkMultiSelection`, rubberband drag selection, `Ctrl+A`) across Grid and List views.
   - Cross-window Wayland `GdkClipboard` file copy/cut/paste and safe non-clobbering move (`Cut` + `Paste`).
   - `+ New File...` creation (`Ctrl+Alt+N`), Inspector executable permission toggle (`chmod +x`), cancellable background operations, and fully async Quick Look archive/PDF workers.

---

## 2. Virtual Read-Only Archive Folders (`archive://`) & `libarchive` Operations

### 2.1 Virtual URI Format
When activating an archive file at local path `/path/to/bundle.tar.gz`, `kajo-files` navigates to a canonical `archive://` URI:
```text
archive:///path/to/bundle.tar.gz!/
archive:///path/to/bundle.tar.gz!/src/include
```
- The `!/` delimiter unambiguously separates the host filesystem archive path (`/path/to/bundle.tar.gz`) from the internal relative archive path (`src/include`), even when the archive filename or internal directories contain `.zip` or `.tar.gz` substrings.
- Helper API in `src/files/files_model.h`:
  ```c
  gboolean kajo_files_parse_archive_uri(const char *uri,
                                        char **out_archive_path,
                                        char **out_inner_path);
  char    *kajo_files_build_archive_uri(const char *archive_path,
                                        const char *inner_path);
  ```

### 2.2 Worker-Thread Enumeration Inside `archive://`
When `kajo_files_enumerate_async()` receives a `GFile` whose URI starts with `archive://`:
1. `enumerate_thread_cb()` parses `archive_path` and `inner_path` (normalized without leading/trailing `/`).
2. Opens `archive_path` via `archive_read_new()` (`archive_read_support_filter_all()`, `archive_read_support_format_all()`).
3. Iterates headers with `archive_read_next_header()` + `archive_read_data_skip()`:
   - Filters entries matching the prefix `inner_path/` (or all entries when `inner_path` is `""`).
   - Splits the remaining relative path at the first `/`:
     - **Direct child entry (`rest` has no `/`):** Creates a `KajoFileItem` with `kajo_file_item_new_manual()` using the entry's `archive_entry_size()`, `archive_entry_mtime()`, `archive_entry_perm()`, `is_dir`, and MIME type guessed via `g_content_type_guess(child_name, NULL, 0, NULL)`.
     - **Implicit or explicit subdirectory (`rest` contains `/`):** Deduplicates synthetic directory names via a `GHashTable *seen_dirs` so archives that omit explicit directory headers still show browsable folders.
4. Each child item's URI is set to `archive://<archive_path>!/<child_rel_path>`.

### 2.3 Contextual Read-Only Archive Banner
When `self->current_dir` has scheme `archive`, `KajoFilesWindow` reveals `archive_banner_revealer` at the top of the directory view:
```text
┌──────────────────────────────────────────────────────────────────────────────────┐
│ 📦 ARCHIVE VIEW (READ-ONLY) — bundle.tar.gz    [ 📦 EXTRACT ALL ]  [ ↩ EXIT ]   │
└──────────────────────────────────────────────────────────────────────────────────┘
```
- `[ 📦 EXTRACT ALL ]` extracts the archive into its parent host directory and navigates to the extracted result.
- `[ ↩ EXIT ]` navigates back to the host directory containing the archive.
- Mutating commands (`+ NEW FOLDER`, `+ NEW FILE`, `PASTE`, `Rename`, `Move to Trash`, `Delete Permanently`) are disabled while browsing inside `archive://`.

### 2.4 On-Demand Single-Entry Extraction for Quick Look, Open & Copy
When the user selects a file inside `archive://` and presses:
- **`Space` (Quick Look)** or **`Enter` / Double-Click (Open in Default App):**
  - If the item is a directory inside the archive, `Enter` navigates into `archive://<archive_path>!/<subdir>`.
  - If the item is a file inside the archive, `kajo_files_op_extract_entry_to_temp_async()` extracts that single entry on a worker thread into `$XDG_CACHE_HOME/kajo/archive-preview/<sha256-of-archive-uri>/<entry_name>` and passes the resulting local `GFile *` to `KajoQuickLook` or `kajo_files_open_item_default()`.
- **`Ctrl+C` (Copy) inside `archive://` followed by `Ctrl+V` (Paste) in a normal directory:**
  - `kajo_files_op_transfer_async()` detects source URIs with scheme `archive://` and extracts the selected archive entries (files or subtrees) directly into `dest_dir`.

### 2.5 Smart Extraction (`kajo_files_op_extract_archive_async`)
To fix both tarbombs and `foo/foo/` double-nesting:
1. Pass 1 (`scan_archive_top_level()`): Read archive headers only (`archive_read_data_skip()`) and count distinct top-level path components (`first_seg`).
2. Pass 2 (Extraction):
   - **Single top-level entry (`n_top == 1`):** Extract directly into `dest_parent_dir`, mapping the single top-level component through `resolve_unique_dest_child(dest_parent_dir, single_top_name)` so existing files/folders are never overwritten.
   - **Multiple top-level entries (`n_top > 1`):** Create `target_dir = resolve_unique_dest_child(dest_parent_dir, strip_archive_stem(archive_basename))` and extract all entries inside `target_dir`.

### 2.6 Creating Archives (`kajo_files_op_compress_async`)
- New operation in `src/files/files_ops.h`:
  ```c
  void kajo_files_op_compress_async(GPtrArray *source_files,
                                    GFile *dest_parent_dir,
                                    const char *archive_filename,
                                    GCancellable *cancellable,
                                    KajoFileOpCallback callback,
                                    gpointer user_data);
  ```
- Triggered from the context menu (`📦 Compress to Archive...`) or keyboard shortcut (`Ctrl+Shift+A`), opening the inline prompt banner (`PROMPT_COMPRESS`) pre-filled with `<stem>.zip` (or `archive.zip` when multiple items are selected).
- Format & filter selection in `libarchive` based on suffix:
  - `.zip` $\rightarrow$ `archive_write_set_format_zip()`
  - `.tar.zst` / `.tzst` $\rightarrow$ `archive_write_set_format_pax_restricted()` + `archive_write_add_filter_zstd()`
  - `.tar.gz` / `.tgz` $\rightarrow$ `archive_write_set_format_pax_restricted()` + `archive_write_add_filter_gzip()`
  - `.tar.xz` / `.txz` $\rightarrow$ `archive_write_set_format_pax_restricted()` + `archive_write_add_filter_xz()`
  - `.tar` $\rightarrow$ `archive_write_set_format_pax_restricted()` + `archive_write_add_filter_none()`

---

## 3. ISO & Disk Image Loop-Mounting (`UDisks2` D-Bus)

### 3.1 Classification
Add `kajo_files_is_disk_image(const char *content_type, const char *filename)` in `src/files/files_model.c`:
- Matches MIME types `application/x-iso9660-image`, `application/x-cd-image`, `application/x-raw-disk-image`, `application/x-apple-diskimage` and extensions `.iso`, `.img`, `.raw`.
- Regular archives (`.zip`, `.tar.*`, `.7z`, `.rar`, `.cpio`, `.deb`, `.rpm`) remain classified by `kajo_files_is_archive()`.

### 3.2 UDisks2 `LoopSetup` + Mount Flow
When the user double-clicks or selects `"💿 Mount Disk Image"` on a disk image:
1. Open the local file descriptor with `open(local_path, O_RDONLY | O_CLOEXEC)`.
2. Create a `GUnixFDList` containing the fd (index `0`) and call `org.freedesktop.UDisks2.Manager.LoopSetup` on `/org/freedesktop/UDisks2/Manager` (`G_BUS_TYPE_SYSTEM`) with parameters:
   - `fd`: handle `0`
   - `options`: `{"read-only": <true>, "autoclear": <true>}`
3. In the async reply callback (`loop_obj_path`, e.g. `/org/freedesktop/UDisks2/block_devices/loop0`):
   - Call `org.freedesktop.UDisks2.Filesystem.Mount` (`options: {}`) on `loop_obj_path` (or if `loop_obj_path` is a partitioned table `org.freedesktop.UDisks2.PartitionTable`, find its first partition object with `org.freedesktop.UDisks2.Filesystem` and mount it).
   - Receive `mount_path` (e.g. `/run/media/$USER/<label>`), refresh `KajoFilesSidebar`, and navigate `KajoFilesWindow` to `file://<mount_path>`.
4. **Sidebar Unmount / Eject:** Because `autoclear: true` was set at loop setup time, clicking the existing `⏏` button in `KajoFilesSidebar` unmounts the filesystem and UDisks2 automatically deletes the loop device.
5. **Fallback:** For `.iso` images, the context menu also includes `"📦 Browse as Archive"` (navigating to `archive://<iso_path>!/`) and `"📦 Extract ISO Contents"`. If `UDisks2` D-Bus is unavailable (e.g., inside a minimal container), activating an `.iso` falls back to `archive://` with a status bar notice.

---

## 4. Persistent View Mode, Sort Order & Chrome Preferences

### 4.1 `ShellConfig` Schema Additions (`src/config.h` & `src/config.c`)
Add the following fields to `ShellConfig` and serialize/deserialize them in `~/.config/kajo/config.json`:

| Field | C Type | JSON Key | Default | Allowed Values |
| :--- | :--- | :--- | :--- | :--- |
| `files_view_mode` | `gchar *` | `"files_view_mode"` | `"grid"` | `"grid"`, `"list"` |
| `files_sort_field` | `gchar *` | `"files_sort_field"` | `"name"` | `"name"`, `"size"`, `"modified"`, `"type"` |
| `files_sort_ascending` | `gboolean` | `"files_sort_ascending"` | `TRUE` | `true`, `false` |
| `files_sort_folders_first` | `gboolean` | `"files_sort_folders_first"` | `TRUE` | `true`, `false` |
| `files_show_hidden` | `gboolean` | `"files_show_hidden"` | `FALSE` | `true`, `false` |
| `files_show_inspector` | `gboolean` | `"files_show_inspector"` | `FALSE` | `true`, `false` |

### 4.2 Load & Save Synchronization
- On window initialization (`kajo_files_window_init()`), load `ShellConfig` via `shell_config_load(NULL)` and apply all six preferences to `KajoFilesWindow` state and button active classes.
- Whenever the user changes view mode (`Ctrl+1` / `Ctrl+2` / header buttons), sort field/direction/folders-first (popover or list column headers), hidden files (`Ctrl+H` / ribbon button), or inspector visibility (`Ctrl+I` / header button), call `files_window_save_prefs(self)` to persist the updated fields to `~/.config/kajo/config.json`.

---

## 5. Breadcrumb Bar Auto-Scroll & Rapid Up-Tree Navigation

### 5.1 Auto-Scroll to Rightmost Segment
1. In `data/ui/files_window.blp`, give the breadcrumb `ScrolledWindow` the template ID `breadcrumb_scroll` and set `hscrollbar-policy: external` so it never draws a scrollbar over the 28px header buttons.
2. In `KajoFilesWindow`, bind `breadcrumb_scroll` and connect to `changed` and `notify::page-size` on `gtk_scrolled_window_get_hadjustment(self->breadcrumb_scroll)`.
3. In `rebuild_breadcrumbs()`, set `self->pending_breadcrumb_scroll_end = TRUE` and schedule a one-shot idle/adjustment sync that sets:
   ```c
   double upper = gtk_adjustment_get_upper(hadj);
   double page  = gtk_adjustment_get_page_size(hadj);
   gtk_adjustment_set_value(hadj, MAX(0.0, upper - page));
   ```
4. Attach a `GtkEventControllerScroll` (`GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES`) to `breadcrumb_scroll` so vertical mouse-wheel scrolling pans horizontally across deep breadcrumb chains.

### 5.2 Full Root-to-Leaf Chain & Archive Breadcrumbs
- **Do not stop walking parents at `$HOME`:** In `rebuild_breadcrumbs()`, walk all the way to `/` so `/` and `/home` remain clickable buttons to the left of `⌂ <username>`.
- **Archive breadcrumbs:** When inside `archive:///path/to/foo.zip!/a/b`, render the host ancestor buttons up to `📦 foo.zip`, followed by `▸` and buttons for `a` and `b`. Clicking any host ancestor exits the archive and navigates to that host directory in one click.

### 5.3 Rapid Parent / Sibling Jump Popover & Middle-Click
- **Right-click on any breadcrumb button:** Opens a compact Aero + Metro popover (`self->breadcrumb_popover`) anchored to that breadcrumb button:
  - Header: `📁 <SEGMENT NAME>`
  - Scrollable list of immediate subdirectories inside that breadcrumb's directory (enumerated asynchronously, up to 50 folders) — clicking any subfolder navigates directly to it.
  - Footer actions: `🗔 Open in New Window` and `🔗 Copy Path`.
- **Middle-click (`GDK_BUTTON_MIDDLE`) or `Ctrl+Left-Click` on any breadcrumb button:** Spawns a new `KajoFilesWindow` at that ancestor URI.

---

## 6. Multi-Selection & Core File Management Improvements

### 6.1 Multi-Selection (`GtkMultiSelection`)
- Replace `GtkSingleSelection *selection_model` with `GtkMultiSelection *selection_model` in `KajoFilesWindow`.
- Set `enable-rubberband: true` on both `grid_view` and `details_listview` in `data/ui/files_window.blp`.
- Add `GPtrArray *files_window_get_selected_items(KajoFilesWindow *self)` (returning a `GPtrArray` of `KajoFileItem *`) and keep `self->selected_item` pointing to the primary/first selected item for Quick Look and the Inspector.
- Wire `Ctrl+A` in `files_window_keys.c` to `gtk_selection_model_select_all(GTK_SELECTION_MODEL(self->selection_model))`.
- Update the status bar selection counter (`"%u selected (%s)"`) and context menu header (`"%u ITEMS SELECTED"` when `> 1` item is selected).

### 6.2 Cross-Window Clipboard & Non-Clobbering Move
- **Cross-window clipboard:** In `src/files/files_ops.c`, when `g_clip_files` is empty or external clipboard content changed, read `text/uri-list` asynchronously from `gdk_display_get_clipboard()` so Copy/Paste works across separate `kajo-files` processes.
- **Safe Move (`Cut` + `Paste`):** In `batch_op_worker` (`BATCH_OP_TRANSFER`), when `ctx->is_move` is `TRUE` and `!g_file_equal(src, dst)`, resolve destination collisions via `resolve_unique_dest_child(ctx->dest_dir, base)` instead of passing `G_FILE_COPY_OVERWRITE`.

### 6.3 `+ New File...`, Executable Toggle & Async Polish
- **`+ New File...` (`PROMPT_NEW_FILE`):** Add `kajo_files_op_create_file_async()` (`g_file_create_async()`), accessible via `Ctrl+Alt+N`, the action ribbon (`+ FILE`), and the directory background context menu (`+ New Empty File...`).
- **Inspector Executable Toggle:** Add a `[ ☑ EXECUTABLE (+X) ]` button in `KajoQuickLook`'s Inspector pane for local regular files that toggles `0111` permission bits via `g_chmod()` and refreshes the item metadata.
- **Async Quick Look Archive/PDF & Operation Cancellation:**
  - Move `populate_archive_preview()` and Inspector PDF thumbnail rendering onto `GTask` worker threads using `self->preview_cancel`.
  - Pass `GCancellable *` into all `kajo_files_op_*_async()` functions and show an active progress/cancel state in the status bar while an operation is in flight.

---

## 7. Verification & Test Plan

1. **Unit & Integration Tests (`tests/test_files_model.c`):**
   - `archive://` URI parsing and building (`kajo_files_parse_archive_uri`, `kajo_files_build_archive_uri`).
   - Creating a `.zip` and `.tar.gz` archive via `kajo_files_op_compress_async()`, enumerating its contents via `archive://` in `kajo_files_enumerate_async()`, and extracting it via smart `kajo_files_op_extract_archive_async()` (verifying both single-root no-double-nesting and multi-root tarbomb wrapping).
   - Disk image classification (`kajo_files_is_disk_image` vs `kajo_files_is_archive`).
   - `ShellConfig` round-trip serialization test for `files_view_mode`, `files_sort_field`, `files_sort_ascending`, `files_sort_folders_first`, `files_show_hidden`, and `files_show_inspector`.
2. **Visual & ASan Verification:**
   - `meson compile -C builddir && meson test -C builddir` (0 warnings, 100% pass).
   - `scripts/asan-test.sh` for leak and lifetime verification.
   - `scripts/ui-shot.sh` headless render of `kajo-files` in both Grid and List views, verifying breadcrumb right-alignment, action ribbon, and archive banner styling against `docs/ui-gallery.html`.
