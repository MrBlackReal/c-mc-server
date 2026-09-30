#!/usr/bin/env python3
"""Sign editing (SignBlock, SignBlockEntity, handleSignUpdate). Runs against vanilla too.

    test_signs.py HOST PORT [OUT.json]     run the checks; write the last sign data Bob saw to OUT.json
    test_signs.py compare A.json B.json    the two results must be identical

tools/test_signs.sh runs it on vanilla and on this server and compares the results.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from mcprobe import enc_str, enc_varint  # noqa: E402
from test_building import Client, P, check, failures  # noqa: E402

UP, EAST = 1, 5
G = -61


class SignClient(Client):
    def __init__(self, *a):
        self.sign_data = {}  # pos -> last block_entity_data NBT
        super().__init__(*a)

    def _handle(self, n, r):
        if n == "open_sign_editor":
            self.events.append((n, (tb.unpack_pos(r.i64()), r.u8() == 1)))
        elif n == "block_entity_data":
            pos = tb.unpack_pos(r.i64())
            r.varint()
            self.sign_data[pos] = r.nbt()
            self.events.append((n, pos))
        else:
            super()._handle(n, r)

    def hold(self, item):
        if item:
            self.creative_slot(36, "minecraft:" + item)
        else:
            self.send("set_creative_mode_slot", struct.pack(">h", 36) + enc_varint(0))  # an empty stack
        self.send("set_carried_item", struct.pack(">h", 0))
        self.pump(0.2)

    def sign_update(self, pos, lines, front=True):
        x, y, z = P(*pos)
        self.send("sign_update", tb.pack_pos(x, y, z) + b"".join(enc_str(s) for s in lines) + enc_varint(1 if front else 0))


def nbt_payload(v):
    """(tag type, payload) for strings, dicts and lists of either."""
    if isinstance(v, str):
        b = v.encode()
        return 8, struct.pack(">H", len(b)) + b
    if isinstance(v, dict):
        out = b""
        for k, x in v.items():
            t, p = nbt_payload(x)
            kb = k.encode()
            out += bytes([t]) + struct.pack(">H", len(kb)) + kb + p
        return 10, out + b"\x00"
    items = [nbt_payload(x) for x in v]
    if len({t for t, _ in items}) > 1:  # mixed list: wrap non-compounds as {"": value}
        items = [(10, p) if t == 10 else nbt_payload({"": x}) for (t, p), x in zip(items, v)]
    return 9, bytes([items[0][0] if items else 0]) + struct.pack(">i", len(items)) + b"".join(p for _, p in items)


def messages(nbt, side="front_text"):
    return [m if isinstance(m, str) else m.get("text") for m in (nbt or {}).get(side, {}).get("messages", [])]


def main():
    if sys.argv[1] == "compare":
        a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))
        for k in sorted(set(a) | set(b)):
            check(a.get(k) == b.get(k), f"sign {k}: vanilla {a.get(k)}\n{' ' * 9}ours    {b.get(k)}")
        finish()
        return

    host, port = sys.argv[1], int(sys.argv[2])
    alice = SignClient(host, port, "Alice")
    bob = SignClient(host, port, "Bob")
    both = lambda t: [c.pump(t) for c in (alice, bob)]
    both(1.0)
    tb.OX, tb.OZ = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    for c in (alice, bob):
        c.command("gamemode creative")
    alice.command(f"tp {tb.OX + 0.5} -60 {tb.OZ + 0.5}")
    bob.command(f"tp {tb.OX + 4.5} -60 {tb.OZ + 0.5}")  # behind the sign
    both(0.5)
    alice.rotate(270, 30)  # looking east
    both(0.2)

    sign = (2, -60, 0)
    print("placing a sign opens the editor")
    alice.hold("oak_sign")
    alice.use_item_on(2, G, 0, UP, 10)
    both(0.5)
    check(alice.saw("open_sign_editor", lambda i: i == (P(*sign), True)), "Alice edits the front")

    print("the editor's text reaches everyone, formatting codes stripped")
    alice.sign_update(sign, ["Hello", "§cRed", "", "x"])
    both(0.5)
    check(messages(bob.sign_data.get(P(*sign))) == ["Hello", "Red", "", "x"], f"Bob sees {messages(bob.sign_data.get(P(*sign)))}")

    print("only the player who may edit can change it")
    alice.sign_update(sign, ["Nope", "", "", ""])
    both(0.5)
    check(messages(bob.sign_data.get(P(*sign)))[0] == "Hello", "a second update is ignored")

    print("using the sign from behind edits the back")
    bob.events.clear()
    bob.hold(None)
    bob.use_item_on(2, -60, 0, EAST, 11, 1.0, 0.5, 0.5)
    both(0.5)
    check(bob.saw("open_sign_editor", lambda i: i == (P(*sign), False)), "Bob edits the back")
    bob.sign_update(sign, ["Back", "", "", ""], front=False)
    both(0.5)
    check(messages(alice.sign_data.get(P(*sign)), "back_text")[0] == "Back", "Alice sees the back text")

    print("dye, glow ink and honeycomb")
    alice.events.clear()
    alice.hold("red_dye")
    alice.use_item_on(2, -60, 0, UP, 12, 0.5, 0.5, 0.5)
    both(0.5)
    data = bob.sign_data.get(P(*sign)) or {}
    check(data.get("front_text", {}).get("color") == "red", f"dyed red: {data.get('front_text')}")
    check(bob.saw("sound", lambda i: i == "minecraft:item.dye.use"), "dye sound")
    alice.hold("glow_ink_sac")
    alice.use_item_on(2, -60, 0, UP, 13, 0.5, 0.5, 0.5)
    both(0.5)
    data = bob.sign_data.get(P(*sign)) or {}
    check(data.get("front_text", {}).get("has_glowing_text") == 1, "glowing")
    alice.hold("honeycomb")
    alice.use_item_on(2, -60, 0, UP, 14, 0.5, 0.5, 0.5)
    both(0.5)
    data = bob.sign_data.get(P(*sign)) or {}
    check(data.get("is_waxed") == 1, "waxed")
    check(bob.saw("level_event", lambda i: i and i[0] == 3003), "wax particles (level event 3003)")

    print("a waxed sign can't be edited")
    alice.events.clear()
    bob.events.clear()
    alice.hold(None)
    alice.use_item_on(2, -60, 0, UP, 15, 0.5, 0.5, 0.5)
    both(0.5)
    check(not alice.saw("open_sign_editor"), "no editor")
    check(bob.saw("sound", lambda i: i == "minecraft:block.sign.waxed_interact_fail"), "waxed sign sound")

    print("dye doesn't apply to a sign without text; the placer can reopen its editor")
    sign2 = (2, -60, 2)
    alice.hold("oak_sign")
    alice.use_item_on(2, G, 2, UP, 16)
    both(0.5)
    alice.events.clear()
    alice.hold("blue_dye")
    alice.use_item_on(2, -60, 2, UP, 17, 0.5, 0.5, 0.5)
    both(0.5)
    data = bob.sign_data.get(P(*sign2)) or {}
    check(data.get("front_text", {}).get("color", "black") == "black", f"still black: {data.get('front_text')}")
    check(alice.saw("open_sign_editor", lambda i: i == (P(*sign2), True)), "editor reopened")

    print("a hanging sign placed with styled text (sign_text_front) keeps the style when edited")
    alice.hold("stone")
    alice.use_item_on(0, G, 3, UP, 18)
    both(0.3)
    # SignText.STREAM_CODEC: four component tags, no filtered lines, dye color (black 15), not glowing.
    lines = [{"text": "old", "color": "gold"}, "", "", ""]
    text = b"".join(bytes([t]) + p for t, p in map(nbt_payload, lines)) + b"\x00" + enc_varint(15) + b"\x00"
    alice.creative_slot(36, "minecraft:oak_hanging_sign", 1, [("minecraft:sign_text_front", text)])
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.pump(0.2)
    alice.events.clear()
    alice.use_item_on(0, -60, 3, 2, 19, 0.5, 0.5, 0.0)  # the stone's north side
    both(0.5)
    hanging = (0, -60, 2)
    check(alice.saw("open_sign_editor", lambda i: i == (P(*hanging), True)), "editor opened")
    alice.sign_update(hanging, ["new", "", "", ""])
    both(0.5)
    first = ((bob.sign_data.get(P(*hanging)) or {}).get("front_text", {}).get("messages") or [None])[0]
    check(first == {"text": "new", "color": "gold"}, f"styled line: {first}")

    if len(sys.argv) > 3:
        out = {f"{x - tb.OX},{y},{z - tb.OZ}": v for (x, y, z), v in bob.sign_data.items()}
        json.dump(out, open(sys.argv[3], "w"), indent=1, sort_keys=True)
    finish()


def finish():
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all sign checks passed")


if __name__ == "__main__":
    main()
