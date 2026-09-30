#!/usr/bin/env bash
# A client without the vanilla pack joins vanilla and this server (tools/test_registries.py); the
# registry elements both sent must match.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$(mktemp -d)}"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$OUT"' EXIT
echo "== vanilla"
"$ROOT/tools/run_vanilla_test.sh" tools/test_registries.py "$OUT/vanilla.json"
echo "== cmc"
"$ROOT/tools/run_test.sh" tools/test_registries.py "$OUT/cmc.json"
echo "== compare"
python3 "$ROOT/tools/test_registries.py" compare "$OUT/vanilla.json" "$OUT/cmc.json"
