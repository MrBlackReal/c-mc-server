#!/usr/bin/env python3
"""Regenerate data/*.json from an official server jar.

    tools/refresh_data.py path/to/minecraft_server.X.jar [--java /usr/lib/jvm/java-25-openjdk/bin/java]

1. Runs the vanilla data generator (--reports) for packets/registries/blocks/item components.
2. Starts the vanilla server in offline mode on a spare port, joins it with mcprobe and records
   the synchronised registries and tags exactly as vanilla sends them (data/vanilla_config.json).
3. Compiles and runs tools/extractor against the jar for block/item behaviour (data/extracted.json).

Afterwards bump kProtocolVersion / kGameVersion in src/server/minecraft_server.hpp and PROTOCOL in
tools/mcprobe.py to the values printed at the end.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DATA = os.path.join(ROOT, "data")
PORT = 25598


def summarize_item_components(directory):
    """data/item_components.json: the few default item components the server needs."""
    out = {}
    for name in sorted(os.listdir(directory)):
        c = json.load(open(os.path.join(directory, name)))["components"]
        e = {"max_stack": c.get("minecraft:max_stack_size", 64)}
        tool = c.get("minecraft:tool")
        if tool is not None and tool.get("can_destroy_blocks_in_creative", True) is False:
            e["no_creative_destroy"] = True
        equippable = c.get("minecraft:equippable")
        if equippable is not None:
            e["equip"] = equippable["slot"]
        if "minecraft:max_damage" in c:
            e["max_damage"] = c["minecraft:max_damage"]
        if tool is not None:
            e["tool"] = {"default_mining_speed": tool.get("default_mining_speed", 1.0),
                         "damage_per_block": tool.get("damage_per_block", 1),
                         "rules": tool.get("rules", [])}
        if c.get("minecraft:attribute_modifiers"):  # armor, attack damage/speed, ...
            e["attributes"] = [{"type": m["type"], "id": m["id"], "amount": m["amount"], "operation": m["operation"],
                                "slot": m.get("slot", "any")} for m in c["minecraft:attribute_modifiers"]]
        if "minecraft:compostable" in c:  # the layers provider: a context_int_provider id or inline
            e["compostable"] = c["minecraft:compostable"]["layers"]
        if "minecraft:dye" in c:  # dyes (applied to signs, sheep, ...)
            e["dye"] = c["minecraft:dye"]
        if "minecraft:block_transformer" in c:  # axes, shovels, hoes: a minecraft:block_transformer
            e["block_transformer"] = c["minecraft:block_transformer"]
        if "minecraft:cooking_fuel" in c:  # furnace fuel: burn time and speed providers
            e["cooking_fuel"] = c["minecraft:cooking_fuel"]
        if "minecraft:jukebox_playable" in c:  # music discs: the minecraft:jukebox_song
            e["jukebox_song"] = c["minecraft:jukebox_playable"]
        e["defaults"] = c  # the whole prototype (DataComponentMap), for patch normalization and the menus
        out["minecraft:" + name[:-5]] = e
    with open(os.path.join(DATA, "item_components.json"), "w") as f:
        json.dump(out, f, separators=(",", ":"))


def pack_loot_tables(jar):
    """data/loot_tables.json: block loot and interaction tables and the predicates they reference, from the jar."""
    import io
    with zipfile.ZipFile(jar) as outer:
        inner_name = next(n for n in outer.namelist() if n.startswith("META-INF/versions/") and n.endswith(".jar"))
        with zipfile.ZipFile(io.BytesIO(outer.read(inner_name))) as z:
            out = {"tables": {}, "predicates": {}}
            for name in z.namelist():
                if not name.endswith(".json"):
                    continue
                # blocks/ (drops) and the block interaction tables (carving, harvesting, tilling).
                folders = [("data/minecraft/loot_table/" + f, "tables") for f in ("blocks/", "carve/", "harvest/", "till/")]
                for folder, key in folders + [("data/minecraft/predicate/", "predicates")]:
                    if name.startswith(folder):
                        rel = name[len("data/minecraft/"):]
                        rel = rel[len("loot_table/"):] if key == "tables" else rel[len("predicate/"):]
                        out[key]["minecraft:" + rel[:-5]] = json.loads(z.read(name))
    with open(os.path.join(DATA, "loot_tables.json"), "w") as f:
        json.dump(out, f, separators=(",", ":"), sort_keys=True)


def pack_folder(jar, folder, out_name):
    """data/<out_name>: every JSON file under data/minecraft/<folder>/ in the jar, by id."""
    import io
    out = {}
    with zipfile.ZipFile(jar) as outer:
        inner_name = next(n for n in outer.namelist() if n.startswith("META-INF/versions/") and n.endswith(".jar"))
        with zipfile.ZipFile(io.BytesIO(outer.read(inner_name))) as z:
            prefix = f"data/minecraft/{folder}/"
            for name in z.namelist():
                if name.startswith(prefix) and name.endswith(".json"):
                    out["minecraft:" + name[len(prefix):-5]] = json.loads(z.read(name))
    with open(os.path.join(DATA, out_name), "w") as f:
        json.dump(out, f, separators=(",", ":"), sort_keys=True)


def pack_language(jar):
    """data/en_us.json: the English translations the server itself renders (console output)."""
    import io
    with zipfile.ZipFile(jar) as outer:
        inner_name = next(n for n in outer.namelist() if n.startswith("META-INF/versions/") and n.endswith(".jar"))
        with zipfile.ZipFile(io.BytesIO(outer.read(inner_name))) as z:
            lang = json.loads(z.read("assets/minecraft/lang/en_us.json"))
    with open(os.path.join(DATA, "en_us.json"), "w") as f:
        json.dump(lang, f, separators=(",", ":"), ensure_ascii=False)


def pack_biome_attributes(jar):
    """data/biome_attributes.json: each biome's environment attributes (fire burnout, ...)."""
    import io
    out = {}
    with zipfile.ZipFile(jar) as outer:
        inner_name = next(n for n in outer.namelist() if n.startswith("META-INF/versions/") and n.endswith(".jar"))
        with zipfile.ZipFile(io.BytesIO(outer.read(inner_name))) as z:
            prefix = "data/minecraft/worldgen/biome/"
            for name in z.namelist():
                if name.startswith(prefix) and name.endswith(".json"):
                    out["minecraft:" + name[len(prefix):-5]] = json.loads(z.read(name)).get("attributes", {})
    with open(os.path.join(DATA, "biome_attributes.json"), "w") as f:
        json.dump(out, f, separators=(",", ":"), sort_keys=True)


