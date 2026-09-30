#!/usr/bin/env python3
"""Item data components: this server's Codec/StreamCodec port against vanilla's.

    test_components.py give  HOST PORT          vanilla only (ops): /give Alice items using many components
    test_components.py dump  HOST PORT OUT      Alice joins; the raw inventory packet is written to OUT
    test_components.py compare A B              the two dumps must be byte-identical
    test_components.py disk  VANILLA_WORLD OUR_WORLD
                                                Alice's saved inventory must be the same NBT in both

tools/test_components.sh runs them: vanilla gives the items and restarts, then this server loads the
same world, sends the inventory (network form, decoded from disk) and saves it again (disk form).
"""
import gzip
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import nbtdump  # noqa: E402
from test_building import Client, check, failures  # noqa: E402

# Maps keyed by registry holders (enchantments, debug_stick_state) iterate in identity-hash order in
# vanilla, which changes between runs; they get one entry here so the packets can be compared
# byte for byte (the disk and hash checks don't depend on the order).
ITEMS = [
    'enchanted_book[stored_enchantments={"minecraft:mending":1}]',
    'diamond_sword[enchantments={"minecraft:sharpness":3},custom_name={text:"Blade",color:"red",bold:true},'
    'lore=["line1",{text:"line2",italic:false}],damage=5,repair_cost=3,rarity="epic",enchantment_glint_override=false,unbreakable={}]',
    'potion[potion_contents={potion:"minecraft:swiftness",custom_color:123456,custom_effects:[{id:"minecraft:speed",amplifier:1b,duration:200,'
    'show_icon:false,hidden_effect:{amplifier:0b,duration:40}}]},potion_duration_scale=0.5f]',
    'written_book[written_book_content={title:"T",author:"A",generation:1,pages:["p1",{raw:"p2",filtered:"q2"}],resolved:true}]',
    'writable_book[writable_book_content={pages:["a",{raw:"b",filtered:"c"}]}]',
    'firework_rocket[fireworks={flight_duration:2,explosions:[{shape:"star",colors:[I;1,2],fade_colors:[I;3],has_trail:true}]}]',
    'firework_star[firework_explosion={shape:"creeper",has_twinkle:true}]',
    'player_head[profile={name:"Notch"},note_block_sound="minecraft:entity.pig.ambient"]',
    'player_head[profile={id:[I;1,2,3,4],name:"Bob",properties:[{name:"textures",value:"abc",signature:"sig"}]}]',
    'white_banner[banner_patterns=[{pattern:"minecraft:stripe_top",color:"red"},{pattern:"minecraft:cross",color:"blue"}]]',
    'leather_chestplate[dyed_color=16711680,trim={material:"minecraft:gold",pattern:"minecraft:coast"}]',
    'crossbow[charged_projectiles=[{id:"minecraft:arrow",count:1},{id:"minecraft:firework_rocket",components:{"minecraft:fireworks":{flight_duration:1}}}]]',
    'bundle[bundle_contents=[{id:"minecraft:stone",count:5},{id:"minecraft:apple"}]]',
    'shulker_box[container=[{slot:0,item:{id:"minecraft:dirt",count:2}},{slot:5,item:{id:"minecraft:stick",components:{"minecraft:custom_name":"S"}}}]]',
    'stone[can_place_on={blocks:"minecraft:dirt"},can_break=[{blocks:["minecraft:stone","minecraft:dirt"]},{blocks:"#minecraft:logs",'
    'state:{axis:"y"}}]]',
    'diamond_pickaxe[tool={rules:[{blocks:"#minecraft:mineable/pickaxe",speed:10,correct_for_drops:true},{blocks:"minecraft:cobweb"}],'
    'default_mining_speed:1.5,damage_per_block:2}]',
    'stick[attribute_modifiers=[{type:"minecraft:attack_damage",id:"minecraft:x",amount:5,operation:"add_value",slot:"mainhand"},'
    '{type:"minecraft:armor",id:"minecraft:y",amount:0.5,operation:"add_multiplied_total",display:{type:"override",value:"Z"}}],'
    'custom_model_data={floats:[1.5f],flags:[true,false],strings:["s"],colors:[123]},tooltip_display={hidden_components:["minecraft:enchantments"]}]',
    'apple[food={nutrition:3,saturation:1.2f},consumable={consume_seconds:2.0f,animation:"drink",on_consume_effects:[{type:"minecraft:apply_effects",'
    'effects:[{id:"minecraft:regeneration",duration:100}],probability:0.5f},{type:"minecraft:clear_all_effects"},'
    '{type:"minecraft:remove_effects",effects:["minecraft:poison","minecraft:wither"]},{type:"minecraft:teleport_randomly",diameter:8.0f}]},'
    'use_cooldown={seconds:1.0f,cooldown_group:"minecraft:g"},use_remainder={id:"minecraft:bowl"}]',
    'diamond_chestplate[equippable={slot:"chest",equip_sound:"minecraft:item.armor.equip_diamond",asset_id:"minecraft:diamond",dispensable:false,'
    'allowed_entities:"minecraft:player"},damage_resistant={types:"#minecraft:is_fire"},repairable={items:"minecraft:diamond"},enchantable={value:10}]',
    'compass[lodestone_tracker={target:{dimension:"minecraft:overworld",pos:[I;1,2,3]},tracked:false}]',
    'suspicious_stew[suspicious_stew_effects=[{id:"minecraft:saturation",duration:7},{id:"minecraft:night_vision"}]]',
    'tropical_fish_bucket[bucket_entity_data={Health:3.0f},tropical_fish/pattern="glitter",tropical_fish/base_color="red",tropical_fish/pattern_color="lime"]',
    'axolotl_bucket[axolotl/variant="blue"]',
    'goat_horn[instrument="minecraft:ponder_goat_horn"]',
    'stone[custom_data={a:1,b:{c:"x",d:[1L,2L]}},item_name="Named",item_model="minecraft:diamond",tooltip_style="minecraft:x",dye="red",'
    'max_stack_size=16,minimum_attack_charge=0.5f,damage_type="minecraft:arrow"]',
    'beehive[bees=[{entity_data:{id:"minecraft:bee",Health:5.0f},ticks_in_hive:10,min_ticks_in_hive:5}],block_state={honey_level:"3"}]',
    'decorated_pot[pot_decorations={back:"minecraft:brick",front:"minecraft:angler_pottery_sherd"}]',
    'debug_stick[debug_stick_state={"minecraft:oak_log":"axis"}]',
    'knowledge_book[recipes=["minecraft:crafting_table","minecraft:stick"]]',
    'chest[container_loot={loot_table:"minecraft:chests/simple_dungeon",seed:5L},lock={items:"minecraft:stick"}]',
    'filled_map[map_id=5,map_decorations={a:{type:"minecraft:player",x:1.0,z:2.0,rotation:3.0f}}]',
    'oak_sign[block_entity_data={id:"minecraft:sign",front_text:{messages:["a","","",""]}}]',
    'armor_stand[entity_data={id:"minecraft:armor_stand",Invisible:1b}]',
    'stick[glider={},death_protection={death_effects:[{type:"minecraft:play_sound",sound:"minecraft:entity.pig.ambient"}]},'
    'weapon={item_damage_per_attack:2},attack_range={max_reach:4.0f},ominous_bottle_amplifier=2,map_id=1,!minecraft:lore]',
    'wolf_spawn_egg[wolf/variant="minecraft:ashen",wolf/collar="blue",painting/variant="minecraft:kebab",villager/variant="minecraft:taiga",'
    'cat/variant="minecraft:tabby",fox/variant="snow",rabbit/variant="gold",horse/variant="creamy",llama/variant="gray",parrot/variant="blue",'
    'salmon/size="large",mooshroom/variant="brown",frog/variant="minecraft:cold",sheep/color="pink",shulker/color="cyan",cat/collar="red"]',
]


