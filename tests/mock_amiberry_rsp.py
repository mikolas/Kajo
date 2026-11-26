#!/usr/bin/env python3
"""Minimal scripted mock Amiberry 8.5.0 GDB RSP TCP server for test_amiga_rsp.

Speaks real GDB RSP wire framing over 127.0.0.1:<ephemeral> including:
- QStartNoAckMode, g/G (18 x 32-bit BE), m/M (incl. 512B $DFF000..$DFF1FF)
- Z0/z0 (enforces even address & len=2), Z2-Z4
- vCont;c / vCont;s / vCont;r<start>,<end>, raw 0x03 break -> T02, D detach
- Single-packet $<hex>#cs qRcmd replies for capabilities, execution-status,
  process-break, segments, tasks, disasm, two-stage step-over, watch range,
  watch events, guest-output, history read, memory-map, plus E16 while running.
"""
import json
import socket
import struct
import sys
import threading
import time


def rsp_csum(data: bytes) -> int:
    return sum(data) & 0xFF


def rsp_frame(payload: str) -> bytes:
    raw = payload.encode("utf-8")
    return b"$%s#%02x" % (raw, rsp_csum(raw))


def hex_utf8(text: str) -> str:
    return text.encode("utf-8").hex()


def make_custom_bank() -> bytearray:
    buf = bytearray(512)
    struct.pack_into(">H", buf, 0x002, 0x03F0)  # DMACONR
    struct.pack_into(">H", buf, 0x004, 0x2000)  # VPOSR
    struct.pack_into(">H", buf, 0x006, 0x2C40)  # VHPOSR
    struct.pack_into(">I", buf, 0x080, 0x00020100)  # COP1LC
    struct.pack_into(">I", buf, 0x0E0, 0x00030000)  # BPL1PT
    struct.pack_into(">H", buf, 0x100, 0x4200)  # BPLCON0
    struct.pack_into(">H", buf, 0x180, 0x0F00)  # COLOR00
    struct.pack_into(">H", buf, 0x182, 0x0FFF)  # COLOR01
    struct.pack_into(">H", buf, 0x1FA, 0x8000)  # PAL
    return buf


