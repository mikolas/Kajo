# Specification: `kajo-web` Cross-Device & Cross-Profile Sync (`docs/spec_kajo_web_sync.md`)

**Status:** Planned Specification — design-only; **not yet implemented**  
**Primary Clients:** `kajo-web` (`org.kajo.Web`, `src/web_cef/` + `src/web/`), `kajo-settings --page web` (`src/settings/pages/page_web.c`), and Windows Chrome companion extension (`extensions/kajo-sync-chrome/`)  
**Transport Backend:** Firebase Authentication (Email/Password REST) + Firebase Realtime Database (REST + `X-Firebase-ETag` / `If-Match` CAS)  
**Encryption:** Existing client-side E2EE (`aes-256-gcm-v1` via `libgcrypt` in `src/web/web_sync_merge.c` + matching WebCrypto in Chrome)  
**Related Specs:** [`docs/spec_kajo_web.md`](spec_kajo_web.md), [`docs/design_system.md`](design_system.md)

---

## 1. Objectives, Architecture & Explicit Non-Goals

This specification defines a zero-custom-backend, end-to-end encrypted browser sync system bridging **`kajo-web` across Linux hosts** and **Google Chrome on Windows** via Firebase Realtime Database.

### 1.1 Core Design Principles

1. **Both Profiles AND Devices Are First-Class Axes:**
   - Sync state is strictly partitioned **first by user account (`uid`)**, **second by sync profile (`sync_profile_slug`)**, and for open tabs **third by device (`device_id`)**.
   - Work bookmarks, history, and open tabs never bleed into Personal profiles.
   - Multiple Kajo profiles on the same machine—and multiple Chrome profiles on Windows—can independently enable or disable sync, bind to matching or distinct `sync_profile_slug`s, and use independent E2EE passphrases.
2. **Firefox-Style *"Tabs from Other Devices"* (No Live Tab Mirroring):**
   - Open tabs and tab groups are **never CRDT-merged across running browsers**. Two open browsers never fight over a shared tab strip.
   - Instead, each participating `(profile, device)` publishes its own **read-only device snapshot**. Other devices pull foreign device snapshots on sync and display them in an **"Other Devices"** panel (`kajo://sync` and the Chrome extension popup) so the user can open individual tabs or a whole window/group on demand.
3. **Predictable Scheduled + Manual Cadence (No Ambient Real-Time Polling):**
   - Sync runs **on browser startup** (`+5s` settle delay), **periodically once per hour** (`sync_interval_min`, default `60`, configurable), **on explicit user click** (`⟳ Sync Now`), and as a **bounded flush on graceful browser close** (`<= 3s` timeout).
   - No constant WebSocket/SSE stream or millisecond debounced tab pushing is required (leaving future optional opt-in *"Send Tab to Device"* as a separate follow-on if ever needed).
4. **Private / Incognito Isolation:**
   - Ephemeral Private windows (`kajo-web --private`) and Chrome Incognito windows **never** initialize `CefSyncEngine`, **never** push tabs/history, and **never** read or write remote sync state.
5. **Client-Side E2EE Zero-Knowledge Storage:**
   - Firebase handles authentication and storage ACLs (`auth.uid === $uid`), while **all synced payloads** (`bookmarks`, `history`, `kajo_settings`, and `devices/<device_id>`) are encrypted client-side with `aes-256-gcm-v1` (PBKDF2-HMAC-SHA256 + AES-256-GCM) before leaving the browser. Only lightweight structural routing metadata (`meta`) or the passphrase salt/verification check need live unencrypted/derived outside ciphertext (see §4).

### 1.2 Explicit Non-Goals (Out of Scope)

- **Direct Google Chrome Sync API:** Google permanently blocked third-party Chromium/CEF builds from Google Chrome Sync in March 2021; `kajo-web` will not attempt to spoof official Chrome keys.
- **No Passwords, Passkeys, or Cookies Syncing:** Active session cookies and saved credentials are **explicitly excluded** to keep blast radius narrow and avoid stale/broken auth tokens across OSes. Dedicated password managers (Bitwarden / KeePassXC / 1Password) cover Windows Chrome ↔ Linux credentials cleanly.
- **No Extensions / Binary Files Syncing:** Only structured JSON browser state is synced.

---

## 2. Existing Foundations in Repository vs. Missing Work

The following components **already exist** in the tree (verified in `src/web/` and `src/config/`) and must be reused rather than duplicated (`AGENTS.md §7`):