def give(host, port):
    alice = Client(host, port, "Alice")
    alice.pump(1.0)
    alice.command("clear Alice")
    alice.pump(0.5)
    for item in ITEMS:
        alice.command(f"give Alice {item}")
        alice.pump(0.25)
    alice.pump(1.0)
    got = sum(1 for v in alice.slots.values() if v)
    print(f"vanilla accepted {got} of {len(ITEMS)} items")
    check(got >= len(ITEMS) - 2, f"vanilla accepted the /give commands ({got}/{len(ITEMS)})")


class RawClient(Client):
    raw = None

    def _handle(self, n, r):
        if n == "container_set_content" and self.raw is None:
            start = r.i
            self.raw = bytes(r.rest())
            r.i = start
        super()._handle(n, r)


def dump(host, port, out):
    alice = RawClient(host, port, "Alice")
    alice.pump(until=lambda: alice.raw is not None, timeout=5)
    alice.pump(0.5)
    check(alice.raw is not None, "inventory received")
    open(out, "wb").write(alice.raw or b"")
    print(f"{len(alice.raw or b'')} bytes written to {out}")


def strip_header(b):
    """container_set_content: window id, state id (varints; the state id differs), items..."""
    i = 0
    for _ in range(2):
        while b[i] & 0x80:
            i += 1
        i += 1
    return b[i:]


def compare(a, b):
    va, vb = strip_header(open(a, "rb").read()), strip_header(open(b, "rb").read())
    if va != vb:
        i = next((k for k in range(min(len(va), len(vb))) if va[k] != vb[k]), min(len(va), len(vb)))
        print(f"    first difference at byte {i} (of {len(va)} / {len(vb)})")
        print(f"    vanilla: {va[max(0, i - 24):i + 24].hex(' ')}")
        print(f"    ours:    {vb[max(0, i - 24):i + 24].hex(' ')}")
    check(va == vb, f"inventory packet identical to vanilla's ({len(va)} bytes)")


def inventory(world):
    """Alice's saved inventory by slot, as typed NBT ((type, value) tuples), so a byte and an int differ."""
    d = os.path.join(world, "players", "data")
    for f in os.listdir(d):
        if f.endswith(".dat"):
            _, tag = nbtdump.read_named(gzip.open(os.path.join(d, f)).read())
            return {it[1]["Slot"][1]: it for it in tag.get("Inventory", ("list", []))[1]}
    return {}


def disk(vanilla_world, our_world):
    a, b = inventory(vanilla_world), inventory(our_world)
    check(a.keys() == b.keys(), f"same slots: {sorted(a)} vs {sorted(b)}")
    same = 0
    for slot in sorted(a):
        if a[slot] == b.get(slot):
            same += 1
        else:
            print(f"    slot {slot} differs:\n      vanilla: {a[slot]}\n      ours:    {b.get(slot)}")
    check(same == len(a), f"{same}/{len(a)} saved items identical to vanilla's")


def main():
    mode = sys.argv[1]
    if mode == "compare":
        compare(sys.argv[2], sys.argv[3])
    elif mode == "disk":
        disk(sys.argv[2], sys.argv[3])
    elif mode == "give":
        give(sys.argv[2], int(sys.argv[3]))
    else:
        dump(sys.argv[2], int(sys.argv[3]), sys.argv[4])
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print(f"{mode}: all checks passed")


if __name__ == "__main__":
    main()
