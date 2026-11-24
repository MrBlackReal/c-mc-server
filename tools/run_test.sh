#!/usr/bin/env bash
# Runs one test script against a fresh server in a temp dir: tools/run_test.sh tools/test_building.py
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
PORT=${PORT:-25631}
WORK="$(mktemp -d)"
cd "$WORK"
echo "eula=true" > eula.txt
printf 'server-port=%s\nonline-mode=false\n' "$PORT" > server.properties
python3 "$ROOT/tools/ops_json.py" Alice Bob > ops.json # (the tests use op commands)
"$BIN" ${SERVER_ARGS:-} < /dev/null > server.log 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true; wait $SERVER 2>/dev/null || true; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do grep -q "Done (" server.log && break; sleep 0.1; done
status=0
timeout ${TIMEOUT:-300} python3 "$ROOT/$1" 127.0.0.1 "$PORT" "${@:2}" || status=$?
if [ $status -ne 0 ]; then echo "--- server.log (tail)"; tail -30 server.log; fi
exit $status
