#include "world/level/chunk/storage/chunk_serializer.hpp"

#include <set>

#include "generated/registries.hpp"
#include "core/registries.hpp"

namespace mc::chunk_serializer {
namespace {

const gen::RegistryInfo* biomeRegistry() {
    static const gen::RegistryInfo* reg = [] {
        for (const auto& r : gen::kSyncedRegistries)
            if (r.name == "minecraft:worldgen/biome") return &r;
        return static_cast<const gen::RegistryInfo*>(nullptr);
    }();
    return reg;
}

std::vector<uint64_t> toUnsigned(const std::vector<int64_t>& v) { return {v.begin(), v.end()}; }

nbt::Tag longArray(const std::vector<uint64_t>& v) { return std::vector<int64_t>(v.begin(), v.end()); }

} // namespace

nbt::Tag writeBlockState(BlockStateId state) {
    uint32_t block = blocks::blockOf(state);
    const auto& info = gen::kBlocks[block];
    if (state == info.default_state) return std::string(info.name);
    nbt::Compound props;
    for (const auto& p : info.properties) props.put(std::string(p.name), std::string(*blocks::getValue(state, p.name)));
    nbt::Compound c;
    c.put("id", std::string(info.name));
    c.put("properties", std::move(props));
    return c;
}

std::optional<BlockStateId> readBlockState(const nbt::Tag& tag) {
    std::string name;
    const nbt::Compound* props = nullptr;
    if (const std::string* s = tag.asString()) {
        name = *s;
    } else if (const nbt::Compound* c = tag.asCompound()) {
        name = c->getString("id", c->getString("Name"));
        props = c->getCompound("properties");
        if (!props) props = c->getCompound("Properties");
    } else {
        return std::nullopt;
    }
    auto block = blocks::byName(name);
    if (!block) return std::nullopt;
    BlockStateId state = blocks::defaultState(*block);
    if (props) {
        for (const auto& [k, v] : props->entries) {
            const std::string* value = v.asString();
            if (!value || !blocks::hasProperty(state, k)) return std::nullopt;
            BlockStateId next = blocks::setValue(state, k, *value);
            if (blocks::getValue(next, k) != *value) return std::nullopt; // unknown value
            state = next;
        }
    }
    return state;
}

LoadResult read(const nbt::Compound& tag, ChunkPos expected, const DimensionInfo& dim, uint32_t defaultBiome, int biomeCount) {
    LoadResult result;
    auto chunk = std::make_unique<LevelChunk>(expected, dim, defaultBiome, biomeCount);
    auto problem = [&](std::string why) {
        if (result.problem.empty()) result.problem = std::move(why);
    };

    int dataVersion = tag.getInt("DataVersion", -1);
    if (dataVersion != kDataVersion) {
        // We can't run DataFixerUpper, so anything else is read best-effort and never written back.
        problem("saved with data version " + std::to_string(dataVersion) + ", expected " + std::to_string(kDataVersion));
    }
    ChunkPos stored{tag.getInt("xPos", expected.x), tag.getInt("zPos", expected.z)};
    if (!(stored == expected)) problem("chunk is stored at the wrong position");
    chunk->status = tag.getString("Status", "minecraft:empty");
    chunk->inhabitedTime = tag.getLong("InhabitedTime");
    int minSection = dim.minY >> 4;

    chunk->storedLight.assign(static_cast<size_t>(dim.sectionCount() + 2), {});
    bool anyLight = false;
    const auto* biomes = biomeRegistry();
    if (const nbt::List* sections = tag.getList("sections")) {
        for (const auto& st : sections->items) {
            const nbt::Compound* sec = st.asCompound();
            if (!sec) continue;
            int y = sec->getInt("Y");
            int lightIndex = y - minSection + 1;
            if (lightIndex >= 0 && lightIndex < static_cast<int>(chunk->storedLight.size())) {
                if (auto* sky = sec->getByteArray("SkyLight")) chunk->storedLight[lightIndex].sky = *sky, anyLight = true;
                if (auto* bl = sec->getByteArray("BlockLight")) chunk->storedLight[lightIndex].block = *bl, anyLight = true;
            }
            int index = y - minSection;
            if (index < 0 || index >= dim.sectionCount()) continue;
            LevelChunkSection& section = chunk->section(index);

            if (const nbt::Compound* bs = sec->getCompound("block_states")) {
                std::vector<uint32_t> palette;
                if (const nbt::List* pal = bs->getList("palette")) {
                    for (const auto& e : pal->items) {
                        auto state = readBlockState(e);
                        if (!state) {
                            problem("unknown block state in section " + std::to_string(y));
                            state = blocks::defaultState(gen::block::AIR);
                        }
                        palette.push_back(*state);
                    }
                }
                std::vector<uint64_t> data;
                if (auto* d = bs->getLongArray("data")) data = toUnsigned(*d);
                if (palette.empty() || !section.states().unpack(palette, data)) problem("invalid block data in section " + std::to_string(y));
            }
            if (const nbt::Compound* bi = sec->getCompound("biomes")) {
                std::vector<uint32_t> palette;
                if (const nbt::List* pal = bi->getList("palette")) {
                    for (const auto& e : pal->items) {
                        const std::string* name = e.asString();
                        auto id = name ? registries::id("minecraft:worldgen/biome", *name) : std::nullopt;
                        if (!id) {
                            problem("unknown biome in section " + std::to_string(y));
                            id = static_cast<int>(defaultBiome);
                        }
                        palette.push_back(static_cast<uint32_t>(*id));
                    }
                }
                std::vector<uint64_t> data;
                if (auto* d = bi->getLongArray("data")) data = toUnsigned(*d);
                if (palette.empty() || !section.biomes().unpack(palette, data)) problem("invalid biome data in section " + std::to_string(y));
            }
            section.recalcBlockCounts();
        }
    }
    (void)biomes;
    if (!anyLight) chunk->storedLight.clear();
    // ChunkStatusTasks.isLighted: at least the LIGHT status, and the light marked correct.
    static const std::set<std::string> kLitStatuses = {"minecraft:light", "minecraft:spawn", "minecraft:full"};
    chunk->lightCorrect = tag.getBool("isLightOn") && kLitStatuses.contains(chunk->status);

    if (const nbt::List* bes = tag.getList("block_entities")) {
        for (const auto& t : bes->items) {
            const nbt::Compound* be = t.asCompound();
            if (!be) continue;
            int x = be->getInt("x"), y = be->getInt("y"), z = be->getInt("z");
            uint32_t block = blocks::blockOf(chunk->getBlockState(x & 15, y, z & 15));
            chunk->blockEntities.push_back({x, y, z, block, *be});
        }
    }
    // Saved heightmaps are used as they are; missing (or malformed) ones are primed.
    chunk->recalculateHeightmaps();
    if (const nbt::Compound* maps = tag.getCompound("Heightmaps")) {
        for (auto type : {HeightmapType::WorldSurface, HeightmapType::OceanFloor, HeightmapType::MotionBlocking, HeightmapType::MotionBlockingNoLeaves}) {
            if (const auto* data = maps->getLongArray(Heightmap::serializationKey(type))) chunk->heightmap(type).setRaw(*data, dim.height);
        }
    }
    chunk->original = tag;
    chunk->dirty = false;
    chunk->readOnly = !result.problem.empty();
    result.chunk = std::move(chunk);
    return result;
}

nbt::Compound write(const LevelChunk& chunk, int64_t gameTime, const light::LevelLightEngine* light) {
    // Start from the loaded tag so data we don't model (structures, ticks, post-processing,
    // blending data, ...) survives a round trip through this server.
    nbt::Compound tag = chunk.original.value_or(nbt::Compound{});
    const DimensionInfo& dim = chunk.dimension();
    int minSection = dim.minY >> 4;
    tag.put("DataVersion", kDataVersion);
    tag.put("xPos", chunk.pos().x);
    tag.put("yPos", minSection);
    tag.put("zPos", chunk.pos().z);
    tag.put("LastUpdate", gameTime);
    tag.put("InhabitedTime", chunk.inhabitedTime);
    tag.put("Status", chunk.status);

    const auto* biomes = biomeRegistry();
    nbt::List sections{nbt::TagType::Compound, {}};
    // SerializableChunkData.copyOf: every light section (one extra below and above the world), with
    // its blocks where it has them and the light data that isn't all zero.
    for (int sy = minSection - 1; sy <= minSection + chunk.sectionCount(); sy++) {
        int i = sy - minSection;
        nbt::Compound sec;
        if (i >= 0 && i < chunk.sectionCount()) {
            const LevelChunkSection& section = chunk.section(i);
            auto packedStates = section.states().pack();
            nbt::Compound states;
            nbt::List statePalette;
            for (uint32_t st : packedStates.palette) statePalette.items.push_back(writeBlockState(st));
            states.put("palette", std::move(statePalette));
            if (!packedStates.data.empty()) states.put("data", longArray(packedStates.data));
            sec.put("block_states", std::move(states));

            auto packedBiomes = section.biomes().pack();
            nbt::Compound bio;
            nbt::List biomePalette;
            for (uint32_t b : packedBiomes.palette)
                biomePalette.items.push_back(biomes && b < biomes->entries.size() ? std::string(biomes->entries[b]) : std::string("minecraft:plains"));
            bio.put("palette", std::move(biomePalette));
            if (!packedBiomes.data.empty()) bio.put("data", longArray(packedBiomes.data));
            sec.put("biomes", std::move(bio));
        }
        if (light) {
            for (auto [layer, key] : {std::pair{light::LightLayer::Block, "BlockLight"}, std::pair{light::LightLayer::Sky, "SkyLight"}}) {
                const light::DataLayer* data = light->dataLayerData(layer, chunk.pos().x, sy, chunk.pos().z);
                if (!data || data->isEmpty()) continue;
                light::DataLayer copy = *data;
                const auto& bytes = copy.data();
                sec.put(key, std::vector<int8_t>(reinterpret_cast<const int8_t*>(bytes.data()), reinterpret_cast<const int8_t*>(bytes.data()) + 2048));
            }
        }
        if (sec.empty()) continue;
        sec.put("Y", static_cast<int8_t>(sy));
        sections.items.push_back(std::move(sec));
    }
    tag.put("sections", std::move(sections));
    if (chunk.lightCorrect) tag.put("isLightOn", int8_t{1});
    else tag.remove("isLightOn");
    nbt::Compound heightmaps;
    for (auto type : {HeightmapType::WorldSurface, HeightmapType::OceanFloor, HeightmapType::MotionBlocking, HeightmapType::MotionBlockingNoLeaves})
        heightmaps.put(Heightmap::serializationKey(type), longArray(chunk.heightmap(type).raw(dim.height)));
    tag.put("Heightmaps", std::move(heightmaps));

    nbt::List blockEntities{nbt::TagType::Compound, {}};
    for (const auto& be : chunk.blockEntities)
        if (blocks::blockOf(chunk.getBlockState(be.x & 15, be.y, be.z & 15)) == be.blockAtLoad) blockEntities.items.push_back(be.tag);
    tag.put("block_entities", std::move(blockEntities));

    if (!tag.contains("structures")) {
        nbt::Compound structures;
        structures.put("References", nbt::Compound{});
        structures.put("starts", nbt::Compound{});
        tag.put("structures", std::move(structures));
    }
    if (!tag.contains("PostProcessing")) {
        nbt::List post;
        for (int i = 0; i < chunk.sectionCount(); i++) post.items.push_back(nbt::List{});
        tag.put("PostProcessing", std::move(post));
    }
    // SavedTick: {i: block, x, y, z, t: ticks until it triggers, p: priority}
    nbt::List blockTicks{nbt::TagType::Compound, {}};
    for (const auto& t : chunk.unportedBlockTicks) blockTicks.items.push_back(t);
    for (const auto& t : chunk.blockTicks) {
        nbt::Compound saved;
        saved.put("i", std::string(gen::kBlocks[t.block].name));
        saved.put("x", t.pos.x);
        saved.put("y", t.pos.y);
        saved.put("z", t.pos.z);
        saved.put("t", static_cast<int32_t>(t.triggerTick - gameTime));
        saved.put("p", t.priority);
        blockTicks.items.push_back(std::move(saved));
    }
    tag.put("block_ticks", std::move(blockTicks));
    if (!tag.contains("fluid_ticks")) tag.put("fluid_ticks", nbt::List{});
    return tag;
}

} // namespace mc::chunk_serializer
