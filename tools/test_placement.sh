#!/usr/bin/env bash
# Places the same blocks on vanilla and on this server (tools/test_placement.py) and compares
# every resulting block state.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$(mktemp -d)}"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$OUT"' EXIT
echo "== vanilla"
"$ROOT/tools/run_vanilla_test.sh" tools/test_placement.py "$OUT/vanilla.json"
echo "== cmc"
"$ROOT/tools/run_test.sh" tools/test_placement.py "$OUT/cmc.json"
echo "== compare"
python3 "$ROOT/tools/test_placement.py" compare "$OUT/vanilla.json" "$OUT/cmc.json"
