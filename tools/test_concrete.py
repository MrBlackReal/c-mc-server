#!/usr/bin/env python3
"""Concrete powder falling fast into deep water hardens where it enters the water (the clip from
its previous position), not where it happens to be at the end of the tick. Runs against vanilla too.

    test_concrete.py HOST PORT
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client, check, failures, state_str  # noqa: E402


def main():
    alice = Client(sys.argv[1], int(sys.argv[2]), "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    x, z = ox + 4, oz + 4
    alice.command(f"fill {x - 2} -64 {z - 2} {x + 2} -52 {z + 2} minecraft:stone")   # a basin...
    alice.command(f"fill {x - 1} -63 {z - 1} {x + 1} -52 {z + 1} minecraft:water")   # ...full of water
    alice.pump(2.0)
    alice.command(f"setblock {x} -10 {z} minecraft:red_concrete_powder")
    alice.pump(6.0)
    found = [y for y in range(-63, -51) if "red_concrete" in state_str(alice.blocks.get((x, y, z), 0))]
    print(f"OBS concrete at y {found}")
    check(found and state_str(alice.blocks[(x, found[0], z)]) == "minecraft:red_concrete", f"hardened: {found}")
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all concrete checks passed")


if __name__ == "__main__":
    main()
