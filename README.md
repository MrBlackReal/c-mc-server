# cmc-server

A C++23 port of the Minecraft Java Edition dedicated server, targeting **26.3 (protocol 777)**.
It speaks the vanilla protocol, so an unmodified 26.3 client can join it.

The code follows the structure of the official server (`net.minecraft.*`), so each piece has an
obvious counterpart to compare against:

| C++                                   | Vanilla                                                              |
|---------------------------------------|----------------------------------------------------------------------|
| `network/friendly_byte_buf`           | `FriendlyByteBuf`, `VarInt`, `ByteBufCodecs`                         |
| `network/connection`                  | `Connection` (framing, compression, listener switching)              |
| `server/network/*`                    | `Server{Handshake,Status,Login,Configuration,Game}PacketListenerImpl` |
| `server/players/player_list`          | `PlayerList`                                                         |
| `server/level/server_player`          | `ServerPlayer`, `PlayerChunkSender`, `ChunkMap` tracking             |
| `world/level/chunk/*`                 | `PalettedContainer`, `Strategy`, `LevelChunkSection`, `LevelChunk`   |
| `server/level/server_level`           | `ServerLevel`, `FlatLevelSource`                                     |
| `nbt/tag`, `network/chat/component`   | `net.minecraft.nbt`, `Component`                                     |
| `build/generated/*` (from `data/`)    | built-in registries, block states, packet ids                        |

## Status

**Milestone 1: join and walk around** ✅
- Status ping (server list), login, compression
- Configuration phase: known-pack negotiation, registry sync, tags
- Classic superflat world, chunk streaming with vanilla's batched, client-acked sender
- Multiple players: tab list, see each other spawn and move, join/leave messages, chat
- Commands: `/gamemode`, `/tp`, `/list`, `/say`, `/help`; console accepts the same, plus `stop`

**Milestone 2: building, inventory, time** ✅
- Breaking (creative instant; survival start/stop digging, unbreakable blocks) and placing,
  with `block_changed_ack` sequencing and block/section updates for everyone tracking the chunk
- Placement rules ported per block class: stairs (incl. corner shapes), slabs (incl. doubling),
  doors, beds, tall plants, trapdoors, fence gates, fences/panes/walls (connections), torches and
  other wall-attached blocks, buttons/levers, signs/banners/skulls, pillars/logs, directional blocks
- Neighbour shape updates: connections update, the other half of doors/beds/plants follows,
  unsupported blocks (torches, flowers, ...) pop off. Doors, trapdoors, gates and levers toggle
- Other players see breaking particles, hear place sounds, see swings and held items/armor
- Inventory: creative slots, held item, pick block, survival clicks (pickup, split, shift-click,
  number-key swap, drag, double-click collect), block items consumed in survival
- Day/night: 26.x world clocks (overworld + end), `/time set|add|query|pause|resume|rate`

**Milestone 3: worlds on disk** ✅
- Vanilla 26.3 world format: `level.dat`, `data/minecraft/{world_gen_settings,world_clocks}.dat`,
  region files in `dimensions/minecraft/overworld/region/` (incl. oversized `.mcc` chunks),
  player files in `players/data/<uuid>.dat`, and the `session.lock` vanilla uses
- Worlds work both ways: a world saved here opens in vanilla 26.3, and a vanilla 26.3 world
  opens here, with blocks, player position, inventory, armor/offhand and clocks intact
- Data we don't model yet is passed through untouched: block entities (sent to clients too),
  entities, structures, scheduled ticks, other saved data, unknown player fields, and item
  components (converted for names, lore, enchantments, damage, ...; others preserved verbatim)
- Autosave every 5 minutes, save on `stop`, `save-all` on the console; unused chunks unload
- Safety: chunks saved by other game versions, or with blocks we don't recognise, are loaded
  read-only and never overwritten. Worlds from older versions are refused with instructions
  to upgrade them once with vanilla (`--forceUpgrade`)

