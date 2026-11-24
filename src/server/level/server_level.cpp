#include "server/level/server_level.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "generated/registries.hpp"
#include "util/log.hpp"
#include "util/json.hpp"
#include "world/entity/experience_orb.hpp"
#include "world/entity/falling_block_entity.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/level/biome/biome_climate.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/block/entity/block_entity_ticker.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/chunk/storage/chunk_serializer.hpp"
#include "world/level/material/fluids.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc {

FlatLevelSource FlatLevelSource::classicFlat() {
    using namespace gen::block;
    return FlatLevelSource({{blocks::defaultState(BEDROCK), 1}, {blocks::defaultState(DIRT), 2}, {blocks::defaultState(GRASS_BLOCK), 1}},
                           static_cast<uint32_t>(registries::id("minecraft:worldgen/biome", "minecraft:plains").value_or(0)));
}

int FlatLevelSource::surfaceY(const DimensionInfo& dim) const {
    int y = dim.minY;
    for (const auto& l : layers_) y += l.height;
    return y;
}

void FlatLevelSource::fill(LevelChunk& chunk, const DimensionInfo& dim) const {
    int y = dim.minY;
    for (const auto& layer : layers_) {
        for (int i = 0; i < layer.height; i++, y++) {
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++) chunk.setBlockState(x, y, z, layer.state);
        }
    }
    chunk.recalculateHeightmaps();
}

void ServerLevel::playSound(int excludeEntity, double x, double y, double z, int32_t sound, int source, float volume, float pitch) {
    if (sound < 0) return;
    int64_t seed = static_cast<int64_t>((static_cast<uint64_t>(random_()) << 32) | random_());
    sounds_.push_back({sound, {}, source, x, y, z, volume, pitch, seed, excludeEntity});
}

void ServerLevel::playDirectSound(double x, double y, double z, std::string sound, int source, float volume, float pitch) {
    int64_t seed = static_cast<int64_t>((static_cast<uint64_t>(random_()) << 32) | random_());
    sounds_.push_back({-1, std::move(sound), source, x, y, z, volume, pitch, seed, -1});
}

std::vector<ServerLevel::BlockEvent> ServerLevel::runBlockEvents() {
    std::vector<BlockEvent> out;
    // Events queued while running (none of ours do) wait for the next tick.
    for (const auto& ev : std::exchange(blockEvents_, {})) {
        BlockStateId state = getBlockState(ev.pos);
        if (blocks::blockOf(state) != ev.block) continue; // ServerLevel.doBlockEvent
        if (block_behaviour::triggerEvent(*this, ev.pos, state, ev.b0, ev.b1)) out.push_back(ev);
    }
    return out;
}

uint32_t ServerLevel::getNoiseBiome(int qx, int qy, int qz) {
    // LevelChunk.getNoiseBiome: the section's biome, with y clamped to the level.
    LevelChunk& chunk = getChunk({qx >> 2, qz >> 2});
    int minQ = minY() >> 2, maxQ = (maxY() >> 2);
    int cq = std::clamp(qy, minQ, maxQ) - minQ;
    return chunk.section(cq >> 2).getBiome(qx & 3, cq & 3, qz & 3);
}

uint32_t ServerLevel::getBiome(BlockPos pos) {
    // BiomeManager.getBiome: pick the nearest of the 8 surrounding quart corners, jittered by a
    // hash of the corner and the obfuscated seed.
    const int64_t seed = obfuscatedSeed();
    auto next = [](int64_t r, int64_t c) {
        uint64_t u = static_cast<uint64_t>(r);
        u *= u * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<int64_t>(u + static_cast<uint64_t>(c));
    };
    auto fiddle = [](int64_t r) {
        int64_t m = (r >> 24) % 1024;
        if (m < 0) m += 1024; // Math.floorMod
        return (static_cast<double>(m) / 1024.0 - 0.5) * 0.9;
    };
    int ax = pos.x - 2, ay = pos.y - 2, az = pos.z - 2;
    int px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
    int best = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 8; i++) {
        bool xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        int cx = xe ? px : px + 1, cy = ye ? py : py + 1, cz = ze ? pz : pz + 1;
        double dx = xe ? fx : fx - 1.0, dy = ye ? fy : fy - 1.0, dz = ze ? fz : fz - 1.0;
        int64_t r = next(seed, cx);
        r = next(r, cy);
        r = next(r, cz);
        r = next(r, cx);
        r = next(r, cy);
        r = next(r, cz);
        double ox = fiddle(r);
        r = next(r, seed);
        double oy = fiddle(r);
        r = next(r, seed);
        double oz = fiddle(r);
        double d = (dz + oz) * (dz + oz) + (dy + oy) * (dy + oy) + (dx + ox) * (dx + ox);
        if (bestDist > d) best = i, bestDist = d;
    }
    return getNoiseBiome((best & 4) == 0 ? px : px + 1, (best & 2) == 0 ? py : py + 1, (best & 1) == 0 ? pz : pz + 1);
}

namespace {
// BiomeManager.obfuscateSeed: Hashing.sha256().hashLong(seed).asLong(), i.e. SHA-256 of the
// little-endian seed, first 8 bytes read little-endian.
int64_t obfuscateSeed(int64_t seed_) {
    uint8_t in[8], digest[SHA256_DIGEST_LENGTH];
    for (int i = 0; i < 8; i++) in[i] = static_cast<uint8_t>(static_cast<uint64_t>(seed_) >> (8 * i));
    SHA256(in, sizeof in, digest);
    uint64_t out = 0;
    for (int i = 7; i >= 0; i--) out = (out << 8) | digest[i];
    return static_cast<int64_t>(out);
}
} // namespace

// The level as the light engine sees it (LightChunkGetter): loaded chunks only.
class ServerLevel::LightChunks : public light::LightChunkGetter {
public:
    explicit LightChunks(ServerLevel& level) : level_(level) {}
    bool hasChunk(int cx, int cz) const override { return level_.getChunkIfLoaded({cx, cz}) != nullptr; }
    BlockStateId blockState(int x, int y, int z) const override {
        LevelChunk* chunk = level_.getChunkIfLoaded({x >> 4, z >> 4});
        return chunk ? chunk->getBlockState(x & 15, y, z & 15) : blocks::defaultState(gen::block::BEDROCK);
    }
    const light::ChunkSkyLightSources* skySources(int cx, int cz) const override {
        LevelChunk* chunk = level_.getChunkIfLoaded({cx, cz});
        return chunk ? &chunk->skyLightSources : nullptr;
    }
    void findBlockLightSources(int cx, int cz, const std::function<void(int, int, int, BlockStateId)>& fn) const override {
        LevelChunk* chunk = level_.getChunkIfLoaded({cx, cz});
        if (!chunk) return;
        for (int i = 0; i < chunk->sectionCount(); i++) {
            const LevelChunkSection& section = chunk->section(i);
            if (section.hasOnlyAir()) continue;
            if (!section.states().maybeHas([](uint32_t st) { return blocks::lightEmission(st) != 0; })) continue;
            int baseY = level_.minY() + i * 16;
            for (int y = 0; y < 16; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        BlockStateId st = section.getBlockState(x, y, z);
                        if (blocks::lightEmission(st) != 0) fn(cx * 16 + x, baseY + y, cz * 16 + z, st);
                    }
        }
    }
    void onLightUpdate(light::LightLayer layer, int64_t section) override {
        // ChunkHolder.sectionLightChanged
        ChunkPos pos{light::sectionX(section), light::sectionZ(section)};
        if (!level_.getChunkIfLoaded(pos)) return;
        int index = light::sectionY(section) - level_.light_->minLightSection();
        if (index < 0 || index > level_.light_->lightSectionCount()) return;
        auto& changes = level_.lightChanges_[pos.pack()];
        changes.pos = pos;
        auto& bits = layer == light::LightLayer::Sky ? changes.sky : changes.block;
        if (bits.size() <= static_cast<size_t>(index / 64)) bits.resize(index / 64 + 1, 0);
        bits[index / 64] |= 1ull << (index % 64);
    }
    int minY() const override { return level_.minY(); }
    int sectionCount() const override { return level_.info().sectionCount(); }

private:
    ServerLevel& level_;
};

