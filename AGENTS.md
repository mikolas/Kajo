# AGENTS.md — Kajo & Settings Guidelines

This document establishes the architecture rules, forbidden practices, design standards, and development learnings for AI coding assistants working on the **Kajo** (`kajo`) and **Kajo Settings** (`kajo-settings`) codebase.

---

## 1. Executive Summary & Tech Stack

The project delivers a modern, lightweight Wayland desktop shell and settings application built for the **Niri Wayland Compositor**.

- **Languages**: Standard C17 (`-std=c17`), GTK4 (`gtk4`), GTK Layer Shell (`gtk4-layer-shell-0`), GLib/GIO (`gio-2.0`), PulseAudio/PipeWire (`libpulse`).
- **Build System**: Meson (`meson`) & Ninja (`ninja`).
- **System Buses**: System D-Bus (`org.freedesktop.NetworkManager`, `org.bluez`, `org.freedesktop.UPower`).
- **Styling System**: Custom Vanilla GTK CSS (`data/defaults/style.css`), scoped to `window.shell-settings-window` and `.shell-popover`.

---

## 2. Forbidden Practices (Strict Anti-Patterns)

❌ **1. NO MOCK DATA IN UI**
- **NEVER** insert static mock device lists, mock Wi-Fi SSIDs, or fake Bluetooth MAC addresses.
- All NetworkManager Wi-Fi access points, BlueZ devices, PipeWire audio sinks/sources, UPower batteries, and Niri monitor outputs **MUST** query real live system data via D-Bus / IPC.

❌ **2. NO HARDCODED D-BUS OBJECT PATHS**
- **NEVER** hardcode fixed paths like `/org/bluez/hci0` or `/org/freedesktop/NetworkManager/Devices/2`.
- Dynamically query system adapters and interfaces using `g_dbus_object_manager_get_objects()` or `GetDevices`.

❌ **3. NO UNSTYLED APPLICATION LAUNCHES**
- When instantiating standalone GTK applications (such as `kajo-settings`), **ALWAYS** initialize `ShellTheme` (`shell_theme_new()` & `shell_theme_apply()`) in `main.c` so `style.css` rules apply to window widgets.

❌ **4. NO MONOLITHIC SOURCE FILES**
- Do **NOT** lump multiple UI pages or unrelated subsystems into single multi-thousand-line files.
- Keep settings page builders cleanly modularized under `src/settings/pages/page_*.c`.

❌ **5. NO MEMORY OR GVARIANT LEAKS**
- Always unref `GVariant*` objects (`g_variant_unref`), D-Bus proxies (`g_object_unref`), error structs (`g_clear_error`), and string allocations (`g_free`).
- Always remove existing child widgets from container boxes before repopulating dynamic D-Bus lists.

❌ **6. NO UN-SCOPED DYNAMIC CSS PROVIDERS**
- **NEVER** pass generic element node selectors like `box { ... }` or `button { ... }` to `gtk_css_provider_load_from_string()` when registering providers via `gtk_style_context_add_provider_for_display()`.
- Because `gdk_display_get_default()` applies CSS globally, generic node selectors will hijack every widget container across the entire application, breaking dark surfaces and forcing unwanted border-radii. Always use unique, scoped class names (e.g. `.swatch-0`, `.accent-tile-0`).

❌ **7. NO UNINITIALIZED `ShellWidgetClass` `.id` MEMBERS**
- **ALWAYS** explicitly define both `.id = "widget-id"` and `.name = "Widget Name"` in `ShellWidgetClass` struct definitions (`src/widgets/*.c`).
- Omitting `.id` defaults the pointer to `NULL`, which crashes `shell_widget_registry_find` during `strcmp(cls->id, id)`.

❌ **8. NO CLAIMING SUCCESS WITHOUT COMPILING**
- Never declare a feature complete or fixed without running `meson compile -C builddir` and verifying 0 errors and 0 warnings.

❌ **9. NO MARKDOWN FILES IN PROJECT ROOT**
- **NEVER** create `.md` markdown files in the project root directory unless explicitly requested by the user.
- Always create documentation, scratch files, plans, and specs inside the `scratch/` directory.