**Milestone 4: mining and drops** ✅
- Dig speed checked with vanilla's formula: tool speed, Efficiency, the right tool for drops,
  slower in the air. Finishing too early is held back until the time is met, like vanilla
- Crack animation (stages 0-10) for other players
- Block drops from the real loot tables, loaded from the jar: no cobblestone from stone by hand,
  doors drop once whichever half breaks, blocks that lose their support drop too
- Tools lose durability (with Unbreaking) and break at zero
- Item entities: gravity, per-block friction (ice), merging, 5-minute despawn, pickup with the
  pickup animation; dropping with Q, Ctrl+Q, out of the inventory and from the creative menu
- Containers: chests, barrels etc. spill their contents when broken; shulker boxes drop with
  their contents (also a filled one broken in creative) and restore them when placed
- Item entities are saved (`entities/` region files) and load both ways between here and vanilla

**Between milestones: blocks that do things** ✅
- Block entities are created with their block (vanilla defaults) and their data is kept live:
  signs (editing, dyes, glow ink, wax, click commands), jukeboxes, lecterns, decorated pots,
  chiseled bookshelves, flower pots, cakes, composters, cauldrons, beehives (honey)
- Scheduled block ticks (saved as `block_ticks`) and block entity tickers: furnaces, smokers and
  blast furnaces cook (fuel, speed, experience), brewing stands brew, jukeboxes play and stop
- Fire spreads, burns out and burns blocks; falling blocks (sand, gravel, anvils, concrete
  powder) fall as entities and land; experience orbs drop from ores, furnaces and grindstones,
  merge, follow players and repair Mending gear
- Menus, synced like vanilla (state ids, remote slot hashes): chests and double chests, barrels,
  shulker boxes, ender chests, hoppers, dispensers/droppers, crafting table and 2x2 grid (all
  recipe types incl. the special ones), furnaces, stonecutter, grindstone, smithing table
  (upgrades, trims), anvil (repair, combine, rename), loom, enchanting table, brewing stand,
  lectern, beacon (paying and choosing effects), crafter (slot toggles, preview)
- Bundles in inventories (insert, take out, selected item); beds (sleeping, skipping the night,
  standing up where there's room); item/block interactions (stripping, tilling, waxing, lighting
  with flint and steel, carving pumpkins, harvesting berries, ...)
- `update_recipes` (property sets and stonecutter recipes), full registry data for clients
  without the vanilla pack, `region-file-compression=lz4`

**Milestone 5: light and heightmaps** ✅
- A port of vanilla's light engine (block and sky light, section storage, sky light sources):
  chunks are lit before they're sent, block changes relight incrementally, and light updates go
  to the players that have the chunk on the edge of their view, as in vanilla
- Light is saved (`SkyLight`/`BlockLight`, `isLightOn`) and saved light is trusted on load; the
  light clients get is identical to vanilla's, both for worlds built here and for worlds saved
  by the other server (`tools/test_light.sh`)
- All four heightmaps (world surface, ocean floor, motion blocking, without leaves), kept up to
  date, saved and loaded
- Light-dependent rules: crops and mushrooms need the right light, loot `light`/`can_see_sky`
  conditions; beacons check their beam and pyramid and give their effects
- `Level.clip` (concrete powder falling fast into water), free-position search for experience
  orbs spawned inside blocks; new entities are shown to players the moment they're added
- `/setblock` and `/fill` (replace mode)

**Random ticks, trees and game rules** ✅
- Game rules: all of vanilla's, saved in `data/minecraft/game_rules.dat`, `/gamerule`; the ported
  ones take effect (`random_tick_speed`, `block_drops`, `entity_drops`, `spread_vines`,
  `fire_spread_radius_around_player`, `advance_time`, ...), client-side ones are sent