namespace {
// LevelChunk as a column for ChunkSkyLightSources.
struct ChunkColumn : light::ChunkSkyLightSources::Column {
    explicit ChunkColumn(const LevelChunk& chunk) : chunk(chunk) {}
    const LevelChunk& chunk;
    BlockStateId blockState(int x, int y, int z) const override {
        int rel = y - chunk.dimension().minY;
        if (rel < 0 || rel >= chunk.dimension().height) return blocks::defaultState(gen::block::AIR);
        return chunk.getBlockState(x, y, z);
    }
    int highestFilledSectionIndex() const override { return chunk.highestFilledSectionIndex(); }
    bool sectionEmpty(int index) const override { return chunk.section(index).hasOnlyAir(); }
    int minY() const override { return chunk.dimension().minY; }
};
} // namespace

ServerLevel::~ServerLevel() = default;

ServerLevel::ServerLevel(std::string dimension, std::string dimensionType, DimensionInfo info, FlatLevelSource generator,
                         std::unique_ptr<RegionFileStorage> storage, std::unique_ptr<RegionFileStorage> entityStorage,
                         bool persistGenerated, int64_t seed, std::function<int()> entityIds)
    : dimension_(std::move(dimension)), dimensionType_(std::move(dimensionType)), info_(info), generator_(std::move(generator)),
      seed_(seed), storage_(std::move(storage)), entityStorage_(std::move(entityStorage)), persistGenerated_(persistGenerated),
      entityIds_(std::move(entityIds)) {
    biomeCount_ = registries::size("minecraft:worldgen/biome");
    obfuscatedSeed_ = obfuscateSeed(seed);
    spawn_ = {0, generator_.surfaceY(info_), 0};
    lightChunks_ = std::make_unique<LightChunks>(*this);
    light_ = std::make_unique<light::LevelLightEngine>(*lightChunks_, dimensionType_ != "minecraft:the_nether" && dimensionType_ != "minecraft:the_end");
    // The dimension type's gameplay attributes (no biome overrides them).
    json::Value elements = json::parse(gen::kRegistryElementsJson);
    const json::Value& attributes = elements["minecraft:dimension_type"][dimensionType_]["attributes"];
    fastLava_ = attributes["minecraft:gameplay/fast_lava"].isBool() && attributes["minecraft:gameplay/fast_lava"].boolean();
    waterEvaporates_ = attributes["minecraft:gameplay/water_evaporates"].isBool() && attributes["minecraft:gameplay/water_evaporates"].boolean();
}

LevelChunk& ServerLevel::getChunk(ChunkPos pos) {
    auto& slot = chunks_[pos.pack()];
    if (slot) return *slot;
    slot = loadOrGenerate(pos);
    LevelChunk& chunk = *slot;
    loadEntities(chunk);
    unpackBlockTicks(chunk);
    initializeLight(chunk);
    return chunk;
}

// SerializableChunkData.read (queueing the saved light) and ChunkStatusTasks.initializeLight.
void ServerLevel::initializeLight(LevelChunk& chunk) {
    ChunkPos pos = chunk.pos();
    chunk.skyLightSources.fillFrom(ChunkColumn(chunk)); // ChunkAccess.initializeLightSources
    if (!chunk.storedLight.empty()) {
        light_->retainData(pos.x, pos.z, true);
        int minLight = light_->minLightSection();
        for (size_t i = 0; i < chunk.storedLight.size(); i++) {
            auto layer = [](const std::optional<std::vector<int8_t>>& bytes) -> std::optional<light::DataLayer> {
                if (!bytes || bytes->size() != 2048) return std::nullopt;
                return light::DataLayer(std::vector<uint8_t>(bytes->begin(), bytes->end()));
            };
            if (auto b = layer(chunk.storedLight[i].block)) light_->queueSectionData(light::LightLayer::Block, pos.x, minLight + static_cast<int>(i), pos.z, std::move(b));
            if (auto sky = layer(chunk.storedLight[i].sky)) light_->queueSectionData(light::LightLayer::Sky, pos.x, minLight + static_cast<int>(i), pos.z, std::move(sky));
        }
        chunk.storedLight.clear();
    }
    std::vector<bool> empty(chunk.sectionCount());
    for (int i = 0; i < chunk.sectionCount(); i++) empty[i] = chunk.section(i).hasOnlyAir();
    light_->initializeLight(pos.x, pos.z, empty, chunk.lightCorrect);
}

LevelChunk& ServerLevel::getLitChunk(ChunkPos pos) {
    LevelChunk& chunk = getChunk(pos);
    if (chunk.lit) return chunk;
    if (!chunk.lightCorrect) // ChunkStatus.LIGHT needs its neighbours at INITIALIZE_LIGHT
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++)
                if (dx != 0 || dz != 0) getChunk({pos.x + dx, pos.z + dz});
    LevelChunk& lit = *chunks_.at(pos.pack()); // (the reference stays valid: chunks are heap-allocated)
    light_->lightChunk(pos.x, pos.z, lit.lightCorrect);
    lit.lightCorrect = true;
    lit.lit = true;
    return lit;
}

int ServerLevel::brightness(light::LightLayer layer, BlockPos pos) {
    getLitChunk({pos.x >> 4, pos.z >> 4});
    return light_->brightness(layer, pos);
}

int ServerLevel::rawBrightness(BlockPos pos, int skyDarken) {
    getLitChunk({pos.x >> 4, pos.z >> 4});
    return light_->rawBrightness(pos, skyDarken);
}

void ServerLevel::runLightUpdates() {
    if (light_->hasLightWork()) light_->runLightUpdates();
}

std::vector<ServerLevel::LightChanges> ServerLevel::takeLightChanges() {
    std::vector<LightChanges> out;
    out.reserve(lightChanges_.size());
    for (auto& [_, c] : lightChanges_) out.push_back(std::move(c));
    lightChanges_.clear();
    return out;
}

void ServerLevel::writeLightData(ByteWriter& out, ChunkPos pos, const std::vector<uint64_t>* skyFilter, const std::vector<uint64_t>* blockFilter) {
    int count = light_->lightSectionCount();
    size_t words = static_cast<size_t>((count + 63) / 64);
    std::vector<uint64_t> masks[4];
    for (auto& m : masks) m.assign(words, 0);
    std::vector<std::array<uint8_t, 2048>> sky, block;
    auto selected = [](const std::vector<uint64_t>* filter, int i) {
        return !filter || (static_cast<size_t>(i / 64) < filter->size() && ((*filter)[i / 64] >> (i % 64) & 1));
    };
    auto prepare = [&](light::LightLayer layer, int i, std::vector<uint64_t>& mask, std::vector<uint64_t>& empty, std::vector<std::array<uint8_t, 2048>>& list) {
        const light::DataLayer* data = light_->dataLayerData(layer, pos.x, light_->minLightSection() + i, pos.z);
        if (!data) return;
        if (data->isEmpty()) {
            empty[i / 64] |= 1ull << (i % 64);
        } else {
            mask[i / 64] |= 1ull << (i % 64);
            light::DataLayer copy = *data;
            list.push_back(copy.data());
        }
    };
    for (int i = 0; i < count; i++) {
        if (selected(skyFilter, i)) prepare(light::LightLayer::Sky, i, masks[0], masks[2], sky);
        if (selected(blockFilter, i)) prepare(light::LightLayer::Block, i, masks[1], masks[3], block);
    }
    // BitSet.toLongArray: without trailing zero words
    for (auto& m : masks) {
        while (!m.empty() && m.back() == 0) m.pop_back();
        out.writeBitSet(m);
    }
    out.writeVarInt(static_cast<int32_t>(sky.size()));
    for (const auto& l : sky) out.writeByteArray(l);
    out.writeVarInt(static_cast<int32_t>(block.size()));
    for (const auto& l : block) out.writeByteArray(l);
}