def pack_registry_elements(jar):
    """data/registry_elements.json: every synced registry element (from data/vanilla_config.json)
    as the jar defines it, sent to clients that don't have the vanilla pack."""
    import io
    cfg = json.load(open(os.path.join(DATA, "vanilla_config.json")))
    synced = set(json.load(open(os.path.join(DATA, "extracted.json")))["synced_environment_attributes"])
    out = {}
    with zipfile.ZipFile(jar) as outer:
        inner_name = next(n for n in outer.namelist() if n.startswith("META-INF/versions/") and n.endswith(".jar"))
        with zipfile.ZipFile(io.BytesIO(outer.read(inner_name))) as z:
            for r in cfg["registries"]:
                folder = "data/minecraft/" + r["registry"].split(":", 1)[1] + "/"
                elements = {}
                for e in r["entries"]:
                    d = json.loads(z.read(folder + e.split(":", 1)[1] + ".json"))
                    if r["registry"] == "minecraft:worldgen/biome":  # Biome.NETWORK_CODEC leaves generation out
                        for k in ("features", "carvers", "spawners", "spawn_costs"):
                            d.pop(k, None)
                    # Only client-synced environment attributes are sent (EnvironmentAttributeMap.NETWORK_CODEC).
                    for key in ("attributes", "tracks"):
                        if isinstance(d, dict) and isinstance(d.get(key), dict):
                            d[key] = {k: v for k, v in d[key].items() if k in synced}
                    elements[e] = d
                out[r["registry"]] = elements
    with open(os.path.join(DATA, "registry_elements.json"), "w") as f:
        json.dump(out, f, separators=(",", ":"), sort_keys=True)


