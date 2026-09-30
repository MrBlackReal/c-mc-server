#!/usr/bin/env python3
"""World persistence checks.

    test_persistence.py build   HOST PORT     Alice builds a pattern, fills her inventory, walks, quits
    test_persistence.py verify  HOST PORT     after a restart: position, inventory and blocks are back
    test_persistence.py compare HOST PORT WORLD_DIR
                                              every block the server sends for the chunks around the
                                              player matches the world's region files on disk

Works against this server and against vanilla (ops needed for /gamemode there).
"""
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import nbtdump  # noqa: E402
from mcprobe import Reader  # noqa: E402
from test_building import ITEM, STATES, Client, check, failures, state_str  # noqa: E402
from test_drops import DropClient  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BLOCKS = json.load(open(os.path.join(ROOT, "data", "blocks.json")))
BIOME_COUNT = 67
MIN_Y = -64

# Pattern relative to the build origin (x, y, z, block item); y is absolute (superflat surface -61).
PATTERN = [(3, -60, 3, "minecraft:stone"), (3, -59, 3, "minecraft:stone"), (4, -60, 3, "minecraft:oak_planks"),
           (5, -60, 3, "minecraft:glass")]
STATE_FILE = os.path.join(os.environ.get("TMPDIR", "/tmp"), "cmc_persistence_state.json")


def state_id(name, props=None):
    for s in BLOCKS[name]["states"]:
        if props is None and s.get("default") or props is not None and s.get("properties", {}) == props:
            return s["id"]
    raise KeyError(name)


# ---------------------------------------------------------------- chunk decoding (network)
class ChunkStore:
    def __init__(self):
        self.sections = {}  # (cx, cz) -> list of 4096-entry arrays (index y<<8|z<<4|x)

    def feed(self, r):
        cx, cz = r.i32(), r.i32()
        for _ in range(r.varint()):
            r.varint()
            for _ in range(r.varint()):
                r.i64()
        data = Reader(r.bytes(r.varint()))
        secs = []
        while data.remaining():
            data.i16(), data.i16()
            secs.append(read_container(data, 4096, 4, 8))
            read_container(data, 64, 1, 3)
        self.sections[(cx, cz)] = secs

    def get(self, x, y, z):
        secs = self.sections.get((x >> 4, z >> 4))
        if secs is None:
            return None
        return secs[(y - MIN_Y) >> 4][((y & 15) << 8) | ((z & 15) << 4) | (x & 15)]


