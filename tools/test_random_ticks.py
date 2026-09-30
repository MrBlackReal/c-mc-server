#!/usr/bin/env python3
"""Random ticks: with game rule random_tick_speed turned up, plants grow, leaves decay, ice and
snow melt next to light, grass spreads and dies, farmland dries out, copper weathers, ... Only
outcomes that don't depend on luck are checked, so the same checks pass on vanilla (OBS lines
show what each server ended with).

    test_random_ticks.py HOST PORT [seconds]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client, check, failures, state_str  # noqa: E402

GROUND = -61  # superflat: grass at -61, air from -60


def main():
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
    alice = Client(sys.argv[1], int(sys.argv[2]), "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    alice.command("time set noon")
    alice.command("gamerule advance_time false")
    alice.pump(0.5)

    def at(dx, y, dz):
        return (ox + dx, y, oz + dz)

    def setblock(p, block):
        alice.command(f"setblock {p[0]} {p[1]} {p[2]} {block}")

    def fill(a, b, block):
        alice.command(f"fill {a[0]} {a[1]} {a[2]} {b[0]} {b[1]} {b[2]} {block}")

    def state(p):
        return state_str(alice.blocks.get(p, 0))

    # Wheat on farmland around water, and a beetroot; the farmland gets wet.
    wx, wz = 8, 0
    fill(at(wx - 1, GROUND, wz - 1), at(wx + 1, GROUND, wz + 1), "minecraft:farmland")
    setblock(at(wx, GROUND, wz), "minecraft:water")
    crops = [(dx, dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1) if (dx, dz) != (0, 0)]
    for dx, dz in crops:
        setblock(at(wx + dx, GROUND + 1, wz + dz), "minecraft:beetroots" if (dx, dz) == (1, 1) else "minecraft:wheat")
    # Dry farmland far from water turns back into dirt.
    setblock(at(-8, GROUND, 0), "minecraft:farmland")
    # Leaves: loose ones decay, ones next to a log stay.
    setblock(at(0, -55, 8), "minecraft:oak_leaves")
    setblock(at(3, -55, 8), "minecraft:oak_log")
    setblock(at(4, -55, 8), "minecraft:oak_leaves")
    # Ice and snow next to glowstone melt; ice in dimmer light stays. (The ice is sunk into the
    # ground, so vanilla's meltwater has nowhere to flow.)
    setblock(at(-8, GROUND, 8), "minecraft:ice")
    setblock(at(-7, GROUND, 8), "minecraft:glowstone")
    setblock(at(-7, GROUND + 1, 8), "minecraft:snow")
    setblock(at(-14, GROUND, 8), "minecraft:ice")  # (block light 8 there)
    # Grass spreads onto a patch of dirt; grass under stone dies.
    fill(at(8, GROUND, 8), at(10, GROUND, 10), "minecraft:dirt")
    setblock(at(-8, GROUND + 1, -8), "minecraft:stone")
    # Sugar cane next to water, cactus on sand: both grow to three blocks.
    setblock(at(12, GROUND, 0), "minecraft:water")
    setblock(at(12, GROUND + 1, 1), "minecraft:sugar_cane")
    setblock(at(-12, GROUND, 0), "minecraft:sand")
    setblock(at(-12, GROUND + 1, 0), "minecraft:cactus")
    # A ripe pumpkin stem on wet farmland grows a pumpkin.
    setblock(at(0, GROUND, 12), "minecraft:farmland")
    setblock(at(0, GROUND, 13), "minecraft:water")
    setblock(at(0, GROUND + 1, 12), "minecraft:pumpkin_stem[age=7]")
    # Berries, nether wart, cocoa, bamboo, redstone ore, copper, amethyst, chorus.
    setblock(at(4, GROUND + 1, -8), "minecraft:sweet_berry_bush")
    setblock(at(6, GROUND, -8), "minecraft:soul_sand")
    setblock(at(6, GROUND + 1, -8), "minecraft:nether_wart")
    setblock(at(8, GROUND + 1, -8), "minecraft:jungle_log")
    setblock(at(8, GROUND + 1, -9), "minecraft:cocoa[facing=south]")
    setblock(at(10, GROUND + 1, -8), "minecraft:bamboo_sapling")
    setblock(at(12, GROUND + 1, -8), "minecraft:redstone_ore[lit=true]")
    setblock(at(0, -56, -12), "minecraft:copper_block")
    setblock(at(-4, -56, -12), "minecraft:budding_amethyst")
    setblock(at(-8, GROUND, -12), "minecraft:end_stone")
    setblock(at(-8, GROUND + 1, -12), "minecraft:chorus_flower")
    # A birch sapling grows into a tree.
    setblock(at(16, GROUND + 1, 16), "minecraft:birch_sapling")
    # Kelp in a glass tank grows to the surface.
    kx, kz = -12, -8
    fill(at(kx - 1, -63, kz - 1), at(kx + 1, -56, kz + 1), "minecraft:glass")
    fill(at(kx, -63, kz), at(kx, -57, kz), "minecraft:water")
    setblock(at(kx, -63, kz), "minecraft:kelp[age=0]")
    alice.pump(1.0)

    alice.command("gamerule random_tick_speed 600")
    # The slowest growers (crops in a ring grow at half speed, the tree) decide when to stop; a
    # server that falls behind gets longer.
    def settled():
        ripe = all(state(at(wx + dx, GROUND + 1, wz + dz)) in ("minecraft:wheat[age=7]", "minecraft:beetroots[age=3]") for dx, dz in crops)
        return ripe and state(at(16, GROUND + 1, 16)).startswith("minecraft:birch_log")
    alice.pump(seconds * 0.75)
    alice.pump(until=settled, timeout=seconds)
    alice.command("gamerule random_tick_speed 3")
    alice.pump(1.0)

    def obs(name, value):
        print(f"OBS {name}: {value}")

    for dx, dz in crops:
        p = at(wx + dx, GROUND + 1, wz + dz)
        want = "minecraft:beetroots[age=3]" if (dx, dz) == (1, 1) else "minecraft:wheat[age=7]"
        check(state(p) == want, f"crop at {dx},{dz} grown: {state(p)}")
        check(state(at(wx + dx, GROUND, wz + dz)) == "minecraft:farmland[moisture=7]", f"wet farmland: {state(at(wx + dx, GROUND, wz + dz))}")
    obs("wheat", state(at(wx - 1, GROUND + 1, wz - 1)))
    dry = state(at(-8, GROUND, 0))  # (dirt, which the grass around may take over again)
    check(dry in ("minecraft:dirt", "minecraft:grass_block[snowy=false]"), f"dry farmland became dirt: {dry}")
    check(state(at(0, -55, 8)) == "minecraft:air", f"loose leaves decayed: {state(at(0, -55, 8))}")
    check(state(at(4, -55, 8)).startswith("minecraft:oak_leaves[distance=1"), f"leaves by the log stay: {state(at(4, -55, 8))}")
    check(state(at(-8, GROUND, 8)) == "minecraft:water[level=0]", f"ice by glowstone melted: {state(at(-8, GROUND, 8))}")
    check(state(at(-7, GROUND + 1, 8)) == "minecraft:air", f"snow on glowstone melted: {state(at(-7, GROUND + 1, 8))}")
    check(alice.blocks.get(at(-14, GROUND, 8)) is not None and state(at(-14, GROUND, 8)) == "minecraft:ice",
          f"ice in the dimmer light stays: {state(at(-14, GROUND, 8))}")
    spread = [state(at(x, GROUND, z)) for x in range(8, 11) for z in range(8, 11)]
    obs("grass patch", sorted(set(spread)))
    check(all(s == "minecraft:grass_block[snowy=false]" for s in spread), f"grass spread over the dirt: {sorted(set(spread))}")
    check(state(at(-8, GROUND, -8)) == "minecraft:dirt", f"grass under stone died: {state(at(-8, GROUND, -8))}")
    cane = [state(at(12, y, 1)) for y in range(GROUND + 1, GROUND + 5)]
    obs("sugar cane", cane)
    check(all(s.startswith("minecraft:sugar_cane") for s in cane[:3]) and cane[3] == "minecraft:air", f"sugar cane three high: {cane}")
    cactus = [state(at(-12, y, 0)) for y in range(GROUND + 1, GROUND + 5)]
    obs("cactus", cactus)
    tall = sum(s.startswith("minecraft:cactus[") for s in cactus)  # (a flower on top stops it growing)
    check(cactus[0].startswith("minecraft:cactus[") and (tall >= 2 or cactus[tall] == "minecraft:cactus_flower"), f"cactus grew: {cactus}")
    check(sum(s.startswith("minecraft:cactus[") for s in cactus) <= 3, f"cactus at most three high: {cactus}")
    stem = state(at(0, GROUND + 1, 12))
    obs("stem", stem)
    check(stem.startswith("minecraft:attached_pumpkin_stem"), f"stem attached: {stem}")
    if stem.startswith("minecraft:attached_pumpkin_stem"):
        facing = stem.split("facing=")[1].rstrip("]")
        dx, dz = {"north": (0, -1), "south": (0, 1), "west": (-1, 0), "east": (1, 0)}[facing]
        check(state(at(dx, GROUND + 1, 12 + dz)) == "minecraft:pumpkin", f"pumpkin where the stem points: {state(at(dx, GROUND + 1, 12 + dz))}")
    check(state(at(4, GROUND + 1, -8)) == "minecraft:sweet_berry_bush[age=3]", f"berries ripe: {state(at(4, GROUND + 1, -8))}")
    check(state(at(6, GROUND + 1, -8)) == "minecraft:nether_wart[age=3]", f"nether wart grown: {state(at(6, GROUND + 1, -8))}")
    check(state(at(8, GROUND + 1, -9)) == "minecraft:cocoa[age=2,facing=south]", f"cocoa grown: {state(at(8, GROUND + 1, -9))}")
    bamboo = [state(at(10, y, -8)) for y in range(GROUND + 1, GROUND + 6)]
    obs("bamboo", bamboo)
    check(all(s.startswith("minecraft:bamboo[") for s in bamboo[:4]), f"bamboo grew: {bamboo}")
    check(state(at(12, GROUND + 1, -8)) == "minecraft:redstone_ore[lit=false]", f"redstone ore went out: {state(at(12, GROUND + 1, -8))}")
    copper = state(at(0, -56, -12))
    obs("copper", copper)
    check(copper in ("minecraft:exposed_copper", "minecraft:weathered_copper", "minecraft:oxidized_copper"), f"copper weathered: {copper}")
    buds = [state(p) for p in (at(-4, -55, -12), at(-4, -57, -12), at(-3, -56, -12), at(-5, -56, -12), at(-4, -56, -11), at(-4, -56, -13))]
    obs("amethyst", buds)
    check(any("amethyst" in s for s in buds), f"amethyst grew: {buds}")
    chorus = state(at(-8, GROUND + 1, -12))
    obs("chorus", chorus)
    check(chorus != "minecraft:chorus_flower[age=0]" and alice.blocks.get(at(-8, GROUND + 1, -12)) is not None, f"chorus flower grew: {chorus}")
    kelp = [state(at(kx, y, kz)) for y in range(-63, -56)]
    obs("kelp", kelp)
    check(all(s == "minecraft:kelp_plant" for s in kelp[:-1]) and kelp[-1].startswith("minecraft:kelp[age="), f"kelp reached the surface: {kelp}")
    tree = [state(at(16, y, 16)) for y in range(GROUND, GROUND + 4)]
    obs("birch", tree)
    check(tree[0] == "minecraft:dirt" and all(t == "minecraft:birch_log[axis=y]" for t in tree[1:]), f"the sapling grew into a birch: {tree}")
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all random tick checks passed")


if __name__ == "__main__":
    main()
