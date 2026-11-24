#!/usr/bin/env python3
"""Mining, loot drops, item entities and pickup (milestone 4). Runs against vanilla too (needs ops).

    test_drops.py HOST PORT
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import check, failures, state_str  # noqa: E402

REG = tb.REG
ENTITY = {v["protocol_id"]: k for k, v in REG["minecraft:entity_type"]["entries"].items()}
ATTRIBUTE = {v["protocol_id"]: k for k, v in REG["minecraft:attribute"]["entries"].items()}
read_item_full = tb.read_item_ex


class DropClient(tb.Client):
    def __init__(self, *a):
        self.entities = {}  # id -> dict(type, pos, item)
        self.cracks = []    # (breaker, pos, stage)
        self.takes = []     # (item id, collector, amount)
        self.spawned = {}   # every item entity ever seen: id -> item
        self.first = {}     # the same, as first seen
        self.full_slots = {}
        self.heights = {}   # entity id -> every y seen
        self.motions = {}   # entity id -> [(y when received, vy)]
        self.attributes = {}  # synced attributes: name -> (base, [(id, amount, operation)])
        self.my_id = None
        self.experience = None
        self.spawned_log = []  # (entity id, type) of every add_entity, even if removed again since
        super().__init__(*a)

    def _handle(self, n, r):
        if n == "add_entity":
            eid = r.varint()
            r.uuid()
            etype = ENTITY[r.varint()]
            pos = (r.f64(), r.f64(), r.f64())
            self.entities[eid] = {"type": etype, "pos": pos, "item": None}
            self.spawned_log.append((eid, etype))
        elif n == "set_entity_data":
            eid = r.varint()
            while True:
                idx = r.u8()
                if idx == 0xFF:
                    break
                ser = r.varint()
                if ser == 7:  # ItemStack
                    val = read_item_full(r)
                    if eid in self.entities and idx == 8:
                        self.entities[eid]["item"] = val
                        self.spawned[eid] = val
                        self.first.setdefault(eid, val)  # before any merging
                elif ser == 0:  # Byte (the shared flags: on fire)
                    flags = r.u8()
                    if eid in self.entities and idx == 0:
                        self.entities[eid]["on_fire"] = bool(flags & 1)
                else:
                    break  # other serializers not needed here
        elif n == "entity_position_sync":
            eid = r.varint()
            if r.varint() == 0:  # PositionPath.Linear
                pos = (r.f64(), r.f64(), r.f64())
            else:  # PositionPath.Stepped: (position, tick offset) steps; it ends at the last
                pos = None
                for _ in range(r.varint()):
                    pos = (r.f64(), r.f64(), r.f64())
                    r.varint()
            if pos and eid in self.entities:
                self.entities[eid]["pos"] = pos
                self.track(eid)
        elif n in ("move_entity_pos", "move_entity_pos_rot"):
            # ClientboundMoveEntityPacket: properties (onGround | stepCount << 1), then a VecDelta:
            # one (x, y, z) delta in 1/4096 blocks, or that many (tick offset, delta) steps
            eid = r.varint()
            steps = r.varint() >> 1
            deltas = [(r.i16(), r.i16(), r.i16())] if steps == 0 else [(r.varint(), r.i16(), r.i16(), r.i16())[1:] for _ in range(steps)]
            if eid in self.entities:
                x, y, z = self.entities[eid]["pos"]
                for dx, dy, dz in deltas:
                    x, y, z = x + dx / 4096, y + dy / 4096, z + dz / 4096
                self.entities[eid]["pos"] = (x, y, z)
                self.track(eid)
        elif n == "remove_entities":
            for _ in range(r.varint()):
                self.entities.pop(r.varint(), None)
        elif n == "set_entity_motion":
            eid = r.varint()
            self.motions.setdefault(eid, []).append((self.entities.get(eid, {}).get("pos", (0, 0, 0))[1], read_lp_vec3(r)[1]))
        elif n == "update_attributes":
            if r.varint() == self.my_id:
                for _ in range(r.varint()):
                    attr = ATTRIBUTE[r.varint()]
                    base = r.f64()
                    mods = [(r.str(), r.f64(), r.varint()) for _ in range(r.varint())]
                    self.attributes[attr] = (base, mods)
        elif n == "login":
            self.my_id = r.i32()
            super()._handle(n, r)
        elif n == "set_experience":
            self.experience = (r.f32(), r.varint(), r.varint())  # progress, level, total
        elif n == "take_item_entity":
            self.takes.append((r.varint(), r.varint(), r.varint()))
        elif n == "block_destruction":
            eid = r.varint()
            self.cracks.append((eid, tb.unpack_pos(r.i64()), r.u8()))
        elif n == "container_set_slot":
            r.varint(), r.varint()
            slot = r.i16()
            self.full_slots[slot] = read_item_full(r)
            self.slots[slot] = self.full_slots[slot][:2] if self.full_slots[slot] else None
            self.events.append((n, slot))
        else:
            super()._handle(n, r)

    def track(self, eid):
        self.heights.setdefault(eid, []).append(self.entities[eid]["pos"][1])

    def items(self, name=None):
        return [e for e in self.entities.values() if e["type"] == "minecraft:item" and e["item"] and (name is None or e["item"][0] == name)]


def read_lp_vec3(r):
    """Vec3.LP_STREAM_CODEC (LpVec3): three 15-bit components scaled by a shared factor."""
    b0 = r.u8()
    if b0 == 0:
        return (0.0, 0.0, 0.0)
    buffer = b0 | r.u8() << 8 | struct.unpack(">I", r.bytes(4))[0] << 16
    scale = buffer & 3
    if buffer & 4:
        scale |= r.varint() << 2
    unpack = lambda v: (min(v & 0x7FFF, 32766) * 2.0 / 32766.0 - 1.0) * scale
    return (unpack(buffer >> 3), unpack(buffer >> 18), unpack(buffer >> 33))


def dig(client, x, y, z, ticks, seq, stop=True):
    client.action(0, x, y, z, seq)
    client.pump(ticks * 0.05)
    if stop:
        client.action(3, x, y, z, seq + 1)


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    alice = DropClient(host, port, "Alice")
    bob = DropClient(host, port, "Bob")
    for c in (alice, bob):
        c.pump(1.0)
    tb.OX, tb.OZ = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    P = tb.P
    bob.command(f"tp {tb.OX - 3 + 0.5} -60 {tb.OZ - 3 + 0.5}")  # vanilla scatters new players; keep Bob clear
    for c in (alice, bob):
        c.pump(0.5)
    alice.rotate(0, 60)  # also tells the server we're on the ground (dig speed /5 in the air)
    alice.command("gamemode creative")
    alice.pump(0.3)
    alice.creative_slot(36, "minecraft:stone", 16)
    alice.slots[36] = ("minecraft:stone", 16)  # creative slot changes aren't echoed
    alice.creative_slot(37, "minecraft:wooden_pickaxe")
    alice.creative_slot(38, "minecraft:oak_door")
    alice.pump(0.3)
    alice.use_item_on(2, -61, 0, 1, 1)  # stone at (2,-60,0)
    alice.use_item_on(2, -61, 1, 1, 2)  # stone at (2,-60,1)
    alice.send("set_carried_item", struct.pack(">h", 2))
    alice.use_item_on(1, -61, 2, 1, 3)  # door at (1,-60,2)
    alice.pump(0.5)
    alice.command("gamemode survival")
    alice.rotate(0, 60)
    alice.pump(0.5)

    print("bare hand on stone: no drop (requires a pickaxe)")
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.pump(0.2)
    # Stone, hand: 1/1.5/100 per tick -> 150 ticks. Send STOP after 160.
    dig(alice, 2, -60, 0, 160, 10)
    for c in (alice, bob):
        c.pump(1.0)
    check(state_str(bob.blocks.get(P(2, -60, 0), 0)) == "minecraft:air", "stone broken by hand after enough time")
    check(not bob.items("minecraft:cobblestone") and not bob.items("minecraft:stone"), f"no drop without the right tool: {bob.items()}")

    print("wooden pickaxe: early stop is not accepted, then finished when progress catches up")
    alice.send("set_carried_item", struct.pack(">h", 1))
    alice.pump(0.2)
    bob.cracks.clear()
    # Stone, wooden pickaxe: 2/1.5/30 per tick -> 22.5 ticks. STOP after 5: rejected for now.
    dig(alice, 2, -60, 1, 5, 20)
    alice.pump(0.1)
    early = alice.blocks.get(P(2, -60, 1))
    for c in (alice, bob):
        c.pump(1.5)
    check(state_str(bob.blocks.get(P(2, -60, 1), 0)) == "minecraft:air", "delayed destroy completes once progress reaches 1")
    stages = [s for (_, p, s) in bob.cracks if p == P(2, -60, 1) and s != 255]
    check(len(stages) >= 3 and stages == sorted(stages), f"Bob saw increasing crack stages: {stages}")
    # The drop may already have been picked up by Alice if it landed next to her (like vanilla).
    drops = bob.items("minecraft:cobblestone")
    seen = [v for v in bob.spawned.values() if v[0] == "minecraft:cobblestone"]
    check(len(seen) == 1, f"one cobblestone item entity spawned: {seen}")
    if drops:
        dx, dy, dz = drops[0]["pos"]
        bx, by, bz = P(2, -60, 1)
        # Drops pop out with a random horizontal kick, so they can land a block or so away.
        check(abs(dx - bx - 0.5) <= 2 and abs(dz - bz - 0.5) <= 2, f"drop landed near the block: {drops[0]['pos']}")
    held = alice.full_slots.get(37)
    check(held and held[0] == "minecraft:wooden_pickaxe" and held[2].get("minecraft:damage") == 1, f"pickaxe took 1 damage: {held}")

    print("pickup")
    if drops:
        dx, dy, dz = drops[0]["pos"]
        alice.command(f"tp {dx:.2f} {dy:.2f} {dz:.2f}")
    for c in (alice, bob):
        c.pump(1.5)
    got = [t for t in alice.takes if t[1] != 0]
    check(any(t[2] == 1 for t in got), f"take_item_entity sent for the pickup: {alice.takes}")
    check(any(v and v[0] == "minecraft:cobblestone" for v in alice.slots.values()), f"cobblestone in Alice's inventory")
    check(not bob.items("minecraft:cobblestone"), "item entity removed for Bob after pickup")

    print("door drops once (lower half's loot)")
    alice.send("set_carried_item", struct.pack(">h", 1))
    dig(alice, 1, -59, 2, 40, 30)  # break the upper half
    for c in (alice, bob):
        c.pump(1.5)
    doors = [v for v in bob.spawned.values() if v[0] == "minecraft:oak_door"]
    check(state_str(bob.blocks.get(P(1, -60, 2), 0)) == "minecraft:air", "both door halves removed")
    check(len(doors) == 1 and doors[0][1] == 1, f"exactly one door dropped: {doors}")

    print("throwing with Q")
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.pump(0.2)
    before = alice.slots.get(36)
    alice.rotate(0, 0)  # look straight ahead (south) so throws are comparable between servers
    alice.pump(0.3)
    t_throw = (time.time(), alice.time[0])
    alice.action(5, 0, 0, 0, 0)  # DROP_ITEM

    def both(seconds):  # pump the two clients interleaved, so time passes once
        end = time.time() + seconds
        while time.time() < end:
            alice.pump(0.01)
            bob.pump(0.01)

    both(0.5)
    thrown = bob.items("minecraft:stone")
    check(len(thrown) == 1 and thrown[0]["item"][1] == 1, f"one stone thrown: {thrown}")
    # Vanilla doesn't echo this slot (the client predicted it); we resend it. Either is fine.
    check(alice.slots.get(36) and alice.slots[36][1] in (before[1], before[1] - 1), f"stack went from {before} to {alice.slots.get(36)}")
    both(0.8)
    if bob.items("minecraft:stone"):
        tx, ty, tz = bob.items("minecraft:stone")[0]["pos"]
        print(f"    thrown stone rests {tz - alice.pos[2]:.2f} blocks ahead, {tx - alice.pos[0]:+.2f} sideways")
    check(bob.items("minecraft:stone"), f"thrown item isn't picked up again immediately (pickup delay 40): "
          f"entities={bob.entities} takes={bob.takes[-3:]} wall={time.time() - t_throw[0]:.2f}s ticks~{alice.time[0] - t_throw[1]}")
    # A flat throw lands ~3.6 blocks away (as in vanilla): walk over to pick it up.
    if bob.items("minecraft:stone"):
        tx, ty, tz = bob.items("minecraft:stone")[0]["pos"]
        alice.send("move_player_pos", struct.pack(">dddB", tx, ty, tz, 1))
    both(2.0)
    check(not bob.items("minecraft:stone"), "after the delay someone picks it up")

    print("decorated pots: a pickaxe cracks them into sherds (bricks here), a hand drops the pot")
    alice.command("gamemode creative")
    alice.command(f"tp {P(0, 0, 0)[0] + 0.5} -60 {P(0, 0, -3)[2] + 0.5}")
    both(0.5)
    alice.creative_slot(38, "minecraft:decorated_pot", 2)
    alice.send("set_carried_item", struct.pack(">h", 2))
    alice.pump(0.3)
    alice.use_item_on(-1, -61, -4, 1, 50)
    alice.use_item_on(1, -61, -4, 1, 51)
    alice.command("gamemode survival")
    both(0.5)
    print("    pots:", [tb.state_str(bob.blocks[P(x, -60, -4)]) if P(x, -60, -4) in bob.blocks else None for x in (-1, 1)])
    known = set(bob.first)
    alice.send("set_carried_item", struct.pack(">h", 1))  # wooden pickaxe
    alice.pump(0.2)
    alice.action(0, -1, -60, -4, 52)  # pots break instantly
    both(1.0)
    got = [bob.first[e] for e in bob.first if e not in known]
    check(sorted(i[:2] for i in got) == [("minecraft:brick", 1)] * 4, f"cracked pot: 4 bricks: {got}")
    known = set(bob.first)
    alice.send("set_carried_item", struct.pack(">h", 3))  # empty hand
    alice.pump(0.2)
    alice.action(0, 1, -60, -4, 53)
    both(1.0)
    got = [bob.first[e] for e in bob.first if e not in known]
    check([i[:2] for i in got] == [("minecraft:decorated_pot", 1)], f"intact pot drops itself: {got}")

    print("mining fatigue slows digging; efficiency is a synced attribute")
    alice.command("gamemode creative")
    alice.command(f"tp {P(0, 0, 0)[0] + 0.5} -60 {P(0, 0, 3)[2] + 0.5}")
    both(0.5)
    alice.creative_slot(36, "minecraft:stone", 4)
    alice.creative_slot(37, "minecraft:wooden_pickaxe")
    ench = tb.enc_varint(1) + tb.enc_varint(tb.SYNCED["minecraft:enchantment"].index("minecraft:efficiency")) + tb.enc_varint(3)
    alice.creative_slot(38, "minecraft:diamond_pickaxe", 1, [("minecraft:enchantments", ench)])
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.pump(0.3)
    alice.use_item_on(2, -61, 3, 1, 60)
    alice.command("gamemode survival")
    alice.command("effect give @s minecraft:mining_fatigue 30 0")
    alice.send("set_carried_item", struct.pack(">h", 1))
    alice.rotate(0, 60)
    both(0.5)
    # Wooden pickaxe on stone: 22.5 ticks, with Mining Fatigue I 75. The crack stages Bob sees follow
    # the server's progress: about stage 4 after 30 ticks (without fatigue it would be past 10).
    bob.cracks.clear()
    alice.action(0, 2, -60, 3, 61)
    both(1.5)
    stages = [st for (_, p, st) in bob.cracks if p == P(2, -60, 3)]
    check(stages and 3 <= max(stages) <= 5, f"with Mining Fatigue, stage ~4 after 30 ticks: {stages}")
    both(2.3)
    stages = [st for (_, p, st) in bob.cracks if p == P(2, -60, 3)]
    check(stages and 9 <= max(stages) <= 12, f"and ~10 after 75: {stages[-3:]}")
    alice.action(3, 2, -60, 3, 62)
    both(0.5)
    check(state_str(bob.blocks.get(P(2, -60, 3), 0)) == "minecraft:air", "then it breaks")
    alice.command("effect clear @s")
    alice.send("set_carried_item", struct.pack(">h", 2))  # Efficiency III: mining_efficiency +10 while held
    both(0.5)
    eff = alice.attributes.get("minecraft:mining_efficiency")
    check(eff and any(abs(m[1] - 10.0) < 1e-6 and m[2] == 0 for m in eff[1]), f"client got mining_efficiency +10: {eff}")
    alice.send("set_carried_item", struct.pack(">h", 0))
    both(0.5)
    eff = alice.attributes.get("minecraft:mining_efficiency")
    check(eff is not None and not eff[1], f"and loses it when switching away: {eff}")

    print("slime blocks bounce items")
    alice.command("gamemode creative")
    alice.pump(0.3)
    alice.creative_slot(36, "minecraft:slime_block")
    alice.creative_slot(37, "minecraft:stone", 1)
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.command(f"tp {P(8, -60, 0)[0] - 1.5} -60 {P(8, -60, 0)[2] + 0.5}")
    alice.pump(0.5)
    alice.use_item_on(8, -61, 0, 1, 40)  # slime block at (8,-60,0)
    alice.pump(0.5)
    alice.command(f"tp {P(8, -60, 0)[0] + 0.5} -59 {P(8, -60, 0)[2] + 0.5}")
    alice.command("gamemode survival")
    both(0.5)  # accept the teleport first: movement before that is ignored
    alice.rotate(0, -90)  # straight up
    alice.send("set_carried_item", struct.pack(">h", 1))
    alice.pump(0.5)
    known = set(bob.spawned)
    alice.action(5, 0, 0, 0, 0)
    both(1.8)
    thrown = [eid for eid in bob.spawned if eid not in known]
    # Vanilla syncs resting/falling items only every 20 ticks (clients simulate the physics), so
    # the bounce shows up in the velocity updates this server sends; vanilla sends none here.
    motions = bob.motions.get(thrown[0], []) if thrown else []
    if not motions:
        print("  skip bounce check: the server sent no item velocity updates")
    else:
        up = [vy for _, vy in motions[1:] if vy > 0.05]
        check(up, f"the item bounced off the slime block (velocity updates: {[round(vy, 3) for _, vy in motions]})")

    print("ores drop experience orbs, which fly to the player; silk touch drops none")
    alice.command("gamemode creative")
    alice.command(f"tp {P(0, 0, 0)[0] + 0.5} -60 {P(0, 0, 6)[2] + 0.5}")
    both(0.5)
    alice.creative_slot(36, "minecraft:diamond_ore", 2)
    alice.creative_slot(37, "minecraft:iron_pickaxe")
    silk = tb.enc_varint(1) + tb.enc_varint(tb.SYNCED["minecraft:enchantment"].index("minecraft:silk_touch")) + tb.enc_varint(1)
    alice.creative_slot(38, "minecraft:iron_pickaxe", 1, [("minecraft:enchantments", silk)])
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.pump(0.3)
    alice.use_item_on(2, -61, 6, 1, 70)  # diamond ore at (2,-60,6)
    alice.use_item_on(-2, -61, 6, 1, 71)  # and at (-2,-60,6)
    alice.command("gamemode survival")
    alice.rotate(90, 45)  # towards -x... then +x below
    both(0.5)
    known = set(bob.entities)
    xp_before = (alice.experience or (0, 0, 0))[2]
    alice.send("set_carried_item", struct.pack(">h", 2))  # silk touch first
    alice.rotate(90, 40)
    both(0.3)
    dig(alice, -2, -60, 6, 25, 72)
    both(1.0)
    orbs = [e for i, e in bob.entities.items() if i not in known and e["type"] == "minecraft:experience_orb"]
    check(not orbs and state_str(bob.blocks.get(P(-2, -60, 6), 0)) == "minecraft:air", f"silk touch: ore mined, no orbs ({len(orbs)})")
    known = set(bob.entities)
    alice.send("set_carried_item", struct.pack(">h", 1))
    alice.rotate(270, 40)
    both(0.3)
    log_start = len(bob.spawned_log)
    dig(alice, 2, -60, 6, 25, 74)
    both(3.0)
    seen_orbs = {i for i, t in bob.spawned_log[log_start:] if t == "minecraft:experience_orb"}  # (they may be collected already)
    check(seen_orbs, f"Bob saw experience orbs from the diamond ore ({len(seen_orbs)})")
    xp = (alice.experience or (0, 0, 0))[2] - xp_before
    check(3 <= xp <= 7, f"Alice collected 3-7 experience: {alice.experience}")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all drop checks passed")


if __name__ == "__main__":
    main()