void ServerLevel::unpackBlockTicks(LevelChunk& chunk) {
    // SerializableChunkData: saved ticks become scheduled ones, relative to the current time.
    if (const nbt::List* fluidTicks = chunk.original ? chunk.original->getList("fluid_ticks") : nullptr) {
        for (const auto& e : fluidTicks->items) {
            const nbt::Compound* t = e.asCompound();
            auto fluid = t ? fluids::byName(t->getString("i")) : std::nullopt;
            if (!fluid || *fluid == fluids::kEmpty) continue;
            chunk.fluidTicks.push_back({*fluid, {t->getInt("x"), t->getInt("y"), t->getInt("z")}, gameTime_ + t->getInt("t"), t->getInt("p"), subTickCount_++});
        }
    }
    const nbt::List* saved = chunk.original ? chunk.original->getList("block_ticks") : nullptr;
    if (!saved) return;
    for (const auto& e : saved->items) {
        const nbt::Compound* t = e.asCompound();
        if (!t) continue;
        std::string name = t->getString("i");
        if (name.find(':') == std::string::npos) name = "minecraft:" + name;
        auto block = blocks::byName(name);
        if (!block || !block_behaviour::hasTick(*block)) {
            chunk.unportedBlockTicks.push_back(*t);
            continue;
        }
        chunk.blockTicks.push_back({*block, {t->getInt("x"), t->getInt("y"), t->getInt("z")}, gameTime_ + t->getInt("t"), t->getInt("p"),
                                    subTickCount_++});
    }
}

bool ServerLevel::hasScheduledTick(BlockPos pos, uint32_t block) {
    LevelChunk* chunk = getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    if (!chunk) return false;
    for (const auto& t : chunk->blockTicks)
        if (t.pos == pos && t.block == block) return true;
    return false;
}

void ServerLevel::scheduleTick(BlockPos pos, uint32_t block, int delay, int priority) {
    if (pos.y < minY() || pos.y > maxY() || hasScheduledTick(pos, block)) return;
    LevelChunk& chunk = getChunk({pos.x >> 4, pos.z >> 4});
    chunk.blockTicks.push_back({block, pos, gameTime_ + delay, priority, subTickCount_++});
    chunk.dirty = true;
}

void ServerLevel::tickBlockEntities() {
    for (auto& [_, chunk] : chunks_) {
        if (!inBlockTickingRange(chunk->pos())) continue; // Level.shouldTickBlocksAt
        for (size_t i = 0; i < chunk->blockEntities.size(); i++) { // (a ticker may add block entities)
            auto& be = chunk->blockEntities[i];
            if (!block_entity::hasTicker(be.tag.getString("id"))) continue;
            BlockPos pos{be.x, be.y, be.z};
            BlockStateId state = chunk->getBlockState(be.x & 15, be.y, be.z & 15);
            if (blocks::blockOf(state) != be.blockAtLoad) continue;
            // (tickers must not add block entities to this chunk: `be` would move)
            if (block_entity::tick(*this, pos, state, be.tag)) chunk->dirty = true;
        }
    }
}

bool ServerLevel::hasScheduledFluidTick(BlockPos pos, uint8_t fluid) {
    LevelChunk* chunk = getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    if (!chunk) return false;
    for (const auto& t : chunk->fluidTicks)
        if (t.pos == pos && t.block == fluid) return true;
    return false;
}

void ServerLevel::scheduleFluidTick(BlockPos pos, uint8_t fluid, int delay, int priority) {
    if (pos.y < minY() || pos.y > maxY() || fluid == fluids::kEmpty || hasScheduledFluidTick(pos, fluid)) return;
    LevelChunk& chunk = getChunk({pos.x >> 4, pos.z >> 4});
    chunk.fluidTicks.push_back({fluid, pos, gameTime_ + delay, priority, subTickCount_++});
    chunk.dirty = true;
}

template <class Run>
void ServerLevel::runTicks(std::vector<LevelChunk::ScheduledTick> LevelChunk::*member, Run run) {
    std::vector<LevelChunk::ScheduledTick> due;
    for (auto& [_, chunk] : chunks_) {
        if (!inEntityTickingRange(chunk->pos())) continue; // LevelTicks' isPositionTickingWithEntitiesLoaded
        auto& ticks = (*chunk).*member;
        auto split = std::stable_partition(ticks.begin(), ticks.end(), [&](const auto& t) { return t.triggerTick > gameTime_; });
        if (split == ticks.end()) continue;
        due.insert(due.end(), split, ticks.end());
        ticks.erase(split, ticks.end());
        chunk->dirty = true;
    }
    std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { // ScheduledTick.DRAIN_ORDER
        if (a.triggerTick != b.triggerTick) return a.triggerTick < b.triggerTick;
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.subTickOrder < b.subTickOrder;
    });
    if (due.size() > 65536) { // LevelTicks' per-tick limit: the rest wait for the next tick
        for (auto it = due.begin() + 65536; it != due.end(); ++it) (getChunk({it->pos.x >> 4, it->pos.z >> 4}).*member).push_back(*it);
        due.resize(65536);
    }
    for (const auto& t : due) run(t);
}

void ServerLevel::tickBlocks() {
    runTicks(&LevelChunk::blockTicks, [&](const LevelChunk::ScheduledTick& t) {
        BlockStateId state = getBlockState(t.pos);
        if (blocks::blockOf(state) == t.block) block_behaviour::tick(*this, t.pos, state); // ServerLevel.tickBlock
    });
}

void ServerLevel::tickFluids() {
    runTicks(&LevelChunk::fluidTicks, [&](const LevelChunk::ScheduledTick& t) {
        BlockStateId state = getBlockState(t.pos); // ServerLevel.tickFluid
        fluids::FluidState fluid = fluids::stateOf(state);
        if (fluid.type == t.block) fluids::tick(*this, t.pos, state, fluid);
    });
}

void ServerLevel::tickChunks() {
    int tickSpeed = gameRules().getInt(GameRules::RandomTickSpeed);
    std::vector<LevelChunk*> ticking; // (ticking may load chunks, which would invalidate the map's iterators)
    for (auto& [_, chunk] : chunks_) {
        int distance = playerChunkDistance(chunk->pos());
        if (distance > simulationDistance) continue; // forEachEntityTickingChunk
        if (distance <= 8) chunk->inhabitedTime++;   // tickSpawningChunk: LevelChunk.incrementInhabitedTime
        ticking.push_back(chunk.get());
    }
    for (LevelChunk* chunk : ticking) tickChunk(*chunk, tickSpeed);
}

