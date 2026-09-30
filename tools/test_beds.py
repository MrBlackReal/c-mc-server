#!/usr/bin/env python3
"""Beds: the spawn point, sleeping, skipping the night. Runs against vanilla too.

    test_beds.py HOST PORT
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from mcprobe import enc_varint  # noqa: E402
from test_building import Client, P, check, failures  # noqa: E402

UP = 1
G = -61


class BedClient(Client):
    def __init__(self, *a):
        self.overlays = []   # action bar texts (translate keys)
        self.chats = []
        self.poses = {}      # entity id -> pose id
        self.sleeping = {}   # entity id -> bed pos or None
        self.animations = []
        self.my_id = None
        super().__init__(*a)

    def _handle(self, n, r):
        if n == "login":
            self.my_id = r.i32()
            r.i = 0
            super()._handle(n, r)
        elif n == "set_action_bar_text":
            t = r.nbt()
            self.overlays.append(t.get("translate") if isinstance(t, dict) else t)
        elif n == "system_chat":
            t = r.nbt()
            key = t.get("translate") if isinstance(t, dict) else t
            (self.overlays if r.u8() else self.chats).append(key)  # overlay messages show above the hotbar
        elif n == "set_entity_data":
            eid = r.varint()
            while True:
                idx = r.u8()
                if idx == 0xFF:
                    break
                ser = r.varint()
                if idx == 6 and ser == 20:
                    self.poses[eid] = r.varint()
                elif idx == 14 and ser == 11:
                    self.sleeping[eid] = tb.unpack_pos(r.i64()) if r.u8() else None
                else:
                    break  # other data: not needed here
        elif n == "animate":
            self.animations.append((r.varint(), r.u8()))
        else:
            super()._handle(n, r)


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    alice = BedClient(host, port, "Alice")
    bob = BedClient(host, port, "Bob")
    both = lambda t: [c.pump(t) for c in (alice, bob)]
    both(1.0)
    tb.OX, tb.OZ = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    alice.command("gamemode creative")
    bob.command("gamemode spectator")  # spectators don't count for sleeping
    alice.command(f"tp {tb.OX + 0.5} -60 {tb.OZ + 0.5}")
    bob.command(f"tp {tb.OX + 0.5} -58 {tb.OZ - 2.5}")
    alice.command("time set 1000")
    both(0.5)
    alice.rotate(270, 30)  # facing east: the bed's foot goes where she clicks, the head beyond it
    both(0.2)
    alice.creative_slot(36, "minecraft:red_bed")
    alice.send("set_carried_item", struct.pack(">h", 0))
    alice.pump(0.2)
    alice.use_item_on(2, G, 0, UP, 10)
    both(0.5)
    head, foot = P(3, -60, 0), P(2, -60, 0)
    check("red_bed[facing=east,occupied=false,part=head]" in tb.state_str(bob.blocks.get(head, 0)), f"bed: {tb.state_str(bob.blocks.get(head, 0))}")
    alice.send("set_carried_item", struct.pack(">h", 1))  # empty hand
    alice.command("gamemode survival")
    both(0.3)

    print("in the day the bed only sets the spawn point")
    alice.use_item_on(2, -60, 0, UP, 11, 0.5, 0.5625, 0.5)
    both(0.5)
    check("block.minecraft.set_spawn" in alice.chats, f"spawn set: {alice.chats}")
    check("block.minecraft.bed.no_sleep" in alice.overlays, f"can't sleep: {alice.overlays}")

    print("at night Alice sleeps")
    alice.command("time set 14000")
    both(0.5)
    alice.overlays.clear()
    alice.use_item_on(2, -60, 0, UP, 12, 0.5, 0.5625, 0.5)
    both(0.5)
    check(bob.poses.get(alice.my_id) == 2 and bob.sleeping.get(alice.my_id) == head, f"Bob sees her sleeping: pose {bob.poses.get(alice.my_id)}, bed {bob.sleeping.get(alice.my_id)}")
    check("occupied=true" in tb.state_str(bob.blocks.get(head, 0)) and "occupied=true" in tb.state_str(bob.blocks.get(foot, 0)), "both halves occupied")
    check("sleep.skipping_night" in alice.overlays, f"announced: {alice.overlays}")

    print("after 100 ticks the night is skipped and she wakes up")
    end = time.time() + 8
    while time.time() < end and bob.poses.get(alice.my_id) == 2:
        both(0.3)
    check(bob.poses.get(alice.my_id) == 0 and bob.sleeping.get(alice.my_id) is None, f"awake: pose {bob.poses.get(alice.my_id)}")
    check((alice.my_id, 0) in bob.animations, f"wake-up animation: {bob.animations}")
    both(1.0)
    clocks = alice.time[1] if alice.time else {}
    ticks = [c[0] % 24000 for c in clocks.values()]
    check(any(t < 200 for t in ticks), f"morning: clock ticks within the day {ticks}")
    check("occupied=false" in tb.state_str(bob.blocks.get(head, 0)), f"bed free: {tb.state_str(bob.blocks.get(head, 0))}")

    print("the leave bed button")
    alice.command("time set 14000")
    both(0.5)
    alice.use_item_on(2, -60, 0, UP, 13, 0.5, 0.5625, 0.5)
    both(0.5)
    alice.send("player_command", enc_varint(alice.my_id) + enc_varint(0) + enc_varint(0))  # STOP_SLEEPING
    both(0.5)
    check(bob.poses.get(alice.my_id) == 0, f"got up: pose {bob.poses.get(alice.my_id)}")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all bed checks passed")


if __name__ == "__main__":
    main()
