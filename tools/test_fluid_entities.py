#!/usr/bin/env python3
"""Items in fluids: water currents carry them, they float up in water, lava burns them (unless
they're fire resistant). Runs against vanilla too; OBS lines show where things ended up.

    test_fluid_entities.py HOST PORT
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import check, failures  # noqa: E402
from test_drops import DropClient  # noqa: E402

GROUND = -61  # superflat: grass at -61, air from -60


def main():
    alice = DropClient(sys.argv[1], int(sys.argv[2]), "Alice")
    bob = DropClient(sys.argv[1], int(sys.argv[2]), "Bob")
    alice.pump(1.0)
    bob.pump(0.5)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    alice.command("gamerule advance_time false")
    alice.command("gamerule random_tick_speed 0")
    alice.pump(0.5)

    def both(seconds):  # pump the two clients interleaved, so time passes once
        end = time.time() + seconds
        while time.time() < end:
            alice.pump(0.01)
            bob.pump(0.01)

    def cmd(c):
        alice.command(c)
        both(0.05)

    def items(name):
        return [(eid, e) for eid, e in bob.entities.items() if e["type"] == "minecraft:item" and e["item"] and e["item"][0] == name]

    def throw_down(item, x, y, z):
        """Alice stands at (x, y, z) looking straight down and drops one `item` (Q)."""
        alice.creative_slot(36, item, 1)
        alice.send("set_carried_item", struct.pack(">h", 0))
        cmd(f"tp Alice {x} {y} {z} 0 90")
        both(0.3)
        alice.rotate(0, 90)
        both(0.1)
        alice.action(5, 0, 0, 0, 0)  # DROP_ITEM
        both(0.2)
        cmd(f"tp Alice {ox + 0.5} -60 {oz + 0.5}")  # (out of pickup range)

    # 1. A trench along +x with a water source at its start and a torch in the way: the water
    # washes the torch away and carries it downstream. (Two deep, so the dropped torch can't hop out.)
    tx, tz = ox + 6, oz + 12
    cmd(f"fill {tx} {GROUND - 1} {tz} {tx + 12} {GROUND} {tz} minecraft:air")
    cmd(f"setblock {tx + 1} {GROUND - 1} {tz} minecraft:torch")
    cmd(f"setblock {tx} {GROUND - 1} {tz} minecraft:water")

    # 2. A pool two deep: a dropped item sinks in, then floats up.
    px, pz = ox - 12, oz + 12
    cmd(f"fill {px - 1} {GROUND - 1} {pz - 1} {px + 1} {GROUND} {pz + 1} minecraft:water")

    # 3. A lava pool: cobblestone burns up, a netherite ingot doesn't.
    lx, lz = ox + 12, oz - 12
    cmd(f"fill {lx - 1} {GROUND} {lz - 1} {lx + 1} {GROUND} {lz + 1} minecraft:lava")
    both(3.0)

    throw_down("minecraft:stone", px + 0.5, GROUND + 2, pz + 0.5)
    throw_down("minecraft:cobblestone", lx + 0.5, GROUND + 3, lz + 0.5)
    throw_down("minecraft:netherite_ingot", lx + 0.5, GROUND + 3, lz + 0.5)
    lowest = {}
    end = time.time() + 6.0
    while time.time() < end:
        both(0.1)
        for eid, e in items("minecraft:stone"):
            lowest[eid] = min(lowest.get(eid, e["pos"][1]), e["pos"][1])

    if os.environ.get("ENTITY_DEBUG"):
        import collections
        print("  DEBUG packets", collections.Counter(n for n, _ in bob.events))
    torches = items("minecraft:torch")
    for _, e in torches:
        print(f"  OBS  torch item at x+{e['pos'][0] - tx:.2f} y{e['pos'][1]:.2f} z{e['pos'][2] - tz:+.2f}")
    check(len(torches) == 1, f"the washed-away torch dropped one item: {torches}")
    check(torches and torches[0][1]["pos"][0] - tx > 4.0, "the current carried it downstream")
    check(torches and abs(torches[0][1]["pos"][2] - tz - 0.5) < 0.5, "and it stayed in the trench")

    stones = items("minecraft:stone")
    for eid, e in stones:
        print(f"  OBS  stone in the pool: lowest y{lowest.get(eid, 0):.2f}, now y{e['pos'][1]:.2f}")
    check(len(stones) == 1, f"the stone dropped into the pool is there: {stones}")
    check(stones and lowest[stones[0][0]] < GROUND, "it sank into the pool")
    check(stones and stones[0][1]["pos"][1] > lowest[stones[0][0]] + 0.3, "and floated back up")

    print(f"  OBS  in the lava: {[e['item'][0] for _, e in bob.entities.items() if e['type'] == 'minecraft:item' and e['item'] and abs(e['pos'][0] - lx - 0.5) < 2]}")
    check(any(v and v[0] == "minecraft:cobblestone" for v in bob.spawned.values()), "the cobblestone was thrown (and seen)")
    check(not items("minecraft:cobblestone"), "cobblestone burns up in lava")
    check(len(items("minecraft:netherite_ingot")) == 1, "a netherite ingot doesn't")

    print("PASS" if not failures else f"FAIL ({len(failures)})")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
