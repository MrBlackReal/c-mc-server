#!/usr/bin/env bash
# Vanilla gives Alice items with many data components and restarts; its inventory packet is the
# reference. This server then loads the same world: its inventory packet must be byte-identical,
# and after it saves, Alice's inventory NBT must equal vanilla's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/cmc-server}"
JAR="${JAR:-$ROOT/minecraft_server.26.3.jar}"
JAVA="${JAVA:-/usr/lib/jvm/java-25-openjdk/bin/java}"
PORT=${PORT:-25661}
WORK="${WORK:-$(mktemp -d)}"
KEEP=${KEEP:-0}
SERVER=
cleanup() {
    [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null && wait "$SERVER" 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    [ "$KEEP" = 1 ] && echo "kept: $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT
start() {  # start KIND DIR LOG
    cd "$2"
    rm -f console; mkfifo console
    if [ "$1" = vanilla ]; then "$JAVA" -Xmx2G -jar "$JAR" nogui < console > "$3" 2>&1 &
    else "$BIN" < console > "$3" 2>&1 &
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
run() {  # run KIND DIR NAME ARGS...
    local kind=$1 dir=$2 name=$3; shift 3
    echo "== $kind: $name"
    start "$kind" "$dir" "$dir/server-$name.log"
    status=0
    timeout 120 python3 "$ROOT/tools/test_components.py" "$@" || status=$?
    stop
    [ $status -eq 0 ] || { echo "--- $dir/server-$name.log (tail)"; tail -30 "$dir/server-$name.log"; exit $status; }
}

V="$WORK/vanilla"; C="$WORK/cmc"
mkdir -p "$V"
echo "eula=true" > "$V/eula.txt"
printf 'server-port=%s\nonline-mode=false\nlevel-type=minecraft\\:flat\ngenerate-structures=false\nspawn-protection=0\nwhite-list=false\n' "$PORT" \
    > "$V/server.properties"
python3 -c 'import hashlib, json, uuid; h = bytearray(hashlib.md5(b"OfflinePlayer:Alice").digest()); h[6] = h[6] & 15 | 48; h[8] = h[8] & 63 | 128
print(json.dumps([{"uuid": str(uuid.UUID(bytes=bytes(h))), "name": "Alice", "level": 4, "bypassesPlayerLimit": False}]))' > "$V/ops.json"
run vanilla "$V" give give 127.0.0.1 "$PORT"
mkdir -p "$C"
cp -r "$V/world" "$V/eula.txt" "$V/server.properties" "$V/ops.json" "$C/"
run vanilla "$V" dump dump 127.0.0.1 "$PORT" "$WORK/vanilla.bin"
run cmc "$C" dump dump 127.0.0.1 "$PORT" "$WORK/cmc.bin"
echo "== compare"
python3 "$ROOT/tools/test_components.py" compare "$WORK/vanilla.bin" "$WORK/cmc.bin"
python3 "$ROOT/tools/test_components.py" disk "$V/world" "$C/world"

echo "== hashes (HashedPatchMap)"
CHECK="${CHECK:-$ROOT/build/cmc-codec-check}"
[ -x "$CHECK" ] || cmake --build "$ROOT/build" --target cmc-codec-check > /dev/null
CP=$(find "$V/versions" "$V/libraries" -name '*.jar' | tr '\n' ':')
mkdir -p "$WORK/hashcheck"
"$(dirname "$JAVA")/javac" -nowarn -cp "$CP" -d "$WORK/hashcheck" "$ROOT/tools/extractor/HashCheck.java" 2> /dev/null
DAT=$(ls "$V/world/players/data/"*.dat)
(cd "$WORK" && "$JAVA" -cp "$WORK/hashcheck:$CP" HashCheck "$DAT" 2> /dev/null | sed -n 's/.*\[STDOUT\]: //p' | sort > hash.vanilla)
"$CHECK" "$DAT" | sort > "$WORK/hash.cmc"
if [ -s "$WORK/hash.vanilla" ] && diff "$WORK/hash.vanilla" "$WORK/hash.cmc"; then
    echo "  ok   all $(wc -l < "$WORK/hash.cmc") component hashes match vanilla's"
else
    echo "  FAIL component hashes differ from vanilla's"
    exit 1
fi
