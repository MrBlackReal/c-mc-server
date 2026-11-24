#!/usr/bin/env python3
"""Fluids: water and lava spreading, falling, finding holes, making new sources, meeting each
other, washing blocks away, leaking out of waterlogged blocks, and sponges. Only settled outcomes
are checked, so the same checks pass on vanilla (OBS lines show what each server ended with).

    test_fluids.py HOST PORT [seconds to let things settle]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client, check, failures, state_str  # noqa: E402

GROUND = -61  # superflat: grass at -61, air from -60
Y = GROUND + 1


def main():
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
    alice = Client(sys.argv[1], int(sys.argv[2]), "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    alice.command("gamerule advance_time false")
    alice.command("gamerule random_tick_speed 0")  # (no lava fires, no melting)
    alice.pump(0.5)

    def at(dx, y, dz):
        return (ox + dx, y, oz + dz)

    def setblock(p, block):
        alice.command(f"setblock {p[0]} {p[1]} {p[2]} {block}")

    def state(p):
        return state_str(alice.blocks.get(p, 0))

    def water(level):
        return f"minecraft:water[level={level}]"

    def lava(level):
        return f"minecraft:lava[level={level}]"

    # 1. A source on flat ground: a diamond, one level weaker per block, 7 blocks out.
    a = (-60, -30)
    setblock(at(a[0], Y, a[1]), "minecraft:water")
    # 2. Two sources a block apart on solid ground make a third between them.
    b = (-30, -30)
    setblock(at(b[0], Y, b[1]), "minecraft:water")
    setblock(at(b[0] + 2, Y, b[1]), "minecraft:water")
    # 3. A waterfall: falling water down to the ground, then spreading there.
    c = (0, -30)
    setblock(at(c[0], Y + 5, c[1]), "minecraft:water")
    # 4. A hole two blocks away: the water only flows towards it.
    d = (30, -30)
    setblock(at(d[0] + 2, GROUND, d[1]), "minecraft:air")
    setblock(at(d[0], Y, d[1]), "minecraft:water")
    # 5. A torch in the way is washed away.
    e = (60, -30)
    setblock(at(e[0] + 1, Y, e[1]), "minecraft:torch")
    setblock(at(e[0], Y, e[1]), "minecraft:water")
    # 6. Lava placed next to water becomes obsidian.
    f = (-60, 30)
    setblock(at(f[0], Y, f[1]), "minecraft:water")
    setblock(at(f[0] + 1, Y, f[1]), "minecraft:lava")
    # 7. Lava falling onto a water source turns it into stone.
    g = (-30, 30)
    setblock(at(g[0], Y, g[1]), "minecraft:water")
    setblock(at(g[0], Y + 3, g[1]), "minecraft:lava")
    # 8. Water leaks out of a waterlogged slab.
    h = (0, 30)
    setblock(at(h[0], Y, h[1]), "minecraft:oak_slab[type=bottom,waterlogged=true]")
    # 9. Lava on flat ground: 3 blocks out (amount 8, 6, 4, 2).
    i = (30, 30)
    setblock(at(i[0], Y, i[1]), "minecraft:lava")
    # 10. A sponge soaks up a pool and turns wet (placed once the pool has spread, below).
    j = (60, 30)
    setblock(at(j[0], Y, j[1]), "minecraft:water")

    # 11. Bubble columns: water sources over soul sand push up, over magma drag down; without the soul
    # sand the column turns back into water.
    bubbles = {}
    for name, dx, below in (("up", -60, "minecraft:soul_sand"), ("down", -40, "minecraft:magma_block"), ("gone", -20, "minecraft:soul_sand")):
        p = at(dx, GROUND, -60)
        bubbles[name] = p
        setblock(p, below)
        for dy in range(1, 4):
            setblock((p[0], p[1] + dy, p[2]), "minecraft:water")

    alice.pump(seconds)
    setblock(bubbles["gone"], "minecraft:stone")
    setblock(at(j[0], Y + 1, j[1]), "minecraft:sponge")
    alice.pump(seconds / 2)

    for name, p in (("source", at(a[0], Y, a[1])), ("1 out", at(a[0] + 1, Y, a[1])), ("3 out", at(a[0], Y, a[1] - 3)),
                    ("7 out", at(a[0] - 7, Y, a[1])), ("diagonal 4+3", at(a[0] + 4, Y, a[1] + 3)), ("8 out", at(a[0] + 8, Y, a[1])),
                    ("diagonal 4+4", at(a[0] + 4, Y, a[1] + 4))):
        print(f"  OBS  diamond {name}: {state(p)}")
    check(state(at(a[0], Y, a[1])) == water(0), "the source stays a source")
    check(state(at(a[0] + 1, Y, a[1])) == water(1) and state(at(a[0], Y, a[1] - 3)) == water(3), "water weakens by one per block")
    check(state(at(a[0] - 7, Y, a[1])) == water(7) and state(at(a[0] + 4, Y, a[1] + 3)) == water(7), "water reaches 7 blocks out")
    check(state(at(a[0] + 8, Y, a[1])) == "minecraft:air" and state(at(a[0] + 4, Y, a[1] + 4)) == "minecraft:air", "and no further")

    print(f"  OBS  between two sources: {state(at(b[0] + 1, Y, b[1]))}")
    check(state(at(b[0] + 1, Y, b[1])) == water(0), "two sources make a new source between them")

    for dy in range(6):
        print(f"  OBS  waterfall y{Y + dy}: {state(at(c[0], Y + dy, c[1]))}")
    for name, p in (("beside the source", at(c[0] + 1, Y + 5, c[1])), ("beside the fall", at(c[0] + 1, Y + 4, c[1])),
                    ("bottom 1 out", at(c[0] + 1, Y, c[1])), ("bottom 7 out", at(c[0], Y, c[1] + 7))):
        print(f"  OBS  waterfall {name}: {state(p)}")
    check(all(state(at(c[0], Y + dy, c[1])) == water(8) for dy in range(5)), "falling water all the way down (level 8)")
    # (once the water below is flowing water, which water can't replace, the source spreads sideways
    # as well, and that falls too)
    check(state(at(c[0] + 1, Y + 5, c[1])) == water(1) and state(at(c[0] + 1, Y + 4, c[1])) == water(8), "the source also spreads sideways, and falls")
    check(state(at(c[0] + 1, Y, c[1])) == water(8) and state(at(c[0], Y, c[1] + 7)) == water(6), "falling water spreads at the bottom")

    for name, p in (("behind", at(d[0] - 1, Y, d[1])), ("side", at(d[0], Y, d[1] + 1)), ("toward", at(d[0] + 1, Y, d[1])),
                    ("edge", at(d[0] + 2, Y, d[1])), ("hole", at(d[0] + 2, GROUND, d[1]))):
        print(f"  OBS  hole {name}: {state(p)}")
    check(state(at(d[0] - 1, Y, d[1])) == "minecraft:air" and state(at(d[0], Y, d[1] + 1)) == "minecraft:air", "water only flows towards the hole")
    check(state(at(d[0] + 1, Y, d[1])) == water(1) and state(at(d[0] + 2, Y, d[1])) == water(2), "down the path to the hole")
    check(state(at(d[0] + 2, GROUND, d[1])) == water(8), "and into it")

    print(f"  OBS  torch: {state(at(e[0] + 1, Y, e[1]))}")
    check(state(at(e[0] + 1, Y, e[1])) == water(1), "water washes the torch away")

    print(f"  OBS  lava next to water: {state(at(f[0] + 1, Y, f[1]))}")
    check(state(at(f[0] + 1, Y, f[1])) == "minecraft:obsidian", "a lava source next to water becomes obsidian")

    for dy in range(4):
        print(f"  OBS  lava onto water y{Y + dy}: {state(at(g[0], Y + dy, g[1]))}")
    check(state(at(g[0], Y, g[1])) == "minecraft:stone", "lava falling onto water makes stone")
    check(state(at(g[0], Y + 3, g[1])) == lava(0), "the lava source stays")

    print(f"  OBS  slab: {state(at(h[0], Y, h[1]))}, next to it: {state(at(h[0] + 1, Y, h[1]))}")
    check(state(at(h[0] + 1, Y, h[1])) == water(1) and state(at(h[0], Y, h[1] - 2)) == water(2), "water leaks out of a waterlogged slab")

    for k in range(5):
        print(f"  OBS  lava {k} out: {state(at(i[0] + k, Y, i[1]))}")
    check([state(at(i[0] + k, Y, i[1])) for k in range(5)] == [lava(0), lava(2), lava(4), lava(6), "minecraft:air"],
          "lava spreads 3 blocks, two levels weaker each")

    print(f"  OBS  sponge: {state(at(j[0], Y + 1, j[1]))}, pool: {state(at(j[0], Y, j[1]))}, edge: {state(at(j[0] + 7, Y, j[1]))}")
    check(state(at(j[0], Y + 1, j[1])) == "minecraft:wet_sponge", "the sponge turns wet")
    check(all(state(at(j[0] + dx, Y, j[1] + dz)) == "minecraft:air" for dx in range(-7, 8) for dz in range(-7, 8)), "and the pool is gone")

    for name, p in bubbles.items():
        print(f"  OBS  bubble column {name}: " + ", ".join(state((p[0], p[1] + dy, p[2])) for dy in range(1, 4)))
    check(all(state((bubbles["up"][0], Y + dy, bubbles["up"][2])) == "minecraft:bubble_column[drag=false]" for dy in range(3)),
          "water over soul sand becomes a bubble column pushing up")
    check(all(state((bubbles["down"][0], Y + dy, bubbles["down"][2])) == "minecraft:bubble_column[drag=true]" for dy in range(3)),
          "water over magma becomes one dragging down")
    check(all(state((bubbles["gone"][0], Y + dy, bubbles["gone"][2])) == water(0) for dy in range(3)), "without soul sand it's water again")

    # Buckets (creative: the bucket in hand stays, a filled one is added to the inventory).
    seq = [100]

    def use_bucket(item, p):
        """Hold `item` and use it looking straight down at `p` from three blocks above."""
        alice.creative_slot(36, item)
        alice.command(f"tp Alice {p[0] + 0.5} {p[1] + 3} {p[2] + 0.5} 0 90")
        alice.pump(0.5)
        seq[0] += 1
        alice.send("use_item", tb.enc_varint(0) + tb.enc_varint(seq[0]) + tb.struct.pack(">ff", 0.0, 90.0))
        alice.pump(0.5)

    k = at(-30, Y, 60)
    setblock(k, "minecraft:water")
    alice.pump(1.0)
    alice.slots.clear()
    use_bucket("minecraft:bucket", k)
    picked = any(v and v[0] == "minecraft:water_bucket" for v in alice.slots.values())
    if os.environ.get("FLUID_DEBUG"):
        print("  DEBUG slots", alice.slots)
    l = at(0, Y, 60)
    use_bucket("minecraft:water_bucket", (l[0], GROUND, l[2]))  # (aimed at the ground: the water goes on top)
    m = at(30, Y, 60)
    setblock(m, "minecraft:oak_slab[type=bottom]")
    use_bucket("minecraft:water_bucket", m)
    n = at(60, Y, 60)
    use_bucket("minecraft:lava_bucket", (n[0], GROUND, n[2]))
    alice.pump(seconds / 2)
    slab_filled = state(m)
    use_bucket("minecraft:bucket", m)
    alice.pump(seconds / 2)

    print(f"  OBS  bucket: picked up {state(k)}, got a water bucket: {picked}")
    check(state(k) == "minecraft:air" and picked, "an empty bucket picks up a water source")
    print(f"  OBS  bucket: emptied {state(l)}, next to it {state((l[0] + 1, Y, l[2]))}")
    check(state(l) == water(0) and state((l[0] + 1, Y, l[2])) == water(1), "a water bucket empties onto the ground")
    print(f"  OBS  bucket: slab {slab_filled}, then {state(m)}, beside {state((m[0] + 1, Y, m[2]))}")
    check(slab_filled == "minecraft:oak_slab[type=bottom,waterlogged=true]", "a water bucket waterlogs the slab clicked")
    check(state(m) == "minecraft:oak_slab[type=bottom,waterlogged=false]" and state((m[0] + 1, Y, m[2])) == "minecraft:air",
          "an empty bucket takes the water back out")
    print(f"  OBS  bucket: lava {state(n)}, next to it {state((n[0] + 1, Y, n[2]))}")
    check(state(n) == lava(0) and state((n[0] + 1, Y, n[2])) == lava(2), "a lava bucket empties onto the ground")

    if os.environ.get("FLUID_DUMP"):  # every block that changed, relative to spawn: diff two servers' dumps
        import json
        with open(os.environ["FLUID_DUMP"], "w") as out:
            json.dump(sorted((p[0] - ox, p[1], p[2] - oz, state(p)) for p in alice.blocks), out, indent=0)

    print("PASS" if not failures else f"FAIL ({len(failures)})")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
