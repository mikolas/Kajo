#!/usr/bin/env python3
"""One-shot importer for copying tabs from Chromium/Thorium profiles into kajo-web.

Supports:
  - Listing Thorium/Chromium profile directories and display names (--list-profiles)
  - Parsing Chromium SNSS session logs (Sessions/Session_*) preserving tab order,
    pinned state, and tab groups
  - Fallback extraction from a Chromium Bookmarks JSON folder (--from-bookmarks-folder)
  - Guarding against overwriting an active kajo-web profile via its runtime IPC socket
  - Backing up existing session.json -> session.json.bak and atomic replacement
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import socket
import sqlite3
import struct
import sys
import time
from typing import Any
from urllib.parse import quote


CMD_SET_TAB_WINDOW = 0
CMD_SET_TAB_INDEX_IN_WINDOW = 2
CMD_UPDATE_TAB_NAVIGATION = 6
CMD_SET_SELECTED_NAVIGATION_INDEX = 7
CMD_SET_SELECTED_TAB_IN_INDEX = 8
CMD_SET_WINDOW_TYPE = 9
CMD_SET_PINNED_STATE = 12
CMD_TAB_CLOSED = 16
CMD_WINDOW_CLOSED = 17
CMD_SET_ACTIVE_WINDOW = 20
CMD_SET_TAB_GROUP = 25
CMD_SET_TAB_GROUP_METADATA = 26
CMD_SET_TAB_GROUP_METADATA2 = 27

INTERNAL_PREFIXES = (
    "chrome://",
    "chrome-extension://",
    "devtools://",
    "chrome-native://",
    "view-source:",
)


class PickleReader:
    """Minimal reader for Chromium base::Pickle payloads inside SNSS records."""

    def __init__(self, payload: bytes) -> None:
        if len(payload) < 4:
            raise ValueError("Truncated Pickle header")
        (declared_size,) = struct.unpack_from("<I", payload, 0)
        end = min(len(payload), 4 + declared_size)
        self._buf = payload[4:end]
        self._pos = 0

    def remaining(self) -> int:
        return len(self._buf) - self._pos

    def _align4(self) -> None:
        rem = self._pos & 3
        if rem:
            self._pos = min(len(self._buf), self._pos + (4 - rem))

    def read_int32(self) -> int:
        if self.remaining() < 4:
            raise ValueError("Truncated Pickle int32")
        (val,) = struct.unpack_from("<i", self._buf, self._pos)
        self._pos += 4
        return val

    def read_uint32(self) -> int:
        if self.remaining() < 4:
            raise ValueError("Truncated Pickle uint32")
        (val,) = struct.unpack_from("<I", self._buf, self._pos)
        self._pos += 4
        return val

    def read_uint64(self) -> int:
        if self.remaining() < 8:
            raise ValueError("Truncated Pickle uint64")
        (val,) = struct.unpack_from("<Q", self._buf, self._pos)
        self._pos += 8
        return val

    def read_bool(self) -> bool:
        return self.read_int32() != 0

    def read_string(self) -> str:
        byte_len = self.read_int32()
        if byte_len < 0 or byte_len > self.remaining():
            raise ValueError(f"Invalid Pickle string length: {byte_len}")
        raw = self._buf[self._pos : self._pos + byte_len]
        self._pos += byte_len
        self._align4()
        return raw.decode("utf-8", errors="replace")

    def read_string16(self) -> str:
        char_len = self.read_int32()
        byte_len = char_len * 2
        if char_len < 0 or byte_len > self.remaining():
            raise ValueError(f"Invalid Pickle string16 length: {char_len}")
        raw = self._buf[self._pos : self._pos + byte_len]
        self._pos += byte_len
        self._align4()
        return raw.decode("utf-16-le", errors="replace")


def sanitize_profile_id(raw: str | None) -> str:
    """Match web_profile_sanitize_id() rules in src/web/web_model.c."""
    if not raw:
        return "default"
    out: list[str] = []
    for ch in raw:
        if len(out) >= 32:
            break
        if ch.isascii() and ch.isalnum():
            out.append(ch.lower())
        elif ch in (" ", "-", "_") and out and out[-1] != "-":
            out.append("-")
    while out and out[-1] == "-":
        out.pop()
    return "".join(out) or "default"


def xdg_data_home() -> Path:
    val = os.environ.get("XDG_DATA_HOME", "").strip()
    return Path(val) if val else (Path.home() / ".local" / "share")


def xdg_runtime_dir() -> Path | None:
    val = os.environ.get("XDG_RUNTIME_DIR", "").strip()
    return Path(val) if val else None


def kajo_session_path(profile_id: str) -> Path:
    return xdg_data_home() / "kajo" / "web-cef" / profile_id / "session.json"


def kajo_profiles_toml_path() -> Path:
    return xdg_data_home() / "kajo" / "web" / "profiles.toml"


def kajo_ipc_socket_path(profile_id: str) -> Path:
    safe = re.sub(r"[^a-z0-9_-]", "-", (profile_id or "default").lower())
    rdir = xdg_runtime_dir()
    if rdir is not None:
        cand = rdir / f"kajo-web-{safe}.sock"
        if len(str(cand)) < 107:
            return cand
    return Path(f"/tmp/kajo-web-{os.getuid()}-{safe}.sock")


def is_kajo_profile_running(profile_id: str, timeout_s: float = 0.25) -> bool:
    sock_path = kajo_ipc_socket_path(profile_id)
    if not sock_path.exists():
        return False
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout_s)
    try:
        s.connect(str(sock_path))
        s.sendall(b'{"ping":true}\n')
        ack = s.recv(16)
        return ack.startswith(b"OK")
    except OSError:
        return False
    finally:
        s.close()


def profile_registered_in_toml(profile_id: str) -> bool:
    if profile_id == "default":
        return True
    p = kajo_profiles_toml_path()
    if not p.is_file():
        return False
    try:
        text = p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return bool(re.search(rf'(?m)^\s*id\s*=\s*"{re.escape(profile_id)}"\s*$', text))


def find_latest_snss_file(profile_dir: Path) -> Path:
    sessions_dir = profile_dir / "Sessions"
    candidates: list[Path] = []
    if sessions_dir.is_dir():
        candidates.extend(
            p for p in sessions_dir.iterdir()
            if p.is_file() and p.name.startswith("Session_")
        )
    # Legacy Chromium layout fallback
    for legacy_name in ("Current Session", "Last Session"):
        lp = profile_dir / legacy_name
        if lp.is_file():
            candidates.append(lp)

    if not candidates:
        raise FileNotFoundError(
            f"No SNSS Session_* files found in {sessions_dir} or {profile_dir}"
        )

    def sort_key(p: Path) -> tuple[float, int, str]:
        st = p.stat()
        m = re.search(r"(\d+)$", p.name)
        suffix_num = int(m.group(1)) if m else 0
        return (st.st_mtime, suffix_num, p.name)

    return max(candidates, key=sort_key)


def _should_skip_url(url: str, include_internal: bool) -> bool:
    u = url.strip()
    if not u:
        return True
    if include_internal:
        return False
    low = u.lower()
    if low == "about:blank":
        return True
    return any(low.startswith(pref) for pref in INTERNAL_PREFIXES)


def load_chromium_favicon_map(profile_dir: Path) -> dict[str, str]:
    """Read page_url -> icon_url mappings from Chromium's Favicons SQLite DB."""
    fav_db = profile_dir / "Favicons"
    if not fav_db.is_file():
        return {}
    uri = f"file:{quote(str(fav_db), safe='/')}?mode=ro&immutable=1"
    conn: sqlite3.Connection | None = None
    out: dict[str, str] = {}
    try:
        conn = sqlite3.connect(uri, uri=True, timeout=0.5)
        cur = conn.execute(
            "SELECT m.page_url, f.url "
            "FROM icon_mapping m "
            "JOIN favicons f ON f.id = m.icon_id"
        )
        for page_url, icon_url in cur:
            if not isinstance(page_url, str) or not isinstance(icon_url, str):
                continue
            cleaned = icon_url.strip()
            low = cleaned.lower()
            if not (low.startswith("http://") or low.startswith("https://")):
                continue
            out.setdefault(page_url, cleaned)
    except sqlite3.Error:
        return {}
    finally:
        if conn is not None:
            conn.close()
    return out


