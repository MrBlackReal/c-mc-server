#!/usr/bin/env python3
"""ClientboundUpdateRecipesPacket: recipe property sets and stonecutter recipes. Runs against
vanilla too.

    test_update_recipes.py HOST PORT OUT.json
    test_update_recipes.py compare A.json B.json   same sets and the same stonecutter entries
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client, check, failures  # noqa: E402


class RecipeClient(Client):
    recipes = None

    def _handle(self, n, r):
        if n == "update_recipes":
            sets = {}
            for _ in range(r.varint()):
                name = r.str()
                sets[name] = sorted(tb.ITEM_NAME[r.varint()] for _ in range(r.varint()))
            cutting = []
            for _ in range(r.varint()):
                k = r.varint()
                ingredient = "#" + r.str() if k == 0 else sorted(tb.ITEM_NAME[r.varint()] for _ in range(k - 1))
                display = r.varint()
                result = (tb.ITEM_NAME[r.varint()], r.varint(), r.varint(), r.varint()) if display == 5 else display
                cutting.append([ingredient, result])
            self.recipes = {"sets": sets, "stonecutter": sorted(json.dumps(c) for c in cutting)}
        else:
            super()._handle(n, r)


def main():
    if sys.argv[1] == "compare":
        a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))
        for name in sorted(set(a["sets"]) | set(b["sets"])):
            check(a["sets"].get(name) == b["sets"].get(name), f"{name}: {len(a['sets'].get(name) or [])} vs {len(b['sets'].get(name) or [])} items")
        check(a["stonecutter"] == b["stonecutter"], f"stonecutter: {len(a['stonecutter'])} vs {len(b['stonecutter'])} recipes, "
              f"{sorted(set(a['stonecutter']) ^ set(b['stonecutter']))[:3]}")
    else:
        c = RecipeClient(sys.argv[1], int(sys.argv[2]), "Alice")
        c.pump(until=lambda: c.recipes is not None, timeout=5)
        check(c.recipes is not None, "got update_recipes")
        json.dump(c.recipes, open(sys.argv[3], "w"))
    print()
    print(f"{len(failures)} check(s) failed" if failures else "all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
