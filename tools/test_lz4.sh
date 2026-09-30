#!/usr/bin/env bash
# region-file-compression=lz4 across servers: this server builds and saves an LZ4 world that
# vanilla then loads (tools/test_persistence.py verify), and the other way round.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
JAR="${JAR:-$ROOT/minecraft_server.26.3.jar}"
JAVA="${JAVA:-/usr/lib/jvm/java-25-openjdk/bin/java}"
PORT=${PORT:-25681}
WORK="$(mktemp -d)"
export TMPDIR="$WORK"
SERVER=
cleanup() { [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT
cd "$WORK"
echo "eula=true" > eula.txt
python3 -c 'import hashlib, json, uuid
def offline(n):
    h = bytearray(hashlib.md5(b"OfflinePlayer:" + n.encode()).digest()); h[6] = h[6] & 15 | 48; h[8] = h[8] & 63 | 128
    return str(uuid.UUID(bytes=bytes(h)))
print(json.dumps([{"uuid": offline(n), "name": n, "level": 4, "bypassesPlayerLimit": False} for n in ("Alice", "Bob")]))' > ops.json
props() {
    printf 'server-port=%s\nonline-mode=false\nlevel-type=minecraft\\:flat\ngenerate-structures=false\nspawn-protection=0\nwhite-list=false\nregion-file-compression=lz4\n' "$PORT" > server.properties
}
start() { # ours|vanilla
    props
    : > server.log
    if [ "$1" = ours ]; then "$BIN" < /dev/null > server.log 2>&1 & else "$JAVA" -Xmx2G -jar "$JAR" nogui < /dev/null > server.log 2>&1 & fi
    SERVER=$!
    for _ in $(seq 600); do grep -q "Done (" server.log && break; sleep 0.1; done
}
stop() {
    if [ "$1" = ours ]; then kill -INT "$SERVER"; else kill -TERM "$SERVER"; fi
    wait "$SERVER" || true
    SERVER=
}
lz4_chunks() { # how many chunks in the overworld's region files use compression 4
    python3 - "$WORK/world/dimensions/minecraft/overworld/region" <<'EOF'
import glob, os, struct, sys
n = total = 0
for f in glob.glob(os.path.join(sys.argv[1], "*.mca")):
    d = open(f, "rb").read()
    for i in range(1024):
        off = struct.unpack(">I", d[i * 4:i * 4 + 4])[0]
        if not off:
            continue
        start = (off >> 8) * 4096
        total += 1
        n += (d[start + 4] & 0x7F) == 4
print(n, total)
EOF
}

echo "== this server writes, vanilla reads"
start ours; python3 "$ROOT/tools/test_persistence.py" build 127.0.0.1 "$PORT"; stop ours
echo "LZ4 chunks: $(lz4_chunks)"
start vanilla; python3 "$ROOT/tools/test_persistence.py" verify 127.0.0.1 "$PORT"; stop vanilla

echo "== vanilla writes, this server reads"
rm -rf world
start vanilla; python3 "$ROOT/tools/test_persistence.py" build 127.0.0.1 "$PORT"; stop vanilla
echo "LZ4 chunks: $(lz4_chunks)"
start ours; python3 "$ROOT/tools/test_persistence.py" verify 127.0.0.1 "$PORT"; stop ours
echo "lz4 test passed"
