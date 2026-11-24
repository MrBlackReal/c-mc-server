#!/usr/bin/env python3
"""Pistons: pushing, pulling (sticky), the 12 block limit, immovable and breakable blocks, slime
block structures, quasi-connectivity and short pulses. Checks settled outcomes and what blocks went
through, so it passes on vanilla too.

    test_pistons.py HOST PORT
    PISTON_DUMP=file: write every block state in the test area (relative to spawn) and what each
    went through, to diff against a vanilla run.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import check, failures  # noqa: E402
from test_redstone import HistoryClient  # noqa: E402

GROUND = -61
Y = GROUND + 1


def main():
    alice = HistoryClient(sys.argv[1], int(sys.argv[2]), "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    alice.command("gamerule advance_time false")
    alice.command("gamerule random_tick_speed 0")
    alice.command(f"fill {ox - 40} {Y} {oz + 4} {ox + 40} {Y + 5} {oz + 30} minecraft:air")
    alice.command(f"tp Alice {ox} {Y} {oz}")
    alice.pump(0.5)

    def at(dx, y, dz):
        return (ox + dx, y, oz + dz)

    def setblock(p, block):
        alice.command(f"setblock {p[0]} {p[1]} {p[2]} {block}")
        alice.pump(0.06)

    def state(p):
        return tb.state_str(alice.blocks.get(p, 0))

    def name(p):
        return state(p).split("[")[0]

    def went_through(p, s):
        return s in alice.history.get(p, [])

    # 1. A piston pushes three blocks and retracts without them; a sticky piston pulls its block back.
    a = (-36, 6)
    setblock(at(a[0], Y, a[1]), "minecraft:piston[facing=east]")
    for k in (1, 2, 3):
        setblock(at(a[0] + k, Y, a[1]), "minecraft:stone")
    setblock(at(a[0], Y, a[1] + 2), "minecraft:sticky_piston[facing=east]")
    setblock(at(a[0] + 1, Y, a[1] + 2), "minecraft:oak_planks")

    # 2. Twelve blocks can be pushed, thirteen can't; obsidian can't be pushed at all.
    b = (-36, 10)
    setblock(at(b[0], Y, b[1]), "minecraft:piston[facing=east]")
    for k in range(1, 13):
        setblock(at(b[0] + k, Y, b[1]), "minecraft:dirt")
    setblock(at(b[0], Y, b[1] + 2), "minecraft:piston[facing=east]")
    for k in range(1, 14):
        setblock(at(b[0] + k, Y, b[1] + 2), "minecraft:dirt")
    setblock(at(b[0], Y, b[1] + 4), "minecraft:piston[facing=east]")
    setblock(at(b[0] + 1, Y, b[1] + 4), "minecraft:obsidian")

    # 3. A torch in front of a block is broken (it drops), the block is pushed.
    c = (-36, 18)
    setblock(at(c[0], Y, c[1]), "minecraft:piston[facing=east]")
    setblock(at(c[0] + 1, Y, c[1]), "minecraft:stone")
    setblock(at(c[0] + 2, Y, c[1]), "minecraft:torch")

    # 4. A sticky piston pulls a slime block and the blocks stuck to it; an upward piston lifts a column.
    d = (-10, 6)
    # (a block up: slime on the ground would be stuck to it)
    setblock(at(d[0], Y + 1, d[1]), "minecraft:sticky_piston[facing=east]")
    setblock(at(d[0] + 1, Y + 1, d[1]), "minecraft:slime_block")
    setblock(at(d[0] + 1, Y + 2, d[1]), "minecraft:stone")
    setblock(at(d[0] + 1, Y + 1, d[1] + 1), "minecraft:gold_block")
    setblock(at(d[0], Y, d[1] + 4), "minecraft:piston[facing=up]")
    setblock(at(d[0], Y + 1, d[1] + 4), "minecraft:sand")
    setblock(at(d[0], Y + 2, d[1] + 4), "minecraft:stone")

    # 5. Quasi-connectivity: a redstone block diagonally above powers the piston, but only once
    # something updates it.
    e = (10, 6)
    setblock(at(e[0], Y, e[1]), "minecraft:piston[facing=east]")
    setblock(at(e[0] - 1, Y + 1, e[1]), "minecraft:redstone_block")

    # 6. Short pulses: an observer's 2-tick pulse into a sticky piston (it leaves its block behind:
    # the block finished moving first), and a piston pushing a redstone block that powers a lamp.
    f = (10, 12)
    setblock(at(f[0], Y, f[1]), "minecraft:sticky_piston[facing=east]")
    setblock(at(f[0] + 1, Y, f[1]), "minecraft:stone")
    setblock(at(f[0] - 1, Y, f[1]), "minecraft:observer[facing=west]")  # outputs east, into the piston
    setblock(at(f[0], Y, f[1] + 4), "minecraft:piston[facing=east]")
    setblock(at(f[0] + 1, Y, f[1] + 4), "minecraft:redstone_block")
    setblock(at(f[0] + 3, Y, f[1] + 4), "minecraft:redstone_lamp")
    alice.pump(1.0)

    # Power things up.
    for p in (at(a[0] - 1, Y, a[1]), at(a[0] - 1, Y, a[1] + 2), at(b[0] - 1, Y, b[1]), at(b[0] - 1, Y, b[1] + 2), at(b[0] - 1, Y, b[1] + 4),
              at(c[0] - 1, Y, c[1]), at(d[0] - 1, Y + 1, d[1]), at(d[0] + 1, Y, d[1] + 4), at(f[0] - 1, Y, f[1] + 4)):
        setblock(p, "minecraft:redstone_block")
    setblock(at(f[0] - 2, Y, f[1]), "minecraft:stone")  # the observer sees it and pulses
    alice.pump(1.0)
    pushed = [name(at(a[0] + k, Y, a[1])) for k in range(0, 5)]
    pulled_out = [name(at(a[0] + k, Y, a[1] + 2)) for k in range(0, 4)]
    slime_out = [state(at(d[0] + 2, Y + 1, d[1])), name(at(d[0] + 2, Y + 2, d[1])), name(at(d[0] + 2, Y + 1, d[1] + 1))]
    column = [name(at(d[0], Y + k, d[1] + 4)) for k in range(0, 5)]
    quasi_before = state(at(e[0], Y, e[1]))
    setblock(at(e[0], Y, e[1] + 1), "minecraft:stone")  # a neighbour update
    alice.pump(0.5)
    # ...and power off again.
    for p in (at(a[0] - 1, Y, a[1]), at(a[0] - 1, Y, a[1] + 2), at(d[0] - 1, Y + 1, d[1]), at(d[0] + 1, Y, d[1] + 4)):
        setblock(p, "minecraft:air")
    alice.pump(1.0)

    print(f"  OBS  pushed: {pushed}, after retracting: {[name(at(a[0] + k, Y, a[1])) for k in range(0, 5)]}")
    check(pushed == ["minecraft:piston", "minecraft:piston_head", "minecraft:stone", "minecraft:stone", "minecraft:stone"],
          "a piston pushes three blocks one block on")
    check([name(at(a[0] + k, Y, a[1])) for k in range(0, 5)] == ["minecraft:piston", "minecraft:air", "minecraft:stone", "minecraft:stone", "minecraft:stone"],
          "and leaves them there when it retracts")
    print(f"  OBS  sticky: out {pulled_out}, back {[name(at(a[0] + k, Y, a[1] + 2)) for k in range(0, 4)]}")
    check(pulled_out[2] == "minecraft:oak_planks" and name(at(a[0] + 1, Y, a[1] + 2)) == "minecraft:oak_planks", "a sticky piston pulls its block back")

    twelve = [name(at(b[0] + k, Y, b[1])) for k in range(0, 14)]
    thirteen = state(at(b[0], Y, b[1] + 2))
    print(f"  OBS  twelve: {twelve[:2]}..{twelve[-2:]}, thirteen: {thirteen}, obsidian: {state(at(b[0], Y, b[1] + 4))}")
    check(twelve[1] == "minecraft:piston_head" and twelve[13] == "minecraft:dirt", "twelve blocks can be pushed")
    check("extended=false" in thirteen, "thirteen can't")
    check("extended=false" in state(at(b[0], Y, b[1] + 4)) and name(at(b[0] + 1, Y, b[1] + 4)) == "minecraft:obsidian", "nor obsidian")

    print(f"  OBS  torch: {[name(at(c[0] + k, Y, c[1])) for k in range(0, 4)]}")
    check(name(at(c[0] + 2, Y, c[1])) == "minecraft:stone" and name(at(c[0] + 3, Y, c[1])) == "minecraft:air", "a torch in the way breaks")

    slime_back = [name(at(d[0] + 1, Y + 1, d[1])), name(at(d[0] + 1, Y + 2, d[1])), name(at(d[0] + 1, Y + 1, d[1] + 1))]
    print(f"  OBS  slime: out {slime_out}, back {slime_back}")
    check(slime_out[1:] == ["minecraft:stone", "minecraft:gold_block"], "a slime block takes the blocks stuck to it along")
    check(slime_back ==
          ["minecraft:slime_block", "minecraft:stone", "minecraft:gold_block"], "and pulls them back")
    print(f"  OBS  column: {column}, back {[name(at(d[0], Y + k, d[1] + 4)) for k in range(0, 5)]}")
    check(column[1:4] == ["minecraft:piston_head", "minecraft:sand", "minecraft:stone"], "an upward piston lifts a column")

    print(f"  OBS  quasi: before {quasi_before}, after {state(at(e[0], Y, e[1]))}")
    check("extended=false" in quasi_before and "extended=true" in state(at(e[0], Y, e[1])), "quasi-connectivity: powered from above once updated")

    print(f"  OBS  pulse: {[state(at(f[0] + k, Y, f[1])) for k in range(0, 3)]}")
    print(f"  OBS  pulse piston: {alice.history.get(at(f[0], Y, f[1]), [])}")
    print(f"  OBS  redstone block pushed: lamp {state(at(f[0] + 3, Y, f[1] + 4))}")
    check(state(at(f[0] + 3, Y, f[1] + 4)) == "minecraft:redstone_lamp[lit=true]", "a pushed redstone block powers what it arrives next to")

    if os.environ.get("PISTON_DUMP"):
        with open(os.environ["PISTON_DUMP"], "w") as out:
            def inside(p):
                return -40 <= p[0] - ox <= 40 and 4 <= p[2] - oz <= 30 and Y - 1 <= p[1] <= Y + 5
            for p in sorted(alice.blocks):
                if inside(p):
                    out.write(f"{p[0] - ox} {p[1]} {p[2] - oz} {state(p)}\n")
            for p in sorted(alice.history):
                if inside(p):
                    out.write(f"history {p[0] - ox} {p[1]} {p[2] - oz} {' > '.join(alice.history[p][:12])}\n")

    print("PASS" if not failures else f"FAIL ({len(failures)})")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
