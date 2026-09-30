#!/usr/bin/env python3
"""Movement validation (ServerGamePacketListenerImpl.handleMovePlayer). Runs against vanilla too.

    test_movement.py HOST PORT
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client, check, failures  # noqa: E402


class MoveClient(Client):
    def __init__(self, *a):
        self.teleports = []    # (x, y, z) of every player_position
        self.disconnect = None
        super().__init__(*a)

    def _handle(self, n, r):
        if n == "player_position":
            start = r.i
            r.varint()
            self.teleports.append((r.f64(), r.f64(), r.f64()))
            r.i = start
        elif n == "disconnect":
            self.disconnect = r.nbt()
            return
        super()._handle(n, r)

    def move(self, x, y, z, on_ground=True):
        self.send("move_player_pos", struct.pack(">dddB", x, y, z, 1 if on_ground else 0))

    def safe_pump(self, t):
        try:
            self.pump(t)
        except (ConnectionError, OSError):
            pass


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    alice = MoveClient(host, port, "Alice")
    alice.pump(1.0)
    x, y, z = alice.pos
    ox, oz = int(x // 1), int(z // 1)
    alice.command("gamemode creative")
    alice.pump(0.3)
    alice.creative_slot(36, "minecraft:stone", 8)
    alice.pump(0.2)
    tb.OX, tb.OZ = ox, oz
    alice.use_item_on(2, -61, 0, 1, 10)  # a stone next to her, to walk into
    alice.pump(0.3)
    alice.command("gamemode survival")
    alice.pump(0.5)

    print("moved too quickly: 20 blocks in one packet")
    alice.teleports.clear()
    alice.move(x + 20, y, z)
    alice.pump(0.5)
    check(alice.teleports and abs(alice.teleports[-1][0] - x) < 0.01, f"sent back: {alice.teleports}")
    alice.pump(0.3)

    print("moved wrongly: into a block")
    alice.teleports.clear()
    alice.move(ox + 2.5, y, oz + 0.5)
    alice.pump(0.5)
    check(alice.teleports and abs(alice.teleports[-1][0] - x) < 0.01, f"sent back out of the stone: {alice.teleports}")
    alice.pump(0.3)

    print("a normal step is accepted")
    alice.teleports.clear()
    alice.move(x - 0.3, y, z)
    alice.pump(0.5)
    check(not alice.teleports, f"no correction: {alice.teleports}")

    print("floating in survival: kicked after 80 ticks")
    fy = y
    for _ in range(6):  # rise 3 blocks, half a block per tick
        fy += 0.5
        alice.move(x - 0.3, fy, z, on_ground=False)
        alice.pump(0.05)
    end = time.time() + 6
    while time.time() < end and alice.disconnect is None:
        alice.move(x - 0.3, fy, z, on_ground=False)  # keep reporting the same spot, like a hovering client
        alice.safe_pump(0.05)
    reason = str(alice.disconnect)
    check(alice.disconnect is not None and "multiplayer.disconnect.flying" in reason, f"kicked: {reason}")

    print("two positions in one tick")
    bob = MoveClient(host, port, "Bob")
    bob.pump(1.0)
    bx, by, bz = bob.pos
    bob.send("client_tick_end")
    bob.c.send(tb.SB["move_player_pos"], struct.pack(">dddB", bx, by, bz, 1))
    bob.c.send(tb.SB["move_player_pos"], struct.pack(">dddB", bx, by, bz, 1))  # no tick end in between
    bob.safe_pump(1.0)
    check(bob.disconnect is not None and "invalid_player_movement" in str(bob.disconnect), f"kicked: {bob.disconnect}")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all movement checks passed")


if __name__ == "__main__":
    main()
