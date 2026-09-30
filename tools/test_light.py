#!/usr/bin/env python3
"""Light and heightmaps against vanilla: builds lit and shaded structures with /setblock and /fill,
then records what a newly joined player gets in its chunk packets (heightmaps, sky and block
light per section).

    test_light.py HOST PORT OUT.json      build, rejoin, write the chunks around spawn
    test_light.py HOST PORT OUT.json record   only record (a world built and saved before)
    test_light.py compare A.json B.json    the two recordings must be identical (phases both have)

tools/test_light.sh runs this on vanilla and on this server and compares the results.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client  # noqa: E402

RADIUS = 2  # chunks around the spawn chunk that are compared

# Structures (absolute block coordinates on the superflat world: the surface is y = -61).
COMMANDS = [
    "fill -6 -56 -6 -2 -56 -2 minecraft:stone",            # a roof: shade below it
    "setblock -4 -60 -4 minecraft:torch",                   # a torch under the roof
    "setblock 3 -60 3 minecraft:glowstone",                 # block light in the open
    "fill 6 -58 6 8 -58 8 minecraft:stone_slab",            # bottom slabs: shape occlusion
    "fill 6 -57 10 8 -57 10 minecraft:oak_stairs[facing=east]",
    "fill 15 -60 0 16 -30 0 minecraft:stone",               # a tall pillar across a chunk border, into higher sections
    "fill 10 -60 -8 12 -58 -6 minecraft:oak_leaves[persistent=true]",  # leaves let light through, dimmed
    "fill 0 -60 12 2 -60 14 minecraft:glass",
    "fill -12 -60 -12 -10 -59 -10 minecraft:slime_block",   # slime dims light by one per block (ice would melt, water flow)
    "setblock -11 -58 -11 minecraft:sea_lantern",
    "fill 20 -61 20 22 -61 22 minecraft:air",               # a hole down to the dirt
    "fill 20 -64 25 23 -63 28 minecraft:air",               # a cave under the surface, with a lantern
    "setblock 21 -64 26 minecraft:lantern",
    "fill -20 -40 -20 -18 -40 -18 minecraft:magma_block",   # floating light sources
    "setblock 18 -50 -18 minecraft:jack_o_lantern",
    "setblock 0 -60 -16 minecraft:snow[layers=3]",
    "setblock 1 -60 -16 minecraft:dirt_path",
    "fill 24 -60 -4 28 -56 0 minecraft:stone",              # a sealed room with a light inside
    "fill 25 -59 -3 27 -57 -1 minecraft:air",
    "setblock 26 -59 -2 minecraft:redstone_lamp[lit=true]",
    "fill -8 -61 8 -4 -61 12 minecraft:air",                # remove the grass layer...
    "fill -8 -61 8 -4 -61 12 minecraft:grass_block",        # ...and put it back
    "setblock 4 -60 -4 minecraft:glowstone",
    "setblock 4 -60 -4 minecraft:air",                      # a light that goes out again
]


def read_bitset(r):
    """ByteBufCodecs.BIT_SET: BitSet.toByteArray (little-endian bytes), length-prefixed."""
    return r.bytes(r.varint())


def bit(data, i):
    return i // 8 < len(data) and (data[i // 8] >> (i % 8)) & 1


def read_light(r):
    masks = [read_bitset(r) for _ in range(4)]
    sky = [r.bytes(r.varint()).hex() for _ in range(r.varint())]
    block = [r.bytes(r.varint()).hex() for _ in range(r.varint())]
    out = {"sky": {}, "block": {}}
    for layer, mask, empty, data in (("sky", masks[0], masks[2], sky), ("block", masks[1], masks[3], block)):
        n = 0
        for i in range(8 * max(len(mask), len(empty), 1)):
            if bit(mask, i):
                out[layer][str(i)] = data[n]
                n += 1
            elif bit(empty, i):
                out[layer][str(i)] = "empty"
    return out


class LightClient(Client):
    def __init__(self, *a):
        self.chunks = {}
        self.light_updates = []
        super().__init__(*a)

    def _handle(self, n, r):
        if n == "level_chunk_with_light":
            cx, cz = r.i32(), r.i32()
            heightmaps = {}
            for _ in range(r.varint()):
                t = r.varint()
                heightmaps[str(t)] = [r.i64() & (2 ** 64 - 1) for _ in range(r.varint())]
            r.bytes(r.varint())  # sections
            for _ in range(r.varint()):  # block entities
                r.u8(), r.i16(), r.varint()
                r.nbt()
            self.chunks[f"{cx},{cz}"] = {"heightmaps": heightmaps, **read_light(r)}
        elif n == "light_update":
            cx, cz = r.varint(), r.varint()
            self.light_updates.append((cx, cz, read_light(r)))
        else:
            super()._handle(n, r)


def record(host, port, name):
    c = LightClient(host, port, name)
    import time
    end = time.time() + 20
    want = {f"{x},{z}" for x in range(-RADIUS, RADIUS + 1) for z in range(-RADIUS, RADIUS + 1)}
    while time.time() < end and not want <= set(c.chunks):
        c.pump(0.5)
    c.pump(1.0)
    return c, {k: v for k, v in c.chunks.items() if k in want}


def main():
    if sys.argv[1] == "compare":
        a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))
        bad = 0
        for phase in a:
            if phase not in b:
                continue
            for chunk in sorted(set(a[phase]) | set(b[phase])):
                ca, cb = a[phase].get(chunk), b[phase].get(chunk)
                if ca is None or cb is None:
                    print(f"{phase} {chunk}: missing on one side")
                    bad += 1
                    continue
                # An all-zero array and an "empty" section mean the same (vanilla sometimes sends
                # the former, depending on the order it lit the spawn chunks in).
                zero = "0" * 4096
                norm = lambda v: "empty" if v == zero else v
                for key in ("heightmaps", "sky", "block"):
                    for idx in sorted(set(ca[key]) | set(cb[key]), key=lambda k: int(k)):
                        if norm(ca[key].get(idx)) != norm(cb[key].get(idx)):
                            va, vb = ca[key].get(idx), cb[key].get(idx)
                            short = lambda v: v if v is None or len(str(v)) < 40 else f"{str(v)[:32]}..."
                            print(f"{phase} {chunk} {key}[{idx}]: {short(va)} != {short(vb)}")
                            bad += 1
        print(f"{bad} difference(s)" if bad else "light identical")
        sys.exit(1 if bad else 0)

    host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    if len(sys.argv) > 4 and sys.argv[4] == "record":  # a world built before: only record it
        _, chunks = record(host, port, "Bob")
        json.dump({"built": chunks}, open(out, "w"))
        print(f"recorded {len(chunks)} chunks")
        return
    alice, before = record(host, port, "Alice")
    alice.command("gamemode creative")
    alice.pump(0.5)
    for cmd in COMMANDS:
        alice.command(cmd)
        alice.pump(0.2)
    alice.pump(3.0)
    print(f"Alice got {len(alice.light_updates)} light update(s) while building")
    bob, after = record(host, port, "Bob")
    # Light updates reach players only for chunks on the border of what they see: light up one
    # block in each chunk along +x and record the updates (the last one per section wins).
    alice.light_updates.clear()
    for k in range(2, 14):
        alice.command(f"setblock {16 * k + 8} -60 8 minecraft:glowstone")
        alice.pump(0.1)
    alice.pump(3.0)
    updates = {}
    for cx, cz, data in alice.light_updates:
        u = updates.setdefault(f"{cx},{cz}", {"heightmaps": {}, "sky": {}, "block": {}})
        u["sky"].update(data["sky"])
        u["block"].update(data["block"])
    print(f"light updates for chunks {sorted(updates)}")
    for k in range(2, 14):  # back to the recorded world (what the other server opens next)
        alice.command(f"setblock {16 * k + 8} -60 8 minecraft:air")
        alice.pump(0.1)
    alice.pump(2.0)
    json.dump({"flat": before, "built": after, "updates": updates}, open(out, "w"))
    print(f"recorded {len(before)} + {len(after)} chunks")


if __name__ == "__main__":
    main()