- Random ticks in the chunks within simulation distance, like vanilla (scheduled ticks and block
  entities now also only run there): crops, stems and pumpkins/melons, sugar cane, cactus (and its
  flower), bamboo, kelp and nether/cave vines, vines, chorus flowers, sweet berries, cocoa, nether
  wart, mushrooms spreading, grass and mycelium spreading and dying, nylium, farmland drying out,
  leaves decaying, ice and snow melting, water freezing in cold biomes (biome temperatures with
  vanilla's noise), copper weathering, amethyst buds, dripstone growing and dripping into
  cauldrons, redstone ore going out, eyeblossoms opening and closing with the day, turtle eggs
  cracking, lava setting things on fire
- Trees: saplings and propagules grow into every vanilla tree (`TreeFeature` with its trunk,
  foliage and root placers and decorators: beehives with bees, vines, podzol, shelf mushrooms,
  hanging propagules, mangrove roots), 2x2 mega trees and huge mushrooms. For the same seed the
  blocks placed are identical to vanilla's (`tools/feature_check.sh` runs vanilla's own feature
  code against ours)
- Bone meal on crops, stems, saplings, azaleas, mushrooms, berries, cocoa, bamboo, kelp and vines,
  pitcher plants and mangrove leaves
- Neighbour shape updates follow vanilla's flags; cactus, sugar cane, farmland, leaves, bamboo and
  chorus flowers react a tick later as in vanilla; `/setblock` shapes the block to its neighbours

