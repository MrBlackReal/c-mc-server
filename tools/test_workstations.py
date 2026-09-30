#!/usr/bin/env python3
"""Workstation menus (stonecutter, grindstone, smithing table, anvil, loom, enchanting table). Runs
against vanilla too; the "OBS" lines are what to compare between the two servers.

    test_workstations.py HOST PORT
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from mcprobe import enc_str, enc_varint  # noqa: E402
from test_building import check, failures  # noqa: E402
from test_menus import MenuClient, title_key  # noqa: E402

UP = 1
G = -61
ENCHANTMENT = {n: i for i, n in enumerate(tb.SYNCED["minecraft:enchantment"])}


def enchants(**levels):
    return enc_varint(len(levels)) + b"".join(enc_varint(ENCHANTMENT["minecraft:" + k]) + enc_varint(v) for k, v in levels.items())


def obs(name, value):
    print(f"OBS {name}: {value}")


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    alice = MenuClient(host, port, "Alice")
    alice.pump(1.0)
    tb.OX, tb.OZ = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    alice.command("gamemode creative")
    alice.command(f"tp {tb.OX + 0.5} -60 {tb.OZ + 0.5}")
    alice.pump(0.5)
    alice.rotate(0, 30)
    alice.pump(0.2)
    empty = lambda slot: alice.send("set_creative_mode_slot", struct.pack(">h", slot) + enc_varint(0))

    def place(item, x, z, seq):
        alice.creative_slot(36, "minecraft:" + item)
        alice.send("set_carried_item", struct.pack(">h", 0))
        alice.pump(0.1)
        alice.use_item_on(x, G, z, UP, seq)
        alice.pump(0.3)
        empty(36)

    place("stonecutter", 2, 0, 10)
    place("grindstone", 2, 2, 11)
    place("smithing_table", -2, 0, 12)
    place("anvil", -2, 2, 13)
    place("loom", 0, 3, 14)
    place("enchanting_table", 0, -3, 15)
    alice.pump(0.3)
    seq = [20]

    def open_at(x, z):
        seq[0] += 1
        alice.use_item_on(x, -60, z, UP, seq[0], 0.5, 1.0, 0.5)
        alice.pump(0.5)
        return alice.menus.get(alice.open_id, {})

    def full(i):
        return alice.menus.get(alice.open_id, {}).get("full", {}).get(i)

    def data():
        return alice.menus.get(alice.open_id, {}).get("data", {})

    print("stonecutter: stone -> choose a recipe -> shift-click the result")
    alice.creative_slot(37, "minecraft:stone", 3)
    alice.pump(0.3)
    menu = open_at(2, 0)
    check(menu.get("type") == "minecraft:stonecutter" and title_key(menu.get("title")) == "container.stonecutter", f"menu {menu.get('type')} {menu.get('title')}")
    alice.click(30, 0, 1)  # hotbar 1 -> input
    check(alice.slot(0) == ("minecraft:stone", 3), f"input {alice.slot(0)}")
    alice.send("container_button_click", enc_varint(alice.open_id) + enc_varint(2))
    alice.pump(0.3)
    obs("stonecutter selected", data().get(0))
    obs("stonecutter result", alice.slot(1))
    check(alice.slot(1) is not None, f"result {alice.slot(1)}")
    alice.click(1, 0, 1)  # everything the input makes
    obs("stonecutter after shift", (alice.slot(0), alice.slot(1), alice.slot(37)))
    check(alice.slot(0) is None and alice.slot(1) is None, f"used up: input {alice.slot(0)}, result {alice.slot(1)}")
    alice.close()
    empty(45)
    empty(44)

    print("grindstone: disenchant a sword, merge two pickaxes")
    alice.creative_slot(37, "minecraft:diamond_sword", 1, [("minecraft:enchantments", enchants(sharpness=3, vanishing_curse=1)),
                                                            ("minecraft:damage", enc_varint(100))])
    alice.creative_slot(38, "minecraft:iron_pickaxe", 1, [("minecraft:damage", enc_varint(200))])
    alice.creative_slot(39, "minecraft:iron_pickaxe", 1, [("minecraft:damage", enc_varint(150)), ("minecraft:enchantments", enchants(efficiency=2))])
    alice.pump(0.3)
    menu = open_at(2, 2)
    check(menu.get("type") == "minecraft:grindstone", f"menu {menu.get('type')}")
    alice.click(31, 0, 1)
    obs("grindstone sword", full(2))
    check(full(2) and full(2)[2].get("minecraft:enchantments") == {"minecraft:vanishing_curse": 1}, f"curse only: {full(2)}")
    alice.click(0, 0, 1)  # back out
    alice.click(32, 0, 1)
    alice.click(33, 0, 1)
    obs("grindstone pickaxes", full(2))
    check(full(2) is not None and full(2)[2].get("minecraft:damage") == 88, f"merged {full(2)}")
    orbs = []
    handle0 = alice._handle
    orb_type = tb.REG["minecraft:entity_type"]["entries"]["minecraft:experience_orb"]["protocol_id"]
    def watch_orbs(n, r):
        if n == "add_entity":
            start = r.i
            r.varint(), r.bytes(16)
            if r.varint() == orb_type:
                orbs.append((round(r.f64() - tb.OX, 3), round(r.f64(), 3), round(r.f64() - tb.OZ, 3)))
            r.i = start
        handle0(n, r)
    alice._handle = watch_orbs
    alice.click(2, 0, 1)
    alice.pump(0.5)
    alice._handle = handle0
    obs("grindstone taken", (alice.slot(0), alice.slot(1), alice.slot(2)))
    obs("grindstone orbs at", sorted(set(orbs)))  # spawned inside the grindstone: moved to a free spot
    check(orbs, "experience orbs")
    alice.close()
    for s in range(36, 45):
        empty(s)
    alice.pump(0.3)

    print("smithing: a netherite upgrade and an armor trim")
    alice.creative_slot(36, "minecraft:netherite_upgrade_smithing_template", 2)
    alice.creative_slot(37, "minecraft:diamond_sword", 1, [("minecraft:damage", enc_varint(33))])
    alice.creative_slot(38, "minecraft:netherite_ingot", 2)
    alice.creative_slot(39, "minecraft:sentry_armor_trim_smithing_template", 1)
    alice.creative_slot(40, "minecraft:iron_chestplate", 1)
    alice.creative_slot(41, "minecraft:gold_ingot", 1)
    alice.pump(0.3)
    menu = open_at(-2, 0)
    check(menu.get("type") == "minecraft:smithing" and title_key(menu.get("title")) == "container.upgrade", f"menu {menu.get('type')} {menu.get('title')}")
    for hot in (0, 1, 2):
        alice.click(31 + hot, 0, 1)
    obs("smithing slots", (alice.slot(0), alice.slot(1), alice.slot(2)))
    obs("smithing upgrade", full(3))
    check(full(3) and full(3)[0] == "minecraft:netherite_sword" and full(3)[2].get("minecraft:damage") == 33, f"upgrade {full(3)}")
    alice.click(3, 0, 1)
    obs("smithing after take", (alice.slot(0), alice.slot(1), alice.slot(2), alice.slot(3)))
    for i in (0, 2):
        alice.click(i, 0, 1)
    for hot in (3, 4, 5):
        alice.click(31 + hot, 0, 1)
    obs("smithing trim", full(3))
    check(full(3) and full(3)[2].get("minecraft:trim") == {"material": "minecraft:gold", "pattern": "minecraft:sentry"}, f"trim {full(3)}")
    obs("smithing error flag", data().get(0))
    alice.close()
    for s in range(36, 45):
        empty(s)
    alice.pump(0.3)

    print("anvil: rename, then combine a sword with a book")
    alice.creative_slot(36, "minecraft:iron_sword", 1, [("minecraft:damage", enc_varint(50))])
    # (one enchantment: vanilla iterates several in identity-hash order, which varies from run to run)
    alice.creative_slot(37, "minecraft:enchanted_book", 1, [("minecraft:stored_enchantments", enchants(sharpness=2))])
    alice.pump(0.3)
    menu = open_at(-2, 2)
    check(menu.get("type") == "minecraft:anvil" and title_key(menu.get("title")) == "container.repair", f"menu {menu.get('type')} {menu.get('title')}")
    alice.click(30, 0, 1)
    alice.send("rename_item", enc_str("Blade"))
    alice.pump(0.3)
    obs("anvil rename", (full(2), data().get(0)))
    check(full(2) and full(2)[2].get("minecraft:custom_name") == "Blade" and data().get(0) == 1, f"renamed {full(2)} cost {data()}")
    alice.click(31, 0, 1)
    obs("anvil combine", (full(2), data().get(0)))
    check(full(2) and full(2)[2].get("minecraft:enchantments") == {"minecraft:sharpness": 2}, f"combined {full(2)} cost {data()}")
    alice.click(2, 0, 1)
    obs("anvil taken", (alice.slot(0), alice.slot(1), alice.slot(2), alice.slot(30)))
    alice.close()
    for s in range(36, 45):
        empty(s)
    alice.pump(0.3)

    print("loom: a banner, a dye and a pattern button")
    alice.creative_slot(36, "minecraft:white_banner", 2)
    alice.creative_slot(37, "minecraft:red_dye", 2)
    alice.creative_slot(38, "minecraft:flower_banner_pattern", 1)
    alice.pump(0.3)
    menu = open_at(0, 3)
    check(menu.get("type") == "minecraft:loom", f"menu {menu.get('type')}")
    alice.click(31, 0, 1)
    alice.click(32, 0, 1)
    alice.send("container_button_click", enc_varint(alice.open_id) + enc_varint(3))
    alice.pump(0.3)
    obs("loom button 3", (data().get(0), full(3)))
    alice.click(33, 0, 1)
    obs("loom with pattern", (data().get(0), full(3)))
    check(full(3) and full(3)[2].get("minecraft:banner_patterns") == [("minecraft:flower", 14)], f"flower {full(3)}")
    alice.click(3, 0, 1)
    obs("loom taken", (alice.slot(0), alice.slot(1), alice.slot(2), alice.slot(3), data().get(0)))
    alice.close()
    for s in range(36, 45):
        empty(s)
    alice.pump(0.3)

    print("enchanting table: costs appear, a button enchants")
    alice.creative_slot(36, "minecraft:diamond_pickaxe", 1)
    alice.creative_slot(37, "minecraft:lapis_lazuli", 5)
    alice.pump(0.3)
    menu = open_at(0, -3)
    check(menu.get("type") == "minecraft:enchantment" and title_key(menu.get("title")) == "container.enchant", f"menu {menu.get('type')} {menu.get('title')}")
    alice.click(29, 0, 1)
    alice.click(30, 0, 1)
    d = data()
    obs("enchanting costs", [d.get(i) for i in range(3)])
    check(all(d.get(i, 0) > 0 for i in range(3)) and all(d.get(4 + i, -1) >= 0 for i in range(3)), f"costs and clues {d}")
    alice.send("container_button_click", enc_varint(alice.open_id) + enc_varint(1))
    alice.pump(0.3)
    obs("enchanted", (full(0) and sorted(full(0)[2].get("minecraft:enchantments", {})), alice.slot(1)))
    check(full(0) and full(0)[2].get("minecraft:enchantments") and alice.slot(1) == ("minecraft:lapis_lazuli", 5), f"enchanted (creative keeps the lapis) {full(0)}, lapis {alice.slot(1)}")
    alice.close()

    for s_ in range(36, 45):
        empty(s_)
    alice.pump(0.3)

    print("lectern: place a book, turn a page, take it")
    pages = [b"one", b"two", b"three"]
    book = enc_varint(len(pages)) + b"".join(enc_varint(len(p)) + p + b"\x00" for p in pages)
    alice.creative_slot(36, "minecraft:writable_book", 1, [("minecraft:writable_book_content", book)])
    alice.pump(0.3)
    place_keep = lambda x, z: (alice.use_item_on(x, -60, z, UP, 90, 0.5, 1.0, 0.5), alice.pump(0.4))
    alice.creative_slot(37, "minecraft:lectern")
    alice.send("set_carried_item", struct.pack(">h", 1))
    alice.pump(0.1)
    alice.use_item_on(4, G, -3, UP, 91)
    alice.pump(0.3)
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.pump(0.1)
    place_keep(4, -3)  # the book goes onto the lectern
    obs("lectern state", tb.state_str(alice.blocks.get(tb.P(4, -60, -3), 0)))
    check("has_book=true" in tb.state_str(alice.blocks.get(tb.P(4, -60, -3), 0)), "the lectern has the book")
    menu = open_at(4, -3)
    check(menu.get("type") == "minecraft:lectern" and title_key(menu.get("title")) == "container.lectern", f"menu {menu.get('type')} {menu.get('title')}")
    obs("lectern book", alice.slot(0))
    alice.send("container_button_click", enc_varint(alice.open_id) + enc_varint(2))
    alice.pump(0.3)
    obs("lectern page after next", data().get(0))
    check(data().get(0) == 1, f"page {data()}")
    alice.send("container_button_click", enc_varint(alice.open_id) + enc_varint(105))
    alice.pump(0.3)
    obs("lectern page after jump", data().get(0))
    check(data().get(0) == 2, f"clamped to the last page: {data()}")
    alice.send("container_button_click", enc_varint(alice.open_id) + enc_varint(3))
    alice.pump(0.5)
    obs("lectern after take", (tb.state_str(alice.blocks.get(tb.P(4, -60, -3), 0)), alice.menus.get(0, {}).get("slots", {}).get(36)))
    check("has_book=false" in tb.state_str(alice.blocks.get(tb.P(4, -60, -3), 0)), "the book is gone")
    alice.close()
    for s_ in range(36, 45):
        empty(s_)
    alice.pump(0.3)

    print("beacon: pay without choosing an effect")
    place("beacon", 6, 0, 92)
    alice.creative_slot(36, "minecraft:iron_ingot", 1)
    alice.pump(0.3)
    menu = open_at(6, 0)
    check(menu.get("type") == "minecraft:beacon" and title_key(menu.get("title")) == "container.beacon", f"menu {menu.get('type')} {menu.get('title')}")
    alice.click(28, 0, 1)  # hotbar 0 -> the payment slot
    obs("beacon payment", (alice.slot(0), data()))
    alice.send("set_beacon", b"\x00\x00")
    alice.pump(0.3)
    obs("beacon after set", alice.slot(0))
    check(alice.slot(0) is None, f"payment used: {alice.slot(0)}")
    alice.close()

    print("beacon on a pyramid: one level, the chosen effect reaches the player")
    effects = []
    handle = alice._handle
    def watch(n, r):
        if n == "update_mob_effect":
            start = r.i
            r.varint()
            effects.append(tb.REG_NAMES["minecraft:mob_effect"][r.varint()] if hasattr(tb, "REG_NAMES") else r.varint())
            r.i = start
        handle(n, r)
    alice._handle = watch
    ox, oz = tb.OX, tb.OZ  # (the test's coordinates are relative to where Alice spawned)
    alice.command(f"fill {ox - 4} -61 {oz - 4} {ox - 2} -61 {oz - 2} minecraft:iron_block")
    alice.command(f"setblock {ox - 3} -60 {oz - 3} minecraft:beacon")
    alice.pump(6.0)  # (levels are worked out every 80 ticks, once the beam has been checked)
    alice.creative_slot(36, "minecraft:iron_ingot", 1)
    alice.pump(0.3)
    menu = open_at(-3, -3)
    check(menu.get("type") == "minecraft:beacon", f"opened {menu.get('type')}; acks {alice.acks[-3:]}, block {tb.state_str(alice.blocks.get(tb.P(-3, -60, -3), 0))}")
    obs("beacon levels", data().get(0))
    check(data().get(0) == 1, f"levels {data()}")
    alice.click(28, 0, 1)
    speed = tb.REG["minecraft:mob_effect"]["entries"]["minecraft:speed"]["protocol_id"]
    alice.send("set_beacon", b"\x01" + enc_varint(speed) + b"\x00")
    alice.pump(0.5)
    obs("beacon effect data", (data().get(1), alice.slot(0)))
    check(data().get(1) == speed + 1, f"primary effect {data()}")
    alice.close()
    end = __import__("time").time() + 6
    while __import__("time").time() < end and not effects:
        alice.pump(0.5)
    obs("effects given", effects)
    check(speed in effects, f"speed given: {effects}")
    alice._handle = handle

    print("crafter: a log shows planks, an empty slot can be disabled")
    place("crafter", 4, 3, 93)
    alice.creative_slot(36, "minecraft:oak_log", 1)
    alice.pump(0.3)
    menu = open_at(4, 3)
    check(menu.get("type") == "minecraft:crafter_3x3" and title_key(menu.get("title")) == "container.crafter", f"menu {menu.get('type')} {menu.get('title')}")
    alice.click(36, 0, 0)  # hotbar 0: pick up the log
    alice.click(4, 0, 0)   # into the middle
    obs("crafter preview", alice.slot(45))
    check(alice.slot(45) == ("minecraft:oak_planks", 4), f"preview {alice.slot(45)}")
    alice.send("container_slot_state_changed", enc_varint(0) + enc_varint(alice.open_id) + b"\x00")
    alice.send("container_slot_state_changed", enc_varint(4) + enc_varint(alice.open_id) + b"\x00")  # (not empty: ignored)
    alice.pump(0.5)
    obs("crafter disabled", [data().get(i, 0) for i in range(10)])
    check(data().get(0) == 1 and data().get(4, 0) == 0, f"disabled slots {data()}")
    alice.close()
    for s_ in range(36, 45):
        empty(s_)
    alice.pump(0.3)

    print("brewing stand: water + nether wart with blaze powder -> awkward potion")
    place("brewing_stand", -4, 0, 94)
    water = b"\x01" + enc_varint(tb.REG["minecraft:potion"]["entries"]["minecraft:water"]["protocol_id"]) + b"\x00" + enc_varint(0) + b"\x00"
    alice.creative_slot(36, "minecraft:blaze_powder", 1)
    alice.creative_slot(37, "minecraft:potion", 1, [("minecraft:potion_contents", water)])
    alice.creative_slot(38, "minecraft:nether_wart", 1)
    alice.pump(0.3)
    menu = open_at(-4, 0)
    check(menu.get("type") == "minecraft:brewing_stand" and title_key(menu.get("title")) == "container.brewing", f"menu {menu.get('type')} {menu.get('title')}")
    alice.click(32, 0, 1)  # blaze powder -> fuel
    alice.click(33, 0, 1)  # water bottle -> a bottle slot
    alice.click(34, 0, 1)  # nether wart -> ingredient
    alice.pump(1.0)
    obs("brewing started", ([alice.slot(i) for i in range(5)], sorted(data().items())))
    check(data().get(1) == 19 and data().get(0, 0) > 350, f"brewing: {data()}")
    check("has_bottle_0=true" in tb.state_str(alice.blocks.get(tb.P(-4, -60, 0), 0)), "a bottle on the stand")
    end = __import__("time").time() + 25
    while __import__("time").time() < end and alice.slot(3) is not None:
        alice.pump(0.5)
    obs("brewed", (full(0), alice.slot(3), sorted(data().items())))
    check(full(0) and full(0)[2].get("minecraft:potion_contents", {}).get("potion") == "minecraft:awkward", f"awkward: {full(0)}")
    alice.close()

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all workstation checks passed")


if __name__ == "__main__":
    main()
