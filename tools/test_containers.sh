#!/usr/bin/env bash
# Vanilla fills containers in a fresh superflat world; then vanilla and this server each break them,
# restart, place the shulker box again, stop, and the saved world is checked.
#   tools/test_containers.sh              both servers
#   SERVERS=cmc tools/test_containers.sh  only this one (still needs vanilla for the setup)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
JAR="${JAR:-$ROOT/minecraft_server.26.3.jar}"
JAVA="${JAVA:-/usr/lib/jvm/java-25-openjdk/bin/java}"
PORT=${PORT:-25651}
SERVERS=${SERVERS:-"vanilla cmc"}
WORK="${WORK:-$(mktemp -d)}"
KEEP=${KEEP:-0}
SERVER=
cleanup() {
    [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null && wait "$SERVER" 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    [ "$KEEP" = 1 ] && echo "kept: $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

# start KIND DIR LOG: runs the server in DIR with a console on fd 3
start() {
    cd "$2"
    rm -f console; mkfifo console
    if [ "$1" = vanilla ]; then
        "$JAVA" -Xmx2G -jar "$JAR" nogui < console > "$3" 2>&1 &
    else
        "$BIN" < console > "$3" 2>&1 &
    fi
    SERVER=$!
    exec 3> console
    for _ in $(seq 600); do grep -q "Done (" "$3" && return; sleep 0.1; done
    echo "server in $2 didn't start"; tail -20 "$3"; exit 1
}
stop() {
    echo stop >&3; exec 3>&-
    for _ in $(seq 300); do kill -0 "$SERVER" 2>/dev/null || break; sleep 0.1; done
    kill "$SERVER" 2>/dev/null && echo "server didn't stop; killed"
    wait "$SERVER" || true
    SERVER=
}
phase() {  # phase KIND DIR NAME
    echo "== $1: $3"
    start "$1" "$2" "$2/server-$3.log"
    status=0
    timeout 120 python3 "$ROOT/tools/test_containers.py" "$3" 127.0.0.1 "$PORT" || status=$?
    stop
    [ $status -eq 0 ] || { echo "--- $2/server-$3.log (tail)"; tail -30 "$2/server-$3.log"; exit $status; }
}

mkdir -p "$WORK/setup"
echo "eula=true" > "$WORK/setup/eula.txt"
printf 'server-port=%s\nonline-mode=false\nlevel-type=minecraft\\:flat\ngenerate-structures=false\nspawn-protection=0\nwhite-list=false\n' "$PORT" \
    > "$WORK/setup/server.properties"
# Ops Alice by her offline-mode UUID (what vanilla assigns; the one the client sends is ignored).
python3 -c 'import hashlib, json, uuid; h = bytearray(hashlib.md5(b"OfflinePlayer:Alice").digest()); h[6] = h[6] & 15 | 48; h[8] = h[8] & 63 | 128
print(json.dumps([{"uuid": str(uuid.UUID(bytes=bytes(h))), "name": "Alice", "level": 4, "bypassesPlayerLimit": False}]))' \
    > "$WORK/setup/ops.json"
phase vanilla "$WORK/setup" setup

for kind in $SERVERS; do
    mkdir -p "$WORK/$kind"
    cp -r "$WORK/setup/world" "$WORK/setup/eula.txt" "$WORK/setup/server.properties" "$WORK/setup/ops.json" "$WORK/$kind/"
    phase "$kind" "$WORK/$kind" break
    phase "$kind" "$WORK/$kind" place
    echo "== $kind: verify"
    python3 "$ROOT/tools/test_containers.py" verify "$WORK/$kind/world"
done
