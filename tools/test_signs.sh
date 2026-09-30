#!/usr/bin/env bash
# Sign editing on vanilla and on this server (tools/test_signs.py), then compares the sign data
# clients ended up with.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$(mktemp -d)}"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$OUT"' EXIT
echo "== vanilla"
"$ROOT/tools/run_vanilla_test.sh" tools/test_signs.py "$OUT/vanilla.json"
echo "== cmc"
"$ROOT/tools/run_test.sh" tools/test_signs.py "$OUT/cmc.json"
echo "== compare"
python3 "$ROOT/tools/test_signs.py" compare "$OUT/vanilla.json" "$OUT/cmc.json"