void ServerLevel::tickChunk(LevelChunk& chunk, int tickSpeed) {
    // ServerLevel.tickChunk
    int minX = chunk.pos().x * 16, minZ = chunk.pos().z * 16;
    for (int i = 0; i < tickSpeed; i++)
        if (levelRandom_.nextInt(48) == 0) tickPrecipitation(blockRandomPos(minX, 0, minZ, 15));
    if (tickSpeed <= 0) return;
    for (int index = 0; index < chunk.sectionCount(); index++) {
        const LevelChunkSection& section = chunk.section(index);
        // LevelChunkSection.isRandomlyTicking (vanilla counts these states as they're set)
        if (!section.states().maybeHas([](uint32_t st) { return (blocks::flags(st) & (blocks::kRandomlyTicking | blocks::kFluidRandomlyTicking)) != 0; }))
            continue;
        int sectionMinY = minY() + index * 16;
        for (int i = 0; i < tickSpeed; i++) {
            BlockPos pos = blockRandomPos(minX, sectionMinY, minZ, 15);
            BlockStateId state = chunk.section(index).getBlockState(pos.x - minX, pos.y - sectionMinY, pos.z - minZ);
            uint32_t f = blocks::flags(state);
            if (f & blocks::kRandomlyTicking) block_behaviour::randomTick(*this, pos, state, levelRandom_);
            if (f & blocks::kFluidRandomlyTicking) block_behaviour::fluidRandomTick(*this, pos, state, levelRandom_);
        }
    }
}

void ServerLevel::tickPrecipitation(BlockPos pos) {
    // ServerLevel.tickPrecipitation: water at the surface freezes in cold biomes. TODO: snow layers
    // and filling cauldrons while it rains (weather).
    LevelChunk& chunk = getChunk({pos.x >> 4, pos.z >> 4});
    BlockPos top{pos.x, chunk.height(HeightmapType::MotionBlocking, pos.x & 15, pos.z & 15), pos.z};
    BlockPos below = top.below();
    if (biome::shouldFreeze(*this, getBiome(top), below)) setBlock(below, blocks::defaultState(gen::block::ICE));
}

std::unique_ptr<LevelChunk> ServerLevel::loadOrGenerate(ChunkPos pos) {
    std::unique_ptr<LevelChunk> slot;
    if (storage_) {
        try {
            if (auto tag = storage_->read(pos.x, pos.z)) {
                auto result = chunk_serializer::read(*tag, pos, info_, generator_.biome(), biomeCount_);
                if (!result.problem.empty())
                    log::warn("Chunk [{}, {}]: {}; loading it read-only (changes to it won't be saved)", pos.x, pos.z, result.problem);
                // Chunks vanilla hasn't finished generating: keep what's there. TODO: world generation stages.
                return std::move(result.chunk);
            }
        } catch (const std::exception& e) {
            log::error("Couldn't load chunk [{}, {}]: {}. It will not be overwritten.", pos.x, pos.z, e.what());
            slot = std::make_unique<LevelChunk>(pos, info_, generator_.biome(), biomeCount_);
            generator_.fill(*slot, info_);
            slot->readOnly = true;
            slot->dirty = false;
            return slot;
        }
    }
    slot = std::make_unique<LevelChunk>(pos, info_, generator_.biome(), biomeCount_);
    generator_.fill(*slot, info_);
    slot->dirty = persistGenerated_;
    return slot;
}

void ServerLevel::loadEntities(LevelChunk& chunk) {
    if (!entityStorage_) return;
    std::optional<nbt::Compound> tag;
    try {
        tag = entityStorage_->read(chunk.pos().x, chunk.pos().z);
    } catch (const std::exception& e) {
        log::error("Couldn't load entities of chunk [{}, {}]: {}", chunk.pos().x, chunk.pos().z, e.what());
        chunk.readOnly = true; // don't overwrite the damaged entity data either
        return;
    }
    if (!tag) return;
    chunk.hadEntityData = true;
    if (const nbt::List* list = tag->getList("Entities")) {
        for (const auto& e : list->items) {
            const nbt::Compound* c = e.asCompound();
            if (!c) continue;
            if (tag->getInt("DataVersion") == kDataVersion) {
                std::string type = c->getString("id");
                std::unique_ptr<Entity> entity;
                if (type == "minecraft:item") entity = ItemEntity::load(nextEntityId(), *c);
                else if (type == "minecraft:experience_orb") entity = ExperienceOrb::load(nextEntityId(), *c);
                else if (type == "minecraft:falling_block") entity = FallingBlockEntity::load(nextEntityId(), *c);
                if (entity) {
                    entities_.push_back(std::move(entity));
                    continue;
                }
            }
            chunk.otherEntities.push_back(*c); // TODO: simulate other entity types
        }
    }
}

void ServerLevel::saveEntities(LevelChunk& chunk) {
    if (!entityStorage_ || !chunk.entitiesDirty || chunk.readOnly) return;
    nbt::List list{nbt::TagType::Compound, {}};
    for (const auto& e : chunk.otherEntities) list.items.push_back(e);
    for (const auto& e : entities_)
        if (!e->removed && e->chunkPos() == chunk.pos()) list.items.push_back(e->save());
    if (list.items.empty() && !chunk.hadEntityData) {
        chunk.entitiesDirty = false;
        return;
    }
    nbt::Compound tag;
    tag.put("DataVersion", kDataVersion);
    tag.put("Position", std::vector<int32_t>{chunk.pos().x, chunk.pos().z});
    tag.put("Entities", std::move(list));
    if (entityStorage_->write(chunk.pos().x, chunk.pos().z, tag)) {
        chunk.entitiesDirty = false;
        chunk.hadEntityData = true;
    }
}

void ServerLevel::unloadEntities(LevelChunk& chunk) {
    for (auto& e : entities_) {
        if (!e->removed && e->chunkPos() == chunk.pos()) {
            e->removed = true;
            removedEntities_.push_back(e->id());
        }
    }
    std::erase_if(entities_, [](const auto& e) { return e->removed; });
}

void ServerLevel::addEntity(std::unique_ptr<Entity> entity) {
    if (LevelChunk* c = getChunkIfLoaded(entity->chunkPos())) c->entitiesDirty = true;
    entities_.push_back(std::move(entity));
    if (onEntityAdded) onEntityAdded(*entities_.back());
}

void ServerLevel::tickEntities() {
    for (size_t n = 0; n < entities_.size(); n++) { // index loop: ticking may spawn/merge, never erase
        Entity& e = *entities_[n];
        if (e.removed) continue;
        ChunkPos before = e.chunkPos();
        LevelChunk* chunk = getChunkIfLoaded(before);
        if (!chunk) continue; // entities only tick in loaded chunks
        e.tickCount++;        // (Level.tickNonPassenger, before Entity.tick)
        e.tick(*this);
        chunk->entitiesDirty = true;
        if (!(e.chunkPos() == before)) {
            if (LevelChunk* now = getChunkIfLoaded(e.chunkPos())) now->entitiesDirty = true;
        }
    }
    for (auto& e : entities_) {
        if (!e->removed) continue;
        removedEntities_.push_back(e->id());
        if (LevelChunk* c = getChunkIfLoaded(e->chunkPos())) c->entitiesDirty = true;
    }
    std::erase_if(entities_, [](const auto& e) { return e->removed; });
}

namespace {
// Direction.getApproximateNearest: the direction closest to (x, y, z) (the first one on ties).
Direction approximateNearest(double x, double y, double z) {
    Direction best = Direction::North;
    double bestDot = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 6; i++) {
        auto d = static_cast<Direction>(i);
        double dot = x * direction::stepX(d) + y * direction::stepY(d) + z * direction::stepZ(d);
        if (dot > bestDot) best = d, bestDot = dot;
    }
    return best;
}

