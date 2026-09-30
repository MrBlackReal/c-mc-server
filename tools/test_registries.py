#!/usr/bin/env python3
"""A client without the vanilla data pack gets every registry element's data and can join.
Runs against vanilla too.

    test_registries.py HOST PORT [OUT.json]    join; write a few received elements to OUT.json
    test_registries.py compare A.json B.json    the elements must match (numbers to float precision)
"""
import json
import os
import sys
import uuid

sys.path.insert(0, os.path.dirname(__file__))
from mcprobe import Conn, enc_str, enc_varint, handshake, load_names  # noqa: E402
from test_building import check, failures  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
NAMES = load_names(os.path.join(ROOT, "data", "packets.json"))
SAMPLES = [("minecraft:dimension_type", "minecraft:overworld"), ("minecraft:worldgen/biome", "minecraft:plains"),
           ("minecraft:damage_type", "minecraft:in_fire"), ("minecraft:chat_type", "minecraft:chat"),
           ("minecraft:enchantment", "minecraft:sharpness"), ("minecraft:painting_variant", "minecraft:kebab"),
           ("minecraft:timeline", "minecraft:day"), ("minecraft:worldgen/biome", "minecraft:jungle")]


def same(a, b):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= 1e-6 * max(1.0, abs(a))  # (a float and the double it came from)
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def normalize(v):
    """Vanilla wraps list elements of mixed type as {"": value}; unwrap them."""
    if isinstance(v, dict):
        return normalize(v[""]) if list(v.keys()) == [""] else {k: normalize(x) for k, x in v.items()}
    if isinstance(v, list):
        return [normalize(x) for x in v]
    return v


def main():
    if sys.argv[1] == "compare":
        a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))
        for k in sorted(set(a) | set(b)):
            check(same(normalize(a.get(k)), normalize(b.get(k))), f"{k}: vanilla {a.get(k)}\n{' ' * 10}ours    {b.get(k)}")
        print()
        print(f"{len(failures)} check(s) failed" if failures else "all registry checks passed")
        sys.exit(1 if failures else 0)
    host, port = sys.argv[1], int(sys.argv[2])
    c = Conn(host, port)
    handshake(c, host, port, 2)
    c.send(0, enc_str("Carol") + uuid.uuid3(uuid.NAMESPACE_DNS, "Carol").bytes)
    while True:
        pid, r = c.recv()
        if pid == 3:
            c.threshold = r.varint()
        elif pid == 2:
            c.send(3)
            break
    c.send(0, enc_str("en_us") + bytes([8]) + enc_varint(0) + b"\x01" + bytes([0x7F]) + enc_varint(1) + b"\x00\x01" + enc_varint(0))
    registries, with_data, samples = 0, 0, {}
    joined = False
    while True:
        pid, r = c.recv()
        n = NAMES.get(("configuration", "clientbound", pid))
        if n == "select_known_packs":
            c.send(7, enc_varint(0))  # we know no packs: send us everything
        elif n == "keep_alive":
            c.send(4, r.bytes(8))
        elif n == "registry_data":
            reg = r.str()
            registries += 1
            for _ in range(r.varint()):
                entry = r.str()
                if r.u8():
                    data = r.nbt()
                    with_data += 1
                    if (reg, entry) in SAMPLES:
                        samples[f"{reg} {entry}"] = data
                else:
                    print(f"  {reg} {entry}: no data")
        elif n == "finish_configuration":
            c.send(3)
            break
        elif n == "disconnect":
            print("disconnected:", r.nbt())
            break
    while not joined:
        pid, r = c.recv()
        if NAMES.get(("play", "clientbound", pid)) == "login":
            joined = True
    check(registries > 20 and with_data > 400, f"{registries} registries, {with_data} elements with data")
    check(len(samples) == len(SAMPLES), f"sample elements received: {sorted(samples)}")
    check(joined, "joined the game")
    if len(sys.argv) > 3:
        json.dump(samples, open(sys.argv[3], "w"), indent=1, sort_keys=True)
    print()
    print(f"{len(failures)} check(s) failed" if failures else "all registry checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
