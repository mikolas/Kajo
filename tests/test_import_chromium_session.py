#!/usr/bin/env python3
"""Unit tests for scripts/import-chromium-session.py."""

import importlib.util
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import tempfile
import threading
import unittest


def load_importer():
    script_path = Path(__file__).resolve().parent.parent / "scripts" / "import-chromium-session.py"
    spec = importlib.util.spec_from_file_location("import_chromium_session", script_path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_pickle(*items: tuple[str, object]) -> bytes:
    body = bytearray()
    for kind, val in items:
        if kind == "int32":
            body += struct.pack("<i", int(val))
        elif kind == "uint64":
            body += struct.pack("<Q", int(val))
        elif kind == "bool":
            body += struct.pack("<i", 1 if val else 0)
        elif kind == "str":
            raw = str(val).encode("utf-8")
            body += struct.pack("<i", len(raw)) + raw
            body += b"\x00" * ((-len(raw)) & 3)
        elif kind == "str16":
            raw = str(val).encode("utf-16-le")
            body += struct.pack("<i", len(raw) // 2) + raw
            body += b"\x00" * ((-len(raw)) & 3)
        else:
            raise ValueError(f"unknown kind {kind}")
    return struct.pack("<I", len(body)) + bytes(body)


def snss_record(cmd_id: int, payload: bytes) -> bytes:
    return struct.pack("<HB", 1 + len(payload), cmd_id) + payload


class ImportChromiumSessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.imp = load_importer()

    def build_sample_snss(self) -> bytes:
        m = self.imp
        chunks = [b"SNSS" + struct.pack("<I", 3)]

        # Window 10 (open, active) and Window 20 (closed)
        chunks.append(snss_record(m.CMD_SET_WINDOW_TYPE, struct.pack("<ii", 10, 0)))
        chunks.append(snss_record(m.CMD_SET_ACTIVE_WINDOW, struct.pack("<i", 10)))

        # Tab 101: unpinned index 2
        chunks.append(snss_record(m.CMD_SET_TAB_WINDOW, struct.pack("<ii", 10, 101)))
        chunks.append(snss_record(m.CMD_SET_TAB_INDEX_IN_WINDOW, struct.pack("<ii", 101, 2)))
        chunks.append(snss_record(
            m.CMD_UPDATE_TAB_NAVIGATION,
            make_pickle(("int32", 101), ("int32", 0), ("str", "https://first.invalid/old"), ("str16", "Old")),
        ))
        chunks.append(snss_record(
            m.CMD_UPDATE_TAB_NAVIGATION,
            make_pickle(("int32", 101), ("int32", 1), ("str", "https://first.invalid/cur"), ("str16", "Current Tab")),
        ))
        chunks.append(snss_record(m.CMD_SET_SELECTED_NAVIGATION_INDEX, struct.pack("<ii", 101, 0)))

        # Tab 102: pinned index 0
        chunks.append(snss_record(m.CMD_SET_TAB_WINDOW, struct.pack("<ii", 10, 102)))
        chunks.append(snss_record(m.CMD_SET_TAB_INDEX_IN_WINDOW, struct.pack("<ii", 102, 0)))
        chunks.append(snss_record(m.CMD_SET_PINNED_STATE, struct.pack("<iB", 102, 1)))
        chunks.append(snss_record(
            m.CMD_UPDATE_TAB_NAVIGATION,
            make_pickle(("int32", 102), ("int32", 0), ("str", "https://mail.example/"), ("str16", "Mail")),
        ))

        # Tab 103: grouped tab at index 1 ("WORK")
        chunks.append(snss_record(m.CMD_SET_TAB_WINDOW, struct.pack("<ii", 10, 103)))
        chunks.append(snss_record(m.CMD_SET_TAB_INDEX_IN_WINDOW, struct.pack("<ii", 103, 1)))
        chunks.append(snss_record(
            m.CMD_UPDATE_TAB_NAVIGATION,
            make_pickle(("int32", 103), ("int32", 0), ("str", "https://docs.example/spec"), ("str16", "Spec")),
        ))
        chunks.append(snss_record(
            m.CMD_SET_TAB_GROUP,
            make_pickle(("int32", 103), ("uint64", 0x1111), ("uint64", 0x2222), ("bool", True)),
        ))
        chunks.append(snss_record(
            m.CMD_SET_TAB_GROUP_METADATA2,
            make_pickle(("uint64", 0x1111), ("uint64", 0x2222), ("str16", "WORK")),
        ))

        # Tab 104: internal chrome://settings (filtered by default)
        chunks.append(snss_record(m.CMD_SET_TAB_WINDOW, struct.pack("<ii", 10, 104)))
        chunks.append(snss_record(m.CMD_SET_TAB_INDEX_IN_WINDOW, struct.pack("<ii", 104, 3)))
        chunks.append(snss_record(
            m.CMD_UPDATE_TAB_NAVIGATION,
            make_pickle(("int32", 104), ("int32", 0), ("str", "chrome://settings"), ("str16", "Settings")),
        ))

        # Tab 105: closed tab (must not appear)
        chunks.append(snss_record(m.CMD_SET_TAB_WINDOW, struct.pack("<ii", 10, 105)))
        chunks.append(snss_record(
            m.CMD_UPDATE_TAB_NAVIGATION,
            make_pickle(("int32", 105), ("int32", 0), ("str", "https://closed.invalid/"), ("str16", "Closed")),
        ))
        chunks.append(snss_record(m.CMD_TAB_CLOSED, struct.pack("<i", 105)))

        # Window 20: closed window (must not appear)
        chunks.append(snss_record(m.CMD_SET_TAB_WINDOW, struct.pack("<ii", 20, 201)))
        chunks.append(snss_record(
            m.CMD_UPDATE_TAB_NAVIGATION,
            make_pickle(("int32", 201), ("int32", 0), ("str", "https://closedwin.invalid/"), ("str16", "WinClosed")),
        ))
        chunks.append(snss_record(m.CMD_WINDOW_CLOSED, struct.pack("<i", 20)))

        # Selected tab in window 10 = index 2 (tab 101 -> "https://first.invalid/old")
        chunks.append(snss_record(m.CMD_SET_SELECTED_TAB_IN_INDEX, struct.pack("<ii", 10, 2)))
        return b"".join(chunks)

    def test_parse_snss_tabs_pins_groups_and_active(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "Session_13370001"
            p.write_bytes(self.build_sample_snss())

            res = self.imp.parse_snss_session(p)
            tabs = res["tabs"]
            self.assertEqual(len(tabs), 3)
            # Order: pinned 102, then unpinned index 1 (103 WORK), index 2 (101)
            self.assertEqual(tabs[0]["uri"], "https://mail.example/")
            self.assertTrue(tabs[0]["pinned"])
            self.assertEqual(tabs[1]["uri"], "https://docs.example/spec")
            self.assertEqual(tabs[1]["group"], "WORK")
            self.assertFalse(tabs[1]["pinned"])
            self.assertEqual(tabs[2]["uri"], "https://first.invalid/old")
            self.assertEqual(res["active_index"], 2)

            res_internal = self.imp.parse_snss_session(p, include_internal=True)
            self.assertEqual(len(res_internal["tabs"]), 4)
            self.assertEqual(res_internal["tabs"][3]["uri"], "chrome://settings")

    def test_bookmarks_folder_fallback(self):
        with tempfile.TemporaryDirectory() as td:
            prof = Path(td) / "Profile 1"
            prof.mkdir()
            (prof / "Bookmarks").write_text(
                json.dumps({
                    "roots": {
                        "bookmark_bar": {
                            "type": "folder",
                            "name": "Bookmarks bar",
                            "children": [
                                {
                                    "type": "folder",
                                    "name": "_kajo_import",
                                    "children": [
                                        {"type": "url", "name": "Alpha", "url": "https://a.example"},
                                        {"type": "url", "name": "Beta", "url": "https://b.example"},
                                    ],
                                }
                            ],
                        }
                    }
                }),
                encoding="utf-8",
            )
            res = self.imp.parse_bookmarks_folder(prof, "_kajo_import")
            self.assertEqual([t["uri"] for t in res["tabs"]], ["https://a.example", "https://b.example"])

    def test_cli_merge_backup_and_running_guard(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            src_prof = td_path / "thorium" / "Profile 1"
            (src_prof / "Sessions").mkdir(parents=True)
            (src_prof / "Sessions" / "Session_1").write_bytes(self.build_sample_snss())

            data_home = td_path / "data"
            runtime_dir = td_path / "run"
            runtime_dir.mkdir()
            old_env = {k: os.environ.get(k) for k in ("XDG_DATA_HOME", "XDG_RUNTIME_DIR")}
            os.environ["XDG_DATA_HOME"] = str(data_home)
            os.environ["XDG_RUNTIME_DIR"] = str(runtime_dir)
            try:
                target_json = data_home / "kajo" / "web-cef" / "work" / "session.json"
                target_json.parent.mkdir(parents=True)
                target_json.write_text(
                    json.dumps({
                        "active_index": 0,
                        "tabs": [
                            {"uri": "https://existing.example/", "title": "Existing", "group": "", "pinned": False},
                            {"uri": "https://mail.example/", "title": "Mail Dup", "group": "", "pinned": False},
                        ],
                    }),
                    encoding="utf-8",
                )

                # Dry-run does not alter session.json
                rc = self.imp.main(["--from", str(src_prof), "--to-profile", "Work", "--merge", "--dry-run"])
                self.assertEqual(rc, 0)
                self.assertFalse(target_json.with_suffix(".json.bak").exists())

                # Running-instance guard rejects write when ping socket responds OK
                sock_path = self.imp.kajo_ipc_socket_path("work")
                srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                srv.bind(str(sock_path))
                srv.listen(2)
                stop_flag = threading.Event()

                def accept_one():
                    srv.settimeout(0.2)
                    while not stop_flag.is_set():
                        try:
                            c, _ = srv.accept()
                        except TimeoutError:
                            continue
                        with c:
                            _ = c.recv(256)
                            c.sendall(b"OK\n")

                t = threading.Thread(target=accept_one, daemon=True)
                t.start()
                try:
                    rc_blocked = self.imp.main(["--from", str(src_prof), "--to-profile", "Work", "--merge"])
                    self.assertNotEqual(rc_blocked, 0)
                finally:
                    stop_flag.set()
                    t.join(timeout=1.0)
                    srv.close()
                    if sock_path.exists():
                        sock_path.unlink()

                # Merge writes deduplicated session and creates .bak
                rc_ok = self.imp.main(["--from", str(src_prof), "--to-profile", "Work", "--merge"])
                self.assertEqual(rc_ok, 0)
                self.assertTrue(target_json.with_suffix(".json.bak").is_file())
                written = json.loads(target_json.read_text(encoding="utf-8"))
                uris = [tab["uri"] for tab in written["tabs"]]
                self.assertEqual(uris.count("https://mail.example/"), 1)
                self.assertIn("https://existing.example/", uris)
                self.assertIn("https://docs.example/spec", uris)
            finally:
                for k, v in old_env.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v

    def test_favicon_db_import(self):
        with tempfile.TemporaryDirectory() as td:
            src_prof = Path(td) / "thorium" / "Default"
            (src_prof / "Sessions").mkdir(parents=True)
            (src_prof / "Sessions" / "Session_1").write_bytes(self.build_sample_snss())

            conn = sqlite3.connect(str(src_prof / "Favicons"))
            try:
                conn.execute("CREATE TABLE favicons (id INTEGER PRIMARY KEY, url TEXT NOT NULL, icon_type INTEGER)")
                conn.execute("CREATE TABLE icon_mapping (id INTEGER PRIMARY KEY, page_url TEXT NOT NULL, icon_id INTEGER)")
                conn.execute("INSERT INTO favicons (id, url, icon_type) VALUES (1, 'https://mail.example/fav.png', 1)")
                conn.execute("INSERT INTO favicons (id, url, icon_type) VALUES (2, 'chrome://theme/bad', 1)")
                conn.execute("INSERT INTO icon_mapping (id, page_url, icon_id) VALUES (10, 'https://mail.example/', 1)")
                conn.execute("INSERT INTO icon_mapping (id, page_url, icon_id) VALUES (11, 'https://docs.example/spec', 2)")
                conn.commit()
            finally:
                conn.close()

            res = self.imp.parse_snss_session(src_prof / "Sessions" / "Session_1")
            by_uri = {t["uri"]: t for t in res["tabs"]}
            self.assertEqual(by_uri["https://mail.example/"].get("favicon_url"), "https://mail.example/fav.png")
            self.assertNotIn("favicon_url", by_uri["https://docs.example/spec"])


if __name__ == "__main__":
    unittest.main()