// VoxelShape.clip for a shape made of `boxes` at `pos`: AABB.clip over the boxes (the face entered
// first), or the starting point when it's inside the shape.
template <class Boxes>
std::optional<ServerLevel::BlockHit> clipShape(const Boxes& boxes, BlockPos pos, double fx, double fy, double fz, double tx, double ty, double tz) {
    if (boxes.empty()) return std::nullopt;
    double dx = tx - fx, dy = ty - fy, dz = tz - fz;
    if (dx * dx + dy * dy + dz * dz < 1.0e-7) return std::nullopt;
    double px = fx + dx * 0.001 - pos.x, py = fy + dy * 0.001 - pos.y, pz = fz + dz * 0.001 - pos.z;
    for (const auto& b : boxes)
        if (px >= b.minX && px < b.maxX && py >= b.minY && py < b.maxY && pz >= b.minZ && pz < b.maxZ)
            return ServerLevel::BlockHit{pos, direction::opposite(approximateNearest(dx, dy, dz)), fx + dx * 0.001, fy + dy * 0.001, fz + dz * 0.001, true};
    // AABB.clip(boxes, from, to, pos) with getDirection / clipPoint
    double scale = 1.0;
    std::optional<Direction> dir;
    auto clipPoint = [&](double da, double db, double dc, double point, double minB, double maxB, double minC, double maxC, Direction face, double a, double b,
                         double c) {
        double t = (point - a) / da;
        double pb = b + t * db, pc = c + t * dc;
        if (0.0 < t && t < scale && minB - 1.0e-7 < pb && pb < maxB + 1.0e-7 && minC - 1.0e-7 < pc && pc < maxC + 1.0e-7) scale = t, dir = face;
    };
    for (const auto& b : boxes) {
        double x0 = pos.x + b.minX, y0 = pos.y + b.minY, z0 = pos.z + b.minZ, x1 = pos.x + b.maxX, y1 = pos.y + b.maxY, z1 = pos.z + b.maxZ;
        if (dx > 1.0e-7) clipPoint(dx, dy, dz, x0, y0, y1, z0, z1, Direction::West, fx, fy, fz);
        else if (dx < -1.0e-7) clipPoint(dx, dy, dz, x1, y0, y1, z0, z1, Direction::East, fx, fy, fz);
        if (dy > 1.0e-7) clipPoint(dy, dz, dx, y0, z0, z1, x0, x1, Direction::Down, fy, fz, fx);
        else if (dy < -1.0e-7) clipPoint(dy, dz, dx, y1, z0, z1, x0, x1, Direction::Up, fy, fz, fx);
        if (dz > 1.0e-7) clipPoint(dz, dx, dy, z0, x0, x1, y0, y1, Direction::North, fz, fx, fy);
        else if (dz < -1.0e-7) clipPoint(dz, dx, dy, z1, x0, x1, y0, y1, Direction::South, fz, fx, fy);
    }
    if (!dir) return std::nullopt;
    return ServerLevel::BlockHit{pos, *dir, fx + scale * dx, fy + scale * dy, fz + scale * dz, false};
}
} // namespace

std::optional<ServerLevel::BlockHit> ServerLevel::clip(double fx, double fy, double fz, double tx, double ty, double tz, ClipBlock blockMode,
                                                       ClipFluid fluidMode) {
    if (fx == tx && fy == ty && fz == tz) return std::nullopt;
    // Level.clip's per-block test: the block's shape and the fluid's, whichever is hit nearer.
    auto hitAt = [&](int bx, int by, int bz) -> std::optional<BlockHit> {
        if (by < minY() || by > maxY()) return std::nullopt;
        LevelChunk* chunk = getChunkIfLoaded({bx >> 4, bz >> 4});
        if (!chunk) return std::nullopt;
        BlockPos pos{bx, by, bz};
        BlockStateId s = chunk->getBlockState(bx & 15, by, bz & 15);
        auto shape = blockMode == ClipBlock::Outline ? gen::kOutlineShapes[gen::kStateOutline[s]] : gen::kShapes[gen::kStateShape[s]];
        auto blockHit = clipShape(shape, pos, fx, fy, fz, tx, ty, tz); // TODO: interaction shapes (clipWithInteractionOverride)
        std::optional<BlockHit> fluidHit;
        fluids::FluidState fluid = fluids::stateOf(s);
        if (fluidMode == ClipFluid::Any ? !fluid.isEmpty() : fluidMode == ClipFluid::SourceOnly && fluid.isSource()) {
            // FlowingFluid.getShape: as high as the fluid (cached per fluid state, so by its own height)
            gen::Box box{0, 0, 0, 1, fluids::ownHeight(fluid), 1};
            fluidHit = clipShape(std::span<const gen::Box>(&box, 1), pos, fx, fy, fz, tx, ty, tz);
        }
        auto dist = [&](const std::optional<BlockHit>& h) {
            return h ? (h->x - fx) * (h->x - fx) + (h->y - fy) * (h->y - fy) + (h->z - fz) * (h->z - fz) : std::numeric_limits<double>::max();
        };
        return dist(blockHit) <= dist(fluidHit) ? blockHit : fluidHit;
    };
    // BlockGetter.traverseBlocks
    auto lerp = [](double t, double a, double b) { return a + t * (b - a); };
    double toX = lerp(-1.0E-7, tx, fx), toY = lerp(-1.0E-7, ty, fy), toZ = lerp(-1.0E-7, tz, fz);
    double fromX = lerp(-1.0E-7, fx, tx), fromY = lerp(-1.0E-7, fy, ty), fromZ = lerp(-1.0E-7, fz, tz);
    int x = static_cast<int>(std::floor(fromX)), y = static_cast<int>(std::floor(fromY)), z = static_cast<int>(std::floor(fromZ));
    if (auto hit = hitAt(x, y, z)) return hit;
    double dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
    int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0, sz = dz > 0 ? 1 : dz < 0 ? -1 : 0;
    double tDX = sx == 0 ? std::numeric_limits<double>::max() : sx / dx, tDY = sy == 0 ? std::numeric_limits<double>::max() : sy / dy,
           tDZ = sz == 0 ? std::numeric_limits<double>::max() : sz / dz;
    auto frac = [](double v) { return v - std::floor(v); };
    double tX = tDX * (sx > 0 ? 1.0 - frac(fromX) : frac(fromX)), tY = tDY * (sy > 0 ? 1.0 - frac(fromY) : frac(fromY)),
           tZ = tDZ * (sz > 0 ? 1.0 - frac(fromZ) : frac(fromZ));
    while (tX <= 1.0 || tY <= 1.0 || tZ <= 1.0) {
        if (tX < tY) {
            if (tX < tZ) x += sx, tX += tDX;
            else z += sz, tZ += tDZ;
        } else if (tY < tZ) {
            y += sy, tY += tDY;
        } else {
            z += sz, tZ += tDZ;
        }
        if (auto hit = hitAt(x, y, z)) return hit;
    }
    return std::nullopt;
}

std::vector<AABB> ServerLevel::collisionBoxes(const AABB& box) {
    std::vector<AABB> out;
    int x0 = static_cast<int>(std::floor(box.minX - 1.0e-7)) - 1, x1 = static_cast<int>(std::floor(box.maxX + 1.0e-7)) + 1;
    int y0 = static_cast<int>(std::floor(box.minY - 1.0e-7)) - 1, y1 = static_cast<int>(std::floor(box.maxY + 1.0e-7)) + 1;
    int z0 = static_cast<int>(std::floor(box.minZ - 1.0e-7)) - 1, z1 = static_cast<int>(std::floor(box.maxZ + 1.0e-7)) + 1;
    for (int bx = x0; bx <= x1; bx++) {
        for (int bz = z0; bz <= z1; bz++) {
            LevelChunk* chunk = getChunkIfLoaded({bx >> 4, bz >> 4});
            if (!chunk) continue;
            for (int by = std::max(y0, minY()); by <= std::min(y1, maxY()); by++) {
                BlockStateId s = chunk->getBlockState(bx & 15, by, bz & 15);
                if (!(blocks::flags(s) & blocks::kHasCollision)) continue;
                for (const auto& b : gen::kShapes[gen::kStateShape[s]]) {
                    AABB a{bx + b.minX, by + b.minY, bz + b.minZ, bx + b.maxX, by + b.maxY, bz + b.maxZ};
                    if (a.intersects(box)) out.push_back(a);
                }
            }
        }
    }
    return out;
}

