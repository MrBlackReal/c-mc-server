#!/usr/bin/env python3
"""Scripted two-client test for building, inventory and time (milestone 2).

    tools/test_building.py HOST PORT

Alice places and breaks blocks; Bob watches. Block states are decoded with data/blocks.json so
the checks read like "Bob sees minecraft:oak_door[half=upper,...] at (2,-59,1)".
"""
import json
import os
import socket
import struct
import sys
import time
import uuid

sys.path.insert(0, os.path.dirname(__file__))
from mcprobe import Conn, Reader, enc_str, enc_varint, handshake, load_names  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DATA = os.path.join(ROOT, "data")

STATES = {}
for name, b in json.load(open(os.path.join(DATA, "blocks.json"))).items():
    for s in b["states"]:
        STATES[s["id"]] = (name, s.get("properties", {}))
REG = json.load(open(os.path.join(DATA, "registries.json")))
ITEM = {k: v["protocol_id"] for k, v in REG["minecraft:item"]["entries"].items()}
ITEM_NAME = {v: k for k, v in ITEM.items()}
PACKETS = json.load(open(os.path.join(DATA, "packets.json")))
SYNCED = {r["registry"]: r["entries"] for r in json.load(open(os.path.join(DATA, "vanilla_config.json")))["registries"]}
SB = {k.split(":")[1]: v["protocol_id"] for k, v in PACKETS["play"]["serverbound"].items()}
NAMES = load_names(os.path.join(DATA, "packets.json"))

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


OX = OZ = 0


def P(x, y, z):
    """Test coordinates are relative to Alice's spawn block in x/z; y is absolute (superflat)."""
    return (OX + x, y, OZ + z)