class MockServer:
    def __init__(self, conn: socket.socket):
        self.conn = conn
        self.lock = threading.Lock()
        self.no_ack = False
        self.running = False
        self.alive = True
        self.regs = [0] * 18
        self.regs[0] = 0x11223344
        self.regs[15] = 0x0007FF00
        self.regs[16] = 0x00002000  # SR
        self.regs[17] = 0x00020000  # PC
        self.ram = bytearray(0x40000)
        self.custom = make_custom_bank()
        self.proc_break = None
        self.breakpoints = set()
        self.watches = []
        self.guest_out = False

    def send_raw(self, raw: bytes):
        with self.lock:
            if not self.alive:
                return
            try:
                self.conn.sendall(raw)
            except OSError:
                self.alive = False

    def send_pkt(self, payload: str):
        self.send_raw(rsp_frame(payload))

    def schedule_stop(self, delay: float, reason_pkt: str, new_pc: int):
        def worker():
            time.sleep(delay)
            with self.lock:
                if not self.alive or not self.running:
                    return
                self.running = False
                self.regs[17] = new_pc
            self.send_pkt(reason_pkt)

        threading.Thread(target=worker, daemon=True).start()

    def handle_rcmd(self, cmd: str) -> str:
        allowed_running = (
            cmd == "capabilities"
            or cmd == "execution-status"
            or cmd.startswith("input ")
        )
        if self.running and not allowed_running:
            return "E16"

        if cmd == "capabilities":
            return hex_utf8(json.dumps({
                "version": "8.5.0",
                "gdbserver": True,
                "segtracker": True,
            }))
        if cmd == "execution-status":
            return hex_utf8(json.dumps({
                "execution": "running" if self.running else "stopped",
                "debugger_stopped": not self.running,
                "host_pause": 0,
                "frame": 42,
                "vpos": 100,
                "hpos": 50,
                "cycles": "123456",
            }))
        if cmd.startswith("process-break name "):
            self.proc_break = cmd[len("process-break name "):].strip()
            return "OK"
        if cmd == "process-break clear":
            self.proc_break = None
            return "OK"
        if cmd == "segments":
            return hex_utf8(json.dumps({
                "process": 0x100000,
                "name": "demo.exe",
                "command": "demo.exe",
                "segments": [
                    {"index": 0, "address": 0x20000, "size": 4096},
                    {"index": 1, "address": 0x21000, "size": 512},
                ],
            }))
        if cmd == "tasks":
            return hex_utf8(json.dumps({
                "tasks": [{
                    "address": 0x100000,
                    "name": "demo.exe",
                    "type": 13,
                    "state": 2,
                    "list": "current",
                    "signals_wait": 0,
                    "saved_sp": 0x22200,
                    "stack_lower": 0x21E00,
                    "stack_upper": 0x22600,
                }]
            }))
        if cmd.startswith("disasm "):
            lines = (
                "Segment 00 ($00020000)\n"
                "00020004 33fc 0f00 00df f180  move.w #$0f00,$00dff180\n"
                "0002000c 4eae ff7c            jsr -$0084(a6)\n"
                "00020010 66f2                 bne.b #$f2 == $00020004 (T)\n"
            )
            return hex_utf8(lines)
        if cmd == "step-over":
            self.running = True
            new_pc = self.regs[17] + 4
            self.schedule_stop(0.02, f"T0511:{new_pc:08x};", new_pc)
            return "OK"
        if cmd.startswith("watch range "):
            self.watches.append(cmd)
            return "OK"
        if cmd == "watch events":
            return hex_utf8(json.dumps({
                "events": [{
                    "pc": 0x20008,
                    "addr": 0xDFF058,
                    "size": 2,
                    "rwi": 2,
                    "src": 1,
                    "old": 0,
                    "value": 0x0CA2,
                    "frame": 42,
                    "vpos": 110,
                    "hpos": 20,
                }]
            }))
        if cmd == "guest-output on":
            self.guest_out = True
            return "OK"
        if cmd == "guest-output read":
            return hex_utf8(json.dumps({
                "records": [{"text": "hello from trap86\n"}]
            }))
        if cmd.startswith("history read"):
            return hex_utf8(json.dumps({
                "history": [{
                    "pc": 0x20004,
                    "frame": 42,
                    "vpos": 100,
                    "hpos": 50,
                    "registers": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16],
                }]
            }))
        if cmd == "memory-map":
            return hex_utf8('<memory-map><memory type="ram" start="0x0" length="0x80000"/></memory-map>')
        return ""

    def handle_pkt(self, payload: str) -> bool:
        if payload == "QStartNoAckMode":
            self.send_pkt("OK")
            self.no_ack = True
            return True
        if payload == "D":
            self.send_pkt("OK")
            self.alive = False
            return False
        if payload.startswith("qRcmd,"):
            cmd = bytes.fromhex(payload[6:]).decode("utf-8", errors="replace")
            self.send_pkt(self.handle_rcmd(cmd))
            return True
        if payload == "g":
            raw = b"".join(struct.pack(">I", v & 0xFFFFFFFF) for v in self.regs)
            self.send_pkt(raw.hex())
            return True
        if payload.startswith("G"):
            raw = bytes.fromhex(payload[1:])
            for i in range(18):
                self.regs[i] = struct.unpack(">I", raw[i * 4:(i + 1) * 4])[0]
            self.send_pkt("OK")
            return True
        if payload.startswith("m"):
            a_s, l_s = payload[1:].split(",", 1)
            addr = int(a_s, 16)
            length = int(l_s, 16)
            if 0xDFF000 <= addr <= 0xDFF1FF and addr + length <= 0xDFF200:
                off = addr - 0xDFF000
                self.send_pkt(bytes(self.custom[off:off + length]).hex())
            else:
                off = min(addr, len(self.ram) - 1)
                data = bytes(self.ram[off:off + length])
                if len(data) < length:
                    data = data + b"\x00" * (length - len(data))
                self.send_pkt(data.hex())
            return True
        if payload.startswith("M"):
            head, hex_data = payload[1:].split(":", 1)
            a_s, _ = head.split(",", 1)
            addr = int(a_s, 16)
            raw = bytes.fromhex(hex_data)
            if addr + len(raw) <= len(self.ram):
                self.ram[addr:addr + len(raw)] = raw
            self.send_pkt("OK")
            return True
        if payload.startswith("Z0,") or payload.startswith("z0,"):
            parts = payload[3:].split(",")
            addr = int(parts[0], 16)
            length = int(parts[1], 16)
            if (addr & 1) != 0 or length != 2:
                self.send_pkt("E01")
            else:
                if payload[0] == "Z":
                    self.breakpoints.add(addr)
                else:
                    self.breakpoints.discard(addr)
                self.send_pkt("OK")
            return True
        if payload.startswith("Z") or payload.startswith("z"):
            self.send_pkt("OK")
            return True
        if payload == "vCont;c":
            self.running = True
            if self.proc_break:
                proc = self.proc_break
                self.proc_break = None
                self.schedule_stop(0.02, f"T05winuae-entry:{proc};11:00020004;", 0x20004)
            return True
        if payload == "vCont;s":
            self.running = True
            new_pc = self.regs[17] + 2
            self.schedule_stop(0.01, f"T0511:{new_pc:08x};", new_pc)
            return True
        if payload.startswith("vCont;r"):
            _, rng = payload.split(";r", 1)
            _, end_s = rng.split(",", 1)
            end_addr = int(end_s, 16)
            self.running = True
            self.schedule_stop(0.01, f"T0511:{end_addr:08x};", end_addr)
            return True
        self.send_pkt("")
        return True

    def serve(self):
        buf = bytearray()
        while self.alive:
            try:
                chunk = self.conn.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            buf.extend(chunk)
            while buf:
                if buf[0] in (ord("+"), ord("-")):
                    del buf[0]
                    continue
                if buf[0] == 3:  # Ctrl+C / 0x03 interrupt
                    del buf[0]
                    with self.lock:
                        self.running = False
                        pc = self.regs[17]
                    self.send_pkt(f"T0211:{pc:08x};")
                    continue
                if buf[0] != ord("$"):
                    idx = buf.find(b"$")
                    if idx < 0:
                        buf.clear()
                        break
                    del buf[:idx]
                    continue
                h = buf.find(b"#", 1)
                if h < 0 or h + 2 >= len(buf):
                    break
                payload = bytes(buf[1:h]).decode("utf-8", errors="replace")
                del buf[:h + 3]
                if not self.no_ack:
                    self.send_raw(b"+")
                if not self.handle_pkt(payload):
                    return


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)
    port = srv.getsockname()[1]
    print(f"PORT={port}", flush=True)

    conn, _ = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    MockServer(conn).serve()
    conn.close()
    srv.close()


if __name__ == "__main__":
    main()