def run_extractor(java, server_dir, work, version_id):
    """data/extracted.json: block classes, per-state flags, sounds, item->block (tools/extractor)."""
    jars = [os.path.join(server_dir, "versions", version_id, f"server-{version_id}.jar")]
    for dirpath, _, files in os.walk(os.path.join(server_dir, "libraries")):
        jars += [os.path.join(dirpath, f) for f in files if f.endswith(".jar")]
    cp = os.pathsep.join(jars)
    javac = os.path.join(os.path.dirname(java), "javac") if os.path.dirname(java) else "javac"
    classes = os.path.join(work, "extractor")
    subprocess.run([javac, "-nowarn", "-cp", cp, "-d", classes, os.path.join(ROOT, "tools", "extractor", "Extractor.java")], check=True)
    subprocess.run([java, "-cp", classes + os.pathsep + cp, "Extractor", os.path.join(DATA, "extracted.json"), os.path.join(DATA, "vanilla_config.json")],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jar")
    ap.add_argument("--java", default="java")
    a = ap.parse_args()
    jar = os.path.abspath(a.jar)
    with zipfile.ZipFile(jar) as z:
        version = json.loads(z.read("version.json"))
    print(f"version {version['id']}, protocol {version['protocol_version']}")

    work = tempfile.mkdtemp(prefix="cmc-refresh-")
    try:
        subprocess.run([a.java, "-DbundlerMainClass=net.minecraft.data.Main", "-jar", jar, "--reports", "--output", "gen"],
                       cwd=work, check=True, stdout=subprocess.DEVNULL)
        for name in ("packets.json", "registries.json", "blocks.json"):
            shutil.copy(os.path.join(work, "gen", "reports", name), os.path.join(DATA, name))
        summarize_item_components(os.path.join(work, "gen", "reports", "minecraft", "components", "item"))
        pack_loot_tables(jar)
        pack_folder(jar, "enchantment", "enchantments.json")  # data-driven enchantment effects
        pack_folder(jar, "context_int_provider", "context_int_providers.json")  # compostable chances, fuel times, ...
        pack_folder(jar, "context_float_provider", "context_float_providers.json")  # furnace speeds
        pack_folder(jar, "jukebox_song", "jukebox_songs.json")  # song lengths
        pack_folder(jar, "block_transformer", "block_transformers.json")  # stripping, tilling, path making
        pack_folder(jar, "recipe", "recipes.json")  # crafting, smelting, stonecutting, ...
        pack_folder(jar, "timeline", "timelines.json")  # every environment attribute track (eyeblossoms open, ...)
        pack_folder(jar, "worldgen/feature", "features.json")  # trees and huge mushrooms (saplings, bone meal)
        pack_biome_attributes(jar)
        pack_language(jar)

        srv = os.path.join(work, "server")
        os.makedirs(srv)
        with open(os.path.join(srv, "eula.txt"), "w") as f:
            f.write("eula=true\n")
        with open(os.path.join(srv, "server.properties"), "w") as f:
            f.write(f"online-mode=false\nserver-port={PORT}\nwhite-list=false\nlevel-type=minecraft\\:flat\n")
        log = open(os.path.join(srv, "log.txt"), "w")
        proc = subprocess.Popen([a.java, "-jar", jar, "nogui"], cwd=srv, stdin=subprocess.PIPE, stdout=log, stderr=log)
        try:
            for _ in range(180):
                time.sleep(1)
                if "Done (" in open(os.path.join(srv, "log.txt")).read():
                    break
            else:
                sys.exit("vanilla server did not start; see " + os.path.join(srv, "log.txt"))
            dump = os.path.join(work, "dump.json")
            env = dict(os.environ)
            subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mcprobe.py"), "join", "127.0.0.1", str(PORT),
                            "--seconds", "2", "--dump", dump, "--protocol", str(version["protocol_version"])],
                           check=True, env=env, stdout=subprocess.DEVNULL)
        finally:
            proc.communicate(b"stop\n", timeout=60)

        d = json.load(open(dump))
        out = {
            "known_packs": [["minecraft", "core", version["id"]]],
            "features": d["features"],
            "registries": [{"registry": r["registry"], "entries": [e["id"] for e in r["entries"]]} for r in d["registries"]],
            "tags": d["tags"],
        }
        with open(os.path.join(DATA, "vanilla_config.json"), "w") as f:
            json.dump(out, f, separators=(",", ":"))
        run_extractor(a.java, srv, work, version["id"])  # (binds the tags from vanilla_config.json)
        pack_registry_elements(jar)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"data/ updated. Set kProtocolVersion={version['protocol_version']} kGameVersion=\"{version['id']}\".")


if __name__ == "__main__":
    main()
