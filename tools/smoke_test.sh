#!/usr/bin/env bash
# Starts the built server in a temp directory, joins it with two probe clients and checks that
# they log in, receive chunks, see each other and receive chat.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
PORT=${PORT:-25611}
WORK="$(mktemp -d)"
trap 'kill $SERVER 2>/dev/null || true; wait $SERVER 2>/dev/null || true; rm -rf "$WORK"' EXIT

cd "$WORK"
echo "eula=true" > eula.txt
printf 'server-port=%s\nonline-mode=false\n' "$PORT" > server.properties
"$BIN" < /dev/null > server.log 2>&1 &
SERVER=$!
for _ in $(seq 50); do grep -q "Done (" server.log && break; sleep 0.1; done

python3 "$ROOT/tools/mcprobe.py" status 127.0.0.1 "$PORT" > /dev/null
python3 "$ROOT/tools/mcprobe.py" join 127.0.0.1 "$PORT" --name Alice --seconds 4 --walk > alice.log &
ALICE=$!
sleep 1
python3 "$ROOT/tools/mcprobe.py" join 127.0.0.1 "$PORT" --name Bob --seconds 2 --chat "smoke test" > bob.log
wait $ALICE

fail=0
check() { grep -q "$2" "$1" || { echo "FAIL: $1 missing '$2'"; fail=1; }; }
check alice.log "RESULT: OK"
check bob.log "RESULT: OK"
check bob.log "add_entity"                # Bob sees Alice
check alice.log "add_entity"              # Alice sees Bob
check alice.log "smoke test"              # chat reached Alice
check alice.log "forget_level_chunk"      # chunk tracking follows movement
check bob.log '"entity_position_sync": [1-9][0-9]'   # Alice's movement reaches Bob
if [ $fail -ne 0 ]; then echo "--- server.log"; cat server.log; exit 1; fi
echo "smoke test passed"