void ServerLevel::saveChunk(LevelChunk& chunk, int64_t gameTime) {
    saveEntities(chunk);
    if (!chunk.dirty || !storage_) return;
    if (chunk.readOnly) {
        if (warnedReadOnly_.insert(chunk.pos().pack()).second)
            log::warn("Not saving changes to read-only chunk [{}, {}]", chunk.pos().x, chunk.pos().z);
        chunk.dirty = false;
        return;
    }
    if (storage_->write(chunk.pos().x, chunk.pos().z, chunk_serializer::write(chunk, gameTime, light_.get()))) {
        chunk.dirty = false;
    } else {
        log::error("Failed to save chunk [{}, {}]", chunk.pos().x, chunk.pos().z);
    }
}

int ServerLevel::saveChunks(int64_t gameTime) {
    int n = 0;
    for (auto& [_, chunk] : chunks_) {
        if (!chunk->dirty && !chunk->entitiesDirty) continue;
        saveChunk(*chunk, gameTime);
        n++;
    }
    return n;
}

BlockStateId ServerLevel::getBlockState(int x, int y, int z) {
    return getChunk({x >> 4, z >> 4}).getBlockState(x & 15, y, z & 15);
}

bool ServerLevel::setBlock(BlockPos pos, BlockStateId state, int flags) { return setBlockAt(pos, state, flags, 0); }

void ServerLevel::sendParticles(std::string_view type, double x, double y, double z, int count, float xDist, float yDist, float zDist,
                                float speed) {
    if (auto id = registries::id("minecraft:particle_type", type)) particles_.push_back({*id, x, y, z, xDist, yDist, zDist, speed, count});
}

void ServerLevel::markChanged(BlockPos pos) {
    int64_t section = (static_cast<int64_t>(pos.x >> 4) & 0x3FFFFF) << 42 | (static_cast<int64_t>(pos.y >> 4) & 0xFFFFF) |
                      (static_cast<int64_t>(pos.z >> 4) & 0x3FFFFF) << 20;
    changed_[section].insert({pos.x, pos.y, pos.z});
}

bool ServerLevel::setBlockAt(BlockPos pos, BlockStateId state, int flags, int depth) {
    if (pos.y < minY() || pos.y > maxY()) return false;
    LevelChunk& chunk = getChunk({pos.x >> 4, pos.z >> 4});
    // LevelChunk.setBlockState: a replaced block's block entity has side effects (containers
    // spill, jukeboxes eject their disc, ...), unless UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS.
    std::optional<nbt::Compound> removedBlockEntity;
    BlockStateId before = chunk.getBlockState(pos.x & 15, pos.y, pos.z & 15);
    if (blocks::blockOf(before) != blocks::blockOf(state) && !(flags & kSkipBlockEntitySideEffects))
        if (auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z); be && be->blockAtLoad == blocks::blockOf(before)) removedBlockEntity = be->tag;
    bool wasEmpty = chunk.section((pos.y - minY()) >> 4).hasOnlyAir();
    BlockStateId old = chunk.setBlockState(pos.x & 15, pos.y, pos.z & 15, state);
    if (old == state) return false;
    // LevelChunk.setBlockState: section emptiness and light properties go to the light engine.
    bool isEmpty = chunk.section((pos.y - minY()) >> 4).hasOnlyAir();
    if (wasEmpty != isEmpty) light_->updateSectionStatus(pos.x >> 4, pos.y >> 4, pos.z >> 4, isEmpty);
    if (light::hasDifferentLightProperties(old, state)) {
        chunk.skyLightSources.update(ChunkColumn(chunk), pos.x & 15, pos.y, pos.z & 15);
        light_->checkBlock(pos);
    }
    if (removedBlockEntity) {
        if (auto it = containers_.find(pos.asLong()); it != containers_.end()) { // menus on it close
            if (auto c = it->second.lock()) c->valid = false;
            containers_.erase(it);
        }
        preRemoveSideEffects(pos, old, std::move(*removedBlockEntity));
    }
    // TODO: BlockBehaviour.affectNeighborsAfterRemoval (redstone components tell their neighbours).
    if (!(flags & kSkipOnPlace)) block_behaviour::onPlace(*this, pos, state, old);
    if (getBlockState(pos) != state) return true; // (onPlace replaced it; Level.setBlock stops there)
    if (flags & kUpdateClients) markChanged(pos);
    if (flags & kUpdateNeighbors) updateNeighborsAt(pos, blocks::blockOf(old)); // TODO: analog output (comparators)
    if (!(flags & kUpdateKnownShape) && depth < 512) {
        // Level.setBlock: the old state's indirect neighbours, then the new state's direct and indirect
        // ones, with UPDATE_NEIGHBORS and UPDATE_SUPPRESS_DROPS dropped (flags & -34).
        int neighbourFlags = flags & ~(kUpdateNeighbors | kSuppressDrops);
        block_behaviour::updateIndirectNeighbourShapes(*this, pos, old, neighbourFlags, depth + 1);
        updateNeighbourShapes(pos, state, neighbourFlags, depth + 1);
        block_behaviour::updateIndirectNeighbourShapes(*this, pos, state, neighbourFlags, depth + 1);
    }
    return true;
}

void ServerLevel::neighborShapeChanged(Direction toNeighbour, BlockPos pos, BlockPos neighbourPos, int flags, int depth) {
    if (neighbourPos.y < minY() || neighbourPos.y > maxY()) return; // (nothing to read there; vanilla sees void air)
    NeighborUpdate u{};
    u.kind = NeighborUpdate::Kind::Shape;
    u.pos = pos, u.neighbourPos = neighbourPos, u.direction = toNeighbour;
    u.neighbourState = getBlockState(neighbourPos);
    u.flags = flags, u.depth = depth;
    addAndRun(pos, u);
}

void ServerLevel::updateNeighbourShapes(BlockPos pos, BlockStateId state, int flags, int depth) {
    // BlockBehaviour.updateNeighbourShapes visits these directions in this order.
    constexpr Direction kOrder[] = {Direction::West, Direction::East, Direction::North, Direction::South, Direction::Down, Direction::Up};
    for (Direction d : kOrder) {
        NeighborUpdate u{};
        u.kind = NeighborUpdate::Kind::Shape;
        u.pos = pos.relative(d), u.neighbourPos = pos, u.direction = direction::opposite(d);
        u.neighbourState = state;
        u.flags = flags, u.depth = depth;
        addAndRun(u.pos, u);
    }
}

namespace {
// NeighborUpdater.UPDATE_ORDER
constexpr Direction kNeighbourUpdateOrder[] = {Direction::West, Direction::East, Direction::Down, Direction::Up, Direction::North, Direction::South};
} // namespace

void ServerLevel::updateNeighborsAtExceptFromFacing(BlockPos pos, uint32_t sourceBlock, std::optional<Direction> skip) {
    NeighborUpdate u{};
    u.kind = NeighborUpdate::Kind::Multi;
    u.pos = pos, u.sourceBlock = sourceBlock;
    u.skip = -1;
    if (skip)
        for (int8_t i = 0; i < 6; i++)
            if (kNeighbourUpdateOrder[i] == *skip) u.skip = i;
    u.idx = u.skip == 0 ? 1 : 0;
    addAndRun(pos, u);
}

