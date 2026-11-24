#!/usr/bin/env python3
"""Hoppers, droppers and dispensers: items moving between containers (read back through
comparators), hoppers locked by redstone, hoppers feeding furnaces and composters and picking up
dropped items, droppers filling a chest, and dispensers placing and picking up water, lighting
fires and growing crops. Checks settled outcomes, so it passes on vanilla too.

    test_hoppers.py HOST PORT
    HOPPER_DUMP=file: every block state in the test area (relative to spawn), to diff against vanilla.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import check, failures  # noqa: E402
from test_redstone import HistoryClient  # noqa: E402

GROUND = -61
Y = GROUND + 1


def items(item, count, slots=1):
    return "{Items:[" + ",".join(f'{{Slot:{k}b,id:"{item}",count:{count}}}' for k in range(slots)) + "]}"


def main():
    alice = HistoryClient(sys.argv[1], int(sys.argv[2]), "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    alice.command("gamerule advance_time false")
    alice.command("gamerule random_tick_speed 0")
    alice.command(f"fill {ox - 30} {Y} {oz + 4} {ox + 30} {Y + 5} {oz + 30} minecraft:air")
    alice.command(f"tp Alice {ox} {Y} {oz}")
    alice.pump(0.5)

    def at(dx, y, dz):
        return (ox + dx, y, oz + dz)

    def setblock(p, block):
        alice.command(f"setblock {p[0]} {p[1]} {p[2]} {block}")
        alice.pump(0.06)

    def state(p):
        return tb.state_str(alice.blocks.get(p, 0))

    def power(p):
        s = state(p)
        return int(s.split("power=")[1].split(",")[0].rstrip("]")) if "power=" in s else None

    def comparator_reading(p, out):  # a comparator facing away from p, a wire after it
        setblock(out[1], "minecraft:redstone_wire")
        setblock(out[0], f"minecraft:comparator[facing={out[2]}]")

    # 1. A chest of 16 ender pearls drains through two hoppers into a third (read by a comparator: 3).
    a = (-26, 8)
    setblock(at(a[0], Y + 2, a[1]), "minecraft:chest" + items("minecraft:ender_pearl", 16))
    setblock(at(a[0], Y + 1, a[1]), "minecraft:hopper[facing=east]")
    setblock(at(a[0] + 1, Y + 1, a[1]), "minecraft:hopper[facing=down]")
    setblock(at(a[0] + 1, Y, a[1]), "minecraft:hopper[facing=down]")
    comparator_reading(None, (at(a[0] + 2, Y, a[1]), at(a[0] + 3, Y, a[1]), "west"))
    # ...and the same chain with the middle hopper locked by a redstone block: nothing gets through.
    setblock(at(a[0], Y + 2, a[1] + 4), "minecraft:chest" + items("minecraft:ender_pearl", 16))
    setblock(at(a[0], Y + 1, a[1] + 4), "minecraft:hopper[facing=east]")
    setblock(at(a[0] + 1, Y + 2, a[1] + 4), "minecraft:redstone_block")
    setblock(at(a[0] + 1, Y + 1, a[1] + 4), "minecraft:hopper[facing=down]")
    setblock(at(a[0] + 1, Y, a[1] + 4), "minecraft:hopper[facing=down]")
    comparator_reading(None, (at(a[0] + 2, Y, a[1] + 4), at(a[0] + 3, Y, a[1] + 4), "west"))

    # 2. Hoppers feed a furnace: raw iron from above, coal from the side; the iron smelts.
    b = (-14, 8)
    setblock(at(b[0], Y, b[1]), "minecraft:furnace[facing=south]")
    setblock(at(b[0], Y + 1, b[1]), "minecraft:hopper[facing=down]" + items("minecraft:raw_iron", 2))
    setblock(at(b[0] - 1, Y, b[1]), "minecraft:hopper[facing=east]" + items("minecraft:coal", 1))
    setblock(at(b[0], Y - 1, b[1]), "minecraft:hopper[facing=down]")
    comparator_reading(None, (at(b[0] + 1, Y - 1, b[1] + 1), at(b[0] + 2, Y - 1, b[1] + 1), "west"))  # (unused)

    # 3. A hopper fills a composter with pumpkin pies (always a layer each); a full composter's
    # bone meal goes into the hopper below it.
    c = (-4, 8)
    setblock(at(c[0], Y, c[1]), "minecraft:composter")
    setblock(at(c[0], Y + 1, c[1]), "minecraft:hopper[facing=down]" + items("minecraft:pumpkin_pie", 3))
    setblock(at(c[0] + 3, Y + 1, c[1]), "minecraft:composter[level=8]")
    setblock(at(c[0] + 3, Y, c[1]), "minecraft:hopper[facing=east]")
    comparator_reading(None, (at(c[0] + 4, Y, c[1] + 1), at(c[0] + 5, Y, c[1] + 1), "west"))  # (unused)

    # 4. Droppers: one into a chest, one out into the air; a dispenser places water, a second pulse
    # picks it up again; flint and steel lights netherrack; bone meal grows wheat.
    d = (6, 8)
    setblock(at(d[0], Y, d[1]), "minecraft:dropper[facing=east]" + items("minecraft:stone", 5))
    setblock(at(d[0] + 1, Y, d[1]), "minecraft:chest")
    comparator_reading(None, (at(d[0] + 2, Y, d[1]), at(d[0] + 3, Y, d[1]), "west"))
    setblock(at(d[0], Y, d[1] + 3), "minecraft:dropper[facing=east]" + items("minecraft:stone", 5))
    w = (26, 20)  # (away from the rest: the water spreads before it's picked up again)
    setblock(at(w[0], Y, w[1]), "minecraft:dispenser[facing=east]" + items("minecraft:water_bucket", 1))
    setblock(at(d[0], Y, d[1] + 9), "minecraft:dispenser[facing=east]" + items("minecraft:flint_and_steel", 1))
    setblock(at(d[0] + 1, Y - 1, d[1] + 9), "minecraft:netherrack")
    setblock(at(d[0], Y, d[1] + 12), "minecraft:dispenser[facing=east]" + items("minecraft:bone_meal", 64))
    setblock(at(d[0] + 1, Y - 1, d[1] + 12), "minecraft:farmland")
    setblock(at(d[0] + 1, Y, d[1] + 12), "minecraft:wheat")
    alice.pump(0.5)

    def pulse(p):
        setblock(p, "minecraft:redstone_block")
        alice.pump(0.4)
        setblock(p, "minecraft:air")
        alice.pump(0.3)

    for _ in range(3):
        pulse(at(d[0] - 1, Y, d[1]))
    pulse(at(d[0] - 1, Y, d[1] + 3))
    pulse(at(w[0] - 1, Y, w[1]))
    water = state(at(w[0] + 1, Y, w[1]))
    pulse(at(w[0] - 1, Y, w[1]))
    pulse(at(d[0] - 1, Y, d[1] + 9))
    pulse(at(d[0] - 1, Y, d[1] + 12))

    # 5. An item dropped onto a hopper goes in.
    e = (18, 8)
    setblock(at(e[0], Y, e[1]), "minecraft:hopper[facing=down]")
    comparator_reading(None, (at(e[0] + 1, Y, e[1]), at(e[0] + 2, Y, e[1]), "west"))
    alice.creative_slot(36, "minecraft:ender_pearl", 16)
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.command(f"tp Alice {ox + e[0] + 0.5} {Y + 1.5} {oz + e[1] + 0.5} 0 90")
    alice.pump(0.3)
    alice.rotate(0, 90)
    alice.pump(0.1)
    alice.action(4, 0, 0, 0, 0)  # DROP_ALL_ITEMS: the whole stack
    alice.pump(0.3)
    alice.command(f"tp Alice {ox} {Y} {oz}")
    alice.pump(6.0)

    print(f"  OBS  chain: {power(at(a[0] + 3, Y, a[1]))}, locked: {power(at(a[0] + 3, Y, a[1] + 4))}, "
          f"locked hopper {state(at(a[0] + 1, Y + 1, a[1] + 4))}")
    check(power(at(a[0] + 3, Y, a[1])) == 3, "16 ender pearls went down the chain (the last hopper reads 3)")
    check(power(at(a[0] + 3, Y, a[1] + 4)) == 3 and "enabled=false" in state(at(a[0] + 1, Y + 1, a[1] + 4)),
          "a locked hopper doesn't push, but the hopper below still pulls out of it")

    print(f"  OBS  furnace: {state(at(b[0], Y, b[1]))}, history {alice.history.get(at(b[0], Y, b[1]), [])}")
    check(any("lit=true" in s for s in alice.history.get(at(b[0], Y, b[1]), [])), "hoppers fuel and fill a furnace, and it lights")

    print(f"  OBS  composter: {state(at(c[0], Y, c[1]))}, full one: {state(at(c[0] + 3, Y + 1, c[1]))}")
    check(state(at(c[0], Y, c[1])) == "minecraft:composter[level=3]", "a hopper composts three pumpkin pies")
    check(state(at(c[0] + 3, Y + 1, c[1])) == "minecraft:composter[level=0]", "a hopper empties a full composter")

    if os.environ.get("HOPPER_DEBUG"):
        for p in (at(d[0], Y, d[1]), at(d[0] + 1, Y, d[1]), at(d[0] + 2, Y, d[1]), at(d[0] + 3, Y, d[1]), at(d[0], Y, d[1] + 9), at(d[0] + 1, Y, d[1] + 9),
                  at(d[0] - 1, Y, d[1] + 9)):
            print("  DEBUG", p, alice.history.get(p))
        print("  DEBUG events", [e for e in alice.events if e[0] == "level_event"][:20])
    print(f"  OBS  dropper into chest: {power(at(d[0] + 3, Y, d[1]))}, dropper out: {state(at(d[0], Y, d[1] + 3))}")
    check(power(at(d[0] + 3, Y, d[1])) == 1, "a dropper puts items into the chest in front")
    check(alice.saw("level_event", lambda i: i[0] == 2000 and i[1] == at(d[0], Y, d[1] + 3)), "a dropper with nothing in front throws an item out")
    print(f"  OBS  dispenser water: {water}, after the second pulse: {state(at(w[0] + 1, Y, w[1]))}")
    check(water == "minecraft:water[level=0]" and state(at(w[0] + 1, Y, w[1])) == "minecraft:air", "a dispenser places water and picks it up again")
    print(f"  OBS  fire: {state(at(d[0] + 1, Y, d[1] + 9))}, wheat: {state(at(d[0] + 1, Y, d[1] + 12))}")
    check(state(at(d[0] + 1, Y, d[1] + 9)).startswith("minecraft:fire"), "a dispenser with flint and steel lights a fire")
    check(state(at(d[0] + 1, Y, d[1] + 12)) != "minecraft:wheat[age=0]", "a dispenser with bone meal grows wheat")

    print(f"  OBS  dropped onto a hopper: {power(at(e[0] + 2, Y, e[1]))}")
    check(power(at(e[0] + 2, Y, e[1])) == 3, "a hopper picks up items dropped onto it")

    if os.environ.get("HOPPER_DUMP"):
        with open(os.environ["HOPPER_DUMP"], "w") as out:
            def inside(p):
                return -30 <= p[0] - ox <= 30 and 4 <= p[2] - oz <= 30 and Y - 1 <= p[1] <= Y + 5
            for p in sorted(alice.blocks):
                if inside(p) and not state(p).startswith("minecraft:wheat") and not state(p).startswith("minecraft:fire"):
                    out.write(f"{p[0] - ox} {p[1]} {p[2] - oz} {state(p)}\n")

    print("PASS" if not failures else f"FAIL ({len(failures)})")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
