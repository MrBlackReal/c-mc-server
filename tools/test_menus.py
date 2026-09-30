#!/usr/bin/env python3
"""Container menus (chests, double chests, barrels, crafting). Runs against vanilla too.

    test_menus.py HOST PORT
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from mcprobe import enc_varint  # noqa: E402
from test_building import Client, P, check, failures  # noqa: E402

UP = 1
G = -61
MENU = {v["protocol_id"]: k for k, v in tb.REG["minecraft:menu"]["entries"].items()}


class MenuClient(Client):
    def __init__(self, *a):
        self.menus = {}  # container id -> {"type", "title", "slots": {index: (item, count)}}
        self.open_id = 0
        self.cursor = None
        super().__init__(*a)

    def _handle(self, n, r):
        if n == "open_screen":
            cid = r.varint()
            self.menus[cid] = {"type": MENU[r.varint()], "title": r.nbt(), "slots": {}}
            self.open_id = cid
            self.events.append((n, cid))
        elif n == "container_set_content" or n == "container_set_slot":
            cid = r.varint()
            self.state_id = r.varint()
            menu = self.menus.setdefault(cid, {"type": "inventory", "title": None, "slots": {}})
            if n == "container_set_content":
                for i in range(r.varint()):
                    item = tb.read_item(r)
                    menu["slots"][i] = item
            else:
                slot = r.i16()
                full = tb.read_item_ex(r)
                menu["slots"][slot] = full[:2] if full else None
                menu.setdefault("full", {})[slot] = full
            self.events.append((n, cid))
        elif n == "container_set_data":
            cid = r.varint()
            key = r.i16()
            self.menus.setdefault(cid, {"type": None, "title": None, "slots": {}}).setdefault("data", {})[key] = r.i16()
        elif n == "container_close":
            self.events.append((n, r.varint()))
        elif n == "set_cursor_item":
            self.cursor = tb.read_item(r)
        else:
            super()._handle(n, r)

    def click(self, slot, button=0, mode=0, cid=None):
        """ServerboundContainerClickPacket without predictions: the server corrects the client."""
        cid = self.open_id if cid is None else cid
        self.send("container_click", enc_varint(cid) + enc_varint(self.state_id) + struct.pack(">hb", slot, button) + enc_varint(mode)
                  + enc_varint(0) + b"\x00")
        self.pump(0.3)

    def close(self):
        self.send("container_close", enc_varint(self.open_id))
        self.open_id = 0
        self.pump(0.3)

    def slot(self, i, cid=None):
        return self.menus.get(self.open_id if cid is None else cid, {}).get("slots", {}).get(i)


def title_key(t):
    return t.get("translate") if isinstance(t, dict) else t


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    alice = MenuClient(host, port, "Alice")
    bob = MenuClient(host, port, "Bob")
    both = lambda t: [c.pump(t) for c in (alice, bob)]
    both(1.0)
    tb.OX, tb.OZ = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    for c in (alice, bob):
        c.command("gamemode creative")
    alice.command(f"tp {tb.OX + 0.5} -60 {tb.OZ + 0.5}")
    bob.command(f"tp {tb.OX + 0.5} -60 {tb.OZ - 1.5}")
    both(0.5)
    alice.rotate(0, 30)
    both(0.2)

    def place(item, x, z, seq):
        alice.creative_slot(36, "minecraft:" + item)
        alice.send("set_carried_item", struct.pack(">h", 0))
        alice.pump(0.1)
        alice.use_item_on(x, G, z, UP, seq)
        both(0.3)

    place("chest", 2, 0, 10)
    place("chest", 2, 3, 11)
    place("chest", 3, 3, 12)  # pairs with the one before: a double chest
    place("barrel", 5, 0, 13)
    place("crafting_table", -2, 0, 14)
    place("furnace", -2, 3, 15)
    alice.creative_slot(36, "minecraft:stone", 10)
    alice.pump(0.3)

    print("a chest opens a 9x3 menu; shift-clicking moves the stack in")
    bob.events.clear()
    alice.use_item_on(2, -60, 0, UP, 20, 0.5, 1.0, 0.5)
    both(0.5)
    menu = alice.menus.get(alice.open_id, {})
    check(menu.get("type") == "minecraft:generic_9x3" and title_key(menu.get("title")) == "container.chest", f"menu: {menu.get('type')} {menu.get('title')}")
    check(bob.saw("block_event", lambda i: i[0] == P(2, -60, 0) and i[1] == 1), f"Bob saw the lid open: {[e for e in bob.events if e[0] == 'block_event']}")
    alice.click(54, 0, 1)  # hotbar 0 (27 chest + 27 inventory slots before it)
    check(alice.slot(0) == ("minecraft:stone", 10) and alice.slot(54) is None, f"moved: chest 0 = {alice.slot(0)}, hotbar = {alice.slot(54)}")
    alice.close()

    print("another player sees what's inside")
    bob.use_item_on(2, -60, 0, UP, 21, 0.5, 1.0, 0.5)
    both(0.5)
    check(bob.slot(0) == ("minecraft:stone", 10), f"Bob sees {bob.slot(0)}")
    bob.click(0, 0, 0)  # pick the stack up
    check(bob.cursor == ("minecraft:stone", 10) and bob.slot(0) is None, f"Bob carries {bob.cursor}")
    bob.click(5, 1, 0)  # right click: one into slot 5
    check(bob.slot(5) == ("minecraft:stone", 1) and bob.cursor == ("minecraft:stone", 9), f"one placed: {bob.slot(5)}, cursor {bob.cursor}")
    bob.click(0, 0, 0)  # the rest into slot 0
    bob.close()

    print("a double chest opens as 9x6")
    alice.use_item_on(2, -60, 3, UP, 22, 0.5, 1.0, 0.5)
    both(0.5)
    menu = alice.menus.get(alice.open_id, {})
    check(menu.get("type") == "minecraft:generic_9x6" and title_key(menu.get("title")) == "container.chestDouble", f"menu: {menu.get('type')} {menu.get('title')}")
    alice.close()

    print("a barrel opens its lid")
    bob.events.clear()
    alice.use_item_on(5, -60, 0, UP, 23, 0.5, 1.0, 0.5)
    both(0.5)
    check(tb.state_str(bob.blocks.get(P(5, -60, 0), 0)).find("open=true") > 0, f"open: {tb.state_str(bob.blocks.get(P(5, -60, 0), 0))}")
    alice.close()
    both(0.3)
    check(tb.state_str(bob.blocks.get(P(5, -60, 0), 0)).find("open=false") > 0, f"closed: {tb.state_str(bob.blocks.get(P(5, -60, 0), 0))}")

    print("the crafting table crafts planks")
    alice.creative_slot(37, "minecraft:oak_log", 1)
    alice.pump(0.2)
    alice.use_item_on(-2, -60, 0, UP, 24, 0.5, 1.0, 0.5)
    both(0.5)
    menu = alice.menus.get(alice.open_id, {})
    check(menu.get("type") == "minecraft:crafting", f"menu: {menu.get('type')}")
    alice.click(38, 0, 0)  # hotbar 1: the log
    alice.click(5, 0, 0)   # into the middle of the grid
    check(alice.slot(0) == ("minecraft:oak_planks", 4), f"result: {alice.slot(0)}")
    alice.click(0, 0, 1)   # shift-click the result
    check(alice.slot(0) is None and alice.slot(5) is None and alice.slot(45) == ("minecraft:oak_planks", 4),
          f"crafted into the last hotbar slot: {alice.slot(45)}, grid {alice.slot(5)}")
    alice.close()

    print("the inventory's 2x2 grid crafts a crafting table")
    alice.click(44, 0, 0, cid=0)  # the planks (hotbar 8)
    for s in (1, 2, 3, 4):
        alice.click(s, 1, 0, cid=0)
    check(alice.slot(0, 0) == ("minecraft:crafting_table", 1), f"result: {alice.slot(0, 0)}")
    alice.click(0, 0, 0, cid=0)  # take it
    check(alice.cursor == ("minecraft:crafting_table", 1) and all(alice.slot(s, 0) is None for s in (1, 2, 3, 4)),
          f"took {alice.cursor}; grid {[alice.slot(s, 0) for s in (1, 2, 3, 4)]}")

    print("a furnace smelts raw iron with coal")
    alice.creative_slot(37, "minecraft:raw_iron", 1)
    alice.creative_slot(38, "minecraft:coal", 1)
    alice.pump(0.2)
    alice.use_item_on(-2, -60, 3, UP, 25, 0.5, 1.0, 0.5)
    both(0.5)
    check(alice.menus.get(alice.open_id, {}).get("type") == "minecraft:furnace", f"menu: {alice.menus.get(alice.open_id, {}).get('type')}")
    alice.click(31, 0, 1)  # raw iron: to the ingredient slot
    alice.click(32, 0, 1)  # coal: to the fuel slot
    check(alice.slot(0) == ("minecraft:raw_iron", 1) and alice.slot(1) is None, f"ingredient {alice.slot(0)}; coal burning, fuel slot {alice.slot(1)}")
    both(2.0)
    check("lit=true" in tb.state_str(bob.blocks.get(P(-2, -60, 3), 0)), f"lit: {tb.state_str(bob.blocks.get(P(-2, -60, 3), 0))}")
    data = alice.menus.get(alice.open_id, {}).get("data", {})
    check(data.get(0, 0) > 0 and data.get(1) == 1600 and data.get(3) == 200, f"burning (lit time, duration, progress, total): {data}")
    end = __import__("time").time() + 12
    while __import__("time").time() < end and alice.slot(2) is None:
        both(0.5)
    check(alice.slot(2) == ("minecraft:iron_ingot", 1) and alice.slot(0) is None, f"smelted: {alice.slot(2)}")
    alice.close()

    print("special recipes in the 2x2 grid: a firework rocket, repairing two pickaxes")
    empty = lambda slot: alice.send("set_creative_mode_slot", struct.pack(">h", slot) + enc_varint(0))
    alice.command("gamemode creative")
    alice.pump(0.3)
    alice.creative_slot(1, "minecraft:paper")
    alice.creative_slot(2, "minecraft:gunpowder")
    alice.pump(0.5)
    check(alice.slot(0, 0) == ("minecraft:firework_rocket", 3), f"rocket: {alice.slot(0, 0)}")
    empty(1)
    empty(2)
    alice.pump(0.3)
    alice.creative_slot(1, "minecraft:wooden_pickaxe", 1, [("minecraft:damage", enc_varint(50))])
    alice.creative_slot(3, "minecraft:wooden_pickaxe", 1, [("minecraft:damage", enc_varint(40))])
    alice.pump(0.5)
    full = alice.menus.get(0, {}).get("full", {}).get(0)
    # durability 59: (59 - 50) + (59 - 40) + 59 * 5 / 100 = 30 left, so 29 damage
    check(full and full[:2] == ("minecraft:wooden_pickaxe", 1) and full[2].get("minecraft:damage") == 29, f"repaired: {full}")
    empty(1)
    empty(3)
    alice.pump(0.3)

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all menu checks passed")


if __name__ == "__main__":
    main()
