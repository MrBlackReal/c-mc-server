#!/usr/bin/env bash
# Build -> stop -> restart -> verify -> compare, on a fresh world in a temp dir.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
PORT=${PORT:-25641}
WORK="${WORK:-$(mktemp -d)}"
KEEP=${KEEP:-0}
export TMPDIR="$WORK"
cd "$WORK"
echo "eula=true" > eula.txt
printf 'server-port=%s\nonline-mode=false\n' "$PORT" > server.properties
SERVER=
start() {
    "$BIN" < /dev/null >> server.log 2>&1 &
    SERVER=$!
    for _ in $(seq 100); do grep -q "Done (" server.log && break; sleep 0.1; done
}
stop() {
    kill -INT "$SERVER"; wait "$SERVER" || true
    : > server.log.done; mv server.log "server.log.$(date +%s%N)"
}
cleanup() { [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null || true; [ "$KEEP" = 1 ] || rm -rf "$WORK"; }
trap cleanup EXIT

start; python3 "$ROOT/tools/test_persistence.py" build 127.0.0.1 "$PORT"; stop
start; python3 "$ROOT/tools/test_persistence.py" verify 127.0.0.1 "$PORT"
python3 "$ROOT/tools/test_persistence.py" compare 127.0.0.1 "$PORT" "$WORK/world"; stop
echo "world kept in: $WORK (KEEP=$KEEP)"
