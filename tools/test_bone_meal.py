#!/usr/bin/env python3
"""Bone meal: saplings grow into trees, crops ripen, a mushroom on mycelium becomes a huge
mushroom, mangrove leaves grow a propagule. Runs against vanilla too (OBS lines show the result).

    test_bone_meal.py HOST PORT
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import UP, Client, check, failures, state_str  # noqa: E402

GROUND = -61


def main():
    alice = Client(sys.argv[1], int(sys.argv[2]), "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    alice.command("time set noon")
    alice.pump(0.5)

    def setblock(dx, y, dz, block):
        alice.command(f"setblock {ox + dx} {y} {oz + dz} {block}")

    def state(dx, y, dz):
        return state_str(alice.blocks.get((ox + dx, y, oz + dz), 0))

    setblock(3, GROUND + 1, 0, "minecraft:oak_sapling")
    setblock(-3, GROUND, 0, "minecraft:farmland")
    setblock(-3, GROUND + 1, 0, "minecraft:wheat")
    setblock(0, GROUND, 3, "minecraft:mycelium")
    setblock(0, GROUND + 1, 3, "minecraft:red_mushroom")
    setblock(0, -57, -3, "minecraft:mangrove_leaves[persistent=true]")
    alice.creative_slot(36, "minecraft:bone_meal", 64)
    alice.pump(1.0)

    seq = 1

    def bone_meal(dx, y, dz, done, tries):
        nonlocal seq
        for _ in range(tries):
            if done():
                return True
            alice.use_item_on(dx, y, dz, UP, seq)
            seq += 1
            alice.pump(0.3)
        alice.pump(0.5)
        return done()

    grew = bone_meal(3, GROUND + 1, 0, lambda: state(3, GROUND + 1, 0).startswith("minecraft:oak_log"), 60)
    trunk = [state(3, y, 0) for y in range(GROUND, GROUND + 5)]
    print(f"OBS oak: {trunk}")
    check(grew, f"sapling grew into a tree: {trunk}")
    check(trunk[0] == "minecraft:dirt", f"dirt under the trunk: {trunk[0]}")
    leaves = [p for p, s in alice.blocks.items() if state_str(s).startswith("minecraft:oak_leaves")]
    check(len(leaves) > 10, f"leaves around it: {len(leaves)}")

    ripe = bone_meal(-3, GROUND + 1, 0, lambda: state(-3, GROUND + 1, 0) == "minecraft:wheat[age=7]", 10)
    check(ripe, f"wheat ripened: {state(-3, GROUND + 1, 0)}")

    huge = bone_meal(0, GROUND + 1, 3, lambda: state(0, GROUND + 1, 3).startswith("minecraft:mushroom_stem"), 60)
    cap = sorted({state_str(s) for p, s in alice.blocks.items() if "red_mushroom_block" in state_str(s)})
    print(f"OBS mushroom: {state(0, GROUND + 1, 3)} cap states {len(cap)}")
    check(huge, f"red mushroom grew huge: {state(0, GROUND + 1, 3)}")
    check(len(cap) > 0, "it has a cap")

    alice.use_item_on(0, -57, -3, 0, seq)  # the underside of the mangrove leaves
    seq += 1
    alice.pump(1.0)
    check(state(0, -58, -3).startswith("minecraft:mangrove_propagule[age=0,hanging=true"), f"hanging propagule: {state(0, -58, -3)}")
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all bone meal checks passed")


if __name__ == "__main__":
    main()
