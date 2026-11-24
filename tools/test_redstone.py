#!/usr/bin/env python3
"""Redstone: wire, torches, repeaters, comparators, observers, levers, buttons, pressure plates and
what they power (lamps, doors, trapdoors, gates, note blocks, hoppers, rails, copper bulbs). Checks
settled outcomes plus what each block went through, so it passes on vanilla too.

    test_redstone.py HOST PORT
    REDSTONE_DUMP=file: write every block state in the test area (relative to spawn), to diff
    against a vanilla run.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import test_building as tb  # noqa: E402
from test_building import Client, check, failures, state_str  # noqa: E402

GROUND = -61  # superflat: grass at -61, air from -60
Y = GROUND + 1


class HistoryClient(Client):
    """Also remembers every state each block went through."""

    def __init__(self, *a):
        self.history = {}
        super().__init__(*a)

    def _handle(self, n, r):
        before = dict(self.blocks) if n in ("block_update", "section_blocks_update") else None
        super()._handle(n, r)
        if before is not None:
            for p, sid in self.blocks.items():
                if before.get(p) != sid:
                    self.history.setdefault(p, []).append(state_str(sid))


def main():
    alice = HistoryClient(sys.argv[1], int(sys.argv[2]), "Alice")
    alice.pump(1.0)
    ox, oz = int(alice.pos[0] // 1), int(alice.pos[2] // 1)
    tb.OX, tb.OZ = ox, oz
    alice.command("gamemode creative")
    alice.command("gamerule advance_time false")
    alice.command("gamerule random_tick_speed 0")
    alice.pump(0.5)
    seq = [10]

    def at(dx, y, dz):
        return (ox + dx, y, oz + dz)

    def setblock(p, block):
        alice.command(f"setblock {p[0]} {p[1]} {p[2]} {block}")
        alice.pump(0.03)

    def state(p):
        return state_str(alice.blocks.get(p, 0))

    def went_through(p, s):
        return s in alice.history.get(p, [])

    def use(p, cy=0.5):
        """Right-click the block at p (absolute) with an empty hand, standing next to it."""
        alice.command(f"tp Alice {p[0] + 0.5} {Y} {p[2] - 1.5}")
        alice.pump(0.3)
        seq[0] += 1
        alice.use_item_on(p[0] - ox, p[1], p[2] - oz, 2, seq[0], 0.5, cy, 0.0)
        alice.pump(0.2)

    alice.command(f"fill {ox - 70} {Y} {oz + 4} {ox + 70} {Y + 4} {oz + 40} minecraft:air")
    alice.command("clear Alice")
    alice.pump(0.5)

    # 1. A lever powering 15 wires: the 15th still has power 1 and lights a lamp; with 16, the lamp stays off.
    a = (-60, 6)
    for k in range(1, 16):
        setblock(at(a[0] + k, Y, a[1]), "minecraft:redstone_wire")
        setblock(at(a[0] + k, Y, a[1] + 2), "minecraft:redstone_wire")
    setblock(at(a[0] + 16, Y, a[1]), "minecraft:redstone_lamp")
    setblock(at(a[0] + 16, Y, a[1] + 2), "minecraft:redstone_wire")
    setblock(at(a[0] + 17, Y, a[1] + 2), "minecraft:redstone_lamp")
    setblock(at(a[0], Y, a[1]), "minecraft:lever[face=floor,facing=east]")
    setblock(at(a[0], Y, a[1] + 2), "minecraft:lever[face=floor,facing=east]")
    use(at(a[0], Y, a[1]))
    use(at(a[0], Y, a[1] + 2))

    # 2. A redstone block through three repeaters (delay 4) into a lamp.
    b = (-40, 6)
    for k in range(1, 4):
        setblock(at(b[0] + k, Y, b[1]), "minecraft:repeater[facing=west,delay=4]")
    setblock(at(b[0] + 4, Y, b[1]), "minecraft:redstone_lamp")
    setblock(at(b[0], Y, b[1]), "minecraft:redstone_block")

    # 3. An inverter: a lever powers a block, the torch on its side goes out (and the lamp it powered).
    c = (-30, 6)
    # (lamps go down first: /setblock doesn't update the block it places, only its neighbours)
    setblock(at(c[0], Y, c[1]), "minecraft:stone")
    setblock(at(c[0] + 2, Y, c[1]), "minecraft:redstone_lamp")
    setblock(at(c[0] + 1, Y, c[1]), "minecraft:redstone_wall_torch[facing=east]")
    setblock(at(c[0] - 1, Y, c[1]), "minecraft:redstone_wire")
    setblock(at(c[0] - 2, Y, c[1]), "minecraft:lever[face=floor,facing=east]")
    # ...and one under a block: a torch on a powered block goes out.
    setblock(at(c[0] + 5, Y, c[1]), "minecraft:stone")
    setblock(at(c[0] + 5, Y + 2, c[1]), "minecraft:redstone_lamp")
    setblock(at(c[0] + 5, Y + 1, c[1]), "minecraft:redstone_torch")

    # 4. A comparator reads a hopper with two full stacks of five slots (signal 6).
    d = (-20, 6)
    setblock(at(d[0], Y, d[1]), 'minecraft:hopper{Items:[{Slot:0b,id:"minecraft:stone",count:64},{Slot:1b,id:"minecraft:stone",count:64}]}')
    setblock(at(d[0] + 1, Y, d[1]), "minecraft:comparator[facing=west]")
    for k in range(2, 6):
        setblock(at(d[0] + k, Y, d[1]), "minecraft:redstone_wire")
    # ...a comparator in subtract mode: 15 from a redstone block minus 5 from a composter.
    setblock(at(d[0], Y, d[1] + 4), "minecraft:redstone_block")
    setblock(at(d[0] + 1, Y, d[1] + 2), "minecraft:composter[level=5]")
    setblock(at(d[0] + 1, Y, d[1] + 3), "minecraft:comparator[facing=north]")
    setblock(at(d[0] + 1, Y, d[1] + 4), "minecraft:comparator[facing=west,mode=subtract]")
    setblock(at(d[0] + 2, Y, d[1] + 4), "minecraft:redstone_wire")
    # ...and reading through a block: cake (14) and a cauldron (level 2).
    setblock(at(d[0], Y, d[1] + 7), "minecraft:cake")
    setblock(at(d[0] + 1, Y, d[1] + 7), "minecraft:comparator[facing=west]")
    setblock(at(d[0] + 2, Y, d[1] + 7), "minecraft:redstone_wire")
    setblock(at(d[0] - 1, Y, d[1] + 9), "minecraft:water_cauldron[level=2]")
    setblock(at(d[0], Y, d[1] + 9), "minecraft:stone")
    setblock(at(d[0] + 1, Y, d[1] + 9), "minecraft:comparator[facing=west]")
    setblock(at(d[0] + 2, Y, d[1] + 9), "minecraft:redstone_wire")

    # 5. Doors, trapdoors and gates open with power, and close again.
    e = (-13, 6)
    setblock(at(e[0], Y, e[1]), "minecraft:iron_door[half=lower,facing=north]")
    setblock(at(e[0], Y + 1, e[1]), "minecraft:iron_door[half=upper,facing=north]")
    setblock(at(e[0] + 1, Y, e[1]), "minecraft:redstone_block")
    setblock(at(e[0] + 3, Y, e[1]), "minecraft:iron_trapdoor[facing=north]")
    setblock(at(e[0] + 4, Y, e[1]), "minecraft:redstone_block")
    setblock(at(e[0] + 6, Y, e[1]), "minecraft:oak_fence_gate[facing=north]")
    setblock(at(e[0] + 7, Y, e[1]), "minecraft:redstone_block")
    setblock(at(e[0] + 9, Y, e[1]), "minecraft:oak_door[half=lower,facing=north]")
    setblock(at(e[0] + 9, Y + 1, e[1]), "minecraft:oak_door[half=upper,facing=north]")
    setblock(at(e[0] + 10, Y + 1, e[1]), "minecraft:redstone_block")  # next to the upper half
    setblock(at(e[0] + 9, Y, e[1] + 3), "minecraft:oak_door[half=lower,facing=north]")
    setblock(at(e[0] + 9, Y + 1, e[1] + 3), "minecraft:oak_door[half=upper,facing=north]")
    setblock(at(e[0] + 10, Y, e[1] + 3), "minecraft:redstone_block")
    alice.pump(0.3)
    setblock(at(e[0] + 10, Y, e[1] + 3), "minecraft:air")

    # 6. A hopper next to a redstone block is locked; powered rails carry power 8 rails on.
    f = (0, 6)
    setblock(at(f[0], Y, f[1]), "minecraft:hopper")
    setblock(at(f[0] + 1, Y, f[1]), "minecraft:redstone_block")
    for k in range(1, 13):
        setblock(at(f[0] + k, Y, f[1] + 3), "minecraft:powered_rail[shape=east_west]")
    setblock(at(f[0], Y, f[1] + 3), "minecraft:redstone_block")
    for k in range(0, 4):
        setblock(at(f[0] + k, Y, f[1] + 6), "minecraft:activator_rail[shape=east_west]")
    setblock(at(f[0] + 1, Y - 1, f[1] + 6), "minecraft:redstone_block")

    # 7. A button lights a lamp for a second; a stone pressure plate while Alice stands on it.
    g = (10, 6)
    setblock(at(g[0], Y, g[1]), "minecraft:stone")
    setblock(at(g[0], Y, g[1] + 1), "minecraft:stone_button[face=wall,facing=south]")
    setblock(at(g[0], Y + 1, g[1]), "minecraft:redstone_lamp")
    setblock(at(g[0] + 3, Y, g[1]), "minecraft:stone_pressure_plate")
    setblock(at(g[0] + 3, Y - 1, g[1]), "minecraft:redstone_lamp")
    setblock(at(g[0] + 5, Y, g[1]), "minecraft:oak_button[face=floor,facing=north]")
    setblock(at(g[0] + 5, Y - 1, g[1]), "minecraft:redstone_lamp")

    # 8. An observer watching a block: one pulse when it changes (the lamp blinks, the note block plays).
    h = (20, 6)
    setblock(at(h[0], Y, h[1]), "minecraft:observer[facing=west]")  # watches h[0]-1, outputs to h[0]+1
    setblock(at(h[0] + 1, Y, h[1]), "minecraft:redstone_lamp")
    setblock(at(h[0], Y, h[1] + 3), "minecraft:observer[facing=west]")
    setblock(at(h[0] + 1, Y, h[1] + 3), "minecraft:note_block")

    # 9. Copper bulbs toggle on each rising edge.
    i = (30, 6)
    setblock(at(i[0], Y, i[1]), "minecraft:copper_bulb")
    setblock(at(i[0] - 1, Y, i[1]), "minecraft:lever[face=floor,facing=east]")

    # 10. Repeater locks, wire up and down steps, strong power through a block.
    j = (40, 6)
    setblock(at(j[0], Y, j[1]), "minecraft:repeater[facing=west]")
    setblock(at(j[0], Y, j[1] + 1), "minecraft:repeater[facing=south]")  # points at the first one's side
    setblock(at(j[0], Y, j[1] + 2), "minecraft:redstone_block")
    setblock(at(j[0] + 3, Y, j[1]), "minecraft:redstone_block")
    setblock(at(j[0] + 4, Y, j[1]), "minecraft:redstone_wire")
    setblock(at(j[0] + 5, Y, j[1]), "minecraft:stone")
    setblock(at(j[0] + 5, Y + 1, j[1]), "minecraft:redstone_wire")
    setblock(at(j[0] + 6, Y + 1, j[1]), "minecraft:stone")
    setblock(at(j[0] + 6, Y + 2, j[1]), "minecraft:redstone_wire")
    setblock(at(j[0] + 7, Y, j[1]), "minecraft:stone")
    setblock(at(j[0] + 7, Y + 1, j[1]), "minecraft:redstone_wire")
    setblock(at(j[0] + 8, Y, j[1]), "minecraft:redstone_wire")
    # a repeater into a block powers it strongly: wire beside the block gets 15, a lamp beside it lights
    setblock(at(j[0] + 2, Y, j[1] + 7), "minecraft:redstone_lamp")
    setblock(at(j[0] + 2, Y, j[1] + 6), "minecraft:stone")
    setblock(at(j[0] + 3, Y, j[1] + 6), "minecraft:redstone_wire")
    setblock(at(j[0] + 1, Y, j[1] + 6), "minecraft:repeater[facing=west]")
    setblock(at(j[0], Y, j[1] + 6), "minecraft:redstone_block")
    # wire doesn't pass power through a block to more wire, but a lamp beside the block lights
    setblock(at(j[0] + 2, Y, j[1] + 11), "minecraft:redstone_lamp")
    setblock(at(j[0] + 2, Y, j[1] + 10), "minecraft:stone")
    setblock(at(j[0] + 3, Y, j[1] + 10), "minecraft:redstone_wire")
    setblock(at(j[0] + 1, Y, j[1] + 10), "minecraft:redstone_wire")
    setblock(at(j[0], Y, j[1] + 10), "minecraft:redstone_block")
    # 11. A torch clock: a torch whose own output, round a loop of wire, powers its block. It
    # flickers until it has turned off 8 times within 3 seconds, then burns out.
    k = (55, 6)
    setblock(at(k[0], Y, k[1]), "minecraft:stone")
    loop = [(2, 0), (2, 1), (2, 2), (1, 2), (0, 2), (-1, 2), (-2, 2), (-2, 1), (-2, 0), (-1, 0)]
    for dx, dz in loop:
        setblock(at(k[0] + dx, Y, k[1] + dz), "minecraft:redstone_wire")
    alice.pump(2.0)

    # Actions: the button, the pressure plate, the observed blocks, the copper bulb's lever.
    use(at(g[0], Y, g[1] + 1))
    use(at(g[0] + 5, Y, g[1]), cy=0.1)
    alice.command(f"tp Alice {ox + g[0] + 3.5} {Y} {oz + g[1] + 0.5}")
    alice.pump(0.6)
    plate_lit = state(at(g[0] + 3, Y - 1, g[1]))
    plate_state = state(at(g[0] + 3, Y, g[1]))
    alice.command(f"tp Alice {ox + g[0] + 3.5} {Y} {oz + g[1] - 3.5}")
    setblock(at(h[0] - 1, Y, h[1]), "minecraft:stone")
    setblock(at(h[0] - 1, Y, h[1] + 3), "minecraft:stone")
    use(at(i[0] - 1, Y, i[1]))
    alice.pump(0.5)
    use(at(i[0] - 1, Y, i[1]))
    alice.pump(0.5)
    bulb_after_one = state(at(i[0], Y, i[1]))
    use(at(i[0] - 1, Y, i[1]))
    alice.pump(0.5)
    use(at(i[0] - 1, Y, i[1]))
    alice.command(f"tp Alice {ox + k[0]} {Y} {oz + k[1] - 3}")  # (level events reach players within 64 blocks)
    setblock(at(k[0] + 1, Y, k[1]), "minecraft:redstone_wall_torch[facing=east]")
    alice.pump(2.5)
    use(at(a[0], Y, a[1]))  # the long line goes dark again
    alice.pump(3.0)  # (buttons, plates and lamps go back to off)

    # ---- checks
    def power(p):
        s = state(p)
        return int(s.split("power=")[1].split(",")[0].rstrip("]")) if "power=" in s else None

    line = [power(at(a[0] + k, Y, a[1] + 2)) for k in range(1, 17)]
    print(f"  OBS  wire powers: {line}")
    check(line == list(range(15, 0, -1)) + [0], "wire loses one per block: 15 next to the lever, 1 at the 15th, 0 at the 16th")
    check(went_through(at(a[0] + 16, Y, a[1]), "minecraft:redstone_lamp[lit=true]"), "the 15th wire still lights a lamp")
    check(state(at(a[0] + 17, Y, a[1] + 2)) == "minecraft:redstone_lamp[lit=false]", "a lamp after 16 wires stays off")
    check(all(power(at(a[0] + k, Y, a[1])) == 0 for k in range(1, 16)) and state(at(a[0] + 16, Y, a[1])) == "minecraft:redstone_lamp[lit=false]",
          "switching the lever off again empties the wire and the lamp goes out")

    torch = alice.history.get(at(k[0] + 1, Y, k[1]), [])
    print(f"  OBS  torch clock: {len(torch)} changes, {torch[:3]}")
    check(alice.saw("level_event", lambda i: i[0] == 1502 and i[1] == at(k[0] + 1, Y, k[1])), "a torch clock burns out")
    check(sum(s.endswith("lit=false]") for s in torch) == 8, "after turning off 8 times")

    print(f"  OBS  repeaters: {[state(at(b[0] + k, Y, b[1])) for k in range(1, 5)]}")
    check(all(state(at(b[0] + k, Y, b[1])).find("powered=true") > 0 for k in range(1, 4)), "the repeaters pass the signal on")
    check(state(at(b[0] + 4, Y, b[1])) == "minecraft:redstone_lamp[lit=true]", "and light the lamp")

    print(f"  OBS  inverter: {state(at(c[0] + 1, Y, c[1]))}, {state(at(c[0] + 2, Y, c[1]))}")
    check(state(at(c[0] + 1, Y, c[1])) == "minecraft:redstone_wall_torch[facing=east,lit=true]", "a torch on an unpowered block is lit")
    check(state(at(c[0] + 2, Y, c[1])) == "minecraft:redstone_lamp[lit=true]", "and powers the lamp in front of it")
    check(state(at(c[0] + 5, Y + 1, c[1])) == "minecraft:redstone_torch[lit=true]", "a standing torch is lit")
    check(state(at(c[0] + 5, Y + 2, c[1])) == "minecraft:redstone_lamp[lit=true]", "and powers the lamp above")

    print(f"  OBS  comparators: hopper {power(at(d[0] + 2, Y, d[1]))}, subtract {power(at(d[0] + 2, Y, d[1] + 4))}, "
          f"cake {power(at(d[0] + 2, Y, d[1] + 7))}, cauldron {power(at(d[0] + 2, Y, d[1] + 9))}")
    check(power(at(d[0] + 2, Y, d[1])) == 6 and power(at(d[0] + 3, Y, d[1])) == 5, "a comparator reads a hopper with 2 of 5 stacks as 6")
    check(power(at(d[0] + 2, Y, d[1] + 4)) == 10, "subtract mode: 15 - 5 = 10")
    check(power(at(d[0] + 2, Y, d[1] + 7)) == 14, "a whole cake reads 14")
    check(power(at(d[0] + 2, Y, d[1] + 9)) == 2, "a cauldron behind a block reads its level")

    print(f"  OBS  doors: {state(at(e[0], Y, e[1]))}, {state(at(e[0] + 3, Y, e[1]))}, {state(at(e[0] + 6, Y, e[1]))}, {state(at(e[0] + 9, Y, e[1]))}")
    check("open=true" in state(at(e[0], Y, e[1])) and "open=true" in state(at(e[0], Y + 1, e[1])), "a powered iron door opens (both halves)")
    check("open=true" in state(at(e[0] + 3, Y, e[1])), "a powered iron trapdoor opens")
    check("open=true" in state(at(e[0] + 6, Y, e[1])), "a powered fence gate opens")
    check("open=true" in state(at(e[0] + 9, Y, e[1])), "power next to a door's upper half opens it")
    print(f"  OBS  closing door: {alice.history.get(at(e[0] + 9, Y, e[1] + 3), [])}")
    check(went_through(at(e[0] + 9, Y, e[1] + 3), "minecraft:oak_door[facing=north,half=lower,hinge=left,open=true,powered=true]")
          and "open=false" in state(at(e[0] + 9, Y, e[1] + 3)), "a door closes when the power goes")

    print(f"  OBS  hopper: {state(at(f[0], Y, f[1]))}")
    check("enabled=false" in state(at(f[0], Y, f[1])), "a hopper next to a redstone block is locked")
    rails = [state(at(f[0] + k, Y, f[1] + 3)).find("powered=true") > 0 for k in range(1, 13)]
    print(f"  OBS  powered rails: {rails}")
    check(rails == [True] * 9 + [False] * 3, "powered rails carry power to 8 more rails")
    act = [state(at(f[0] + k, Y, f[1] + 6)).find("powered=true") > 0 for k in range(0, 4)]
    print(f"  OBS  activator rails: {act}")

    for name, p in (("button", at(g[0], Y, g[1] + 1)), ("button lamp", at(g[0], Y + 1, g[1])), ("plate lamp", at(g[0] + 3, Y - 1, g[1])),
                    ("floor button lamp", at(g[0] + 5, Y - 1, g[1]))):
        print(f"  OBS  {name}: {alice.history.get(p, [])[-4:]}")
    check(went_through(at(g[0], Y + 1, g[1]), "minecraft:redstone_lamp[lit=true]"), "a pressed button lights a lamp")
    check(state(at(g[0], Y + 1, g[1])) == "minecraft:redstone_lamp[lit=false]" and "powered=false" in state(at(g[0], Y, g[1] + 1)),
          "the button pops back out and the lamp goes off")
    check(went_through(at(g[0] + 5, Y - 1, g[1]), "minecraft:redstone_lamp[lit=true]"), "a floor button powers the block below")
    print(f"  OBS  plate while standing on it: {plate_state}, lamp {plate_lit}")
    check(plate_state == "minecraft:stone_pressure_plate[powered=true]" and plate_lit == "minecraft:redstone_lamp[lit=true]",
          "standing on a stone pressure plate powers it")
    check(state(at(g[0] + 3, Y, g[1])) == "minecraft:stone_pressure_plate[powered=false]", "and it goes off after stepping off")

    print(f"  OBS  observer: {alice.history.get(at(h[0], Y, h[1]), [])}, lamp {alice.history.get(at(h[0] + 1, Y, h[1]), [])}")
    check(went_through(at(h[0], Y, h[1]), "minecraft:observer[facing=west,powered=true]")
          and state(at(h[0], Y, h[1])) == "minecraft:observer[facing=west,powered=false]", "an observer pulses when the block it watches changes")
    check(went_through(at(h[0] + 1, Y, h[1]), "minecraft:redstone_lamp[lit=true]"), "the pulse lights the lamp behind it")
    np = at(h[0] + 1, Y, h[1] + 3)
    check(alice.saw("block_event", lambda i: i[0] == np), "and plays the note block")

    print(f"  OBS  copper bulb: after one switch {bulb_after_one}, history {alice.history.get(at(i[0], Y, i[1]), [])}")
    check(bulb_after_one == "minecraft:copper_bulb[lit=true,powered=false]", "a copper bulb stays lit after its power goes")
    check(state(at(i[0], Y, i[1])) == "minecraft:copper_bulb[lit=false,powered=false]", "and turns off with the next pulse")

    print(f"  OBS  locked repeater: {state(at(j[0], Y, j[1]))}")
    check("locked=true" in state(at(j[0], Y, j[1])), "a repeater powered from the side by a repeater is locked")
    steps = [power(at(j[0] + 4, Y, j[1])), power(at(j[0] + 5, Y + 1, j[1])), power(at(j[0] + 6, Y + 2, j[1])),
             power(at(j[0] + 7, Y + 1, j[1])), power(at(j[0] + 8, Y, j[1]))]
    print(f"  OBS  wire up and down: {steps}, shapes {state(at(j[0] + 4, Y, j[1]))}, {state(at(j[0] + 7, Y + 1, j[1]))}")
    check(steps == [15, 14, 13, 12, 11], "wire climbs blocks and comes back down")
    print(f"  OBS  strong: wire {power(at(j[0] + 3, Y, j[1] + 6))}, lamp {state(at(j[0] + 2, Y, j[1] + 7))}; "
          f"weak: wire {power(at(j[0] + 3, Y, j[1] + 10))}, lamp {state(at(j[0] + 2, Y, j[1] + 11))}")
    check(power(at(j[0] + 3, Y, j[1] + 6)) == 15, "a repeater powers a block strongly (wire next to it gets 15)")
    check(state(at(j[0] + 2, Y, j[1] + 7)) == "minecraft:redstone_lamp[lit=true]", "and a lamp next to it")
    check(power(at(j[0] + 3, Y, j[1] + 10)) == 0, "wire doesn't power wire through a block")
    check(state(at(j[0] + 2, Y, j[1] + 11)) == "minecraft:redstone_lamp[lit=true]", "but a lamp next to it lights")

    # 13. Tripwire between two hooks: they attach, and someone walking through powers them.
    n = (-40, 30)
    setblock(at(n[0] - 1, Y, n[1]), "minecraft:stone")
    setblock(at(n[0] + 5, Y, n[1]), "minecraft:stone")
    setblock(at(n[0] - 2, Y, n[1]), "minecraft:redstone_lamp")
    setblock(at(n[0], Y, n[1]), "minecraft:tripwire_hook[facing=east]")
    setblock(at(n[0] + 4, Y, n[1]), "minecraft:tripwire_hook[facing=west]")
    for k in (1, 2, 3):
        setblock(at(n[0] + k, Y, n[1]), "minecraft:tripwire")
    alice.pump(0.5)
    hook_attached = state(at(n[0], Y, n[1]))
    alice.command(f"tp Alice {ox + n[0] + 2.5} {Y} {oz + n[1] + 0.5}")
    alice.pump(0.5)
    hook_walked = state(at(n[0], Y, n[1]))
    lamp_walked = state(at(n[0] - 2, Y, n[1]))
    alice.command(f"tp Alice {ox + n[0] + 2.5} {Y} {oz + n[1] - 3.5}")
    alice.pump(1.5)
    print(f"  OBS  tripwire: attached {hook_attached}, walked through {hook_walked} lamp {lamp_walked}, after {state(at(n[0], Y, n[1]))}")
    check("attached=true" in hook_attached and "powered=false" in hook_attached, "tripwire hooks attach to the wire between them")
    check("powered=true" in hook_walked and lamp_walked == "minecraft:redstone_lamp[lit=true]", "walking through the wire powers them")
    check("attached=true" in state(at(n[0], Y, n[1])) and "powered=false" in state(at(n[0], Y, n[1])), "and they switch off again afterwards")

    # 12. Daylight detectors through the day (the sun angle's eased timeline decides the strength).
    m = (-60, 30)
    setblock(at(m[0], Y, m[1]), "minecraft:daylight_detector")
    setblock(at(m[0] + 2, Y, m[1]), "minecraft:daylight_detector[inverted=true]")
    readings = []
    for t in (1000, 5000, 11000, 12500, 13500, 18000, 23500):
        alice.command(f"time set {t}")
        alice.pump(1.3)  # (they update every 20 ticks)
        readings.append((t, power(at(m[0], Y, m[1])), power(at(m[0] + 2, Y, m[1]))))
    print(f"  OBS  daylight detectors (time, normal, inverted): {readings}")
    check(readings[1][1] == 15 and readings[1][2] == 0, "a daylight detector reads 15 at noon, inverted 0")
    check(readings[5][1] == 0 and readings[5][2] > 0, "and 0 at midnight, inverted more")

    if os.environ.get("REDSTONE_DUMP"):  # every block state in the area, relative to spawn
        with open(os.environ["REDSTONE_DUMP"], "w") as out:
            out.write(f"daylight {readings}\n")
            for p in sorted(alice.blocks):
                if -70 <= p[0] - ox <= 70 and 4 <= p[2] - oz <= 40 and Y - 1 <= p[1] <= Y + 4 and "daylight" not in state(p):
                    out.write(f"{p[0] - ox} {p[1]} {p[2] - oz} {state(p)}\n")
            for p in sorted(alice.history):  # and what each went through (order within a position is exact)
                if -70 <= p[0] - ox <= 70 and 4 <= p[2] - oz <= 40 and Y - 1 <= p[1] <= Y + 4:
                    out.write(f"history {p[0] - ox} {p[1]} {p[2] - oz} {' > '.join(alice.history[p][:12])}\n")

    print("PASS" if not failures else f"FAIL ({len(failures)})")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