**Online mode and secure chat** ✅
- `online-mode=true`: the encrypted login (RSA-1024 key exchange, AES/CFB8 on the connection) and
  the session server check, run off the main thread like vanilla's authenticator threads; players
  get their account's id and skin; logging in twice kicks the older connection;
  `prevent-proxy-connections` passes the IP along. The services are found through the same
  discovery document authlib uses (`MINECRAFT_API_DISCOVERY_HOST` / `MINECRAFT_API_ENV` override it,
  like vanilla's `minecraft.api.*` properties)
- Secure chat: chat sessions (profile keys checked against Mojang's services keys), signed chat
  relayed as player chat with vanilla's message chain, last-seen acknowledgements and signature
  cache; signed `/say`; `enforce-secure-profile` refuses unsigned chat. Chat types (`<Alice> hi`,
  `[Alice] hi`), the `[Not Secure]` log tag, and players' display names with their click and hover
  events as in vanilla

Known gaps: bone meal on grass, moss, nylium, fungi and flowers (they place world-gen features);
turtle eggs don't hatch and dried ghasts stay hydrated (no mobs); no food/hunger, mobs,
explosions, portals, weather, maps (cartography table), fluids or redstone; Haste/Mining Fatigue
don't affect dig speed; the anvil compares a typed name with the item's name key, not its English
text.

**Next milestones**
1. Fluids, redstone
2. Nether/End, entities, AI, physics; noise world generation; Brigadier (with it, signed
   arguments for every command, /msg and /me); using items in the air (food, bows, buckets, spawn eggs)

Known shortcuts are marked `TODO` in the source.

## Build and run

Requirements: a C++23 compiler (GCC 14+/Clang 18+), CMake 3.25+, Python 3, zlib, OpenSSL.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
mkdir -p run && cd run
../build/cmc-server          # first run writes eula.txt; set eula=true and start again
```

It reads and writes `server.properties` like vanilla. Supported keys so far: `server-ip`,
`server-port`, `motd`, `max-players`, `view-distance`, `simulation-distance`,
`network-compression-threshold`, `gamemode`, `difficulty`, `hardcore`, `accepts-transfers`,
`level-name`, `level-seed`, `level-type`, `sync-chunk-writes`, `region-file-compression`,
`online-mode`, `enforce-secure-profile`, `prevent-proxy-connections`.

Worlds: new worlds are superflat (the only generator ported so far, so `level-type` defaults to
`minecraft:flat`). Existing vanilla worlds with normal terrain load fine; chunks that don't exist
yet are generated as superflat and only saved if you change them, so vanilla can still generate
the real terrain there later. The Nether and the End aren't loaded yet.

Connect a 26.3 client to `localhost:25565`. Run with `--debug` to log protocol details.

## Testing

`tools/mcprobe.py` is a headless protocol client. It logs in, goes through configuration,
enters play and reports what the server sent:

```sh
tools/mcprobe.py status 127.0.0.1 25565
tools/mcprobe.py join 127.0.0.1 25565 --name Alice --walk --chat "hi" --dump out.json
tools/smoke_test.sh        # builds nothing; runs build/cmc-server with two probe clients
tools/run_test.sh tools/test_building.py        # scripted building/inventory/time checks
tools/run_test.sh tools/test_drops.py           # mining, loot, item entities, pickup, throwing
tools/test_persistence.sh                       # build, restart, verify, compare with disk
tools/test_containers.sh                        # vanilla fills containers; both servers break them
tools/run_test.sh tools/test_menus.py           # chests, crafting, furnaces (also against vanilla)
tools/run_test.sh tools/test_workstations.py    # stonecutter ... crafter; compare the OBS lines
tools/run_test.sh tools/test_bundles.py         # bundles in the inventory
tools/test_light.sh                             # light and heightmaps vs vanilla, incl. saved worlds
tools/run_test.sh tools/test_random_ticks.py    # growth, decay, melting with random_tick_speed up
tools/run_test.sh tools/test_bone_meal.py       # trees, crops, huge mushrooms from bone meal
BUNDLE=<unzipped jar> tools/feature_check.sh    # trees/huge mushrooms vs vanilla's code, same seeds
tools/test_online.sh                            # online-mode logins, both servers, stand-in services
tools/test_chat.sh                              # secure chat with enforce-secure-profile, both servers
tools/nbtdump.py world/level.dat                # inspect NBT files and region chunks
BIN=build-asan/cmc-server tools/smoke_test.sh   # same, against a sanitizer build
```

Protocol correctness is checked against a vanilla server. The probe dumps can be diffed
(`--dump`): chunk (0,0) of the superflat world matches vanilla section for section. And
`tools/test_building.py HOST PORT` runs unchanged against vanilla (with the test players opped)
and passes the same checks, so its packet decoding matches both servers. World compatibility is
checked the same way: `test_persistence.py verify`/`compare` pass on a world saved here and then
opened by vanilla, and on a world built in vanilla and then opened here. `test_drops.py` also runs
against vanilla. `test_containers.sh` needs Java 25 (`JAVA=...`) and the jar: vanilla fills chests,
a barrel and shulker boxes with `/setblock`, then vanilla and this server each run the same
break → restart → place → check-the-saved-world steps on a copy of that world.

## Data files and updating to a new game version

`data/` holds facts extracted from the official jar. The build turns them into C++ tables:

- `packets.json`, `registries.json`, `blocks.json`: from the vanilla data generator (`--reports`)
- `vanilla_config.json`: synchronized registry order and tags, recorded from a vanilla server
- `extracted.json`: block classes, per-state flags (replaceable, collision, sturdy faces, fluids,
  light emission/dampening), light occlusion faces, hardness, sounds and item→block mapping, from
  `tools/extractor` run against the jar
- `item_components.json`: every item's default components (and a digest: max stack size, tool,
  equippable, fuel, ...) from the components report
- `loot_tables.json`, `recipes.json`, `enchantments.json`, `*_providers.json`, ...: data packs
  from the jar (loot, recipes, enchantment effects, context providers, jukebox songs, ...)

To move to a new version:

```sh
tools/refresh_data.py minecraft_server.X.jar --java /usr/lib/jvm/java-25-openjdk/bin/java
```

Then update `kProtocolVersion`/`kGameVersion` in `src/server/minecraft_server.hpp` and fix
whatever packet layouts changed. Diffing the probe dumps shows where.

## Legal

Minecraft is © Mojang AB. This project contains no Mojang code or assets. 
Use of the game is subject to the [Minecraft EULA](https://aka.ms/MinecraftEULA).
