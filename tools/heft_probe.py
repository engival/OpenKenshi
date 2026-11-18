#!/usr/bin/env python3
"""Raw-socket SIMLINK probe that plays the way the client does at Heft (docs/PROFILING.md "Heft under live load").

Connects to a running OpenKenshi-server, answers HELLO with the server's own dataHash, sends set_interest, set_view and the
speed, keeps the session alive with PING, counts SNAPSHOT_END arrivals and asks for PROFILE (order 120) every few seconds.
Usage: heft_probe.py PORT SECONDS SPEED [X Z [RADIUS]]   (default: the cage_prisoner Wanderer, interest 2000)
"""
import socket
import struct
import sys
import time


def frame(ftype, payload=b""):
	return struct.pack("<IH", 2 + len(payload), ftype) + payload


def order(seq, kind, body=b""):
	return frame(100, struct.pack("<IH", seq, kind) + body)


def main():
	port, seconds, speed = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
	x, z, radius = (float(sys.argv[4]), float(sys.argv[5]), float(sys.argv[6]) if len(sys.argv) > 6 else 2000.0) if len(sys.argv) > 5 else (68331.0, -71190.0, 2000.0)
	sock = socket.create_connection(("127.0.0.1", port))
	sock.settimeout(0.05)
	buf = b""
	state = {"hello": False}
	seq = 1
	ends = []
	profiles = []
	start = time.monotonic()
	nextPing = nextProf = 0.0
	sent = False

	def pump():
		nonlocal buf
		try:
			data = sock.recv(1 << 20)
		except socket.timeout:
			data = b""
		if data == b"" and False:
			raise SystemExit("closed")
		buf += data
		while len(buf) >= 4:
			(length,) = struct.unpack_from("<I", buf, 0)
			if len(buf) < 4 + length:
				break
			(ftype,) = struct.unpack_from("<H", buf, 4)
			payload = buf[6:4 + length]
			buf = buf[4 + length:]
			yield ftype, payload

	while time.monotonic() - start < seconds:
		for ftype, payload in pump():
			now = time.monotonic()
			if ftype == 1 and not state["hello"]:
				magic, proto, role, dhash = struct.unpack_from("<IHBQ", payload, 0)
				name = b"heft_probe"
				sock.sendall(frame(1, struct.pack("<IHBQ", magic, proto, 0, dhash) + struct.pack("<H", len(name)) + name))
				state["hello"] = True
			elif ftype == 11:
				ends.append(now)
			elif ftype == 70:
				tick, window = struct.unpack_from("<QI", payload, 0)
				mean, p50, p99, mx, budget, chars, squads, pq, pus = struct.unpack_from("<9f", payload, 12)
				profiles.append((now - start, tick, window, mean, p50, p99, mx, budget, chars, squads, pq, pus))
			elif ftype == 3:
				print("ERROR frame", payload[2:], file=sys.stderr)
		now = time.monotonic()
		if state["hello"] and not sent:
			sock.sendall(order(seq, 50, struct.pack("<3f", x, z, radius)))
			sock.sendall(order(seq + 1, 51, struct.pack("<2f", x, z)))
			sock.sendall(order(seq + 2, 41, struct.pack("<f", speed)))
			seq += 3
			sent = True
		if state["hello"] and now - start >= nextPing:
			sock.sendall(frame(4))
			nextPing = now - start + 2.0
		if state["hello"] and now - start >= nextProf:
			sock.sendall(order(seq, 120))
			seq += 1
			nextProf = now - start + 10.0
	gaps = sorted((b - a) * 1000.0 for a, b in zip(ends, ends[1:]))
	if gaps:
		print("snapshots %d in %.0f s (%.1f/s); gap ms p50 %.1f p99 %.1f max %.1f" % (len(ends), seconds, len(ends) / seconds,
			gaps[len(gaps) // 2], gaps[int(len(gaps) * 0.99)], gaps[-1]))
	print("t,tick,window,mean_us,p50_us,p99_us,max_us,budget_us,chars,squads,pathq,path_us")
	for row in profiles:
		print(",".join("%.1f" % v if isinstance(v, float) else str(v) for v in row))


main()
