#!/usr/bin/env bash
# Light and heightmaps against vanilla (tools/test_light.py): both servers build the same lit
# structures and must send identical light; then each opens the world the other saved and must
# send that same light again (saved light is trusted, heightmaps are loaded).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
JAR="${JAR:-$ROOT/minecraft_server.26.3.jar}"
JAVA="${JAVA:-/usr/lib/jvm/java-25-openjdk/bin/java}"
PORT=${PORT:-25683}
WORK="$(mktemp -d)"
export TMPDIR="$WORK"
SERVER=
cleanup() { [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null || true; [ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"; }
trap cleanup EXIT
cd "$WORK"
echo "eula=true" > eula.txt
python3 -c 'import hashlib, json, uuid
def offline(n):
    h = bytearray(hashlib.md5(b"OfflinePlayer:" + n.encode()).digest()); h[6] = h[6] & 15 | 48; h[8] = h[8] & 63 | 128
    return str(uuid.UUID(bytes=bytes(h)))
print(json.dumps([{"uuid": offline(n), "name": n, "level": 4, "bypassesPlayerLimit": False} for n in ("Alice", "Bob")]))' > ops.json
printf 'server-port=%s\nonline-mode=false\nlevel-type=minecraft\\:flat\ngenerate-structures=false\nspawn-protection=0\nwhite-list=false\n' "$PORT" > server.properties
start() { # ours|vanilla
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
run() { timeout 300 python3 "$ROOT/tools/test_light.py" 127.0.0.1 "$PORT" "$@"; }

echo "== vanilla builds"
start vanilla; run "$WORK/vanilla.json"; stop vanilla
echo "== this server opens vanilla's world"
start ours; run "$WORK/ours_on_vanilla.json" record; stop ours
rm -rf world
echo "== this server builds"
start ours; run "$WORK/ours.json"; stop ours
echo "== vanilla opens this server's world"
start vanilla; run "$WORK/vanilla_on_ours.json" record; stop vanilla

status=0
echo "== compare: built"; python3 "$ROOT/tools/test_light.py" compare "$WORK/vanilla.json" "$WORK/ours.json" || status=1
echo "== compare: vanilla's world here"; python3 "$ROOT/tools/test_light.py" compare "$WORK/vanilla.json" "$WORK/ours_on_vanilla.json" || status=1
echo "== compare: this server's world in vanilla"; python3 "$ROOT/tools/test_light.py" compare "$WORK/ours.json" "$WORK/vanilla_on_ours.json" || status=1
[ $status = 0 ] && echo "light test passed"
exit $status
