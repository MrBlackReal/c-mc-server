#!/usr/bin/env bash
# Grows every sapling tree and huge mushroom feature with the same seeds here (cmc-feature-check)
# and in vanilla's own code (tools/extractor/FeatureCheck.java) and diffs the blocks placed.
#
#   tools/feature_check.sh [seeds per feature] [feature ...]
#
# Needs the unpacked server jar and libraries: BUNDLE=<dir with META-INF/versions and libraries>
# (unzip minecraft_server.26.3.jar), and Java 25 (JAVA=...).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JAVA="${JAVA:-/usr/lib/jvm/java-25-openjdk/bin/java}"
BUNDLE="${BUNDLE:?set BUNDLE to the unpacked server jar}"
SEEDS=${1:-50}
shift || true
FEATURES=("$@")
if [ ${#FEATURES[@]} -eq 0 ]; then
    FEATURES=(oak fancy_oak oak_bees_005 fancy_oak_bees_005 spruce mega_spruce mega_pine mangrove tall_mangrove azalea_tree birch
              birch_bees_005 jungle_tree_no_vine mega_jungle_tree acacia cherry cherry_bees_005 dark_oak pale_oak_bonemeal red_poplar
              orange_poplar yellow_poplar huge_red_mushroom huge_brown_mushroom)
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
CP=$(find "$BUNDLE" -name "*.jar" | tr '\n' ':')
"$(dirname "$JAVA")/javac" -nowarn -cp "$CP" -d "$WORK/classes" "$ROOT/tools/extractor/FeatureCheck.java" 2>&1 | grep -v "^Note:" || true
cmake --build "$ROOT/build" --target cmc-feature-check -j4 > /dev/null
failed=0
for f in "${FEATURES[@]}"; do
    "$ROOT/build/cmc-feature-check" "minecraft:$f" 1 "$SEEDS" > "$WORK/ours.txt" 2> "$WORK/ours.err"
    # (vanilla's logger prefixes System.out lines with "[time] [main/INFO]: [STDOUT]: ")
    (cd "$WORK" && "$JAVA" -cp "$WORK/classes:$CP" FeatureCheck "$ROOT/data" "minecraft:$f" 1 "$SEEDS" > "$WORK/java.out" 2> "$WORK/java.err") \
        || { tail -20 "$WORK/java.out" "$WORK/java.err"; exit 1; }
    sed -n 's/^.*\[STDOUT\]: //p' "$WORK/java.out" > "$WORK/vanilla.txt"
    placed=$(grep -c "placed=1" "$WORK/vanilla.txt" || true)
    if diff -q "$WORK/ours.txt" "$WORK/vanilla.txt" > /dev/null; then
        echo "  ok   $f ($placed/$SEEDS placed)"
    else
        echo "  FAIL $f"
        diff "$WORK/ours.txt" "$WORK/vanilla.txt" | head -20 || true
        failed=$((failed + 1))
    fi
done
[ $failed -eq 0 ] && echo "all features match vanilla" || { echo "$failed feature(s) differ"; exit 1; }