def read_container(r, n, min_bits, max_bits):
    bits = r.u8()
    if bits == 0:
        v = r.varint()
        return [v] * n
    palette = [r.varint() for _ in range(r.varint())] if bits <= max_bits else None
    per = 64 // bits
    longs = [struct.unpack(">Q", r.bytes(8))[0] for _ in range((n + per - 1) // per)]
    out = []
    for i in range(n):
        v = (longs[i // per] >> ((i % per) * bits)) & ((1 << bits) - 1)
        out.append(palette[v] if palette else v)
    return out


def attach_chunks(client):
    store = ChunkStore()
    orig = client._handle

    def handle(n, r):
        if n == "level_chunk_with_light":
            store.feed(r)
        else:
            orig(n, r)

    client._handle = handle
    return store


# ---------------------------------------------------------------- disk decoding
def disk_block(world, x, y, z, cache={}):
    cx, cz = x >> 4, z >> 4
    key = (world, cx, cz)
    if key not in cache:
        path = os.path.join(world, "dimensions", "minecraft", "overworld", "region", f"r.{cx >> 5}.{cz >> 5}.mca")
        cache[key] = None
        if os.path.exists(path):
            for lx, lz, ctype, body, _, _ in nbtdump.region_chunks(path):
                if (lx, lz) == (cx & 31, cz & 31):
                    cache[key] = nbtdump.plain(nbtdump.read_named(nbtdump.decompress(ctype, body)))
    chunk = cache[key]
    if chunk is None:
        return None
    for sec in chunk["sections"]:
        if sec["Y"] != y >> 4 or "block_states" not in sec:
            continue
        bs = sec["block_states"]
        palette = [disk_state(e) for e in bs["palette"]]
        if len(palette) == 1:
            return palette[0]
        bits = max(4, (len(palette) - 1).bit_length())
        per = 64 // bits
        i = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15)
        v = (bs["data"][i // per] & 0xFFFFFFFFFFFFFFFF) >> ((i % per) * bits) & ((1 << bits) - 1)
        return palette[v]
    return None


def disk_state(e):
    if isinstance(e, dict) and set(e) == {""}:  # ListTag wrapper for mixed lists
        e = e[""]
    if isinstance(e, str):
        return state_id(e)
    return state_id(e.get("id", e.get("Name")), e.get("properties", e.get("Properties", {})))


# ---------------------------------------------------------------- phases
def build(host, port):
    alice = Client(host, port, "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    alice.command("gamemode creative")
    alice.command("gamerule random_tick_speed 0")  # (the world must stay as built: no grass dying under the stone)
    alice.pump(0.3)
    alice.rotate(0, 45)
    seq = 100
    for x, y, z, item in PATTERN:
        alice.creative_slot(36, item)
        alice.pump(0.1)
        alice.use_item_on(ox + x, y - 1, oz + z, 1, seq)  # click the top face of the block below
        seq += 1
        alice.pump(0.2)
    alice.creative_slot(36, "minecraft:diamond_pickaxe")
    alice.creative_slot(37, "minecraft:stone", 42)
    alice.creative_slot(5, "minecraft:iron_helmet")
    alice.creative_slot(45, "minecraft:shield")
    alice.command("time set 12345")
    # A thrown item that should survive the restart (entities/r.X.Z.mca).
    alice.creative_slot(38, "minecraft:diamond", 3)
    alice.send("set_carried_item", struct.pack(">h", 2))
    alice.rotate(0, 0)
    alice.pump(0.3)
    alice.action(4, 0, 0, 0, 0)  # DROP_ALL_ITEMS: the whole stack
    alice.pump(2.0)
    alice.send("set_carried_item", struct.pack(">h", 0))
    target = (alice.pos[0] + 1.25, alice.pos[1], alice.pos[2] - 0.75)
    alice.send("move_player_pos_rot", struct.pack(">dddffB", *target, 90.0, 10.0, 1))
    alice.pump(1.0)
    json.dump({"origin": [ox, oz], "pos": target}, open(STATE_FILE, "w"))
    for x, y, z, item in PATTERN:
        # Offsets are relative to Alice's spawn in x/z, like test_building.
        got = alice.blocks.get((ox + x, y, oz + z))
        check(got is not None and state_str(got) == item, f"placed {item} at {(ox + x, y, oz + z)}: {state_str(got) if got else None}")
    print("built; state saved to", STATE_FILE)


def verify(host, port):
    st = json.load(open(STATE_FILE))
    ox, oz = st["origin"]
    alice = DropClient(host, port, "Alice")
    chunks = attach_chunks(alice)
    alice.pump(2.0)
    diamonds = alice.items("minecraft:diamond")
    check(len(diamonds) == 1 and diamonds[0]["item"][1] == 3, f"thrown diamonds are still on the ground: {alice.items()}")
    px, py, pz = st["pos"]
    check(abs(alice.pos[0] - px) < 0.01 and abs(alice.pos[2] - pz) < 0.01, f"position restored: {alice.pos} (expected {st['pos']})")
    check(alice.slots.get(36) == ("minecraft:diamond_pickaxe", 1), f"hotbar slot 0: {alice.slots.get(36)}")
    check(alice.slots.get(37) == ("minecraft:stone", 42), f"hotbar slot 1: {alice.slots.get(37)}")
    check(alice.slots.get(5) == ("minecraft:iron_helmet", 1), f"helmet: {alice.slots.get(5)}")
    check(alice.slots.get(45) == ("minecraft:shield", 1), f"offhand: {alice.slots.get(45)}")
    check(alice.clocks.get(0) and alice.clocks[0][0] >= 12345, f"overworld clock kept: {alice.clocks.get(0)}")
    for x, y, z, item in PATTERN:
        got = chunks.get(ox + x, y, oz + z)
        check(got is not None and state_str(got) == item, f"{item} at {(ox + x, y, oz + z)} after restart: {state_str(got) if got is not None else None}")


def compare(host, port, world):
    alice = Client(host, port, "Alice")
    chunks = attach_chunks(alice)
    alice.pump(3.0)
    compared = mismatched = 0
    for (cx, cz), secs in sorted(chunks.sections.items()):
        for y in range(MIN_Y, MIN_Y + 16 * len(secs)):
            if y > -40:  # superflat: everything interesting is near the bottom
                break
            for z in range(cz * 16, cz * 16 + 16):
                for x in range(cx * 16, cx * 16 + 16):
                    disk = disk_block(world, x, y, z)
                    if disk is None:
                        continue
                    net = chunks.get(x, y, z)
                    compared += 1
                    if net != disk:
                        mismatched += 1
                        if mismatched <= 5:
                            print(f"    mismatch at {(x, y, z)}: disk {state_str(disk)} vs server {state_str(net)}")
    check(compared > 1000, f"compared {compared} blocks from disk")
    check(mismatched == 0, f"{mismatched} blocks differ between disk and what the server sent")


def main():
    mode, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    {"build": lambda: build(host, port), "verify": lambda: verify(host, port),
     "compare": lambda: compare(host, port, sys.argv[4])}[mode]()
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print(f"{mode}: all checks passed")


if __name__ == "__main__":
    main()
