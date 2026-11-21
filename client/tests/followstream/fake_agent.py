#!/usr/bin/env python3
"""A fake inspector agent for kclient's follow mode (docs/FOLLOW_STREAM.md, kenshi-cym.40.2.5).

Listens on 127.0.0.1:PORT, waits for the client's `hello` and `ready`, then plays a record file and logs everything the client sends.
Never talks to the original: the records come from the file.

usage: fake_agent.py --port N --records FILE --log FILE [--report IMPORT_REPORT.json] [--linger SECONDS]

Record file: one record per line, `DELAY record...`: wait DELAY seconds (float) after the previous line, then send the record. Blank lines and
lines starting with # are skipped. {name} placeholders are filled from the import report (the server writes it at import): {m0} {m1} ... the
member ids in report order, {sel0} the first selected id, {cx} {cy} {cz} the camera pivot, {cx+30} style offsets: {cx+30}.
A raw line (sent as is, no substitution of the delay) can be forced with `RAW text`.
Drift view (kenshi-cym.40.2.6): `GHOST id x,y,z [state] [shift_s]` makes the agent send an `orig` for that id once a second from then on, at that point (static),
`TRACK id FILE [shift_s]` the same along a recorded path (FILE lines `dt x y z`: seconds since the sim unpaused, linear between), `RUN [speed]` marks the sim as unpaused
(the original's clock then runs on, estimated from the client's `clock t=` beat; before it, `orig t` is the `align` time itself). shift_s adds seconds to the
`orig t`, in game seconds (a control: a deliberately wrong clock; one tick is 1.65 game s, the render delay 3.3). The offset between the sim's clock and the original's is the first client beat minus `align`.
A background thread sends `clock t=... day=D hours=H` once a second after `ready`.
Exit when the client closes the stream or after --linger seconds past the last record.
"""
import argparse, json, re, socket, sys, threading, time

GAME_HOUR_S = 2618.1819 / 24.0   # wall seconds of one game hour at 1x (ksim/core/clock.hpp)

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
    st = {"align": None, "run": False, "speed": 1.0, "simt0": None, "clk": None, "clk_wall": 0.0, "run_t0": None}
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
                if s.startswith("clock t="):
                    try:
                        v = float(s.split()[1][2:]); st["clk"] = v; st["clk_wall"] = time.time()
                        if st["simt0"] is None: st["simt0"] = v
                    except ValueError: pass
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
    ghosts = []   # dicts: id, pos | track, state, shift
    def orig_t():
        """the original's game hours now (the sim's clock estimate minus the offset), or the align time while paused"""
        if st["align"] is None: return None
        if not st["run"] or st["clk"] is None or st["simt0"] is None: return st["align"]
        return st["clk"] + (time.time() - st["clk_wall"]) * st["speed"] / GAME_HOUR_S - (st["simt0"] - st["align"])
    def ghost_loop():
        while not stop.is_set():
            t = orig_t()
            if t is not None:
                for g in ghosts:
                    ahead = 0.3 if st["run"] else 0.0   # wall seconds
                    tt = t + ahead * st["speed"] / GAME_HOUR_S
                    if "track" in g:
                        # a recorded path: each sample goes out once, with its OWN exact time (align + dt), when the estimated clock gets within 0.5 wall s of it;
                        # the client interpolates at its drawn time, so the agent's clock estimate decides only how early a sample is sent, never d
                        dt_now = ((t - st["align"]) * 3600.0 + 0.5 * st["speed"] / GAME_HOUR_S * 3600.0) if st["run"] else 0.0
                        pts = g["track"]
                        while g["next"] < len(pts) and pts[g["next"]][0] <= dt_now:
                            q = pts[g["next"]]; g["next"] += 1
                            send("orig id=%s pos=%.3f,%.3f,%.3f yaw=0.000 t=%.6f" % (g["id"], q[1], q[2], q[3], st["align"] + (q[0] + g["shift"]) / 3600.0))
                        continue
                    else: p = g["pos"]
                    send("orig id=%s pos=%.3f,%.3f,%.3f yaw=0.000 t=%.6f%s" % (g["id"], p[0], p[1], p[2], tt + g["shift"] / 3600.0, (" state=" + g["state"]) if g["state"] else ""))
            stop.wait(0.1 if (st["run"] and any("track" in g for g in ghosts)) else 1.0)   # a tracked walk is sampled at 10 Hz so the 1 Hz interpolation is not the error
    threading.Thread(target=ghost_loop, daemon=True).start()
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
        if rec.startswith("GHOST ") or rec.startswith("TRACK ") or rec.split()[0] == "RUN":
            w = fill(rec).split()
            if w[0] == "RUN": st["run"] = True; st["speed"] = float(w[1]) if len(w) > 1 else 1.0; log("RUN"); continue
            g = {"id": w[1], "state": "", "shift": 0.0, "next": 0}
            if w[0] == "GHOST":
                g["pos"] = [float(x) for x in w[2].split(",")]
                rest = w[3:]
            else:
                g["track"] = [[float(x) for x in ln.split()] for ln in open(w[2]) if ln.strip() and not ln.startswith("#")]
                rest = w[3:]
            for x in rest:
                try: g["shift"] = float(x)
                except ValueError: g["state"] = x
            ghosts.append(g); continue
        if rec.startswith("align t="): st["align"] = float(rec.split("=")[1])
        send(rec[4:] if rec.startswith("RAW ") else fill(rec))
    stop.wait(a.linger)
    log("done")

main()
