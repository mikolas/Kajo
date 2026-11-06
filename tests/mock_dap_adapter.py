#!/usr/bin/env python3
"""Scripted Debug Adapter used by the kajo-ide DAP tests.

Behaviour: after configurationDone it stops once at the first breakpoint (or at
line 1 when none was set). Each `next` / `stepIn` advances the line by one, the
first `continue` runs on to a second stop, the next one terminates the debuggee.
"""
import json
import socket
import sys

inp = sys.stdin.buffer
outp = sys.stdout.buffer
breakpoints = {}   # path -> [lines]
state = {"line": 1, "path": "/tmp/mock.c", "stops": 0}


def read_message():
    headers = {}
    while True:
        line = inp.readline()
        if not line:
            return None
        line = line.decode().strip()
        if line == "":
            break
        k, _, v = line.partition(":")
        headers[k.lower()] = v.strip()
    body = inp.read(int(headers.get("content-length", "0")))
    return json.loads(body)


seq = 0


def send(obj):
    global seq
    seq += 1
    obj["seq"] = seq
    data = json.dumps(obj).encode()
    outp.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
    outp.flush()


def respond(req, body=None, success=True, message=None):
    r = {"type": "response", "request_seq": req["seq"], "success": success,
         "command": req["command"]}
    if body is not None:
        r["body"] = body
    if message:
        r["message"] = message
    send(r)


def event(name, body=None):
    e = {"type": "event", "event": name}
    if body is not None:
        e["body"] = body
    send(e)


def stop(reason):
    state["stops"] += 1
    event("stopped", {"reason": reason, "threadId": 1, "allThreadsStopped": True})


def main():
    while True:
        req = read_message()
        if req is None:
            return
        cmd = req.get("command")
        args = req.get("arguments", {})
        if cmd == "initialize":
            respond(req, {"supportsConfigurationDoneRequest": True})
        elif cmd == "launch":
            state["program"] = args.get("program")
            if args.get("program") == "/nonexistent":
                respond(req, success=False, message="program not found")
                continue
            event("initialized")
            respond(req)
            event("output", {"category": "console", "output": "mock adapter launched %s\n" % args.get("program")})
        elif cmd == "setBreakpoints":
            path = args["source"]["path"]
            lines = [b["line"] for b in args.get("breakpoints", [])]
            breakpoints[path] = lines
            respond(req, {"breakpoints": [{"verified": True, "line": l} for l in lines]})
        elif cmd == "setExceptionBreakpoints":
            respond(req, {})
        elif cmd == "configurationDone":
            respond(req)
            for p, ls in breakpoints.items():
                if ls:
                    state["path"], state["line"] = p, ls[0]
                    break
            stop("breakpoint" if any(breakpoints.values()) else "entry")
        elif cmd == "threads":
            respond(req, {"threads": [{"id": 1, "name": "main"}]})
        elif cmd == "stackTrace":
            respond(req, {"stackFrames": [
                {"id": 10, "name": "compute", "line": state["line"], "column": 1,
                 "source": {"name": "mock.c", "path": state["path"]}},
                {"id": 11, "name": "main", "line": 42, "column": 1,
                 "source": {"name": "mock.c", "path": state["path"]}},
            ], "totalFrames": 2})
        elif cmd == "scopes":
            respond(req, {"scopes": [{"name": "Locals", "variablesReference": 100 + args["frameId"],
                                      "expensive": False}]})
        elif cmd == "variables":
            ref = args["variablesReference"]
            if ref == 777:
                respond(req, {"variables": [
                    {"name": "x", "value": "1", "type": "int", "variablesReference": 0},
                    {"name": "y", "value": "2", "type": "int", "variablesReference": 0}]})
            else:
                respond(req, {"variables": [
                    {"name": "count", "value": "%d" % state["line"], "type": "int", "variablesReference": 0},
                    {"name": "name", "value": '"kajo"', "type": "char *", "variablesReference": 0},
                    {"name": "point", "value": "{...}", "type": "struct P", "variablesReference": 777}]})
        elif cmd in ("next", "stepIn", "stepOut"):
            respond(req)
            state["line"] += 1
            event("continued", {"threadId": 1})
            stop("step")
        elif cmd == "continue":
            respond(req, {"allThreadsContinued": True})
            event("continued", {"threadId": 1, "allThreadsContinued": True})
            if state["stops"] < 2:
                state["line"] += 10
                stop("breakpoint")
            else:
                event("output", {"category": "stdout", "output": "program output\n"})
                event("exited", {"exitCode": 0})
                event("terminated")
        elif cmd == "pause":
            respond(req)
            stop("pause")
        elif cmd == "evaluate":
            respond(req, {"result": "%s = 42" % args.get("expression", ""), "variablesReference": 0})
        elif cmd in ("terminate", "disconnect"):
            respond(req)
            if cmd == "disconnect":
                return
        else:
            respond(req, success=False, message="unsupported: %s" % cmd)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--listen":
        # TCP mode (like the Java debug server): serve one connection on 127.0.0.1
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", int(sys.argv[2])))
        srv.listen(1)
        srv.settimeout(15)
        conn, _ = srv.accept()
        inp = conn.makefile("rb")
        outp = conn.makefile("wb")
    main()