def parse_snss_session(
    session_file: Path,
    *,
    include_internal: bool = False,
    window_groups: bool = False,
    favicon_map: dict[str, str] | None = None,
) -> dict[str, Any]:
    if favicon_map is None:
        inferred_dir = session_file.parent.parent if session_file.parent.name == "Sessions" else session_file.parent
        favicon_map = load_chromium_favicon_map(inferred_dir)
    data = session_file.read_bytes()
    if len(data) < 8 or data[:4] != b"SNSS":
        raise ValueError(f"{session_file}: not a valid SNSS session file")
    (version,) = struct.unpack_from("<I", data, 4)
    if version not in (1, 3):
        raise ValueError(f"{session_file}: unsupported SNSS version {version}")

    tab_window: dict[int, int] = {}
    tab_index: dict[int, int] = {}
    tab_navs: dict[int, dict[int, tuple[str, str]]] = {}
    tab_selected_nav: dict[int, int] = {}
    tab_pinned: dict[int, bool] = {}
    tab_group_tok: dict[int, tuple[int, int]] = {}
    closed_tabs: set[int] = set()

    window_selected_tab: dict[int, int] = {}
    window_types: dict[int, int] = {}
    closed_windows: set[int] = set()
    active_window_id: int | None = None
    group_titles: dict[tuple[int, int], str] = {}

    pos = 8
    n = len(data)
    while pos + 2 <= n:
        (rec_size,) = struct.unpack_from("<H", data, pos)
        if rec_size == 0:
            break
        rec_end = pos + 2 + rec_size
        if rec_end > n or rec_size < 1:
            break
        cmd_id = data[pos + 2]
        payload = data[pos + 3 : rec_end]
        pos = rec_end

        if cmd_id == CMD_SET_TAB_WINDOW and len(payload) >= 8:
            win_id, tid = struct.unpack_from("<ii", payload, 0)
            tab_window[tid] = win_id
            closed_tabs.discard(tid)
            closed_windows.discard(win_id)
        elif cmd_id == CMD_SET_TAB_INDEX_IN_WINDOW and len(payload) >= 8:
            tid, idx = struct.unpack_from("<ii", payload, 0)
            tab_index[tid] = idx
        elif cmd_id == CMD_UPDATE_TAB_NAVIGATION:
            try:
                pr = PickleReader(payload)
                tid = pr.read_int32()
                nav_idx = pr.read_int32()
                url = pr.read_string()
                title = pr.read_string16()
            except ValueError:
                continue
            tab_navs.setdefault(tid, {})[nav_idx] = (url, title)
        elif cmd_id == CMD_SET_SELECTED_NAVIGATION_INDEX and len(payload) >= 8:
            tid, nav_idx = struct.unpack_from("<ii", payload, 0)
            tab_selected_nav[tid] = nav_idx
        elif cmd_id == CMD_SET_SELECTED_TAB_IN_INDEX and len(payload) >= 8:
            win_id, sel_idx = struct.unpack_from("<ii", payload, 0)
            window_selected_tab[win_id] = sel_idx
        elif cmd_id == CMD_SET_WINDOW_TYPE and len(payload) >= 8:
            win_id, wtype = struct.unpack_from("<ii", payload, 0)
            window_types[win_id] = wtype
        elif cmd_id == CMD_SET_PINNED_STATE and len(payload) >= 5:
            tid, pinned = struct.unpack_from("<iB", payload, 0)
            tab_pinned[tid] = bool(pinned)
        elif cmd_id == CMD_TAB_CLOSED and len(payload) >= 4:
            (tid,) = struct.unpack_from("<i", payload, 0)
            closed_tabs.add(tid)
        elif cmd_id == CMD_WINDOW_CLOSED and len(payload) >= 4:
            (win_id,) = struct.unpack_from("<i", payload, 0)
            closed_windows.add(win_id)
        elif cmd_id == CMD_SET_ACTIVE_WINDOW and len(payload) >= 4:
            (active_window_id,) = struct.unpack_from("<i", payload, 0)
        elif cmd_id == CMD_SET_TAB_GROUP:
            try:
                pr = PickleReader(payload)
                tid = pr.read_int32()
                high = pr.read_uint64()
                low = pr.read_uint64()
                has_group = pr.read_bool() if pr.remaining() >= 4 else (high != 0 or low != 0)
            except ValueError:
                continue
            if has_group and (high != 0 or low != 0):
                tab_group_tok[tid] = (high, low)
            else:
                tab_group_tok.pop(tid, None)
        elif cmd_id in (CMD_SET_TAB_GROUP_METADATA, CMD_SET_TAB_GROUP_METADATA2):
            try:
                pr = PickleReader(payload)
                high = pr.read_uint64()
                low = pr.read_uint64()
                title = pr.read_string16()
            except ValueError:
                continue
            if high != 0 or low != 0:
                group_titles[(high, low)] = title

    windows_tabs: dict[int, list[int]] = {}
    for tid, win_id in tab_window.items():
        if tid in closed_tabs or win_id in closed_windows:
            continue
        if not include_internal and window_types.get(win_id, 0) != 0:
            continue
        windows_tabs.setdefault(win_id, []).append(tid)

    ordered_win_ids = sorted(
        windows_tabs.keys(),
        key=lambda wid: (0 if wid == active_window_id else 1, wid),
    )

    tabs_out: list[dict[str, Any]] = []
    active_out_idx = 0
    found_active = False

    for w_num, wid in enumerate(ordered_win_ids, start=1):
        tids = sorted(
            windows_tabs[wid],
            key=lambda tid: (0 if tab_pinned.get(tid, False) else 1, tab_index.get(tid, 1 << 30), tid),
        )
        sel_idx_in_win = window_selected_tab.get(wid, -1)
        is_active_win = (wid == active_window_id) or (active_window_id is None and w_num == 1)

        for pos_in_win, tid in enumerate(tids):
            navs = tab_navs.get(tid)
            if not navs:
                continue
            nav_i = tab_selected_nav.get(tid)
            if nav_i is None or nav_i not in navs:
                nav_i = max(navs.keys())
            url, title = navs[nav_i]
            if _should_skip_url(url, include_internal):
                continue

            pinned = bool(tab_pinned.get(tid, False))
            grp_name = ""
            tok = tab_group_tok.get(tid)
            if tok and not pinned:
                grp_name = group_titles.get(tok, "")
                if not grp_name:
                    grp_name = f"GROUP-{tok[1] & 0xFFFF:04X}"
            if window_groups and len(ordered_win_ids) > 1 and not grp_name and not pinned:
                grp_name = f"WIN-{w_num}"

            is_sel = False
            if sel_idx_in_win >= 0:
                raw_idx = tab_index.get(tid, pos_in_win)
                is_sel = (raw_idx == sel_idx_in_win or pos_in_win == sel_idx_in_win)
            if is_active_win and is_sel and not found_active:
                active_out_idx = len(tabs_out)
                found_active = True

            tab_obj: dict[str, Any] = {
                "uri": url,
                "title": title or url,
                "group": grp_name,
                "pinned": pinned,
            }
            fav = favicon_map.get(url, "") if favicon_map else ""
            if fav:
                tab_obj["favicon_url"] = fav
            tabs_out.append(tab_obj)

    return {"active_index": active_out_idx, "tabs": tabs_out}