| Existing Symbol / File | Current State | Role in Sync Implementation |
| :--- | :--- | :--- |
| `KajoWebBookmark`, `web_sync_merge_bookmarks()`, `web_sync_bookmarks_{to,from}_json()` (`src/web/web_sync_merge.{h,c}`) | Implemented & tested (`web-model`) | Deterministic UUID + `updated_at` + `deleted` tombstone CRDT merge and `schema: 1` JSON codec. Reused directly. |
| `web_sync_encrypt_payload()`, `web_sync_decrypt_payload()` (`src/web/web_sync_merge.{h,c}`) | Implemented & tested (`web-model`) | `aes-256-gcm-v1` PBKDF2-HMAC-SHA256 (`100000` iter) + AES-256-GCM via `libgcrypt`, with `explicit_bzero()` scrubbing. |
| `KajoWebStorage` SQLite bookmarks (`web_storage_list_bookmarks(..., include_deleted)`, `web_storage_replace_bookmarks()`) (`src/web/web_storage.{h,c}`) | Implemented | SQLite table already stores `uuid`, `updated_at`, and `deleted` tombstones cleanly. |
| `KajoWebProfileInfo.sync_enabled`, `last_sync_epoch` (`src/web/web_model.{h,c}`) | Parsed/serialized in `profiles.toml` | Expand with `sync_profile_slug` and per-profile sync metadata. |
| `KajoWebCfg.sync_*` fields (`src/config/kajo_config_types.h`, `src/config/schemas/schema_web.c`) | Reserved in `web.toml` (`sync_oauth_*` unused) | Replace unused `sync_oauth_*` with Firebase project/auth keys while retaining `sync_bookmarks`, `sync_history`, `sync_tabs_and_groups`, `sync_settings`, `sync_e2ee_enabled`. |

**Not yet built:** Firebase auth & REST client (`CefSyncEngine`), `libsecret` keyring bridge, history/settings CRDT merge & tombstones, per-device open-tabs snapshot serialization, `kajo://sync` UI, `kajo-settings` Sync card, and `extensions/kajo-sync-chrome/`.

---

## 3. Firebase Realtime Database Topology & Authorization

### 3.1 Database Hierarchy (Per User → Profile → Device)

```text
users/
  <uid>/
    devices_registry/                      # Optional human lookup for all devices of this user
      <device-id>/
        name: "kajo-workstation"
        platform: "linux-kajo" | "win-chrome" | "linux-chrome"
        last_seen_ms: 1795600000000
    profiles/
      <sync-profile-slug>/                 # e.g. "default", "personal", "work" ([a-z0-9][a-z0-9_-]{0,31})
        meta/
          display_name: "WORK"             # optional non-sensitive label
          created_ms: 1795600000000
          kdf_check: { <e2ee-envelope-of-constant-canary> }  # optional wrong-passphrase pre-check (§4.2)
        bookmarks/                         # E2EE blob (or plaintext wrapper if E2EE off): merged CRDT
          <encrypted-envelope>
        history/                           # E2EE blob: bounded merged history + recent tombstones
          <encrypted-envelope>
        kajo_settings/                     # E2EE blob: Kajo-only portable web.toml + adblock allowlist + webapps metadata
          <encrypted-envelope>
        devices/
          <device-id>/                     # Single-writer owned by that specific browser instance
            <encrypted-envelope>           # Contains DeviceTabsSnapshot JSON
```

**Why this layout:**
- **Zero write contention on tabs:** `devices/<device-id>` is written **only** by the owning browser instance (`PUT` overwrite), so two awake devices never overwrite each other's tabs.
- **Clean profile separation:** `<sync-profile-slug>` maps a Kajo profile (or Windows Chrome profile) explicitly to a sync namespace. Two machines can map local profile directories cleanly (`"work"` ↔ Chrome `"Work"`) regardless of Chrome's internal `"Profile 2"` directory name.
- **Per-data-type isolation:** Downloading a bookmark change does not require downloading `kajo_settings` or rewriting another device's tab snapshot.

### 3.2 Firebase Console & Security Rules

One-time setup by the user in a personal Firebase project (Spark $0 free tier is sufficient):
1. Enable **Authentication → Sign-in method → Email/Password**.
2. Create the single user account (or disable public sign-up in Firebase Console after account creation).
3. Set **Realtime Database Rules** strictly enforcing per-UID isolation and rejecting malformed writes:

```json
{
  "rules": {
    "users": {
      "$uid": {
        ".read": "auth != null && auth.uid === $uid",
        ".write": "auth != null && auth.uid === $uid",
        "profiles": {
          "$slug": {
            ".validate": "$slug.matches(/^[a-z0-9][a-z0-9_-]{0,31}$/)"
          }
        }
      }
    }
  }
}
```

### 3.3 Authentication & Token Lifecycle (Pure HTTPS REST)

Neither `kajo-web` nor the Chrome extension pulls in a heavy Firebase client SDK; both talk directly to Google's documented REST endpoints:

