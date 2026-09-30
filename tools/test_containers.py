#!/usr/bin/env python3
"""Container contents when breaking and placing (milestone 4). Needs a world whose containers were
filled by vanilla; tools/test_containers.sh makes one and runs every phase against both servers.

    test_containers.py setup  HOST PORT     vanilla only (needs ops): fill chests, a barrel, shulker boxes
    test_containers.py break  HOST PORT     break them: chests spill, shulker boxes keep their contents
    test_containers.py place  HOST PORT     after a restart: place the picked-up shulker box again
    test_containers.py verify WORLD_DIR     after a stop: the placed box has its contents on disk

Coordinates are absolute; Alice stands at (0.5, -60, 0.5) on a superflat world.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import nbtdump  # noqa: E402
from mcprobe import enc_varint  # noqa: E402
from test_building import ITEM, check, failures, state_str  # noqa: E402
from test_drops import DropClient, dig  # noqa: E402

SWORD = '{Slot:%db,id:"minecraft:iron_sword",count:1,components:{"minecraft:damage":10}}'
CHEST = (2, -60, 0)       # broken in creative: contents only
BARREL = (2, -60, 2)      # broken by hand in survival: contents plus the barrel
SHULKER = (0, -60, 2)     # broken by hand in survival: one box that keeps its contents
SHULKER_C = (-2, -60, 0)  # broken in creative, filled: still drops (ShulkerBoxBlock.playerWillDestroy)
EMPTY_C = (-2, -60, 2)    # broken in creative, empty: nothing
PLACED = (0, -60, -2)     # where the picked-up box is placed again
CREATIVE_PLACED = (2, -60, -2)  # a box made in the creative inventory, with contents set by the client


class Client(DropClient):
    def __init__(self, *a):
        self.first = {}  # item entity id -> item as first seen; merges don't change these
        super().__init__(*a)

    def _handle(self, n, r):
        before = dict(self.spawned)
        super()._handle(n, r)
        for eid, item in self.spawned.items():
            if eid not in before:
                self.first[eid] = item

    def dropped(self):
        """Totals of everything that spawned since the last reset: {item: count}, plus the stacks."""
        totals = {}
        for item in self.first.values():
            totals[item[0]] = totals.get(item[0], 0) + item[1]
        return totals, list(self.first.values())

    def reset(self):
        self.first.clear()


def join(host, port):
    alice = Client(host, port, "Alice")
    alice.pump(1.0)
    alice.rotate(0, 60)  # on the ground: full dig speed
    return alice


def setup(host, port):
    alice = join(host, port)
    alice.command("tp Alice 0.5 -60 0.5")
    for cmd in (f"setblock {CHEST[0]} {CHEST[1]} {CHEST[2]} minecraft:chest{{Items:["
                '{Slot:0b,id:"minecraft:diamond",count:5},{Slot:13b,id:"minecraft:cobblestone",count:64},'
                f"{SWORD % 26}]}}",
                f"setblock {BARREL[0]} {BARREL[1]} {BARREL[2]} minecraft:barrel{{Items:["
                '{Slot:4b,id:"minecraft:gold_ingot",count:12}]}',
                f"setblock {SHULKER[0]} {SHULKER[1]} {SHULKER[2]} minecraft:red_shulker_box{{CustomName:\"Loot\",Items:["
                f'{{Slot:0b,id:"minecraft:emerald",count:7}},{SWORD % 26}]}}',
                f"setblock {SHULKER_C[0]} {SHULKER_C[1]} {SHULKER_C[2]} minecraft:blue_shulker_box{{Items:["
                '{Slot:3b,id:"minecraft:apple",count:2}]}',
                f"setblock {EMPTY_C[0]} {EMPTY_C[1]} {EMPTY_C[2]} minecraft:white_shulker_box"):
        alice.command(cmd)
        alice.pump(0.2)
    alice.pump(1.0)
    for pos, block in ((CHEST, "minecraft:chest"), (BARREL, "minecraft:barrel"), (SHULKER, "minecraft:red_shulker_box"),
                       (SHULKER_C, "minecraft:blue_shulker_box"), (EMPTY_C, "minecraft:white_shulker_box")):
        got = alice.blocks.get(pos)
        check(got is not None and state_str(got).startswith(block), f"{block} at {pos}: {state_str(got) if got else None}")


def pump_all(clients, seconds, until=None):
    """Pumps the clients interleaved, so time passes once; stops early once until() holds."""
    end = time.time() + seconds
    while time.time() < end and not (until and until()):
        for c in clients:
            c.pump(0.01)


def broken(clients, bob, pos, what, expected):
    """Waits for the block to go and the expected drops to show up (a slow server, e.g. a sanitizer
    build, finishes a delayed destroy late), plus a moment for anything extra. Checks both."""
    gone = lambda: bob.blocks.get(pos) is not None and state_str(bob.blocks[pos]) == "minecraft:air"
    pump_all(clients, 8.0, gone)
    check(gone(), f"{what} broken (Bob sees {state_str(bob.blocks[pos]) if pos in bob.blocks else 'no update'})")
    pump_all(clients, 5.0, lambda: bob.dropped()[0] == expected)
    pump_all(clients, 0.5)
    totals, stacks = bob.dropped()
    check(totals == expected, f"drops {totals}, expected {expected}")
    return stacks


def break_phase(host, port):
    alice = join(host, port)
    check(abs(alice.pos[0] - 0.5) < 0.01 and abs(alice.pos[2] - 0.5) < 0.01, f"Alice is where setup left her: {alice.pos}")
    bob = Client(host, port, "Bob")  # watches: vanilla doesn't echo Alice's own breaks to her
    clients = (alice, bob)
    alice.command("gamemode survival")
    pump_all(clients, 1.0)
    alice.rotate(0, 60)

    print("red shulker box, by hand in survival: one box, contents kept inside")
    bob.reset()
    dig(alice, *SHULKER, 70, 10)  # hardness 2, hand: 60 ticks
    stacks = broken(clients, bob, SHULKER, "red shulker box", {"minecraft:red_shulker_box": 1})
    check(stacks and stacks[0][2], f"dropped box carries components (name/contents): {stacks}")
    # The contents are sent to clients (tooltips, and the creative inventory sends them back).
    contents = stacks[0][2].get("minecraft:container") if stacks else None
    check(contents and len(contents) == 27 and contents[0] == ("minecraft:emerald", 7, {})
          and contents[26] == ("minecraft:iron_sword", 1, {"minecraft:damage": 10}), f"clients see the contents: {contents}")
    boxes = bob.items("minecraft:red_shulker_box")
    if boxes:
        alice.command("tp {:.2f} {:.2f} {:.2f}".format(*boxes[0]["pos"]))
    has_box = lambda: [(slot, v) for slot, v in alice.full_slots.items() if v and v[0] == "minecraft:red_shulker_box"]
    pump_all(clients, 5.0, has_box)
    check(len(has_box()) == 1, f"box picked up: {has_box()}")
    alice.command("tp 0.5 -60 0.5")
    pump_all(clients, 0.5)
    alice.rotate(0, 60)

    print("barrel, by hand in survival: barrel plus contents")
    bob.reset()
    dig(alice, *BARREL, 85, 20)  # hardness 2.5, hand: 75 ticks
    broken(clients, bob, BARREL, "barrel", {"minecraft:barrel": 1, "minecraft:gold_ingot": 12})

    alice.command("gamemode creative")
    pump_all(clients, 0.5)

    print("filled shulker box, in creative: still drops, no loose apples")
    bob.reset()
    alice.action(0, *SHULKER_C, 31)
    broken(clients, bob, SHULKER_C, "blue shulker box", {"minecraft:blue_shulker_box": 1})

    print("empty shulker box, in creative: nothing")
    bob.reset()
    alice.action(0, *EMPTY_C, 32)
    broken(clients, bob, EMPTY_C, "white shulker box", {})

    # (last: stacks spilled from the chest can still turn up as new entities in later steps)
    print("chest, in creative: contents spill, no chest")
    bob.reset()
    alice.action(0, *CHEST, 30)
    stacks = broken(clients, bob, CHEST, "chest", {"minecraft:diamond": 5, "minecraft:cobblestone": 64, "minecraft:iron_sword": 1})
    cobble = [s[1] for s in stacks if s[0] == "minecraft:cobblestone"]
    check(len(cobble) >= 3 and all(10 <= c <= 30 for c in cobble[:-1]), f"cobblestone spilled in stacks of 10-30: {cobble}")
    sword = [s for s in stacks if s[0] == "minecraft:iron_sword"]
    check(sword and sword[0][2].get("minecraft:damage") == 10, f"sword kept its damage: {sword}")
    # Leave the red box in the hotbar for the next phase, and keep creative from wiping it.
    alice.command("gamemode survival")
    pump_all(clients, 0.5)


def place_phase(host, port):
    alice = join(host, port)
    held = [(slot, v) for slot, v in alice.full_slots.items() if v and v[0] == "minecraft:red_shulker_box"]
    slots = [slot for slot, v in alice.slots.items() if v and v[0] == "minecraft:red_shulker_box"]
    check(len(slots) == 1 and 36 <= slots[0] <= 44, f"box still in the hotbar after the restart: {slots}")
    if not slots:
        return
    alice.send("set_carried_item", struct.pack(">h", slots[0] - 36))
    alice.pump(0.3)
    alice.use_item_on(PLACED[0], PLACED[1] - 1, PLACED[2], 1, 40)
    alice.pump(until=lambda: not alice.slots.get(slots[0]), timeout=6.0)
    alice.pump(0.3)
    got = alice.blocks.get(PLACED)
    # Vanilla doesn't echo the placer's own block (the client predicted it); verify reads it from disk.
    check(got is None or state_str(got).startswith("minecraft:red_shulker_box"), f"box placed: {state_str(got) if got else None}")
    check(not alice.slots.get(slots[0]), f"box used up: {alice.slots.get(slots[0])} {held}")

    print("creative inventory: a box whose contents the client set, placed")
    alice.command("gamemode creative")
    alice.pump(0.5)
    contents = enc_varint(1) + b"\x01" + enc_varint(ITEM["minecraft:diamond"]) + enc_varint(3) + b"\x00\x00"
    alice.creative_slot(37, "minecraft:blue_shulker_box", 1, [("minecraft:container", contents)])
    alice.send("set_carried_item", struct.pack(">h", 1))
    alice.pump(0.3)
    alice.use_item_on(CREATIVE_PLACED[0], CREATIVE_PLACED[1] - 1, CREATIVE_PLACED[2], 1, 41)
    alice.pump(1.0)
    alice.command("gamemode survival")
    alice.pump(0.5)


def block_entity(world, x, y, z):
    cx, cz = x >> 4, z >> 4
    path = os.path.join(world, "dimensions", "minecraft", "overworld", "region", f"r.{cx >> 5}.{cz >> 5}.mca")
    for lx, lz, ctype, body, _, _ in nbtdump.region_chunks(path):
        if (lx, lz) == (cx & 31, cz & 31):
            chunk = nbtdump.plain(nbtdump.read_named(nbtdump.decompress(ctype, body)))
            return next((be for be in chunk.get("block_entities", []) if (be["x"], be["y"], be["z"]) == (x, y, z)), None)
    return None


def verify(world):
    be = block_entity(world, *PLACED)
    check(be is not None and be.get("id") == "minecraft:shulker_box", f"placed box has a block entity: {be}")
    if not be:
        return
    items = {it["Slot"]: (it["id"], it["count"], it.get("components", {})) for it in be.get("Items", [])}
    check(items.get(0, (None, 0))[:2] == ("minecraft:emerald", 7), f"slot 0: 7 emeralds: {items.get(0)}")
    sword = items.get(26)
    check(sword and sword[0] == "minecraft:iron_sword" and sword[2].get("minecraft:damage") == 10, f"slot 26: damaged sword: {sword}")
    check(len(items) == 2, f"nothing else: {items}")
    check("Loot" in str(be.get("CustomName")), f"name kept: {be.get('CustomName')}")
    be = block_entity(world, *CREATIVE_PLACED)
    items = [(it["Slot"], it["id"], it["count"]) for it in (be or {}).get("Items", [])]
    check(items == [(0, "minecraft:diamond", 3)], f"box from the creative inventory has its 3 diamonds: {be}")
    for pos in (CHEST, BARREL, SHULKER, SHULKER_C, EMPTY_C):
        check(block_entity(world, *pos) is None, f"no block entity left at {pos}")


def main():
    mode = sys.argv[1]
    if mode == "verify":
        verify(sys.argv[2])
    else:
        host, port = sys.argv[2], int(sys.argv[3])
        {"setup": setup, "break": break_phase, "place": place_phase}[mode](host, port)
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print(f"{mode}: all checks passed")


if __name__ == "__main__":
    main()