def _find_bookmark_folders(node: dict[str, Any], target_lower: str, out: list[dict[str, Any]]) -> None:
    if node.get("type") == "folder":
        if str(node.get("name", "")).strip().lower() == target_lower:
            out.append(node)
        for child in node.get("children", []) or []:
            if isinstance(child, dict):
                _find_bookmark_folders(child, target_lower, out)


def parse_bookmarks_folder(
    profile_dir: Path,
    folder_name: str,
    *,
    include_internal: bool = False,
) -> dict[str, Any]:
    bm_path = profile_dir / "Bookmarks"
    if not bm_path.is_file():
        raise FileNotFoundError(f"Bookmarks file not found: {bm_path}")
    raw = json.loads(bm_path.read_text(encoding="utf-8"))
    roots = raw.get("roots", {})
    matches: list[dict[str, Any]] = []
    target_lower = folder_name.strip().lower()
    for root_node in roots.values():
        if isinstance(root_node, dict):
            _find_bookmark_folders(root_node, target_lower, matches)
    if not matches:
        raise ValueError(f"Bookmark folder '{folder_name}' not found in {bm_path}")

    favicon_map = load_chromium_favicon_map(profile_dir)
    tabs_out: list[dict[str, Any]] = []
    def collect_urls(node: dict[str, Any]) -> None:
        for ch in node.get("children", []) or []:
            if not isinstance(ch, dict):
                continue
            if ch.get("type") == "url":
                url = str(ch.get("url", "")).strip()
                if not _should_skip_url(url, include_internal):
                    tab_obj: dict[str, Any] = {
                        "uri": url,
                        "title": str(ch.get("name", "")).strip() or url,
                        "group": "",
                        "pinned": False,
                    }
                    fav = favicon_map.get(url, "")
                    if fav:
                        tab_obj["favicon_url"] = fav
                    tabs_out.append(tab_obj)
            elif ch.get("type") == "folder":
                collect_urls(ch)

    for folder_node in matches:
        collect_urls(folder_node)

    return {"active_index": 0, "tabs": tabs_out}