1. **Interactive Sign-In (one-time per device/app install):**
   - `POST https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=<firebase_api_key>`
   - Body: `{ "email": "...", "password": "...", "returnSecureToken": true }`
   - Response returns `{ localId (uid), idToken, refreshToken, expiresIn }`.
   - **Credential hygiene (`AGENTS.md §4.E`):** On Linux (`kajo-web` / `kajo-settings`), copy email + password into `g_autofree` buffers, immediately clear the `GtkPasswordEntry` / DOM `<input type="password">` buffer, never write the password to disk/TOML/logs, and scrub temporary buffers with `explicit_bzero()`.
2. **Token Refresh (on every sync if `now >= id_token_expiry_us - 60s`):**
   - `POST https://securetoken.googleapis.com/v1/token?key=<firebase_api_key>`
   - Form/JSON body: `grant_type=refresh_token&refresh_token=<stored_refresh_token>`
   - `refreshToken` remains valid indefinitely until explicitly revoked or account password is changed; if `securetoken` returns `TOKEN_EXPIRED` / `USER_DISABLED` / `INVALID_REFRESH_TOKEN`, transition sync state to `ERROR_AUTH` and surface a non-blocking re-auth prompt without discarding local data.
3. **RTDB Read / Conditional Write (CAS):**
   - **Read:** `GET <rtdb_url>/users/<uid>/profiles/<slug>/<node>.json?auth=<idToken>` with header `X-Firebase-ETag: true`. Capture returned `ETag` header (or `"null_etag"` when body is JSON `null`).
   - **Merged-node Write (`bookmarks`, `history`, `kajo_settings`):** `PUT` with header `If-Match: <captured_etag>`.
     - If server responds `412 Precondition Failed`: re-fetch remote node + ETag, run deterministic CRDT merge against current local state, and retry **at most 2 times** (`CONFLICT_RETRY`). After 3 failed CAS attempts, abort cleanly into `ERROR_NETWORK` until next cycle.
   - **Single-writer Write (`devices/<device-id>`):** Plain `PUT <rtdb_url>/.../devices/<device-id>.json?auth=<idToken>` (no CAS contention possible since `device_id` is unique per install).
   - **Delete device snapshot ("Forget Device"):** `DELETE <rtdb_url>/.../devices/<target_device_id>.json?auth=<idToken>`.

### 3.4 Secret Storage

| Platform | Secrets Stored (`refresh_token`, `e2ee_passphrase` per `<sync-profile-slug>`) | Mechanism |
| :--- | :--- | :--- |
| **Linux (`kajo-web` & `kajo-settings`)** | Keyring schema `org.kajo.Web.Sync` with attributes `service`, `uid`, `sync_profile_slug` | **`libsecret-1`** (`secret_password_store`, `secret_password_lookup`, `secret_password_clear`) over D-Bus Secret Service. If no Secret Service daemon is unlocked/present, surface an explicit status badge (`Keyring locked / unavailable — passphrase held in memory only for session`) rather than silently saving plaintext secrets to TOML (`AGENTS.md §8.1`). |
| **Windows Chrome (`kajo-sync-chrome`)** | `refreshToken`, `passphrase`, `apiKey`, `rtdbUrl`, `uid`, `deviceId` | `chrome.storage.local` (profile-isolated in Chrome OS profile directory, protected by OS user account ACLs). Optional *"Lock E2EE passphrase in memory only (prompt once per browser start via `chrome.storage.session`)"* setting. |

---

## 4. Client-Side E2EE Wire Format & Cross-Runtime Interoperability

### 4.1 Wire Envelope (`aes-256-gcm-v1`)

