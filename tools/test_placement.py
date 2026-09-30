#!/usr/bin/env python3
"""Block placement against vanilla: places blocks in fixed arrangements and records the states.

    test_placement.py HOST PORT OUT.json       run the placements, write {pos: state} to OUT.json
    test_placement.py compare A.json B.json     the two results must be identical

tools/test_placement.sh runs the placements on vanilla and on this server and compares them, so
every getStateForPlacement / updateShape / canSurvive port is checked against the real thing.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client, check, failures, state_str  # noqa: E402

UP, DOWN, NORTH, SOUTH, WEST, EAST = 1, 0, 2, 3, 4, 5
FACE_OFFSET = {UP: (0, 1, 0), DOWN: (0, -1, 0), NORTH: (0, 0, -1), SOUTH: (0, 0, 1), WEST: (-1, 0, 0), EAST: (1, 0, 0)}
G = -61  # superflat grass surface; blocks placed on it go to y = -60

# Each group: where Alice stands (relative x, z), her rotation, then the placements:
# (item, clicked block (x, y, z), face, sneaking[, cursor (cx, cy, cz)]).
GROUPS = [
    ("chests", (0, 0), (0, 30), [
        ("chest", (2, G, 2), UP, False), ("chest", (3, G, 2), UP, False),       # a double chest
        ("chest", (5, G, 2), UP, False), ("chest", (5, -60, 2), EAST, True),    # sneaking next to one: stays single
        ("copper_chest", (2, G, 4), UP, False), ("exposed_copper_chest", (3, G, 4), UP, False),
        ("trapped_chest", (-2, G, 2), UP, False), ("trapped_chest", (-2, G, 3), UP, False),
    ]),
    ("facing", (0, -6), (135, 20), [
        ("hay_block", (2, G, -8), UP, False), ("campfire", (2, -60, -8), UP, False),
        ("decorated_pot", (3, G, -8), UP, False), ("calibrated_sculk_sensor", (4, G, -8), UP, False),
        ("skeleton_skull", (1, G, -8), UP, False), ("oak_sign", (0, G, -8), UP, False),
        ("anvil", (-1, G, -8), UP, False), ("furnace", (-2, G, -8), UP, False),
        ("crafter", (1, G, -9), UP, False), ("lectern", (2, G, -9), UP, False),
    ]),
    ("stacking", (0, 12), (180, 45), [
        ("white_candle", (1, G, 10), UP, False), ("white_candle", (1, -60, 10), UP, False), ("white_candle", (1, -60, 10), UP, False),
        ("snow", (2, G, 10), UP, False), ("snow", (2, -60, 10), UP, False), ("snow", (2, -60, 10), UP, False),
        ("pink_petals", (3, G, 10), UP, False), ("pink_petals", (3, -60, 10), UP, False),
        ("sea_pickle", (0, G, 10), UP, False), ("sea_pickle", (0, -60, 10), UP, False),
        ("turtle_egg", (-1, G, 10), UP, False), ("turtle_egg", (-1, -60, 10), UP, False),
        ("leaf_litter", (-2, G, 10), UP, False),
    ]),
    ("rails", (8, 0), (90, 40), [
        ("rail", (6, G, 0), UP, False), ("rail", (6, G, 1), UP, False), ("rail", (6, G, 2), UP, False),
        ("rail", (7, G, 2), UP, False),                                          # turns the corner
        ("stone", (6, G, -1), UP, False), ("rail", (6, -60, -1), UP, False),    # ascending
        ("powered_rail", (9, G, 0), UP, False), ("powered_rail", (9, G, 1), UP, False),
        ("powered_rail", (10, G, 1), UP, False),                                 # can't curve
    ]),
    ("redstone", (8, 8), (90, 40), [
        ("redstone", (6, G, 7), UP, False), ("redstone", (6, G, 8), UP, False), ("redstone", (7, G, 8), UP, False),
        ("redstone", (9, G, 6), UP, False),                                      # a lone dot shows as a cross
        ("stone", (5, G, 10), UP, False), ("redstone", (5, -60, 10), UP, False), ("redstone", (6, G, 10), UP, False),
        ("repeater", (9, G, 9), UP, False), ("redstone", (9, G, 10), UP, False),
    ]),
    ("walls", (-8, 0), (270, 30), [
        ("stone", (-6, G, 0), UP, False), ("stone", (-6, -60, 0), UP, False), ("stone", (-6, -59, 0), UP, False),
        ("vine", (-6, -59, 0), EAST, False), ("glow_lichen", (-6, -60, 0), EAST, False),
        ("oak_log", (-6, G, 2), UP, False), ("oak_log", (-6, -60, 2), UP, False),
        ("stone_button", (-6, -60, 2), EAST, False), ("lantern", (-6, -59, 2), DOWN, False),
        ("bell", (-6, -60, 0), NORTH, False), ("bell", (-5, G, -2), UP, False),
        ("tripwire_hook", (-6, -60, 2), SOUTH, False),
        ("jungle_log", (-8, G, 3), UP, False), ("cocoa_beans", (-8, -60, 3), EAST, False),
    ]),
    ("growth", (-8, 10), (270, 45), [
        ("oak_leaves", (-6, G, 8), UP, False), ("oak_leaves", (-6, -60, 8), UP, False),
        ("scaffolding", (-6, G, 10), UP, False), ("scaffolding", (-6, -60, 10), UP, False), ("scaffolding", (-6, -60, 10), WEST, False),
        ("pointed_dripstone", (-5, G, 12), UP, False), ("pointed_dripstone", (-5, -60, 12), UP, False),
        ("red_mushroom_block", (-7, G, 12), UP, False), ("red_mushroom_block", (-7, -60, 12), UP, False),
        ("white_concrete_powder", (-8, G, 8), UP, False), ("bamboo", (-8, G, 10), UP, False),
        ("big_dripleaf", (-9, G, 10), UP, False), ("pale_moss_carpet", (-9, G, 8), UP, False),
    ]),
    ("misc", (0, -14), (0, 35), [
        ("stone", (1, G, -12), UP, False), ("stone", (1, -60, -12), UP, False), ("stone", (1, -59, -12), UP, False),
        ("oak_hanging_sign", (1, -58, -12), SOUTH, False),                       # on a wall: faces across
        ("stone", (2, -59, -12), WEST, False), ("oak_hanging_sign", (2, -59, -12), DOWN, False),
        ("tripwire_hook", (1, -60, -12), SOUTH, False), ("oak_log", (1, G, -10), UP, False),
        ("tripwire_hook", (1, -60, -10), NORTH, False), ("string", (1, G, -11), UP, False),
        ("end_stone", (-2, G, -12), UP, False), ("chorus_plant", (-2, -60, -12), UP, False),
        ("chorus_plant", (-2, -59, -12), UP, False),  # (a chorus flower would grow on random ticks)
        ("glass_pane", (-1, G, -10), UP, False), ("glass_pane", (-2, G, -10), UP, False),
        ("oak_fence", (3, G, -10), UP, False), ("oak_fence", (3, G, -11), UP, False),
    ]),
    # BlockState.useItemOn / useWithoutItem (None: an empty hand).
    ("interactions", (0, 19), (0, 35), [
        ("flower_pot", (0, G, 22), UP, False), ("dandelion", (0, -60, 22), UP, False), ("poppy", (0, -60, 22), UP, False),
        ("flower_pot", (1, G, 22), UP, False), ("dandelion", (1, -60, 22), UP, False), (None, (1, -60, 22), UP, False),
        ("cake", (2, G, 22), UP, False), ("white_candle", (2, -60, 22), UP, False),
        ("cake", (3, G, 22), UP, False), (None, (3, -60, 22), UP, False),                   # a bite (creative)
        ("composter", (-1, G, 22), UP, False)] + [("cake", (-1, -60, 22), UP, False)] * 7 + [
        ("pumpkin", (-2, G, 22), UP, False), ("shears", (-2, -60, 22), NORTH, False, (0.5, 0.5, 0.0)),
        ("note_block", (4, G, 22), UP, False), ("skeleton_skull", (4, -60, 22), UP, False),  # on top, not a note
        ("stone", (-3, G, 23), UP, False), ("stone", (-3, -60, 23), UP, False), ("stone", (-3, -59, 23), UP, False),
        ("stone", (-3, -58, 23), NORTH, False, (0.5, 0.5, 0.0)),
        ("oak_hanging_sign", (-3, -58, 22), DOWN, False), ("oak_hanging_sign", (-3, -59, 22), DOWN, False),  # chained
        (None, (-1, -60, 22), UP, False),                                                     # the compost is ready by now
    ]),
    ("interactions 2", (0, 25), (0, 35), [
        ("chiseled_bookshelf", (0, G, 27), UP, False),                                        # faces north, towards Alice
        ("book", (0, -60, 27), NORTH, False, (0.2, 0.8, 0.0)), ("book", (0, -60, 27), NORTH, False, (0.8, 0.2, 0.0)),
        (None, (0, -60, 27), NORTH, False, (0.2, 0.8, 0.0)),                                  # takes the first one back
        ("lectern", (1, G, 27), UP, False), ("writable_book", (1, -60, 27), UP, False),
        ("respawn_anchor", (2, G, 27), UP, False), ("glowstone", (2, -60, 27), UP, False), ("glowstone", (2, -60, 27), UP, False),
        ("copper_golem_statue", (3, G, 27), UP, False), (None, (3, -60, 27), UP, False),
        ("cauldron", (-1, G, 27), UP, False), ("water_bucket", (-1, -60, 27), UP, False), ("glass_bottle", (-1, -60, 27), UP, False),
        ("cauldron", (-2, G, 27), UP, False), ("lava_bucket", (-2, -60, 27), UP, False),
        ("cauldron", (-3, G, 27), UP, False), ("powder_snow_bucket", (-3, -60, 27), UP, False), ("bucket", (-3, -60, 27), UP, False),
        ("decorated_pot", (4, G, 27), UP, False), ("dirt", (4, -60, 27), UP, False), (None, (4, -60, 27), UP, False),
        ("jukebox", (5, G, 27), UP, False), ("music_disc_cat", (5, -60, 27), UP, False),   # has_record
        ("jukebox", (6, G, 27), UP, False), ("music_disc_13", (6, -60, 27), UP, False), (None, (6, -60, 27), UP, False),
    ]),
    # ItemStack.useOn: block transformers, honeycomb, flint and steel, fire charges.
    ("items", (0, 30), (0, 35), [
        ("oak_log", (0, G, 32), UP, False), ("iron_axe", (0, -60, 32), UP, False),               # stripped
        ("iron_shovel", (1, G, 32), UP, False), ("iron_hoe", (2, G, 32), UP, False),               # path, farmland
        ("rooted_dirt", (3, G, 32), UP, False), ("iron_hoe", (3, -60, 32), UP, False),             # dirt (+ hanging roots)
        ("copper_block", (4, G, 32), UP, False), ("honeycomb", (4, -60, 32), UP, False),
        ("exposed_copper", (5, G, 32), UP, False), ("iron_axe", (5, -60, 32), UP, False),          # scraped
        ("waxed_cut_copper_stairs", (6, G, 32), UP, False), ("iron_axe", (6, -60, 32), UP, False),  # wax off
        ("soul_sand", (-1, G, 32), UP, False), ("flint_and_steel", (-1, -60, 32), UP, False),     # soul fire
        ("white_candle", (-2, G, 32), UP, False), ("flint_and_steel", (-2, -60, 32), UP, False, (0.5, 0.5, 0.5)),
        ("red_candle", (-3, G, 32), UP, False), ("fire_charge", (-3, -60, 32), UP, False, (0.5, 0.5, 0.5)),
    ]),
    # FallingBlock / FallingBlockEntity: placed against the side of a pillar, they drop.
    ("falling", (0, 35), (0, 35), [
        ("stone", (0, G, 37), UP, False), ("stone", (0, -60, 37), UP, False), ("stone", (0, -59, 37), UP, False),
        ("sand", (0, -59, 37), EAST, False, (1.0, 0.5, 0.5)),                  # (1,-59,37) -> lands at (1,-60,37)
        ("gravel", (0, -58, 37), EAST, False, (1.0, 0.5, 0.5)),                # (1,-58,37) -> lands on the sand
        ("anvil", (0, -59, 37), WEST, False, (0.0, 0.5, 0.5)),                 # (-1,-59,37) -> one block down
        ("dragon_egg", (0, -58, 37), SOUTH, False, (0.5, 0.5, 1.0)),           # (0,-58,38) -> two blocks down
        ("torch", (2, G, 37), UP, False), ("stone", (2, G, 36), UP, False), ("stone", (2, -60, 36), UP, False),
        ("sand", (2, -59, 36), SOUTH, False, (0.5, 0.5, 1.0)),                 # onto the torch: breaks, drops
    ]),
]


def place_all(host, port):
    alice = Client(host, port, "Alice")
    bob = Client(host, port, "Bob")
    for c in (alice, bob):
        c.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    bob.command("gamemode creative")  # flying: vanilla kicks floating survival players
    bob.command(f"tp {ox + 0.5} -40 {oz + 0.5}")  # high above, out of the way
    alice.command("gamemode creative")
    alice.command("gamerule random_tick_speed 0")  # (both servers: nothing grows, dries out or spreads meanwhile)

    def both(t):
        for c in (alice, bob):
            c.pump(t)

    both(0.5)
    seq = 100
    for name, (sx, sz), (yrot, xrot), steps in GROUPS:
        alice.command(f"tp {ox + sx + 0.5} -60 {oz + sz + 0.5}")
        both(0.5)
        alice.rotate(yrot, xrot)
        both(0.2)
        for step in steps:
            item, (x, y, z), face, sneak = step[:4]
            cursor = step[4] if len(step) > 4 else (0.5, 1.0 if face == UP else 0.0 if face == DOWN else 0.5, 0.5)
            if item:
                alice.creative_slot(36, "minecraft:" + item)
            else:
                alice.send("set_creative_mode_slot", struct.pack(">h", 36) + tb.enc_varint(0))
            alice.send("set_carried_item", struct.pack(">h", 0))
            alice.send("player_input", bytes([0x20 if sneak else 0]))
            alice.pump(0.1)
            alice.use_item_on(x, y, z, face, seq, *cursor)
            seq += 1
            both(0.25)
        alice.send("player_input", bytes([0]))
        both(0.3)
    both(1.0)
    return {f"{x - ox},{y},{z - oz}": state_str(sid) for (x, y, z), sid in bob.blocks.items()}


def main():
    if sys.argv[1] == "compare":
        a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))
        # Random ticks are off, but can run before the game rule is set: they turn covered grass into
        # dirt and spread grass onto dirt, dry out farmland, spread vines and grow cocoa and bamboo.
        # Vanilla's game events wake sculk sensors (not ported). Skip those.
        def name(v):
            return (v or "").split("[")[0]
        def random_tick(k):
            ground = k.split(",")[1] == str(G)
            grassy = lambda v: v is None or v.startswith("minecraft:grass_block") or v == "minecraft:dirt"
            growing = {"minecraft:cocoa", "minecraft:bamboo", "minecraft:bamboo_sapling"}
            soil = lambda v: grassy(v) or name(v) == "minecraft:farmland"
            return (ground and grassy(a.get(k)) and grassy(b.get(k))) or \
                   ((a.get(k) or "").startswith("minecraft:grass_block") and soil(b.get(k))) or \
                   (b.get(k) is None and (a.get(k) or "").startswith("minecraft:vine")) or \
                   (name(a.get(k)) in growing and (b.get(k) is None or name(b.get(k)) in growing)) or \
                   (name(a.get(k)) == name(b.get(k)) and "sculk_sensor_phase=cooldown" in (a.get(k) or ""))
        diffs = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k) and not random_tick(k))
        for k in diffs[:40]:
            print(f"    {k}: vanilla {a.get(k)}\n    {' ' * len(k)}  ours    {b.get(k)}")
        check(not diffs, f"{len(set(a) | set(b)) - len(diffs)} block states identical, {len(diffs)} differ")
    else:
        result = place_all(sys.argv[1], int(sys.argv[2]))
        json.dump(result, open(sys.argv[3], "w"), indent=0, sort_keys=True)
        print(f"{len(result)} block states recorded")
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