def list_chromium_profiles(browser_dir: Path) -> int:
    if not browser_dir.is_dir():
        print(f"Directory not found: {browser_dir}", file=sys.stderr)
        return 1

    info_cache: dict[str, Any] = {}
    local_state = browser_dir / "Local State"
    if local_state.is_file():
        try:
            ls = json.loads(local_state.read_text(encoding="utf-8"))
            info_cache = ls.get("profile", {}).get("info_cache", {}) or {}
        except Exception:
            info_cache = {}

    found_dirs: set[str] = set(info_cache.keys())
    for child in sorted(browser_dir.iterdir()):
        if child.is_dir() and ((child / "Sessions").is_dir() or (child / "Bookmarks").is_file()):
            found_dirs.add(child.name)

    if not found_dirs:
        print(f"No Chromium/Thorium profiles found under {browser_dir}")
        return 0

    print(f"Profiles in {browser_dir}:")
    for pdir_name in sorted(found_dirs):
        pdir = browser_dir / pdir_name
        if not pdir.is_dir():
            continue
        meta = info_cache.get(pdir_name, {}) if isinstance(info_cache, dict) else {}
        disp = meta.get("name") or pdir_name
        email = meta.get("user_name") or ""
        sess_info = "no Session_*"
        try:
            latest = find_latest_snss_file(pdir)
            mtime = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(latest.stat().st_mtime))
            sess_info = f"{latest.name} ({mtime})"
        except FileNotFoundError:
            pass
        acct = f" <{email}>" if email else ""
        print(f"  - {pdir_name!r:14} name={disp!r}{acct}  latest={sess_info}")
        print(f"      path: {pdir}")
    return 0