All encrypted nodes share the exact JSON envelope already implemented in [`src/web/web_sync_merge.c`](file:///home/mikolas/src/Kajo/src/web/web_sync_merge.c):

```json
{
  "e2ee": "aes-256-gcm-v1",
  "iter": 100000,
  "salt": "<base64(16 random bytes)>",
  "iv":   "<base64(12 nonce bytes)>",
  "tag":  "<base64(16 AES-GCM auth tag bytes)>",
  "ct":   "<base64(ciphertext bytes of UTF-8 JSON plaintext)>"
}
```

- **`libgcrypt` (C17):** Uses `GCRY_KDF_PBKDF2` + `GCRY_MD_SHA256`, `salt` 16 B, `iv` 12 B, AES-256-GCM key 32 B, detached 16 B authentication `tag`. Validated bounds on decrypt: `10000 <= iter <= 2000000`, `salt_len == 16`, `iv_len == 12`, `tag_len == 16`.
- **WebCrypto (`SubtleCrypto` in Windows Chrome MV3):**
  - `crypto.subtle.importKey('raw', utf8(passphrase), 'PBKDF2', false, ['deriveKey'])`
  - `deriveKey({ name: 'PBKDF2', salt, iterations: iter, hash: 'SHA-256' }, baseKey, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt'])`
  - **CRITICAL WebCrypto ↔ `libgcrypt` interoperability detail:** WebCrypto's `AES-GCM` **appends** the 16-byte authentication tag onto the end of the encrypted `ArrayBuffer` (`ct || tag`). When **encrypting** in Chrome, split `buf.slice(0, buf.byteLength - 16)` → `ct` and `buf.slice(buf.byteLength - 16)` → `tag`. When **decrypting** in Chrome, concatenate `ct || tag` before passing to `crypto.subtle.decrypt({ name: 'AES-GCM', iv, tagLength: 128 }, key, combined)`.
  - Both runtimes must pass a shared **fixed-vector golden test** (a frozen `aes-256-gcm-v1` envelope encrypted by `web_sync_encrypt_payload()` decrypted in JS/Node WebCrypto, and vice versa).

### 4.2 Passphrase Verification & Keyring-Mismatch Safety

- When `sync_e2ee_enabled = true`:
  1. Never silently fall back to plaintext if decryption fails.
  2. Store an encrypted canary at `profiles/<slug>/meta/kdf_check`: ciphertext of `"{\"kajo_sync_canary\":1}"`.
  3. On first pull (or passphrase change), verify decryption of `kdf_check` or existing `bookmarks`/`history` node **before any remote write**. If decryption fails (`G_IO_ERROR_PERMISSION_DENIED` / `OperationError`), **halt the sync cycle immediately**, transition to `ERROR_PASSPHRASE`, and never overwrite remote state.
  4. When changing E2EE passphrase intentionally, prompt for confirmation + perform an atomic re-encrypt push of all enabled nodes in that profile.

### 4.3 Plaintext Envelope Option (`sync_e2ee_enabled = false`)

If the user explicitly disables E2EE (`sync_e2ee_enabled = false`), payloads are wrapped with `"e2ee": "none"` (`{ "e2ee": "none", "payload": { ... } }`) so both code paths remain explicit and versioned. Remote E2EE blobs encountered while `sync_e2ee_enabled == false` do **not** get overwritten — sync enters `ERROR_PASSPHRASE` instructing the user to enable E2EE and enter the passphrase.

---

## 5. Payload Schemas & Deterministic Merge Rules

### 5.1 Bookmarks (`profiles/<slug>/bookmarks`, `schema: 1`)

Reuses the exact JSON format already emitted by `web_sync_bookmarks_to_json()` (`src/web/web_sync_merge.c`):

```json
{
  "schema": 1,
  "bookmarks": [
    {
      "uuid": "550e8400-e29b-41d4-a716-446655440000",
      "title": "Kajo Architecture",
      "uri": "https://example.org/kajo",
      "folder": "Reading/Linux",
      "updated_at": 1795600123456,
      "deleted": false
    }
  ]
}
```

- **Merge rule (`web_sync_merge_bookmarks`):** Union by `uuid`; higher `updated_at` (UTC ms) wins; tie-breaker favors `deleted: true`.
- **Tombstone GC:** Tombstones (`deleted: true`) older than **30 days** (`> 2592000000 ms`) are pruned during periodic compaction once every participating device has synced past the cutoff (or safely after 30 days).
- **Windows Chrome mapping:** Chrome's `chrome.bookmarks` uses ephemeral numeric node IDs (`"42"`) per machine and a hierarchy (`Bookmarks Bar`, `Other Bookmarks`, subfolders). The Chrome extension maintains a persistent local `chromeId <-> syncUuid` map + slash-joined folder path (`"Bookmarks Bar/Dev"`) normalized to `folder` in `kajo-web`, with a root-folder mapping rule (default: Chrome `"Bookmarks Bar/..."` ↔ Kajo `"Bookmarks Bar/..."` or flat `""` configurable in extension options). Only HTTP/HTTPS/file URLs supported on both sides are mirrored; Chrome `javascript:` bookmarklets are preserved untouched when merging if marked non-navigable.

### 5.2 History (`profiles/<slug>/history`, `schema: 1`)

```json
{
  "schema": 1,
  "max_entries": 2500,
  "entries": [
    {
      "uri": "https://example.org/docs",
      "title": "Example Docs",
      "visit_count": 7,
      "last_visit_ms": 1795600456789,
      "deleted_at_ms": 0
    }
  ]
}
```

- **URL filtering (both `kajo-web` and Chrome):** Exclude internal/ephemeral URIs (`kajo://*`, `kajo-chrome://*`, `chrome://*`, `chrome-extension://*`, `about:*`, `blob:*`, `data:*`, `view-source:*`).
- **Merge rule (`web_sync_merge_history()` in `src/web/web_sync_merge.c`):**
  - Keyed by canonical `uri`.
  - Effective modification time is `max(last_visit_ms, deleted_at_ms)`.
  - If `deleted_at_ms >= last_visit_ms` and `deleted_at_ms > 0`, deletion wins (tombstone stays for 30 days so deletes propagate cleanly across machines).
  - If a newer visit arrives (`last_visit_ms > deleted_at_ms`), the URI is resurrected with `visit_count = max(local.visit_count, remote.visit_count)` (and non-empty `title` from the newer `last_visit_ms`).
  - **Bounded blob size:** After merging, sort live entries descending by `last_visit_ms` and cap at **`2500` entries** (+ active tombstones < 30 days) so RTDB payload & PBKDF2/AES-GCM time stay well under `< 1 MB` / `< 30 ms`. Local full SQLite FTS5 history remains unbounded on disk.
- **Chrome history mapping:** Extension queries `chrome.history.search({ text: '', maxResults: 2500, startTime })` and merges deletions reported via `chrome.history.onVisitRemoved` (`allHistory` deletes or specific URLs).

### 5.3 Device Open Tabs Snapshot (`profiles/<slug>/devices/<device-id>`, `schema: 1`)

```json
{
  "schema": 1,
  "device_id": "a9c3e1b4-...",
  "device_name": "thinkpad-arch",
  "platform": "linux-kajo",
  "updated_at_ms": 1795600900000,
  "windows": [
    {
      "window_id": "main",
      "tabs": [
        {
          "title": "Wayland Protocols",
          "url": "https://wayland.app/",
          "group": "Research",
          "pinned": true,
          "active": true,
          "last_active_ms": 1795600899000
        }
      ]
    }
  ]
}
```

- **Single-writer ownership:** Each browser installation generates a random persistent `device_id` UUID (e.g. saved in `~/.local/share/kajo/web/profiles/<profile>/sync_device.json` on Linux; `chrome.storage.local` on Windows Chrome), editable `device_name` (defaults to OS hostname / `"Windows Chrome"`), and `platform` (`"linux-kajo"` / `"win-chrome"` / `"linux-chrome"`).
- **Tab filtering:** Strip internal URIs (`kajo://newtab`, `kajo://*`, `chrome://newtab`, `about:blank`, etc.). Include group name (Chrome `tabGroups.get(tab.groupId).title` ↔ Kajo `t->group`), `pinned`, and `active`. Include suspended tabs in `kajo-web` (`KajoWebTabMeta`), excluding ephemeral popup windows unless part of the main tab strip.
- **No overwrite / no merge:** Foreign `devices/<other-device-id>` blobs are treated as read-only caches stored locally in memory/disk (`sync_foreign_devices.json` cache) for display in `kajo://sync` and the Chrome extension popup.

### 5.4 Kajo Settings & Web Apps Metadata (`profiles/<slug>/kajo_settings`, `schema: 1`, Kajo-only)

Synced **only between `kajo-web` instances** (ignored by the Windows Chrome extension):
- **Portable `web.toml` preferences** (`rail_mode`, `rail_side`, `rail_width_px`, `top_bar_mode`, `show_tab_domains`, `new_tab_page`, `new_tab_custom_url`, `search_engine`, `search_custom_url`, `default_zoom_pct`, `default_font_size_px`, `mono_font_size_px`, `min_font_size_px`, `suspend_idle_min`, `color_scheme`, `cookie_policy`, `adblock_enabled`, `adblock_auto_sync`, `https_only`, `autoplay_policy`) synced with per-field timestamps or snapshot LWW timestamp (`updated_at_ms`).
- **Explicitly excluded from sync (machine-local):** `download_dir` (paths differ across hosts), `adblock_custom_rules_path`, `hardware_acceleration` (GPU driver dependent), `firebase_api_key` / keyring refs.
- **Adblock host allowlist (`CefAdblockEngine`):** Unordered set with tombstones so pausing adblock on a site carries between Linux devices.
- **Web App catalog metadata (optional):** `start_url`, `scope`, `name`, `theme_color`, `icon_url` (desktop `.desktop` regeneration triggered locally on install if requested, never blindly creating XDG files in the background without user opt-in).

---

## 6. Sync Lifecycle, State Machine & Resource Discipline

### 6.1 Sync Triggers & Off-Main-Thread Execution (`AGENTS.md §4.D`, `§7`)

- **Triggers (per enabled profile when `!cfg_.private_mode`):**
  1. **Startup:** `5s` after window creation once network context and keyring lookup complete (never blocks window paint).
  2. **Periodic:** One-hour timer (`sync_interval_min * 60s`, default `60m`, GLib/Chromium UI task scheduling `CefURLRequest` + `GTask` for PBKDF2). Zero sub-minute timers or idle polling (`< 0.05%` idle CPU).
  3. **Manual:** Explicit click on `⟳ Sync Now` in `kajo://sync`, `topbar`/`profiles` shortcut, `kajo-settings`, or Chrome extension popup.
  4. **Graceful Close:** Triggered inside `CanClose()` / `OnWindowClosing()` with a **bounded `3s` flush budget** (push dirty local device snapshot + changed bookmarks/history without stalling desktop logout/shutdown indefinitely; force-closes safely if timeout expires or network is unreachable).
- **Thread safety:**
  - Keyring lookup (`libsecret`) and `libgcrypt` PBKDF2-HMAC-SHA256 (`100000` iter ~10–30 ms per blob) MUST run on a worker pool (`GTask` / `TID_USER_VISIBLE`), never blocking the GTK or CEF `TID_UI` thread.
  - HTTP requests in `kajo-web` run natively through CEF's `CefURLRequest` (honoring system proxy/TLS roots without pulling in `libcurl`).
  - All async callbacks pass `GWeakRef` / `CefRefPtr` + alive/generation cancellation tokens (`closing_`, `sync_gen_`), checking cancellation before updating UI or SQLite (`AGENTS.md §7`).

### 6.2 State Machine (`AGENTS.md §8.3`)

Every state transition updates both internal telemetry and the UI badge immediately:

```text
  [DISABLED]  <── toggle off / signed out ──  [IDLE]  <─────────────────────┐
      │                                          │                          │
      │ enable + valid config                    │ timer / startup / manual │
      ▼                                          ▼                          │
  [AUTH_REFRESH] ── token fail (revoked) ──> [ERROR_AUTH]                   │
      │                                          ▲                          │
      │ valid idToken                            │ (clear error on re-auth) │
      ▼                                          │                          │
  [PULLING] ── HTTP >=500 / timeout ───────> [ERROR_NETWORK] ───────────────┘
      │                                          ▲   (auto-clears next cycle)
      │ ETag + ciphertext fetched                │
      ▼                                          │
  [DECRYPT_AND_MERGE] ── bad key/tag ──────> [ERROR_PASSPHRASE] (hard stop: never push)
      │
      ▼
  [PUSHING] ── 412 CAS mismatch (< 3x) ────> [CONFLICT_RETRY] ──> [PULLING]
      │
      │ 200 OK (all dirty nodes committed)
      ▼
    [IDLE] (update last_sync_epoch + last_error = "")
```

- **Coalescing rule:** If a manual sync is clicked while `AUTH_REFRESH`/`PULLING`/`MERGING`/`PUSHING` is active, coalesce cleanly (ignore duplicate trigger or set `resync_queued = true`, never running two concurrent sync cycles on the same profile).
- **Profile switch / close during active sync (`CANCELLING`):** Cancel active `CefURLRequest` handles and `GCancellable`s, wait/drop callbacks safely without mutating torn-down profile state.

---

## 7. Configuration Schema Additions (`web.toml` & `profiles.toml`)

All existing sync keys in [`src/config/schemas/schema_web.c`](file:///home/mikolas/src/Kajo/src/config/schemas/schema_web.c) must be updated cleanly when implementation starts:

### 7.1 `web.toml` (`KajoWebCfg`)

| Key | Type & Default | Purpose |
| :--- | :--- | :--- |
| `sync_firebase_api_key` | `string = ""` | Firebase Web API key from Firebase Console. |
| `sync_firebase_rtdb_url` | `string = ""` | Normalized `https://<project>.firebaseio.com` (validated `https://` only; trailing `/` stripped). |
| `sync_device_name` | `string = ""` | Display name for this machine (defaults to `g_get_host_name()` when empty). |
| `sync_interval_min` | `int = 60` (`15..1440`) | Background sync cadence in minutes (`60` default). |
| `sync_on_startup` | `bool = true` | Sync 5s after profile startup. |
| `sync_on_close` | `bool = true` | Perform bounded device-tabs + pending bookmark/history flush on exit. |
| `sync_bookmarks` | `bool = true` | *(Already in `schema_web.c`)* Merge bookmarks (`profiles/<slug>/bookmarks`). |
| `sync_history` | `bool = true` | *(Already in `schema_web.c`)* Merge history (`profiles/<slug>/history`). |
| `sync_tabs_and_groups` | `bool = true` | *(Already in `schema_web.c`)* Publish this device's tabs & show foreign device tabs. |
| `sync_settings` | `bool = true` | *(Already in `schema_web.c`)* Sync portable `kajo-web` prefs + allowlist. |
| `sync_e2ee_enabled` | `bool = true` | *(Already in `schema_web.c`; flip default to `true` when shipping sync)* Encrypt all synced nodes with AES-256-GCM. |
| *Deprecate/Remove* | `sync_oauth_client_id`, `sync_oauth_client_secret` | Unused Google Drive OAuth placeholders removed cleanly from `schema_web.c` (or ignored gracefully on load via `TOML` unknown-key compatibility). |

### 7.2 `profiles.toml` (`KajoWebProfileInfo`)

Each `[[profiles]]` entry already has `sync_enabled` and `last_sync_epoch` in [`src/web/web_model.h`](file:///home/mikolas/src/Kajo/src/web/web_model.h); add:
- `sync_profile_slug` (`string`, sanitized `[a-z0-9][a-z0-9_-]{0,31}`, defaults to profile `id`).
- `sync_device_id` (`string`, generated UUID v4 on first sync enable for that profile, persisted alongside profile data).

---

## 8. User Interface Specification (Aero + Metro + Chrome Companion)

> **Implementation note (`AGENTS.md §3.5`, `§8.1`):** Final UI styling and widget polish should align with the cross-suite UI polish pass when built. Every new control must reflect live backend state (`ERROR_*`, `SYNCING`, `LAST SYNC`), with **zero** unbacked toggles or auto-opening error popups.

### 8.1 `kajo-web`: `kajo://sync` Page & Palette Commands

- **Topbar / Command Palette access:**
  - `kajo://sync` added to internal `kajo://` navigation bar alongside `NEW TAB | BOOKMARKS | HISTORY | ADBLOCK | PERMISSIONS | APPS | DOWNLOADS`.
  - Command Palette (`Ctrl+K`) commands: `Sync Now`, `Open Sync & Other Devices (kajo://sync)`.
- **Minimal Topbar Status Indicator:**
  - Small subtle indicator near the profile chip (`⟳` spinning while syncing, amber `⚠` dot only when `ERROR_AUTH` / `ERROR_PASSPHRASE` requires attention). Tooltip shows exact profile slug + `Last synced 14m ago`. Never steals focus or pops up modals.
- **`kajo://sync` Layout (matching existing `BuildKajoShellPage` Aero + Metro tokens):**
  1. **Account & Profile Header Card:** Active Kajo profile badge + bound `<sync-profile-slug>`, current device name, `LAST SYNC: HH:MM` (or error banner with direct `Re-authenticate` / `Enter Passphrase` button), `[ ⟳ SYNC NOW ]`, `[ OPEN SYNC SETTINGS ]` (`kajo-settings --page web`).
  2. **Tabs from Other Devices (primary section):**
     - Grouped by remote device (`DESKTOP · WIN-CHROME`, `LAPTOP · LINUX-KAJO`), showing device `last_updated` relative age (`"12m ago"`).
     - Tabs preserve group badges (`[RESEARCH]`), pin indicators, title, and truncated origin.
     - Actions per row: **Click** → opens tab in current window (`foreground = true`); per window/group header: `[ OPEN ALL (N) ]`; per device header: `[ FORGET DEVICE ]` (with inline 2-step confirmation to `DELETE` remote snapshot).
     - Search filter input (`Filter tabs from other devices...`).

### 8.2 `kajo-settings --page web`: Sync Card (`page_web.c`)

Follows [`docs/design_system.md`](design_system.md) §4.3 controls and explicit **SAVE & APPLY** + **REVERT** semantics:
- **Account Connection Group:** Firebase RTDB URL entry, Web API Key entry, Email + `GtkPasswordEntry` sign-in bar, live Account Status row (`Signed in as user@example.com` or `Signed out`), `[ SIGN OUT ]`.
- **This Machine & Active Profile Group:** Device Name entry, Profile Sync Mapping table/selector (`sync_enabled` toggle card + editable `sync_profile_slug` per Kajo profile).
- **E2EE Passphrase Row:** `GtkPasswordEntry` (`show-peek-icon`, scrubbed with `explicit_bzero()` immediately after `libsecret` commit) + Passphrase verification status (`Verified via kdf_check` / `Not set`).
- **Sync Scope & Cadence Switches:** `Sync Bookmarks`, `Sync History`, `Sync Tabs from Other Devices`, `Sync Kajo Settings`, `Sync on Startup`, `Sync on Exit`, and `Sync Interval` (`Segmented` `[ 30m ] [ 60m ] [ 2h ] [ 6h ]`).

### 8.3 Windows Chrome Companion Extension (`extensions/kajo-sync-chrome/`)

Minimal Manifest V3 extension (`permissions`: `bookmarks`, `history`, `tabs`, `tabGroups`, `storage`, `alarms`, `host_permissions`: `https://*.firebaseio.com/*`, `https://identitytoolkit.googleapis.com/*`, `https://securetoken.googleapis.com/*`):
- **Popup (`popup.html`):** Styled in matching Aero + Metro dark `#141414` + `0px` radius + configurable `@metro-accent`. Shows:
  - Bound sync profile slug + `[ ⟳ Sync Now ]` + last-sync timestamp / error state.
  - **"Tabs from Other Devices"** list (all other `devices/*` snapshots for that `<sync-profile-slug>`): click opens tab in current Chrome window; `[ Open All ]` restores group/window.
- **Options Page (`options.html`):** Firebase RTDB URL, Web API Key, Email/Password sign-in, `Sync Profile Slug` (so Chrome Work profile → `"work"`, Chrome Personal → `"personal"`), `Device Name`, E2EE Passphrase, interval (`chrome.alarms`, `periodInMinutes: 60`), and manual bookmark/history/tabs toggles.
- **Background Service Worker (`sw.js`):**
  - `chrome.runtime.onStartup` alarm + hourly `chrome.alarms.create('kajo-sync', { periodInMinutes })`.
  - Runs WebCrypto `aes-256-gcm-v1` encrypt/decrypt + Firebase REST fetch (`X-Firebase-ETag` / `If-Match`) with identical merge semantics to `web_sync_merge.c`.

---

## 9. Planned Modules & Phased Rollout

### 9.1 C/C++ Modules (`src/web/`, `src/web_cef/`, `src/settings/`)

- `src/web/web_sync_merge.{h,c}` — Add `KajoWebHistorySyncEntry`, `web_sync_merge_history()`, `web_sync_history_{to,from}_json()`, 30-day tombstone compaction helper, and `KajoWebSettingsSyncPayload` JSON codecs.
- `src/web/web_storage.{h,c}` — Add `history_tombstones` SQLite table + `web_storage_record_history_delete()` / `web_storage_list_history_for_sync()` / `web_storage_apply_merged_history()` (preserving existing `history_fts` search index).
- `src/web_cef/cef_sync.{h,cc}` — `CefSyncEngine` state machine, Firebase Auth/RTDB `CefURLRequest` client, `ETag` CAS retry loop, off-UI-thread `GTask` crypto, `DeviceTabsSnapshot` collector/cache.
- `src/web_cef/cef_keyring.{h,cc}` — Thin `libsecret-1` wrapper cleanly isolated for optional mock injection in unit tests.
- `src/web_cef/cef_chrome_ui.{h,cc}` — Add `kajo://sync` page + topbar status telemetry fields in `ChromeStateSnapshot`.
- `src/web_cef/cef_window.{h,cc}` — Bind per-profile `CefSyncEngine`, trigger startup/hourly/`CanClose()` flush (`<= 3s`), handle `kajo://sync` queries (`sync_now`, `open_remote_tab`, `open_remote_window`, `forget_device`).
- `src/settings/pages/page_web.c` & `src/config/` — Update `schema_web.c`, `KajoWebCfg`, and Sync settings UI card.
- `extensions/kajo-sync-chrome/` — Unpacked MV3 extension (`manifest.json`, `crypto.js`, `firebase_rest.js`, `merge.js`, `sw.js`, `popup.{html,js,css}`, `options.{html,js,css}`).

### 9.2 Verification Plan (When Implemented)

1. **Unit Tests (`web-model`, `web-cef-core`):**
   - Bookmarks & history CRDT merge (conflicts, deletes vs. newer visits, 30-day tombstone expiry, 2500-entry history cap).
   - E2EE golden interoperability test: `aes-256-gcm-v1` test vector decrypted identically by `libgcrypt` (`web_sync_decrypt_payload`) and Node/Chromium WebCrypto `SubtleCrypto`.
   - Mock local HTTP Firebase RTDB server testing `AUTH_REFRESH`, `412 Precondition Failed` CAS retry loop, wrong-passphrase `ERROR_PASSPHRASE` zero-write guarantee, `ERROR_AUTH` on revoked token, and close-time cancellation under **ASan + UBSan (`scripts/asan-test.sh`)**.
2. **Visual Verification (`AGENTS.md §1.7`):**
   - Capture `kajo-settings --page web` and `kajo-web kajo://sync` via `scripts/ui-shot.sh` and inspect spacing, `0px` corners, badge contrast, and `docs/ui-gallery.html` consistency.
3. **Cross-Platform Manual Test (Linux `kajo-web` ↔ Windows Chrome):**
   - Load `extensions/kajo-sync-chrome/` unpacked on Windows Chrome across 2 profiles (`"personal"`, `"work"`).
   - Verify bookmark add/rename/delete, history visit/delete, profile isolation, wrong-passphrase rejection, and *"Tabs from Other Devices"* roundtrip across hourly and manual sync.