---

## 3. Mandatory Development Standards (What MUST be Done)

### A. Architectural Organization
- **Main Settings Container**: [src/settings/settings_window.c](file:///home/mikolas/src/Kajo/src/settings/settings_window.c) serves purely as a lightweight container shell (~150 lines) managing the sidebar list and `GtkStack`.
- **Modular Pages**: Individual preference pages live under [src/settings/pages/](file:///home/mikolas/src/Kajo/src/settings/pages/):
  - `page_panel.c`, `page_widgets.c`, `page_niri.c`, `page_displays.c`, `page_network.c`, `page_bluetooth.c`, `page_audio.c`, `page_notifications.c`, `page_theme.c`, `page_about.c`.
- **Shared Helpers**: Common UI builders (`create_settings_page_card`, `create_string_dropdown`, `create_metro_toggle_tile`, `create_simple_item_row`) live in [src/settings/settings_common.c](file:///home/mikolas/src/Kajo/src/settings/settings_common.c).

### B. Dynamic D-Bus Scanning & Auto-Refresh
- Scanner buttons (`[ 🔍 REFRESH WI-FI SCAN ]`, `[ 🔍 SCAN NEARBY BLUETOOTH DEVICES ]`) must trigger real D-Bus discovery (`RequestScan`, `StartDiscovery`).
- Scanner handlers **MUST** register a post-scan auto-refresh timeout (`g_timeout_add(1500, ...)` or similar) to automatically clear and re-populate UI list containers when scan results arrive.

### C. Declarative UI Architecture (Blueprint)
- All UI templates for popouts (`src/widgets/`) and settings pages (`src/settings/pages/`) use Blueprint Markup (`.blp`) compiled via `blueprint-compiler` to define static layouts, headers, and button bars, while using `gtk_widget_class_bind_template_child()` to append dynamic live D-Bus rows in C.

---

## 4. Design & Aesthetic Principles

- **Rich Modern Metro Aesthetic**: Dark mode glassmorphism, subtle card hover states, curated color palettes, accent tiles (`section-card-left`, `section-card-center`, `section-card-right`), and clean typography.
- **Micro-Interactions**: Smooth hover states, active toggle switches, and responsive layout spacing.
- **Clean Responsive Layouts**: Utilize GTK4 flex boxes (`GTK_ORIENTATION_VERTICAL` / `GTK_ORIENTATION_HORIZONTAL`) with explicit expansion policies (`gtk_widget_set_hexpand`, `gtk_widget_set_vexpand`).

---

## 5. Git Branching, Commit Discipline & Public PR Workflow

❌ **1. NO DIRECT COMMITS TO `master`**
- Direct commits or pushes to `master` are **strictly forbidden** (unless explicitly requested by the user). `master` represents public, stable releases.

🌿 **2. FEATURE & BUGFIX BRANCHES ONLY**
- All development must take place on dedicated local feature or bugfix branches (e.g., `feat/media-spectrum-analyzer`, `fix/codebase-audit`).

🔒 **3. NO PREMATURE / UNREQUESTED REMOTE PUSHES**
- **NEVER** push local feature or bugfix branches to remote (`origin`) unless explicitly requested by the user. Keep all interim development commits 100% local.

🔒 **4. NO INCOMPLETE COMMITS**
- **NEVER** commit work until the entire feature or bugfix on that branch is 100% complete, fully tested, and verified clean (`meson compile -C builddir` with 0 errors and 0 warnings).

🐙 **5. PULL REQUESTS VIA `gh` CLI**
- All code changes must be submitted via Pull Requests (`gh pr create`) and merged (`gh pr merge`) after thorough review.

---

## 6. Verification Checklist Before Commit

1. Run `meson compile -C builddir` $\rightarrow$ Must compile cleanly with 0 errors and 0 warnings.
2. Run test executable (e.g. `./builddir/kajo-settings --page network`) $\rightarrow$ Verify CSS styles load correctly without console errors.
3. Check git diff $\rightarrow$ Ensure formatting matches project style and no temporary scratch files are staged.