def merge_sessions(
    existing: dict[str, Any],
    incoming: dict[str, Any],
    dedup: bool = True,
) -> dict[str, Any]:
    base_tabs = list(existing.get("tabs", []) or [])
    seen = {t.get("uri") for t in base_tabs if isinstance(t, dict) and t.get("uri")}
    for tab in incoming.get("tabs", []) or []:
        uri = tab.get("uri", "")
        if dedup and uri in seen:
            continue
        base_tabs.append(tab)
        if uri:
            seen.add(uri)

    # Sort invariant expected by web_model (pinned precede unpinned)
    pinned = [t for t in base_tabs if t.get("pinned")]
    unpinned = [t for t in base_tabs if not t.get("pinned")]
    merged_tabs = pinned + unpinned

    act_idx = int(existing.get("active_index", 0) or 0)
    if merged_tabs:
        act_idx = max(0, min(act_idx, len(merged_tabs) - 1))
    else:
        act_idx = 0
    return {"active_index": act_idx, "tabs": merged_tabs}


def write_session_atomic(out_path: Path, session_data: dict[str, Any]) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if out_path.is_file():
        bak_path = out_path.with_suffix(".json.bak")
        bak_path.write_bytes(out_path.read_bytes())
    tmp_path = out_path.with_suffix(".json.tmp")
    tmp_path.write_text(json.dumps(session_data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.chmod(tmp_path, 0o600)
    os.replace(tmp_path, out_path)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="One-shot importer of Chromium/Thorium open tabs into a kajo-web profile."
    )
    p.add_argument(
        "--list-profiles",
        metavar="DIR",
        help="List profile directories inside a Chromium/Thorium config dir (e.g. ~/.config/thorium) and exit.",
    )
    p.add_argument(
        "--from",
        dest="from_dir",
        metavar="PROFILE_DIR",
        help="Source Chromium/Thorium profile directory (e.g. ~/.config/thorium/Default).",
    )
    p.add_argument(
        "--session-file",
        metavar="FILE",
        help="Explicit SNSS Session_* file instead of auto-detecting the latest under PROFILE_DIR/Sessions/.",
    )
    p.add_argument(
        "--from-bookmarks-folder",
        metavar="FOLDER",
        help="Extract tabs from FOLDER in PROFILE_DIR/Bookmarks instead of parsing binary SNSS.",
    )
    p.add_argument(
        "--to-profile",
        default="default",
        help="Target kajo-web profile slug or name (default: 'default').",
    )
    p.add_argument(
        "--out",
        metavar="FILE",
        help="Override destination session.json path directly (skips IPC running check).",
    )
    p.add_argument(
        "--merge",
        action="store_true",
        help="Append imported tabs to existing session.json instead of replacing it.",
    )
    p.add_argument(
        "--no-dedup",
        action="store_true",
        help="With --merge, keep duplicate URIs instead of deduplicating.",
    )
    p.add_argument(
        "--window-groups",
        action="store_true",
        help="Assign WIN-N group names to ungrouped tabs when importing multiple Thorium windows.",
    )
    p.add_argument(
        "--include-internal",
        action="store_true",
        help="Keep chrome://, devtools://, about:blank, and non-normal windows.",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Preview extracted tabs without writing session.json.",
    )
    p.add_argument(
        "--force",
        action="store_true",
        help="Write even if kajo-web appears to be running for the target profile.",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)

    if args.list_profiles:
        return list_chromium_profiles(Path(args.list_profiles).expanduser())

    if not args.from_dir and not args.session_file:
        print("Error: provide --list-profiles DIR, --from PROFILE_DIR, or --session-file FILE", file=sys.stderr)
        return 2

    src_profile_dir = Path(args.from_dir).expanduser() if args.from_dir else None
    if src_profile_dir and not src_profile_dir.is_dir():
        print(f"Error: source profile directory does not exist: {src_profile_dir}", file=sys.stderr)
        return 1

    if args.from_bookmarks_folder:
        if not src_profile_dir:
            print("Error: --from-bookmarks-folder requires --from PROFILE_DIR", file=sys.stderr)
            return 2
        imported = parse_bookmarks_folder(
            src_profile_dir,
            args.from_bookmarks_folder,
            include_internal=args.include_internal,
        )
        source_desc = f"{src_profile_dir / 'Bookmarks'} [folder: {args.from_bookmarks_folder}]"
    else:
        sess_file = (
            Path(args.session_file).expanduser()
            if args.session_file
            else find_latest_snss_file(src_profile_dir)  # type: ignore[arg-type]
        )
        if not sess_file.is_file():
            print(f"Error: SNSS file not found: {sess_file}", file=sys.stderr)
            return 1
        imported = parse_snss_session(
            sess_file,
            include_internal=args.include_internal,
            window_groups=args.window_groups,
            favicon_map=load_chromium_favicon_map(src_profile_dir) if src_profile_dir else None,
        )
        source_desc = str(sess_file)

    profile_id = sanitize_profile_id(args.to_profile)
    out_path = Path(args.out).expanduser() if args.out else kajo_session_path(profile_id)

    if not args.dry_run and not args.out and not args.force:
        if is_kajo_profile_running(profile_id):
            print(
                f"Error: kajo-web profile '{profile_id}' is currently running.\n"
                f"Close kajo-web first (otherwise it overwrites session.json on exit), or pass --force.",
                file=sys.stderr,
            )
            return 1

    final_data = imported
    if args.merge and out_path.is_file():
        existing = json.loads(out_path.read_text(encoding="utf-8"))
        final_data = merge_sessions(existing, imported, dedup=not args.no_dedup)
    else:
        # Ensure pinned tabs precede unpinned tabs (web_model invariant)
        tabs = imported.get("tabs", [])
        pinned = [t for t in tabs if t.get("pinned")]
        unpinned = [t for t in tabs if not t.get("pinned")]
        reordered = pinned + unpinned
        act_idx = int(imported.get("active_index", 0) or 0)
        if tabs and 0 <= act_idx < len(tabs):
            act_uri = tabs[act_idx]
            try:
                act_idx = reordered.index(act_uri)
            except ValueError:
                act_idx = 0
        else:
            act_idx = 0
        final_data = {"active_index": act_idx, "tabs": reordered}

    tabs = final_data["tabs"]
    pinned_cnt = sum(1 for t in tabs if t.get("pinned"))
    groups = sorted({t.get("group", "") for t in tabs if t.get("group")})

    print(f"Source      : {source_desc}")
    print(f"Target      : {out_path} (profile='{profile_id}')")
    print(f"Tabs total  : {len(tabs)} (pinned={pinned_cnt}, groups={len(groups)}, active_index={final_data['active_index']})")
    if groups:
        print(f"Group names : {', '.join(groups)}")

    if args.dry_run:
        for idx, t in enumerate(tabs):
            flags = []
            if idx == final_data["active_index"]:
                flags.append("ACT")
            if t.get("pinned"):
                flags.append("PIN")
            if t.get("group"):
                flags.append(f"GRP:{t['group']}")
            tag = f"[{','.join(flags)}] " if flags else ""
            print(f"  {idx + 1:3d}. {tag}{t['title']} -> {t['uri']}")
        print("Dry-run complete: no files written.")
        return 0

    if not args.out and not profile_registered_in_toml(profile_id):
        print(
            f"Note: profile '{profile_id}' is not listed in {kajo_profiles_toml_path()} yet.\n"
            f"Create it in kajo-web (Ctrl+Shift+M) or launch with: kajo-web --profile {profile_id}",
            file=sys.stderr,
        )

    write_session_atomic(out_path, final_data)
    print(f"Wrote {len(tabs)} tabs to {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
