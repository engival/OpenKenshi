#!/usr/bin/env python3
"""A fake inspector agent for kclient's follow mode (docs/FOLLOW_STREAM.md, kenshi-cym.40.2.5).

Listens on 127.0.0.1:PORT, waits for the client's `hello` and `ready`, then plays a record file and logs everything the client sends.
Never talks to the original: the records come from the file.

usage: fake_agent.py --port N --records FILE --log FILE [--report IMPORT_REPORT.json] [--linger SECONDS]

Record file: one record per line, `DELAY record...`: wait DELAY seconds (float) after the previous line, then send the record. Blank lines and
lines starting with # are skipped. {name} placeholders are filled from the import report (the server writes it at import): {m0} {m1} ... the
member ids in report order, {sel0} the first selected id, {cx} {cy} {cz} the camera pivot, {cx+30} style offsets: {cx+30}.
A raw line (sent as is, no substitution of the delay) can be forced with `RAW text`.
A background thread sends `clock t=... day=D hours=H` once a second after `ready`.
Exit when the client closes the stream or after --linger seconds past the last record.
"""
import argparse, json, re, socket, sys, threading, time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--records", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--report")
    ap.add_argument("--linger", type=float, default=5.0)
    ap.add_argument("--wait-report", type=float, default=60.0, help="seconds to wait for the report file after ready")
    a = ap.parse_args()
    t0 = time.time()
    logf = open(a.log, "a", buffering=1)
    def log(s):
        logf.write("%8.3f %s\n" % (time.time() - t0, s))
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port)); srv.listen(1)
    log("listening 127.0.0.1:%d" % a.port)
    conn, _ = srv.accept()
    log("client connected")
    f = conn.makefile("rwb", buffering=0)
    stop = threading.Event()
    sendlock = threading.Lock()
    ready = threading.Event()
    def send(line):
        with sendlock:
            try:
                conn.sendall((line + "\n").encode())
                log("SEND " + line)
            except OSError as e:
                log("send failed: %s" % e); stop.set()
    def reader():
        buf = b""
        while not stop.is_set():
            try: d = conn.recv(4096)
            except OSError: break
            if not d: log("client closed"); break
            buf += d
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                s = ln.decode(errors="replace"); log("RECV " + s)
                if s.startswith("ready"): ready.set()
        stop.set(); ready.set()
    threading.Thread(target=reader, daemon=True).start()
    # the agent sends nothing but hello before ready
    send("hello v=1")
    ready.wait()
    if stop.is_set(): return
    subs = {}
    if a.report:
        end = time.time() + a.wait_report
        while time.time() < end:
            try:
                rep = json.load(open(a.report)); break
            except (OSError, ValueError): time.sleep(0.2)
        else:
            log("no report at %s" % a.report); return
        for i, m in enumerate(rep["members"]): subs["m%d" % i] = m["id"]
        for i, s in enumerate(rep.get("selection", [])): subs["sel%d" % i] = s
        c = rep["camera"]["pos"]; subs["cx"], subs["cy"], subs["cz"] = c
        log("report: %d members, selection %s, camera %s" % (len(rep["members"]), rep.get("selection"), c))
    def fill(text):
        def r(m):
            name, off = m.group(1), m.group(2)
            v = subs[name]
            return ("%.3f" % (v + float(off))) if off else str(v)
        return re.sub(r"\{(\w+)([+-][0-9.]+)?\}", r, text)
    def beat():
        n = 0
        while not stop.is_set():
            hrs = 8.0 + n / 3600.0
            send("clock t=%.6f day=1 hours=%.6f" % (hrs, hrs)); n += 1
            stop.wait(1.0)
    threading.Thread(target=beat, daemon=True).start()
    for raw in open(a.records):
        raw = raw.rstrip("\n")
        if not raw.strip() or raw.lstrip().startswith("#"): continue
        delay, _, rec = raw.partition(" ")
        if stop.wait(float(delay)): break
        send(rec[4:] if rec.startswith("RAW ") else fill(rec))
    stop.wait(a.linger)
    log("done")

main()
