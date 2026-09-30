#!/usr/bin/env bash
# Runs one test script against a fresh vanilla 26.3 server (superflat, Alice and Bob opped):
#   tools/run_vanilla_test.sh tools/test_building.py
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JAR="${JAR:-$ROOT/minecraft_server.26.3.jar}"
JAVA="${JAVA:-/usr/lib/jvm/java-25-openjdk/bin/java}"
PORT=${PORT:-25671}
WORK="$(mktemp -d)"
cd "$WORK"
echo "eula=true" > eula.txt
printf 'server-port=%s\nonline-mode=false\nlevel-type=minecraft\\:flat\ngenerate-structures=false\nspawn-protection=0\nwhite-list=false\n' "$PORT" \
    > server.properties
# Ops by offline-mode UUID (vanilla assigns it and ignores the one the client sends).
python3 -c 'import hashlib, json, uuid
def offline(n):
    h = bytearray(hashlib.md5(b"OfflinePlayer:" + n.encode()).digest()); h[6] = h[6] & 15 | 48; h[8] = h[8] & 63 | 128
    return str(uuid.UUID(bytes=bytes(h)))
print(json.dumps([{"uuid": offline(n), "name": n, "level": 4, "bypassesPlayerLimit": False} for n in ("Alice", "Bob")]))' > ops.json
"$JAVA" -Xmx2G -jar "$JAR" nogui < /dev/null > server.log 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true; wait $SERVER 2>/dev/null || true; rm -rf "$WORK"' EXIT
for _ in $(seq 600); do grep -q "Done (" server.log && break; sleep 0.1; done
status=0
timeout ${TIMEOUT:-300} python3 "$ROOT/$1" 127.0.0.1 "$PORT" "${@:2}" || status=$?
if [ $status -ne 0 ]; then echo "--- server.log (tail)"; tail -30 server.log; fi
exit $status