void ServerLevel::neighborChanged(BlockPos pos, uint32_t sourceBlock) {
    NeighborUpdate u{};
    u.kind = NeighborUpdate::Kind::Simple;
    u.pos = pos, u.sourceBlock = sourceBlock;
    addAndRun(pos, u);
}

void ServerLevel::updateNeighboursOnBlockSet(BlockPos pos, BlockStateId oldState) {
    (void)oldState; // TODO: oldState.affectNeighborsAfterRemoval when the block changed (redstone)
    BlockStateId state = getBlockState(pos);
    updateNeighborsAt(pos, blocks::blockOf(state)); // TODO: analog output (comparators)
}

void ServerLevel::addAndRun(BlockPos pos, NeighborUpdate update) {
    // CollectingNeighborUpdater.addAndRun
    bool wasUpdating = neighborUpdateCount_ > 0;
    bool limitReached = maxChainedNeighborUpdates >= 0 && neighborUpdateCount_ >= maxChainedNeighborUpdates;
    neighborUpdateCount_++;
    if (!limitReached) {
        if (wasUpdating) addedThisLayer_.push_back(update);
        else updateStack_.push_back(update);
    } else if (neighborUpdateCount_ - 1 == maxChainedNeighborUpdates) {
        log::error("Too many chained neighbor updates. Skipping the rest. First skipped position: {}, {}, {}", pos.x, pos.y, pos.z);
    }
    if (!wasUpdating) runNeighborUpdates();
}

void ServerLevel::runNeighborUpdates() {
    // CollectingNeighborUpdater.runUpdates
    struct Reset {
        ServerLevel& l;
        ~Reset() {
            l.updateStack_.clear();
            l.addedThisLayer_.clear();
            l.neighborUpdateCount_ = 0;
        }
    } reset{*this};
    while (!updateStack_.empty() || !addedThisLayer_.empty()) {
        for (auto it = addedThisLayer_.rbegin(); it != addedThisLayer_.rend(); ++it) updateStack_.push_back(*it);
        addedThisLayer_.clear();
        // (the stack only changes here, so its top stays put while an update runs)
        while (addedThisLayer_.empty()) {
            NeighborUpdate current = updateStack_.back();
            bool more = runNext(current);
            updateStack_.back() = current;
            if (!more) {
                updateStack_.pop_back();
                break;
            }
        }
    }
}

bool ServerLevel::runNext(NeighborUpdate& u) {
    switch (u.kind) {
    case NeighborUpdate::Kind::Shape: { // NeighborUpdater.executeShapeUpdate
        if (u.pos.y < minY() || u.pos.y > maxY()) return false;
        BlockStateId current = getBlockState(u.pos);
        if ((u.flags & kUpdateSkipShapeUpdateOnWire) && block_behaviour::isRedstoneWire(current)) return false;
        BlockStateId updated = block_behaviour::updateShape(current, *this, u.pos, u.direction, u.neighbourPos, u.neighbourState);
        // Block.updateOrDestroy
        if (updated == current) return false;
        if (blocks::isAir(updated)) destroyBlock(u.pos, (u.flags & kSuppressDrops) == 0, nullptr, u.flags, u.depth);
        else setBlockAt(u.pos, updated, u.flags & ~kSuppressDrops, u.depth);
        return false;
    }
    case NeighborUpdate::Kind::Simple: // NeighborUpdater.executeUpdate
        if (u.pos.y >= minY() && u.pos.y <= maxY()) block_behaviour::neighborChanged(*this, u.pos, getBlockState(u.pos), u.sourceBlock);
        return false;
    case NeighborUpdate::Kind::Multi: { // MultiNeighborUpdate.runNext
        BlockPos p = u.pos.relative(kNeighbourUpdateOrder[u.idx++]);
        if (p.y >= minY() && p.y <= maxY()) block_behaviour::neighborChanged(*this, p, getBlockState(p), u.sourceBlock);
        if (u.idx < 6 && u.idx == u.skip) u.idx++;
        return u.idx < 6;
    }
    }
    return false;
}

bool ServerLevel::destroyBlock(BlockPos pos, bool dropItems, const ItemStack* tool, int flags, int depth) {
    (void)flags; // (always UPDATE_ALL; `flags` only decide the drops)
    BlockStateId state = getBlockState(pos);
    if (blocks::isAir(state)) return false;
    // Block.spawnDestroyParticles (BaseFireBlock: none; StrawBedBlock: its own event)
    if (blocks::is(state, gen::block::STRAW_BED)) levelEvent(2014, pos, static_cast<int>(state));
    else if (!blocks::isInstance(blocks::blockOf(state), "BaseFireBlock")) levelEvent(2001, pos, static_cast<int>(state));
    if (dropItems) {
        LevelChunk& chunk = getChunk({pos.x >> 4, pos.z >> 4});
        std::optional<nbt::Compound> be;
        if (auto* b = chunk.blockEntityAt(pos.x, pos.y, pos.z)) be = b->tag;
        dropResources(state, pos, be ? &*be : nullptr, tool, nullptr);
    }
    return setBlockAt(pos, fluids::legacyBlock(state), kUpdateAll, depth); // FluidState.createLegacyBlock
}

void ServerLevel::dropResources(BlockStateId state, BlockPos pos, const nbt::Compound* blockEntity, const ItemStack* tool,
                                const ServerPlayer* breaker) {
    uint32_t block = blocks::blockOf(state);
    loot::BlockContext ctx{*this, random_, state, pos, tool, breaker, blockEntity};
    for (auto& drop : loot::getRandomItems(blocks::behaviour(block).lootTable, ctx)) popResource(pos, std::move(drop));
    spawnAfterBreak(state, pos, tool);
}

std::shared_ptr<BlockEntityContainer> ServerLevel::blockEntityContainer(BlockPos pos) {
    if (auto it = containers_.find(pos.asLong()); it != containers_.end())
        if (auto c = it->second.lock(); c && c->valid) return c;
    LevelChunk* chunk = getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    if (!be || blocks::blockOf(getBlockState(pos)) != be->blockAtLoad) return nullptr;
    std::string type = be->tag.getString("id");
    int size = BlockEntityContainer::sizeFor(type);
    if (size == 0 && type != "minecraft:ender_chest") return nullptr; // (ender chests: only the lid)
    auto c = std::make_shared<BlockEntityContainer>(*this, pos, type, size);
    containers_[pos.asLong()] = c;
    return c;
}

void ServerLevel::spawnAfterBreak(BlockStateId state, BlockPos pos, const ItemStack* tool) {
    // Block.spawnAfterBreak: experience from ores and sculk blocks (Block.tryDropExperience, with
    // the tool's minecraft:block_experience effects - Silk Touch), and from spawners.
    uint32_t block = blocks::blockOf(state);
    auto popExperience = [&](int amount) { // Block.popExperience
        if (gameRules().getBool(GameRules::BlockDrops)) ExperienceOrb::award(*this, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, amount);
    };
    auto tryDropExperience = [&](int sampled) {
        static const ItemStack kNoTool;
        int xp = static_cast<int>(enchantments::processValue(*this, tool ? *tool : kNoTool, "minecraft:block_experience", static_cast<float>(sampled)));
        if (xp > 0) popExperience(xp);
    };
    auto between = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(random_); };
    std::string_view range = blocks::behaviour(block).xpRange;
    if (!range.empty()) { // DropExperienceBlock.xpRange: an IntProvider (constant or uniform)
        json::Value v = json::parse(range);
        tryDropExperience(v.isNumber() ? static_cast<int>(v.number()) : between(static_cast<int>(v["min_inclusive"].number()),
                                                                                static_cast<int>(v["max_inclusive"].number())));
    } else if (blocks::isInstance(block, "RedStoneOreBlock")) {
        tryDropExperience(between(1, 5));
    } else if (blocks::isInstance(block, "SculkSensorBlock") || blocks::isInstance(block, "SculkShriekerBlock") ||
               blocks::isInstance(block, "SculkCatalystBlock")) {
        tryDropExperience(5);
    } else if (blocks::isInstance(block, "SpawnerBlock")) {
        popExperience(15 + between(0, 14) + between(0, 14));
    }
}

