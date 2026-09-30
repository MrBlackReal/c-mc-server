#!/usr/bin/env python3
"""Bundles in the inventory: picking items up with a bundle, clicking items onto it, taking them
out with right clicks. Runs against vanilla too.

    test_bundles.py HOST PORT
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from mcprobe import enc_varint  # noqa: E402
from test_building import check, failures  # noqa: E402
from test_menus import MenuClient  # noqa: E402


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    alice = MenuClient(host, port, "Alice")
    alice.pump(1.0)
    alice.command("gamemode creative")
    alice.pump(0.5)
    alice.creative_slot(36, "minecraft:bundle", 1)
    alice.creative_slot(37, "minecraft:stone", 10)
    alice.creative_slot(38, "minecraft:ender_pearl", 16)
    alice.creative_slot(39, "minecraft:dirt", 5)
    alice.pump(0.5)
    alice.command("gamemode survival")  # (creative inventories are client-side)
    alice.pump(0.5)
    full = lambda i: alice.menus.get(0, {}).get("full", {}).get(i)
    contents = lambda: (full(36) or (None, None, {}))[2].get("minecraft:bundle_contents")

    print("carrying the bundle, a left click takes the stack in the slot")
    alice.click(36, 0, 0, cid=0)
    check(alice.cursor and alice.cursor[0] == "minecraft:bundle", f"carrying {alice.cursor}")
    alice.click(37, 0, 0, cid=0)
    print("OBS stone taken:", alice.slot(37, 0), alice.cursor)
    check(alice.slot(37, 0) is None, f"slot 37 now {alice.slot(37, 0)}")
    print("pearls only fit as far as the weight allows (16 stack: 1/16 each)")
    alice.click(38, 0, 0, cid=0)
    print("OBS pearls left:", alice.slot(38, 0))
    check(alice.slot(38, 0) is not None and alice.slot(38, 0)[1] == 3, f"pearls left {alice.slot(38, 0)}")
    print("a right click on an empty slot drops the newest stack there")
    alice.click(40, 1, 0, cid=0)
    print("OBS dropped into 40:", alice.slot(40, 0))
    check(alice.slot(40, 0) is not None and alice.slot(40, 0)[0] == "minecraft:ender_pearl", f"slot 40 {alice.slot(40, 0)}")
    alice.click(36, 0, 0, cid=0)  # put the bundle back
    print("OBS bundle:", full(36))
    check(contents() == [("minecraft:stone", 10, {})], f"contents {contents()}")

    print("an item clicked onto the bundle goes in; a right click takes one stack out")
    alice.click(39, 0, 0, cid=0)  # pick up the dirt
    alice.click(36, 0, 0, cid=0)  # onto the bundle
    print("OBS after dirt:", full(36), alice.cursor)
    # (the probe claims an empty cursor in its clicks, so neither server resends the now empty one)
    check(contents() == [("minecraft:dirt", 5, {}), ("minecraft:stone", 10, {})], f"contents {contents()}")
    alice.send("bundle_item_selected", enc_varint(36) + enc_varint(1))  # point at the stone
    alice.pump(0.2)
    alice.click(36, 1, 0, cid=0)
    print("OBS took:", alice.cursor, full(36))
    check(alice.cursor == ("minecraft:stone", 10) and contents() == [("minecraft:dirt", 5, {})], f"took {alice.cursor}, left {contents()}")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all bundle checks passed")


if __name__ == "__main__":
    main()