def pack_pos(x, y, z):
    return struct.pack(">Q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))


def unpack_pos(v):
    x = v >> 38
    y = (v << 52 & 0xFFFFFFFFFFFFFFFF) >> 52
    z = (v << 26 & 0xFFFFFFFFFFFFFFFF) >> 38
    if y >= 2048:
        y -= 4096
    if z >= 1 << 25:  # 26-bit signed
        z -= 1 << 26
    return x, y, z


def state_str(sid):
    name, props = STATES[sid]
    return name + ("[" + ",".join(f"{k}={v}" for k, v in sorted(props.items())) + "]" if props else "")


COMPONENT = {v["protocol_id"]: k for k, v in REG["minecraft:data_component_type"]["entries"].items()}
COMPONENT_ID = {k: v for v, k in COMPONENT.items()}
POTION_NAME = {v["protocol_id"]: k for k, v in REG["minecraft:potion"]["entries"].items()}
SOUND_NAME = {v["protocol_id"]: k for k, v in REG["minecraft:sound_event"]["entries"].items()}
VARINT_COMPONENTS = ("minecraft:damage", "minecraft:max_damage", "minecraft:repair_cost", "minecraft:max_stack_size")


class UndecodedComponent(Exception):
    pass


def read_component(r, t):
    """One component value (the StreamCodec form) for the types the tests look at."""
    if t in VARINT_COMPONENTS:
        return r.varint()
    if t in ("minecraft:custom_name", "minecraft:item_name", "minecraft:custom_data"):
        return r.nbt()
    if t == "minecraft:lore":
        return [r.nbt() for _ in range(r.varint())]
    if t == "minecraft:container":  # list of optional ItemStackTemplates
        return [read_template(r) if r.bool() else None for _ in range(r.varint())]
    if t == "minecraft:bundle_contents":  # list of ItemStackTemplates
        return [read_template(r) for _ in range(r.varint())]
    if t in ("minecraft:enchantments", "minecraft:stored_enchantments"):  # holder -> level
        names = SYNCED["minecraft:enchantment"]
        return {names[r.varint()]: r.varint() for _ in range(r.varint())}
    if t == "minecraft:trim":  # (material, pattern) registry holders
        return {"material": _holder(r, "minecraft:trim_material"), "pattern": _holder(r, "minecraft:trim_pattern")}
    if t == "minecraft:banner_patterns":  # layers of (pattern holder, dye color)
        return [(_holder(r, "minecraft:banner_pattern"), r.varint()) for _ in range(r.varint())]
    if t in ("minecraft:unbreakable", "minecraft:glider"):
        return True
    if t == "minecraft:potion_contents":  # optional potion, optional color, effects, optional name
        potion = POTION_NAME[r.varint()] if r.bool() else None
        color = r.i32() if r.bool() else None
        if r.varint():
            raise UndecodedComponent(t)  # (custom effects)
        name = r.str() if r.bool() else None
        return {"potion": potion, "color": color, "name": name}
    raise UndecodedComponent(t)


def _holder(r, registry):
    i = r.varint()
    if i == 0:
        raise UndecodedComponent(registry)  # an inline value
    return SYNCED[registry][i - 1]


def read_patch(r):
    added, removed = r.varint(), r.varint()
    comps = {}
    for _ in range(added):
        t = COMPONENT[r.varint()]
        try:
            comps[t] = read_component(r, t)
        except UndecodedComponent:
            comps[t] = "?"
            raise
    for _ in range(removed):
        comps["!" + COMPONENT[r.varint()]] = True
    return comps


def read_template(r):
    item = ITEM_NAME[r.varint()]
    count = r.varint()
    return (item, count, read_patch(r))


def read_item_ex(r):
    """ItemStack.OPTIONAL_STREAM_CODEC -> (item, count, components). Components the tests don't
    decode end decoding (marked "?"); the rest of the packet is then unreliable."""
    count = r.varint()
    if count <= 0:
        return None
    item = ITEM_NAME[r.varint()]
    added, removed = r.varint(), r.varint()
    comps = {}
    for _ in range(added):
        t = COMPONENT[r.varint()]
        try:
            comps[t] = read_component(r, t)
        except UndecodedComponent:
            comps[t] = "?"
            return (item, count, comps)
    for _ in range(removed):
        comps["!" + COMPONENT[r.varint()]] = True
    return (item, count, comps)


def read_item(r):
    v = read_item_ex(r)
    return v[:2] if v else None


class Client:
    def __init__(self, host, port, name):
        self.name = name
        self.c = Conn(host, port)
        handshake(self.c, host, port, 2)
        self.c.send(0, enc_str(name) + uuid.uuid3(uuid.NAMESPACE_DNS, name).bytes)
        self.blocks = {}       # (x,y,z) -> state id, from block updates
        self.events = []       # (packet name, info)
        self.slots = {}        # inventory slot -> (item, count)
        self.acks = []
        self.time = None
        self.clocks = {}
        self._login()

    def _login(self):
        c = self.c
        while True:
            pid, r = c.recv()
            if pid == 3:
                c.threshold = r.varint()
            elif pid == 1:  # an encryption request (online mode): see test_online.py
                self.on_encryption_request(r)
            elif pid == 2:
                self.login_uuid = uuid.UUID(bytes=r.bytes(16))
                self.login_name = r.str()
                self.login_properties = [(r.str(), r.str(), r.str() if r.bool() else None) for _ in range(r.varint())]
                c.send(3)
                break
            else:
                raise RuntimeError(f"login failed ({pid}): {r.rest()[:200]}")
        c.send(0, enc_str("en_us") + bytes([8]) + enc_varint(0) + b"\x01" + bytes([0x7F]) + enc_varint(1) + b"\x00\x01" + enc_varint(0))
        while True:
            pid, r = c.recv()
            n = NAMES.get(("configuration", "clientbound", pid))
            if n == "select_known_packs":
                packs = [(r.str(), r.str(), r.str()) for _ in range(r.varint())]
                c.send(7, enc_varint(len(packs)) + b"".join(enc_str(x) for p in packs for x in p))
            elif n == "keep_alive":
                c.send(4, r.bytes(8))
            elif n == "finish_configuration":
                c.send(3)
                break
        self.pump(until=lambda: self.pos is not None, timeout=5)

    pos = None
    state_id = 0

    def on_encryption_request(self, r):
        raise RuntimeError("the server wants online-mode encryption")

    def send(self, name, payload=b""):
        if name.startswith("move_player"):
            self.c.send(SB["client_tick_end"])  # a real client ends a tick before each move (one position per tick)
        self.c.send(SB[name], payload)

    def pump(self, seconds=0.0, until=None, timeout=3.0):
        deadline = time.time() + (timeout if until else seconds)
        self.c.s.settimeout(0.05)
        while time.time() < deadline:
            if until and until():
                return True
            try:
                pid, r = self.c.recv()
            except socket.timeout:
                continue
            self._handle(NAMES.get(("play", "clientbound", pid), str(pid)), r)
        return until() if until else True

    def _handle(self, n, r):
        if n == "keep_alive":
            self.send("keep_alive", r.bytes(8))
        elif n == "player_position":
            tid = r.varint()
            x, y, z = r.f64(), r.f64(), r.f64()
            r.f64(), r.f64(), r.f64()
            yr, xr = r.f32(), r.f32()
            self.send("accept_teleportation", enc_varint(tid) + struct.pack(">dddff", x, y, z, yr, xr))
            if self.pos is None:
                self.send("player_loaded")
            self.pos = (x, y, z)
        elif n == "chunk_batch_finished":
            self.send("chunk_batch_received", struct.pack(">f", 64.0))
        elif n == "block_update":
            p = unpack_pos(r.i64())
            self.blocks[p] = r.varint()
            self.events.append((n, p))
        elif n == "section_blocks_update":
            sec = r.i64()
            sign = lambda v, bits: v - (1 << bits) if v >= 1 << (bits - 1) else v
            sx = sec >> 42
            sy = sign(sec & 0xFFFFF, 20)
            sz = sign((sec >> 20) & 0x3FFFFF, 22)
            for _ in range(r.varint()):
                v = _read_varlong(r)
                sid, local = v >> 12, v & 0xFFF
                p = (sx * 16 + (local >> 8 & 15), sy * 16 + (local & 15), sz * 16 + (local >> 4 & 15))
                self.blocks[p] = sid
                self.events.append(("block_update", p))
        elif n == "block_changed_ack":
            self.acks.append(r.varint())
        elif n == "container_set_content":
            r.varint()
            self.state_id = r.varint()
            for i in range(r.varint()):
                item = read_item_ex(r)
                self.slots[i] = item[:2] if item else None
                if item and "?" in item[2].values():
                    break  # undecoded component: the rest of the packet is unreliable
            self.events.append((n, None))
        elif n == "container_set_slot":
            r.varint()
            self.state_id = r.varint()
            slot = r.i16()
            self.slots[slot] = read_item(r)
            self.events.append((n, slot))
        elif n == "block_event":
            self.events.append((n, (unpack_pos(r.i64()), r.u8(), r.u8(), r.varint())))
        elif n == "set_cursor_item":
            self.events.append((n, read_item(r)))
        elif n == "set_time":
            game = r.i64()
            clocks = {}
            for _ in range(r.varint()):
                cid = r.varint()
                clocks[cid] = (_read_varlong(r), r.f32(), r.f32())
            if clocks:
                self.clocks = clocks
            self.time = (game, self.clocks)
            self.events.append((n, (game, clocks)))
        elif n in ("level_event", "sound", "swing_animation", "set_equipment", "add_entity"):
            info = None
            if n == "level_event":
                info = (r.i32(), unpack_pos(r.i64()), r.i32())
            elif n == "sound":
                info = SOUND_NAME.get(r.varint() - 1)  # Holder: registry id + 1
            elif n == "set_equipment":
                eid = r.varint()
                while True:
                    slot = r.u8()
                    self.events.append((n, (eid, slot & 0x7F, read_item(r))))
                    if not slot & 0x80:
                        return
            self.events.append((n, info))

    def saw(self, name, pred=lambda i: True):
        return any(n == name and pred(i) for n, i in self.events)

    # --- actions
    def creative_slot(self, slot, item, count=1, components=()):
        """components: (type name, value bytes) pairs, sent length-prefixed like the client does."""
        comps = b"".join(enc_varint(COMPONENT_ID[t]) + enc_varint(len(v)) + v for t, v in components)
        self.send("set_creative_mode_slot", struct.pack(">h", slot) + enc_varint(count) + enc_varint(ITEM[item])
                  + enc_varint(len(components)) + b"\x00" + comps)

    def use_item_on(self, x, y, z, face, seq, cx=0.5, cy=1.0, cz=0.5):
        x, y, z = P(x, y, z)
        self.send("use_item_on", enc_varint(0) + pack_pos(x, y, z) + enc_varint(face) + struct.pack(">fff", cx, cy, cz)
                  + b"\x00\x00" + enc_varint(seq))

    def action(self, action, x, y, z, seq, face=1):
        x, y, z = P(x, y, z)
        self.send("player_action", enc_varint(action) + pack_pos(x, y, z) + bytes([face]) + enc_varint(seq))

    def rotate(self, yrot, xrot):
        self.send("move_player_rot", struct.pack(">ffB", yrot, xrot, 1))

    def command(self, cmd):
        self.send("chat_command", enc_str(cmd))


def _read_varlong(r):
    n = 0
    for shift in range(0, 70, 7):
        b = r.u8()
        n |= (b & 0x7F) << shift
        if not b & 0x80:
            return n
    raise ValueError("varlong too long")


UP, NORTH, SOUTH, WEST, EAST = 1, 2, 3, 4, 5


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    alice = Client(host, port, "Alice")
    bob = Client(host, port, "Bob")
    for c in (alice, bob):
        c.pump(1.0)
    global OX, OZ
    OX, OZ = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    print(f"join (Alice at {alice.pos})")
    # Vanilla scatters new players around spawn; keep Bob out of the build area (x/z offsets 0..5).
    bob.command(f"tp {OX - 3 + 0.5} -60 {OZ - 3 + 0.5}")
    for c in (alice, bob):
        c.pump(0.5)
    check(alice.saw("container_set_content"), "Alice received her inventory (container_set_content)")
    check(alice.time is not None and len(alice.time[1]) == 2, f"Alice received full clock sync {alice.time}")

    print("creative inventory + held item")
    alice.command("gamemode creative")
    alice.pump(0.3)
    alice.creative_slot(36, "minecraft:stone", 64)
    alice.creative_slot(37, "minecraft:oak_door")
    alice.creative_slot(38, "minecraft:oak_stairs")
    alice.creative_slot(39, "minecraft:oak_fence")
    alice.creative_slot(40, "minecraft:torch")
    alice.creative_slot(5, "minecraft:diamond_helmet")
    for c in (alice, bob):
        c.pump(0.5)
    check(bob.saw("set_equipment", lambda i: i and i[1] == 0 and i[2] == ("minecraft:stone", 64)),
          "Bob sees Alice holding stone (set_equipment mainhand)")
    check(bob.saw("set_equipment", lambda i: i and i[2] and i[2][0] == "minecraft:diamond_helmet"),
          "Bob sees Alice's helmet")

    print("place stone")
    alice.rotate(0, 60)
    alice.use_item_on(2, -61, 0, UP, seq=1)
    for c in (alice, bob):
        c.pump(0.5)
    check(1 in alice.acks, "Alice got block_changed_ack(1)")
    check(bob.blocks.get(P(2, -60, 0)) is not None and state_str(bob.blocks[P(2, -60, 0)]) == "minecraft:stone",
          f"Bob sees stone at (2,-60,0): {state_str(bob.blocks[P(2, -60, 0)]) if P(2, -60, 0) in bob.blocks else None}")
    check(bob.saw("sound"), "Bob hears the place sound")
    check(bob.saw("swing_animation"), "Bob sees Alice swing")

    print("break stone (creative)")
    alice.action(0, 2, -60, 0, seq=2)
    for c in (alice, bob):
        c.pump(0.5)
    check(2 in alice.acks, "Alice got block_changed_ack(2)")
    check(state_str(bob.blocks.get(P(2, -60, 0), 0)) == "minecraft:air", "Bob sees the stone removed")
    check(bob.saw("level_event", lambda i: i and i[0] == 2001 and i[1] == P(2, -60, 0)), "Bob gets break particles (level_event 2001)")

    print("door (two halves, facing, open/close)")
    alice.send("set_carried_item", struct.pack(">h", 1))
    alice.rotate(0, 30)  # facing south
    alice.use_item_on(2, -61, 1, UP, seq=3)
    for c in (alice, bob):
        c.pump(0.5)
    lower, upper = bob.blocks.get(P(2, -60, 1)), bob.blocks.get(P(2, -59, 1))
    check(lower is not None and STATES[lower][0] == "minecraft:oak_door" and STATES[lower][1]["half"] == "lower"
          and STATES[lower][1]["facing"] == "south", f"lower door half: {state_str(lower) if lower else None}")
    check(upper is not None and STATES[upper][0] == "minecraft:oak_door" and STATES[upper][1]["half"] == "upper",
          f"upper door half: {state_str(upper) if upper else None}")
    alice.use_item_on(2, -60, 1, NORTH, seq=4, cy=0.5)  # right-click the door: opens it
    for c in (alice, bob):
        c.pump(0.5)
    check(STATES[bob.blocks[P(2, -60, 1)]][1]["open"] == "true" and STATES[bob.blocks[P(2, -59, 1)]][1]["open"] == "true",
          "door opened (both halves)")
    check(bob.saw("sound", lambda i: i == "minecraft:block.wooden_door.open"), "Bob hears the door open")
    check(not alice.saw("sound", lambda i: i == "minecraft:block.wooden_door.open"), "Alice's client plays it itself")
    alice.action(0, 2, -59, 1, seq=5)  # break the upper half: lower must go too
    for c in (alice, bob):
        c.pump(0.5)
    check(state_str(bob.blocks[P(2, -60, 1)]) == "minecraft:air" and state_str(bob.blocks[P(2, -59, 1)]) == "minecraft:air",
          "breaking the upper half removes the whole door")

    print("stairs + fences connect")
    alice.send("set_carried_item", struct.pack(">h", 2))
    alice.rotate(-90, 30)  # facing east
    alice.use_item_on(1, -61, -2, UP, seq=6)
    alice.send("set_carried_item", struct.pack(">h", 3))
    alice.use_item_on(3, -61, -2, UP, seq=7)
    alice.use_item_on(4, -61, -2, UP, seq=8)
    for c in (alice, bob):
        c.pump(0.5)
    st = bob.blocks.get(P(1, -60, -2))
    check(st is not None and STATES[st][0] == "minecraft:oak_stairs" and STATES[st][1]["facing"] == "east"
          and STATES[st][1]["half"] == "bottom", f"stairs facing east: {state_str(st) if st else None}")
    f1, f2 = bob.blocks.get(P(3, -60, -2)), bob.blocks.get(P(4, -60, -2))
    check(f1 is not None and STATES[f1][1]["east"] == "true", f"first fence connects east: {state_str(f1) if f1 else None}")
    check(f2 is not None and STATES[f2][1]["west"] == "true", f"second fence connects west: {state_str(f2) if f2 else None}")

    print("torch on a wall")
    alice.send("set_carried_item", struct.pack(">h", 4))
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.use_item_on(2, -61, 3, UP, seq=9)  # stone pillar
    alice.send("set_carried_item", struct.pack(">h", 4))
    alice.rotate(0, 0)
    alice.use_item_on(2, -60, 3, NORTH, seq=10, cz=0.0, cy=0.5)
    for c in (alice, bob):
        c.pump(0.5)
    t = bob.blocks.get(P(2, -60, 2))
    check(t is not None and STATES[t][0] == "minecraft:wall_torch" and STATES[t][1]["facing"] == "north",
          f"wall torch facing north: {state_str(t) if t else None}")
    alice.action(0, 2, -60, 3, seq=11)  # remove the support
    for c in (alice, bob):
        c.pump(0.5)
    check(state_str(bob.blocks.get(P(2, -60, 2), 0)) == "minecraft:air", "torch pops off when its wall is removed")

    print("note blocks and repeaters")
    alice.creative_slot(41, "minecraft:note_block")
    alice.creative_slot(42, "minecraft:repeater")
    alice.send("set_carried_item", struct.pack(">h", 5))
    alice.pump(0.3)
    alice.use_item_on(4, -61, 4, UP, seq=20)
    alice.send("set_carried_item", struct.pack(">h", 6))
    alice.pump(0.3)
    alice.use_item_on(5, -61, 4, UP, seq=21)
    for c in (alice, bob):
        c.pump(0.5)
    nb = bob.blocks.get(P(4, -60, 4))
    check(nb is not None and STATES[nb][0] == "minecraft:note_block" and STATES[nb][1]["instrument"] == "harp" and STATES[nb][1]["note"] == "0",
          f"note block placed: {state_str(nb) if nb else None}")
    bob.events.clear()
    alice.send("set_carried_item", struct.pack(">h", 7))  # empty hand
    alice.pump(0.2)
    alice.use_item_on(4, -60, 4, UP, seq=22)  # tune: note 1, and play it
    alice.use_item_on(5, -60, 4, UP, seq=23)  # repeater delay 2
    for c in (alice, bob):
        c.pump(0.5)
    nb = bob.blocks.get(P(4, -60, 4))
    check(nb is not None and STATES[nb][1]["note"] == "1", f"note block tuned: {state_str(nb) if nb else None}")
    check(bob.saw("sound", lambda i: i == "minecraft:block.note_block.harp"), "Bob hears the note")
    check(bob.saw("block_event", lambda i: i[0] == P(4, -60, 4) and i[1:3] == (0, 0)), "Bob gets the note block event (particle)")
    rp = bob.blocks.get(P(5, -60, 4))
    check(rp is not None and STATES[rp][1]["delay"] == "2", f"repeater delay adjusted: {state_str(rp) if rp else None}")

    print("walls: tall sides under a block")
    alice.creative_slot(43, "minecraft:cobblestone_wall", 3)
    alice.send("set_carried_item", struct.pack(">h", 7))
    alice.pump(0.3)
    alice.use_item_on(-2, -61, 2, UP, seq=24)
    alice.use_item_on(-1, -61, 2, UP, seq=25)
    alice.send("set_carried_item", struct.pack(">h", 0))  # stone
    alice.pump(0.3)
    alice.use_item_on(-2, -60, 2, UP, seq=26)  # stone on top of the first wall
    for c in (alice, bob):
        c.pump(0.6)
    w1, w2 = bob.blocks.get(P(-2, -60, 2)), bob.blocks.get(P(-1, -60, 2))
    check(w1 is not None and STATES[w1][1]["east"] == "tall" and STATES[w1][1]["up"] == "true" and STATES[w1][1]["west"] == "none",
          f"wall under a block: {state_str(w1) if w1 else None}")
    check(w2 is not None and STATES[w2][1]["west"] == "low", f"wall next to it: {state_str(w2) if w2 else None}")
    alice.pump(0.2)

    print("adventure mode: can_place_on / can_break")
    block_id = lambda n: REG["minecraft:block"]["entries"][n]["protocol_id"]

    def predicate_list(block):  # [BlockPredicate{blocks: [block]}] in the network form
        return enc_varint(1) + b"\x01" + enc_varint(2) + enc_varint(block_id(block)) + b"\x00\x00" + enc_varint(0) + enc_varint(0)

    alice.creative_slot(44, "minecraft:stone", 8, [("minecraft:can_place_on", predicate_list("minecraft:grass_block"))])
    alice.creative_slot(43, "minecraft:wooden_pickaxe", 1, [("minecraft:can_break", predicate_list("minecraft:stone"))])
    alice.pump(0.3)
    alice.command("gamemode adventure")
    alice.send("set_carried_item", struct.pack(">h", 8))
    alice.pump(0.3)
    alice.rotate(0, 60)
    alice.use_item_on(0, -61, 4, UP, seq=27)   # on grass: allowed
    alice.pump(0.3)
    alice.use_item_on(0, -60, 4, UP, seq=28)   # on top of that stone: not allowed
    for c in (alice, bob):
        c.pump(0.6)
    a, b = bob.blocks.get(P(0, -60, 4)), bob.blocks.get(P(0, -59, 4))
    check(a is not None and state_str(a) == "minecraft:stone", f"adventure: placed on grass: {state_str(a) if a else None}")
    check(b is None or state_str(b) == "minecraft:air", f"adventure: not on stone: {state_str(b) if b else None}")
    alice.action(0, 0, -61, 4, seq=29)  # holding the stone (no can_break): grass can't be broken
    alice.action(2, 0, -61, 4, seq=29)
    alice.send("set_carried_item", struct.pack(">h", 7))  # the pickaxe may break stone
    alice.pump(0.2)
    alice.action(0, 0, -60, 4, seq=30)
    alice.pump(1.5)
    alice.action(3, 0, -60, 4, seq=31)
    for c in (alice, bob):
        c.pump(1.0)
    g, st = bob.blocks.get(P(0, -61, 4)), bob.blocks.get(P(0, -60, 4))
    check(g is None or STATES[g][0] == "minecraft:grass_block", f"adventure: grass not broken: {state_str(g) if g else None}")
    check(st is not None and state_str(st) == "minecraft:air", f"adventure: stone broken with the can_break pickaxe: {state_str(st) if st else None}")
    alice.command("gamemode creative")
    alice.pump(0.3)

    print("survival: consumption, bedrock, inventory clicks")
    alice.command("gamemode survival")
    alice.pump(0.3)
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.use_item_on(5, -61, 0, UP, seq=12)
    alice.pump(0.5)
    check(alice.slots.get(36) == ("minecraft:stone", 63), f"stone count decremented in survival: {alice.slots.get(36)}")
    alice.action(0, 1, -64, 0, seq=13)  # bedrock is unbreakable
    alice.action(3, 1, -64, 0, seq=13)
    alice.pump(0.5)
    # Vanilla sends nothing here; we resend the block. Either way it must not become air.
    check(STATES[alice.blocks.get(P(1, -64, 0), 88)][0] == "minecraft:bedrock", "bedrock can't be broken in survival")
    # Left click stone stack in hotbar slot 36, then place it into main inventory slot 9.
    click = lambda slot, button, mode: alice.send(
        "container_click", enc_varint(0) + enc_varint(0) + struct.pack(">hb", slot, button) + enc_varint(mode) + enc_varint(0) + b"\x00")
    click(36, 0, 0)
    click(9, 0, 0)
    alice.pump(0.5)
    check(alice.slots.get(9) == ("minecraft:stone", 63) and alice.slots.get(36) is None,
          f"moved stone stack 36 -> 9 by clicking: 9={alice.slots.get(9)} 36={alice.slots.get(36)}")
    click(9, 0, 1)  # shift-click back to the hotbar
    alice.pump(0.5)
    check(alice.slots.get(36) == ("minecraft:stone", 63), f"shift-click moved it back to the hotbar: {alice.slots.get(36)}")
    # A click the client predicted correctly (its HashedStacks match) isn't echoed back.
    alice.pump(0.3)

    def hashed(item=None, count=0):
        return b"\x00" if item is None else b"\x01" + enc_varint(ITEM[item]) + enc_varint(count) + b"\x00\x00"

    def predicted_click(slot, changed, carried):
        body = b"".join(struct.pack(">h", s) + hashed(*h) for s, h in changed)
        alice.send("container_click", enc_varint(0) + enc_varint(alice.state_id) + struct.pack(">hb", slot, 0) + enc_varint(0)
                   + enc_varint(len(changed)) + body + hashed(*carried))

    alice.events.clear()
    predicted_click(36, [(36, ())], ("minecraft:stone", 63))
    alice.pump(0.5)
    predicted_click(36, [(36, ("minecraft:stone", 63))], ())
    alice.pump(0.5)
    echoed = [e for e in alice.events if e[0] in ("container_set_slot", "set_cursor_item", "container_set_content")]
    check(not echoed, f"correctly predicted clicks aren't echoed: {echoed}")
    # A wrong prediction (the pickup leaves the slot empty and the stack on the cursor) is corrected.
    predicted_click(36, [(36, ("minecraft:stone", 5))], ())
    alice.pump(0.5)
    cursor = [e[1] for e in alice.events if e[0] == "set_cursor_item"]
    check(("container_set_slot", 36) in alice.events and alice.slots.get(36) is None and cursor == [("minecraft:stone", 63)],
          f"a wrong prediction is corrected: slot {alice.slots.get(36)}, cursor {cursor}")
    predicted_click(36, [(36, ("minecraft:stone", 63))], ())  # put it back
    alice.pump(0.3)

    print("time")
    before = alice.time[1][0][0]
    alice.command("gamemode creative")
    alice.command("time set noon")
    alice.pump(0.5)
    check(alice.time[1] and alice.time[1][0][0] % 24000 == 6000, f"time set noon -> {alice.time[1].get(0)}")
    alice.command("time pause")
    alice.pump(0.5)
    check(alice.time[1][0][2] == 0.0, "paused clock is sent with rate 0")
    alice.command("time set 0.5d")  # TimeArgument units: d = 24000, s = 20, t = 1
    alice.pump(0.5)
    check(alice.time[1][0][0] == 12000, f"time set 0.5d -> {alice.time[1][0][0]}")
    alice.command("time add 3s")
    alice.pump(0.5)
    check(alice.time[1][0][0] == 12060, f"time add 3s -> {alice.time[1][0][0]}")
    alice.command("time resume")
    alice.pump(1.5)
    check(alice.time[0] > 0, f"game time advances ({alice.time[0]}, clock was {before})")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all building checks passed")


if __name__ == "__main__":
    main()
