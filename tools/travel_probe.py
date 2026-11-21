#!/usr/bin/env python3
"""SIMLINK probe for a long travel at speed (kenshi-k16.6.604): like heft_probe.py, but for a server started with
`--scenario newgame --start <sid>`. Takes the player squad from PLAYER_SQUADS (31), follows the leader with set_view / set_interest the way
the client does, gives one move_to DX units east of the first seen position, sets the speed, and logs t, tick, leader x/z and the distance
to the goal every 5 s, snapshots delivered, and PROFILE (order 120) every 10 s.
Usage: travel_probe.py PORT SECONDS SPEED DX [RADIUS]
"""
import socket, struct, sys, time


def frame(ftype, payload=b""):
	return struct.pack("<IH", 2 + len(payload), ftype) + payload


def order(seq, kind, body=b""):
	return frame(100, struct.pack("<IH", seq, kind) + body)


def main():
	port, seconds, speed, dx = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
	radius = float(sys.argv[5]) if len(sys.argv) > 5 else 2000.0
	sock = socket.create_connection(("127.0.0.1", port))
	sock.settimeout(0.05)
	buf = b""
	hello = False
	seq = 1
	ends, profiles, track, types = [], [], [], {}
	members = []
	pos = {}
	goal = None
	ordered = False
	start = time.monotonic()
	nextPing = nextProf = nextView = nextTrack = 0.0
	lastTick = 0
	sock.settimeout(0.02)
	while time.monotonic() - start < seconds:
		try:
			data = sock.recv(1 << 22)
		except socket.timeout:
			data = b""
		buf += data
		while len(buf) >= 6:
			(length,) = struct.unpack_from("<I", buf, 0)
			if len(buf) < 4 + length:
				break
			(ftype,) = struct.unpack_from("<H", buf, 4)
			payload = buf[6:4 + length]
			buf = buf[4 + length:]
			now = time.monotonic()
			types[ftype] = types.get(ftype, 0) + 1
			if ftype == 1 and not hello:
				magic, proto, role, dhash = struct.unpack_from("<IHBQ", payload, 0)
				name = b"travel_probe"
				sock.sendall(frame(1, struct.pack("<IHBQ", magic, proto, 0, dhash) + struct.pack("<H", len(name)) + name))
				hello = True
			elif ftype == 11:
				ends.append(now)
			elif ftype == 31 and not members:
				# u64 tick, u64 faction, u32 strid, u16 x5, then squads {u64 id, str, u16 m, u64 x m}
				at = 8 + 8 + 4 + 2 * 4
				(n,) = struct.unpack_from("<H", payload, at)
				at += 2
				for _ in range(n):
					at += 8
					(sl,) = struct.unpack_from("<H", payload, at)
					at += 2 + sl
					(m,) = struct.unpack_from("<H", payload, at)
					at += 2
					for _ in range(m):
						members.append(struct.unpack_from("<Q", payload, at)[0])
						at += 8
				print("members", members, file=sys.stderr)
			elif ftype == 21 and members:
				for off in (0, 8):
					(i,) = struct.unpack_from("<Q", payload, off)
					if i in members:
						x, y, z = struct.unpack_from("<3f", payload, off + 8)
						pos[i] = (x, y, z)
						break
			elif ftype == 70:
				tick, window = struct.unpack_from("<QI", payload, 0)
				lastTick = tick
				mean, p50, p99, mx, budget, chars, squads, pq, pus = struct.unpack_from("<9f", payload, 12)
				profiles.append((now - start, tick, window, mean, p50, p99, mx, budget, chars, squads, pq, pus))
			elif ftype == 3:
				print("ERROR frame", payload[2:], file=sys.stderr)
		now = time.monotonic()
		t = now - start
		if hello and t >= nextPing:
			sock.sendall(frame(4))
			nextPing = t + 2.0
		if hello and t >= nextProf:
			sock.sendall(order(seq, 120))
			seq += 1
			nextProf = t + 10.0
		if members and members[0] in pos:
			lx, ly, lz = pos[members[0]]
			if not ordered:
				goal = (lx + dx, lz)
				sock.sendall(order(seq, 41, struct.pack("<f", speed)))
				sock.sendall(order(seq + 1, 1, struct.pack("<H", len(members)) + b"".join(struct.pack("<Q", m) for m in members)
					+ struct.pack("<3fB", goal[0], goal[1], 0.0, 0)))
				seq += 2
				ordered = True
				print("ordered from", (lx, ly, lz), "to", goal, file=sys.stderr)
			if t >= nextView:
				sock.sendall(order(seq, 50, struct.pack("<3f", lx, lz, radius)))
				sock.sendall(order(seq + 1, 51, struct.pack("<2f", lx, lz)))
				seq += 2
				nextView = t + 0.5
			if t >= nextTrack:
				track.append((t, lastTick, lx, lz, ((goal[0] - lx) ** 2 + (goal[1] - lz) ** 2) ** 0.5 if goal else 0, len(ends)))
				nextTrack = t + 5.0
		elif hello and members and t >= nextView:
			# no dynamic yet: look wide at the origin-ish start so the body is streamed
			nextView = t + 1.0
	gaps = sorted((b - a) * 1000.0 for a, b in zip(ends, ends[1:]))
	if gaps:
		print("snapshots %d in %.0f s (%.1f/s); gap ms p50 %.1f p99 %.1f max %.1f" % (len(ends), seconds, len(ends) / seconds,
			gaps[len(gaps) // 2], gaps[int(len(gaps) * 0.99)], gaps[-1]))
	print("frame types", sorted(types.items()))
	print("track: t,tick,x,z,dist_to_goal,snapshots")
	for row in track:
		print(",".join("%.1f" % v if isinstance(v, float) else str(v) for v in row))
	print("t,tick,window,mean_us,p50_us,p99_us,max_us,budget_us,chars,squads,pathq,path_us")
	for row in profiles:
		print(",".join("%.1f" % v if isinstance(v, float) else str(v) for v in row))


main()