void ServerLevel::popResource(BlockPos pos, ItemStack stack) {
    // Block.popResource
    if (stack.isEmpty() || !gameRules().getBool(GameRules::BlockDrops)) return;
    std::uniform_real_distribution<double> d(-0.25, 0.25);
    double x = pos.x + 0.5 + d(random_), y = pos.y + 0.5 + d(random_) - ItemEntity::kHeight / 2, z = pos.z + 0.5 + d(random_);
    if (ItemEntity* e = ItemEntity::spawn(*this, x, y, z, std::move(stack), random_)) e->pickupDelay = ItemEntity::kDefaultPickupDelay;
}

void ServerLevel::preRemoveSideEffects(BlockPos pos, BlockStateId state, nbt::Compound blockEntity) {
    std::string type = blockEntity.getString("id");
    if (type == "minecraft:jukebox") {
        block_entity::jukebox::popOut(*this, pos, blockEntity);
        levelEvent(1011, pos, 0); // JukeboxBlockEntity.setRemoved
    } else if (type == "minecraft:beacon") { // BeaconBlockEntity.setRemoved
        playBlockSound(-1, pos, registries::id("minecraft:sound_event", "minecraft:block.beacon.deactivate").value_or(-1), 1.0f, 1.0f);
        block_entity::forgetBeacon(*this, pos);
    } else if (type == "minecraft:lectern") {
        const nbt::Compound* book = blockEntity.getCompound("Book");
        auto stack = book && blocks::getBool(state, "has_book") ? item_nbt::read(*book) : std::nullopt;
        if (stack && !stack->isEmpty()) {
            Direction facing = direction::byName(blocks::getValue(state, "facing").value_or("north"));
            double x = pos.x + 0.5 + 0.25f * direction::stepX(facing), z = pos.z + 0.5 + 0.25f * direction::stepZ(facing);
            if (ItemEntity* e = ItemEntity::spawn(*this, x, pos.y + 1, z, std::move(*stack), random_)) e->pickupDelay = ItemEntity::kDefaultPickupDelay;
        }
    } else if (type == "minecraft:decorated_pot") { // a single-item Container
        if (const nbt::Compound* item = blockEntity.getCompound("item")) {
            nbt::Compound slot = *item;
            slot.put("Slot", int8_t{0});
            nbt::Compound asContainer;
            asContainer.put("Items", nbt::List{nbt::TagType::Compound, {std::move(slot)}});
            dropContents(pos, asContainer);
        }
    } else if (BlockEntityContainer::isFurnace(type)) { // the items, then the experience of what it cooked
        dropContents(pos, blockEntity);
        block_entity::awardFurnaceExperience(*this, blockEntity, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
    } else if (type != "minecraft:shulker_box") { // (shulker boxes keep their contents in the item)
        dropContents(pos, blockEntity);           // BlockEntity.preRemoveSideEffects for Containers
    }
}

void ServerLevel::dropContents(BlockPos pos, const nbt::Compound& blockEntity) {
    // Containers.dropContents: spill a container's items where it stood.
    const nbt::List* list = blockEntity.getList("Items");
    if (!list) return;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    auto triangle = [&](double mode, double deviation) { return mode + deviation * (u(random_) - u(random_)); };
    for (const auto& e : list->items) {
        const nbt::Compound* c = e.asCompound();
        auto stack = c ? item_nbt::read(*c) : std::nullopt;
        if (!stack) continue;
        double size = ItemEntity::kWidth, range = 1.0 - size;
        double x = pos.x + u(random_) * range + size / 2, y = pos.y + u(random_) * range, z = pos.z + u(random_) * range + size / 2;
        while (!stack->isEmpty()) {
            ItemStack part = stack->split(std::uniform_int_distribution<int>(10, 30)(random_));
            auto item = std::make_unique<ItemEntity>(nextEntityId(), UUID::random(), x, y, z, std::move(part));
            item->vx = triangle(0.0, 0.11485000171139836);
            item->vy = triangle(0.2, 0.11485000171139836);
            item->vz = triangle(0.0, 0.11485000171139836);
            addFreshEntity(std::move(item));
        }
    }
}

std::vector<ServerLevel::SectionChanges> ServerLevel::takeChanges() {
    std::vector<SectionChanges> out;
    for (auto& [section, set] : changed_) {
        SectionChanges sc{section, {}, {}};
        for (auto [x, y, z] : set) sc.positions.push_back({x, y, z});
        sc.chunk = {sc.positions[0].x >> 4, sc.positions[0].z >> 4};
        out.push_back(std::move(sc));
    }
    changed_.clear();
    return out;
}

// ---------------------------------------------------------------- ServerClockManager

ServerClockManager::ServerClockManager() {
    clocks_.resize(std::max(1, registries::size("minecraft:world_clock")));
    overworld_ = registries::id("minecraft:world_clock", "minecraft:overworld").value_or(0);
}

ServerClockManager::Clock* ServerClockManager::byName(std::string_view name) {
    auto id = registries::id("minecraft:world_clock", name);
    return id && *id < static_cast<int>(clocks_.size()) ? &clocks_[*id] : nullptr;
}

std::string_view ServerClockManager::name(int id) const {
    for (const auto& r : gen::kSyncedRegistries)
        if (r.name == "minecraft:world_clock" && id < static_cast<int>(r.entries.size())) return r.entries[id];
    return "";
}

void ServerClockManager::tick(bool advanceTime) {
    gameTime_++;
    if (!advanceTime) return;
    for (auto& c : clocks_) {
        if (c.paused) continue;
        c.partialTick += c.rate;
        int full = static_cast<int>(std::floor(c.partialTick));
        c.partialTick -= static_cast<float>(full);
        c.totalTicks += full;
    }
}

// The day timeline's gameplay/sky_light_level multiplier at `ticks` into the day (keyframes
// interpolated linearly, wrapping around the period).
float ServerClockManager::skyLightMultiplier(int64_t totalTicks) {
    static const std::vector<std::pair<int, float>> keys = [] {
        std::vector<std::pair<int, float>> out;
        json::Value all = json::parse(gen::kRegistryElementsJson);
        const json::Value& day = all["minecraft:timeline"]["minecraft:day"];
        for (const auto& k : day["tracks"]["minecraft:gameplay/sky_light_level"]["keyframes"].array())
            out.emplace_back(static_cast<int>(k["ticks"].number()), static_cast<float>(k["value"].number()));
        return out;
    }();
    if (keys.empty()) return 1.0f;
    int period = 24000;
    int t = static_cast<int>(((totalTicks % period) + period) % period);
    for (size_t i = 0; i < keys.size(); i++) {
        auto [t0, v0] = keys[i];
        auto [t1, v1] = i + 1 < keys.size() ? keys[i + 1] : std::pair{keys[0].first + period, keys[0].second};
        int tt = t < keys[0].first ? t + period : t;
        if (tt >= t0 && tt < t1) return v0 + (v1 - v0) * static_cast<float>(tt - t0) / static_cast<float>(t1 - t0);
    }
    return keys.back().second;
}


} // namespace mc
