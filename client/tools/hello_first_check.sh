#!/bin/bash
# HELLO is always the first simlink frame (kenshi-k16.2.406.1). A fake server holds its own HELLO back DELAY s; kclient --play gate_heft
# issues the playtest load (type 103) as soon as the socket is up, i.e. BEFORE the server's HELLO is decoded. The fake server records the
# type of every frame the client sends: the first must be HELLO (1) and the load (103) must follow it, not be dropped.
# Usage: client/tools/hello_first_check.sh [PORT]   (binary client/build/kclient; port in 47830-47839). PASS / FAIL, exit 0 / 1.
set -u
tree=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
port=${1:-47838}
out=$(mktemp -d)
cat > "$out/fake.py" <<'PY'
import socket, struct, sys, time
port, delay, log = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port)); s.listen(1); s.settimeout(120)
c, _ = s.accept()
time.sleep(delay)
name = b"fake"
body = struct.pack("<HIHBQ", 1, 0x4D49534B, 7, 1, 0) + struct.pack("<H", len(name)) + name
c.sendall(struct.pack("<I", len(body)) + body)
buf = b""; types = []
c.settimeout(1.0)
end = time.time() + 15
while time.time() < end and 103 not in types:
    try:
        d = c.recv(65536)
    except socket.timeout:
        continue
    if not d: break
    buf += d
    while len(buf) >= 6:
        n = struct.unpack("<I", buf[:4])[0]
        if len(buf) < 4 + n: break
        types.append(struct.unpack("<H", buf[4:6])[0]); buf = buf[4 + n:]
open(log, "w").write(" ".join(map(str, types)) + "\n")
PY
python3 "$out/fake.py" "$port" 2 "$out/types.txt" &
srv=$!
sleep 0.5
cd "$tree" && HEADLESS_TIMEOUT=40 timeout 60 nice -n 10 client/tools/headless.sh -- client/build/kclient --simlink 127.0.0.1:$port --play gate_heft --size 640x360 > "$out/client.log" 2>&1
wait $srv
types=$(cat "$out/types.txt" 2>/dev/null)
echo "frames the client sent: ${types:-none}   (logs: $out)"
set -- $types
if [ "${1:-}" = 1 ] && [[ " $types " == *" 103 "* ]]; then echo "PASS: HELLO first, the load followed"; exit 0; fi
echo "FAIL: first frame ${1:-none}, load 103 present: $([[ " $types " == *" 103 "* ]] && echo yes || echo no)"; exit 1
