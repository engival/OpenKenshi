#!/usr/bin/env python3
"""Raw-socket SIMLINK probe for the far-walk stall measurement (kenshi-k16.6.699; docs/PROFILING.md "Slow-tick log").

Like heft_probe.py, plus: after a start delay it sends a move_to for the player character (the id read from CHAR_DYNAMIC's player flag) and
writes every SNAPSHOT_END arrival and the player's CHAR_DYNAMIC arrivals (wall time, sim tick, x, z) to OUT.
Usage: walk_probe.py PORT SECONDS SPEED GOALX GOALZ OUT [CX CZ RADIUS]
"""
import socket
import struct
import sys
import time


def frame(ftype, payload=b""):
	return struct.pack("<IH", 2 + len(payload), ftype) + payload


def main():
	port, seconds, speed, gx, gz = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
	out = open(sys.argv[6], "w")
	cx, cz, radius = (float(sys.argv[7]), float(sys.argv[8]), float(sys.argv[9])) if len(sys.argv) > 9 else (67122.9, -72113.6, 2000.0)
	sock = socket.create_connection(("127.0.0.1", port))
	sock.settimeout(0.02)
	buf = b""
	hello = False
	pid = None
	seq = 1
	start = time.monotonic()
	nextPing = 0.0
	sent = moved = False
	moveAt = 0.0
	followAt = 0.0
	print("t,kind,tick,x,z", file=out)
	while time.monotonic() - start < seconds:
		try:
			data = sock.recv(1 << 20)
		except socket.timeout:
			data = b""
		now = time.monotonic() - start
		buf += data
		while len(buf) >= 4:
			(length,) = struct.unpack_from("<I", buf, 0)
			if len(buf) < 4 + length:
				break
			(ftype,) = struct.unpack_from("<H", buf, 4)
			payload = buf[6:4 + length]
			buf = buf[4 + length:]
			if ftype == 1 and not hello:
				magic, proto, role, dhash = struct.unpack_from("<IHBQ", payload, 0)
				name = b"walk_probe"
				sock.sendall(frame(1, struct.pack("<IHBQ", magic, proto, 0, dhash) + struct.pack("<H", len(name)) + name))
				hello = True
			elif ftype == 11:
				print("%.4f,end,,," % now, file=out)
			elif ftype == 21 and len(payload) >= 44:
				tick, cid, x, y, z = struct.unpack_from("<QQfff", payload, 0)
				(flags,) = struct.unpack_from("<I", payload, 40)
				if flags & (1 << 6):
					pid = pid or cid
					if cid == pid:
						print("%.4f,dyn,%d,%.1f,%.1f" % (now, tick, x, z), file=out)
						cx, cz = x, z
			elif ftype == 3:
				print("ERROR frame", payload[2:], file=sys.stderr)
		if hello and not sent:
			sock.sendall(frame(100, struct.pack("<IH", seq, 50) + struct.pack("<3f", cx, cz, radius)))
			sock.sendall(frame(100, struct.pack("<IH", seq + 1, 51) + struct.pack("<2f", cx, cz)))
			sock.sendall(frame(100, struct.pack("<IH", seq + 2, 41) + struct.pack("<f", speed)))
			seq += 3
			sent = True
			moveAt = now + 5.0
		if sent and pid and not moved and now >= moveAt:
			sock.sendall(frame(100, struct.pack("<IH", seq, 1) + struct.pack("<HQ", 1, pid) + struct.pack("<fffB", gx, gz, 0.0, 0)))
			seq += 1
			moved = True
			print("%.4f,move,,%.1f,%.1f" % (now, gx, gz), file=out)
		if hello and now >= nextPing:
			sock.sendall(frame(4))
			nextPing = now + 2.0
		# keep the interest / view on the walker, as the client's follow does
		if moved and now >= followAt:
			sock.sendall(frame(100, struct.pack("<IH", seq, 50) + struct.pack("<3f", cx, cz, radius)))
			sock.sendall(frame(100, struct.pack("<IH", seq + 1, 51) + struct.pack("<2f", cx, cz)))
			seq += 2
			followAt = now + 1.0
	out.close()


main()
