#!/usr/bin/env bash
# Online-mode logins against vanilla and this server (tools/test_online.py), both pointed at the
# stand-in session services of tools/mock_services.py.
#   tools/test_online.sh            both servers
#   SERVERS=cmc tools/test_online.sh
# TEST=test_chat.py ENFORCE=true runs the secure chat test the same way (tools/test_chat.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
JAR="${JAR:-$ROOT/minecraft_server.26.3.jar}"
JAVA="${JAVA:-/usr/lib/jvm/java-25-openjdk/bin/java}"
PORT=${PORT:-25651}
SERVICES_PORT=${SERVICES_PORT:-25652}
SERVERS=${SERVERS:-"cmc vanilla"}
TEST=${TEST:-test_online.py}
ENFORCE=${ENFORCE:-false}
WORK="$(mktemp -d)"
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2> /dev/null || true; done
    wait 2> /dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT
python3 "$ROOT/tools/mock_services.py" "$SERVICES_PORT" &
PIDS+=($!)
DISCOVERY="http://127.0.0.1:$SERVICES_PORT/minecraft/client"
for _ in $(seq 100); do curl -sf "$DISCOVERY" > /dev/null && break; sleep 0.1; done  # (up before the servers ask)
status=0
for server in $SERVERS; do
    dir="$WORK/$server"
    mkdir -p "$dir"
    cd "$dir"
    echo "eula=true" > eula.txt
    printf 'server-port=%s\nonline-mode=true\nenforce-secure-profile=%s\nlevel-type=minecraft\\:flat\nwhite-list=false\nspawn-protection=0\n' "$PORT" "$ENFORCE" > server.properties
    # Ops by the accounts' ids (the session server's, not the offline ones).
    echo '[{"uuid":"0f4c2a7e-1b3d-4c5e-8f9a-0b1c2d3e4f50","name":"Alice","level":4,"bypassesPlayerLimit":false}]' > ops.json
    if [ "$server" = vanilla ]; then
        "$JAVA" -Xmx2G -Dminecraft.api.discovery.host="$DISCOVERY" -jar "$JAR" nogui < /dev/null > server.log 2>&1 &
    else
        MINECRAFT_API_DISCOVERY_HOST="$DISCOVERY" "$BIN" < /dev/null > server.log 2>&1 &
    fi
    pid=$!
    PIDS+=($pid)
    for _ in $(seq 600); do grep -q "Done (" server.log && break; sleep 0.1; done
    echo "== $server"
    if ! timeout 120 python3 "$ROOT/tools/$TEST" 127.0.0.1 "$PORT" "$SERVICES_PORT"; then
        status=1
        echo "--- server.log (tail)"
        tail -20 server.log
    fi
    kill "$pid" 2> /dev/null || true
    wait "$pid" 2> /dev/null || true
    cd "$ROOT"
done
exit $status
