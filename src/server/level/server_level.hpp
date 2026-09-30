#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <functional>
#include <random>

#include "core/direction.hpp"
#include "world/entity/item_entity.hpp"
#include "world/level/chunk/level_chunk.hpp"
#include "world/phys/aabb.hpp"
#include "world/level/chunk/storage/region_file.hpp"
#include "world/level/gamerules/game_rules.hpp"
#include "util/legacy_random.hpp"

namespace mc {

class ServerPlayer;
class BlockEntityContainer;

// Port of FlatLevelSource with the default "classic flat" layers.
class FlatLevelSource {
public:
    struct Layer {
        BlockStateId state;
        int height;
    };
    FlatLevelSource(std::vector<Layer> layers, uint32_t biome) : layers_(std::move(layers)), biome_(biome) {}
    static FlatLevelSource classicFlat();

    void fill(LevelChunk& chunk, const DimensionInfo& dim) const;
    uint32_t biome() const { return biome_; }
    int surfaceY(const DimensionInfo& dim) const; // first free y

private:
    std::vector<Layer> layers_;
    uint32_t biome_;
};

class ServerLevel {
public:
    // Block.UPDATE_* flags
    static constexpr int kUpdateNeighbors = 1;
    static constexpr int kUpdateClients = 2;
    static constexpr int kUpdateAll = kUpdateNeighbors | kUpdateClients;
    static constexpr int kUpdateInvisible = 4;
    static constexpr int kUpdateKnownShape = 16; // no neighbour shape updates
    static constexpr int kSuppressDrops = 32;
    static constexpr int kSkipBlockEntitySideEffects = 256;
    static constexpr int kSkipOnPlace = 512;

    // `storage` may be null (nothing is saved). `persistGenerated`: whether chunks we generate are
    // worth saving untouched; false when our generator differs from the world's (so vanilla can
    // generate the real terrain there later).
    ServerLevel(std::string dimension, std::string dimensionType, DimensionInfo info, FlatLevelSource generator,
                std::unique_ptr<RegionFileStorage> storage, std::unique_ptr<RegionFileStorage> entityStorage, bool persistGenerated,
                int64_t seed, std::function<int()> entityIds);
    ~ServerLevel();

    const std::string& dimension() const { return dimension_; }
    const std::string& dimensionType() const { return dimensionType_; }
    const DimensionInfo& info() const { return info_; }
    int minY() const { return info_.minY; }
    int maxY() const { return info_.minY + info_.height - 1; }
    // ServerLevel.isFlat: the world's generator is superflat (else it's vanilla terrain we can't
    // generate yet). Also decides the sea level (FlatLevelSource: -63, noise overworld: 63).
    bool isFlat() const { return persistGenerated_; }
    int seaLevel() const { return isFlat() ? -63 : 63; }
    int64_t seed() const { return seed_; }
    // BiomeManager.obfuscateSeed: what clients get instead of the seed.
    int64_t obfuscatedSeed() const { return obfuscatedSeed_; }
    // Level.getBiome (BiomeManager's seeded fuzzy zoom) and the stored quart-resolution biome;
    // minecraft:worldgen/biome ids.
    uint32_t getBiome(BlockPos pos);
    uint32_t getNoiseBiome(int qx, int qy, int qz);

    LevelChunk& getChunk(ChunkPos pos); // loads/generates on demand (with light initialized)
    // A chunk that went through the LIGHT step: its neighbours are loaded and its light sources
    // propagated (or its saved light is trusted). Chunks are lit before they're sent.
    LevelChunk& getLitChunk(ChunkPos pos);

    // Light (LevelLightEngine): Level.getBrightness, getRawBrightness and canSeeSky.
    light::LevelLightEngine& lightEngine() { return *light_; }
    int brightness(light::LightLayer layer, BlockPos pos);
    int rawBrightness(BlockPos pos, int skyDarken);
    bool canSeeSky(BlockPos pos) { return brightness(light::LightLayer::Sky, pos) >= 15; }
    // Runs the queued light work (the light engine's update at the end of a tick).
    void runLightUpdates();
    // ChunkMap.addEntity: players in range see a new entity right away (set by PlayerList).
    std::function<void(Entity&)> onEntityAdded;
    // The overworld clock's total ticks: where the day timeline is (set by the server).
    std::function<int64_t()> dayTime = [] { return int64_t{0}; };
    // Level.getSkyDarken (15 - the sky light level; set by the server from the overworld clock).
    std::function<int()> skyDarken = [] { return 0; };
    // Called after light work that clients may need to hear about right away (set by PlayerList).
    std::function<void()> onLightWork;
    // Light sections changed since the last call, per chunk (ChunkHolder's changed-light filters).
    struct LightChanges {
        ChunkPos pos;
        std::vector<uint64_t> sky, block; // bit per light section index
    };
    std::vector<LightChanges> takeLightChanges();
    // ClientboundLightUpdatePacketData: every light section, or only those set in `sky`/`block`.
    void writeLightData(ByteWriter& out, ChunkPos pos, const std::vector<uint64_t>* sky = nullptr, const std::vector<uint64_t>* block = nullptr);
    LevelChunk* getChunkIfLoaded(ChunkPos pos) {
        auto it = chunks_.find(pos.pack());
        return it == chunks_.end() ? nullptr : it->second.get();
    }

    // Entities (only item entities are simulated so far).
    int nextEntityId() { return entityIds_(); }
    std::mt19937& random() { return random_; }
    // Level.random, a LegacyRandomSource: what random ticks and features draw from.
    LegacyRandom& levelRandom() { return levelRandom_; }
    // Level.getBlockRandomPos: a position in the 16x16 column at (x, z), y within yMask of `y`.
    BlockPos blockRandomPos(int x, int y, int z, int yMask) {
        randValue_ = static_cast<int32_t>(static_cast<uint32_t>(randValue_) * 3u + 1013904223u);
        int val = randValue_ >> 2;
        return {x + (val & 15), y + (val >> 16 & yMask), z + (val >> 8 & 15)};
    }
    // Level.isEmptyBlock
    bool isEmptyBlock(BlockPos pos) { return blocks::isAir(getBlockState(pos)); }
    // LevelReader.getMaxLocalRawBrightness: raw brightness with the current sky darkening.
    int maxLocalRawBrightness(BlockPos pos) { return rawBrightness(pos, skyDarken()); }
    // Level.isRainingAt (no weather yet: never).
    bool isRainingAt(BlockPos) { return false; }
    // ServerChunkCache.tickChunks' random ticks: ServerLevel.tickChunk for each chunk in block
    // ticking range, with game rule random_tick_speed.
    void tickChunks();
    // Chunk distance (Chebyshev) to the nearest player, INT_MAX without players (set by the server),
    // and the simulation distance: DistanceManager's simulation ticket levels.
    std::function<int(ChunkPos)> playerChunkDistance = [](ChunkPos) { return 0; };
    int simulationDistance = 10;
    // DistanceManager.inEntityTickingRange (random ticks, scheduled ticks) and inBlockTickingRange
    // (block entity tickers).
    bool inEntityTickingRange(ChunkPos pos) { return playerChunkDistance(pos) <= simulationDistance; }
    bool inBlockTickingRange(ChunkPos pos) { return playerChunkDistance(pos) <= simulationDistance + 1; }
    template <class T>
    T* addFreshEntity(std::unique_ptr<T> entity) {
        T* raw = entity.get();
        addEntity(std::move(entity));
        return raw;
    }
    const std::vector<std::unique_ptr<Entity>>& entities() const { return entities_; }
    // Level.getEntitiesOfClass: live entities of type T whose box intersects `box`.
    template <class T>
    std::vector<T*> entitiesIn(const AABB& box) {
        std::vector<T*> out;
        for (auto& e : entities_)
            if (auto* t = dynamic_cast<T*>(e.get()); t && !t->removed && t->boundingBox().intersects(box)) out.push_back(t);
        return out;
    }
    std::vector<ItemEntity*> itemsIn(const AABB& box) { return entitiesIn<ItemEntity>(box); }
    void tickEntities();
    // Ids of entities removed since the last call (for ClientboundRemoveEntitiesPacket).
    std::vector<int> takeRemovedEntities() { return std::exchange(removedEntities_, {}); }
    // Block collision boxes intersecting `box` (Level.getBlockCollisions), from loaded chunks only.
    std::vector<AABB> collisionBoxes(const AABB& box);
    // Level.clip with ClipContext.Block.COLLIDER (and Fluid.SOURCE_ONLY if `sourceFluids`): the block the
    // segment hits first, if any.
    std::optional<BlockPos> clip(double fx, double fy, double fz, double tx, double ty, double tz, bool sourceFluids);
    BlockStateId getBlockState(int x, int y, int z);
    BlockStateId getBlockState(BlockPos p) { return getBlockState(p.x, p.y, p.z); }
    // Level.setBlock: sets the block, runs neighbour shape updates (updateShape, unless
    // kUpdateKnownShape) and records the change for clients (kUpdateClients). Returns false if
    // nothing changed.
    bool setBlock(BlockPos pos, BlockStateId state, int flags = kUpdateAll);
    // LevelAccessor.neighborShapeChanged: re-run `pos`'s updateShape for its neighbour towards `toNeighbour`.
    void neighborShapeChanged(Direction toNeighbour, BlockPos pos, BlockPos neighbourPos, int flags, int depth);
    // Level.destroyBlock: break particles, optionally the block's drops, then air (or its fluid).
    bool destroyBlock(BlockPos pos, bool dropResources, const ItemStack* tool = nullptr, int flags = kUpdateAll);
    // Block.popResource / Containers.dropContents
    void popResource(BlockPos pos, ItemStack stack);
    void dropContents(BlockPos pos, const nbt::Compound& blockEntity);
    // The live container of the block entity at `pos` (shared by menus and tickers), or null if
    // there's none; see BlockEntityContainer.
    std::shared_ptr<BlockEntityContainer> blockEntityContainer(BlockPos pos);
    // Block.spawnAfterBreak: experience orbs from ores, sculk and spawners.
    void spawnAfterBreak(BlockStateId state, BlockPos pos, const ItemStack* tool);
    // BlockEntity.preRemoveSideEffects for a removed block entity (as saved) of the block `state`.
    void preRemoveSideEffects(BlockPos pos, BlockStateId state, nbt::Compound blockEntity);
    // Block.dropResources: loot table (with `tool`), plus container contents.
    void dropResources(BlockStateId state, BlockPos pos, const nbt::Compound* blockEntity, const ItemStack* tool, const ServerPlayer* breaker);

    // Level.levelEvent, delivered by PlayerList to players within 64 blocks (except `exclude`).
    struct LevelEvent {
        int type;
        BlockPos pos;
        int data;
        int excludeEntity;
    };
    void levelEvent(int type, BlockPos pos, int data, int excludeEntity = -1) { events_.push_back({type, pos, data, excludeEntity}); }
    std::vector<LevelEvent> takeLevelEvents() { return std::exchange(events_, {}); }

    // Level.playSound(except, x, y, z, sound, source, volume, pitch): ClientboundSoundPacket to
    // players within the sound's range (16 blocks, more for volume > 1), except `excludeEntity`.
    struct Sound {
        int32_t sound; // minecraft:sound_event id, or -1 for directSound
        std::string directSound; // Holder.direct(SoundEvent.createVariableRangeEvent(id))
        int source;    // SoundSource ordinal
        double x, y, z;
        float volume, pitch;
        int64_t seed;
        int excludeEntity;
    };
    static constexpr int kSoundSourceRecords = 2;
    static constexpr int kSoundSourceBlocks = 4;
    static constexpr int kSoundSourcePlayers = 7;
    void playSound(int excludeEntity, double x, double y, double z, int32_t sound, int source, float volume, float pitch);
    void playBlockSound(int excludeEntity, BlockPos pos, int32_t sound, float volume, float pitch) {
        playSound(excludeEntity, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, sound, kSoundSourceBlocks, volume, pitch);
    }
    void playDirectSound(double x, double y, double z, std::string sound, int source, float volume, float pitch);
    std::vector<Sound> takeSounds() { return std::exchange(sounds_, {}); }

    // ServerLevel.sendParticles for particle types without options: players within 32 blocks get
    // ClientboundLevelParticlesPacket.
    struct Particles {
        int32_t type; // minecraft:particle_type id
        double x, y, z;
        float xDist, yDist, zDist, speed;
        int count;
    };
    void sendParticles(std::string_view type, double x, double y, double z, int count, float xDist, float yDist, float zDist, float speed);
    std::vector<Particles> takeParticles() { return std::exchange(particles_, {}); }

    // Level.blockEvent: run at the end of the tick (BlockBehaviour.triggerEvent) and, if it did
    // something, sent to players within 64 blocks (ClientboundBlockEventPacket).
    struct BlockEvent {
        BlockPos pos;
        uint32_t block;
        int b0, b1;
    };
    void blockEvent(BlockPos pos, uint32_t block, int b0, int b1) { blockEvents_.push_back({pos, block, b0, b1}); }
    // ServerLevel.runBlockEvents: the events that should reach clients.
    std::vector<BlockEvent> runBlockEvents();

    // Scheduled block ticks (LevelTicks): `block`'s tick() runs at `pos` after `delay` game ticks,
    // unless one is already pending for that block there. Lower priorities run first.
    void scheduleTick(BlockPos pos, uint32_t block, int delay, int priority = 0);
    bool hasScheduledTick(BlockPos pos, uint32_t block);
    // ServerLevel.tick's blockTicks.tick: runs the ticks that are due, in trigger/priority/schedule order.
    void tickBlocks();
    // Level.getDifficulty (Difficulty id) and ChunkMap.anyPlayerCloseEnoughTo (non-spectators
    // within `distance` of the block's corner); set by the server.
    int difficulty = 2;
    std::function<bool(BlockPos, int)> anyPlayerCloseEnoughTo = [](BlockPos, int) { return false; };
    // ServerLevel.canSpreadFireAround: game rule fire_spread_radius_around_player.
    bool canSpreadFireAround(BlockPos pos) {
        int radius = gameRules().getInt(GameRules::FireSpreadRadiusAroundPlayer);
        return radius == -1 || anyPlayerCloseEnoughTo(pos, radius);
    }
    // The players in this level (Level.players()); set by the server.
    std::function<std::vector<ServerPlayer*>()> players = [] { return std::vector<ServerPlayer*>{}; };

    // Level.getGameRules (the server's; set by it).
    GameRules& gameRules() { return *gameRules_; }
    void setGameRules(GameRules* rules) { gameRules_ = rules; }

    // Level.tickBlockEntities: block entities with a ticker (jukeboxes, ...).
    void tickBlockEntities();
    // The game time scheduled ticks are measured against; set by the server every tick.
    void setGameTime(int64_t t) { gameTime_ = t; }
    int64_t gameTime() const { return gameTime_; }

    // Block changes since the last call, grouped by section (ChunkHolder.changedBlocksPerSection).
    struct SectionChanges {
        int64_t sectionPos;
        ChunkPos chunk;
        std::vector<BlockPos> positions;
    };
    std::vector<SectionChanges> takeChanges();
    // Positions where a falling block landed this tick (ClientboundAddTransientBlockPacket).
    std::vector<BlockPos> transientBlocks;
    // Level.sendBlockUpdated: clients get the block (and its block entity data) again.
    void sendBlockUpdated(BlockPos pos) { markChanged(pos); }
    // Level.blockEntityChanged: the chunk needs saving (clients aren't told).
    void blockEntityChanged(BlockPos pos) {
        if (LevelChunk* chunk = getChunkIfLoaded({pos.x >> 4, pos.z >> 4})) chunk->dirty = true;
    }

    // SignBlockEntity.playerWhoMayEdit (not saved): the entity id of the player allowed to send
    // new text for the sign at a position.
    std::optional<int> signEditor(BlockPos pos) const {
        auto it = signEditors_.find(pos.asLong());
        return it == signEditors_.end() ? std::nullopt : std::optional<int>(it->second);
    }
    void setSignEditor(BlockPos pos, std::optional<int> player) {
        if (player) signEditors_[pos.asLong()] = *player;
        else signEditors_.erase(pos.asLong());
    }
    // SignBlockEntity.tick: drops editors for which `keep(pos, player)` is false.
    template <class Keep>
    void tickSignEditors(Keep keep) {
        std::erase_if(signEditors_, [&](const auto& e) { return !keep(BlockPos::fromLong(e.first), e.second); });
    }

    struct SpawnPoint {
        int x, y, z;
        float yaw = 0, pitch = 0;
    };
    const SpawnPoint& spawn() const { return spawn_; }
    void setSpawn(SpawnPoint s) { spawn_ = s; }

    // Writes dirty chunks (ChunkMap.saveAllChunks). Returns how many were written.
    int saveChunks(int64_t gameTime);
    // Saves and drops chunks for which `keep` is false (ChunkMap unloading).
    template <class Keep>
    void unloadChunks(Keep keep, int64_t gameTime);
    size_t loadedChunkCount() const { return chunks_.size(); }

private:
    void saveChunk(LevelChunk& chunk, int64_t gameTime);
    std::unique_ptr<LevelChunk> loadOrGenerate(ChunkPos pos);
    void loadEntities(LevelChunk& chunk);
    void saveEntities(LevelChunk& chunk);
    void unloadEntities(LevelChunk& chunk);
    void updateNeighbourShapes(BlockPos pos, int flags, int depth);
    bool setBlockAt(BlockPos pos, BlockStateId state, int flags, int depth);
    void markChanged(BlockPos pos);
    void addEntity(std::unique_ptr<Entity> entity);
    void unpackBlockTicks(LevelChunk& chunk);
    void initializeLight(LevelChunk& chunk);
    void tickChunk(LevelChunk& chunk, int tickSpeed);
    void tickPrecipitation(BlockPos pos);
    class LightChunks;

    std::string dimension_;
    std::string dimensionType_;
    DimensionInfo info_;
    FlatLevelSource generator_;
    int biomeCount_;
    int64_t seed_;
    SpawnPoint spawn_;
    std::unordered_map<int64_t, std::unique_ptr<LevelChunk>> chunks_;
    std::map<int64_t, std::set<std::tuple<int, int, int>>> changed_;
    std::unique_ptr<RegionFileStorage> storage_;
    std::unique_ptr<RegionFileStorage> entityStorage_;
    bool persistGenerated_;
    std::set<int64_t> warnedReadOnly_;
    std::function<int()> entityIds_;
    std::mt19937 random_{std::random_device{}()};
    LegacyRandom levelRandom_{static_cast<int64_t>((static_cast<uint64_t>(std::random_device{}()) << 32) | std::random_device{}())};
    int32_t randValue_ = static_cast<int32_t>(std::random_device{}());
    std::vector<std::unique_ptr<Entity>> entities_;
    std::vector<int> removedEntities_;
    std::vector<LevelEvent> events_;
    std::vector<Sound> sounds_;
    std::vector<Particles> particles_;
    int64_t obfuscatedSeed_ = 0;
    std::vector<BlockEvent> blockEvents_;
    std::unordered_map<int64_t, int> signEditors_;
    std::unordered_map<int64_t, std::weak_ptr<BlockEntityContainer>> containers_;
    int64_t gameTime_ = 0;
    int64_t subTickCount_ = 0;
    GameRules defaultGameRules_;
    GameRules* gameRules_ = &defaultGameRules_;
    std::unique_ptr<LightChunks> lightChunks_;
    std::unique_ptr<light::LevelLightEngine> light_;
    std::unordered_map<int64_t, LightChanges> lightChanges_;
};

template <class Keep>
void ServerLevel::unloadChunks(Keep keep, int64_t gameTime) {
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (keep(it->second->pos())) {
            ++it;
            continue;
        }
        saveChunk(*it->second, gameTime);
        unloadEntities(*it->second);
        ChunkPos pos = it->second->pos();
        it = chunks_.erase(it);
        light_->removeChunk(pos.x, pos.z); // ThreadedLevelLightEngine.updateChunkStatus
        lightChanges_.erase(pos.pack());
    }
}

// Port of ServerClockManager: one clock per minecraft:world_clock entry (overworld day cycle, end).
class ServerClockManager {
public:
    struct Clock {
        int64_t totalTicks = 0;
        float partialTick = 0;
        float rate = 1.0f;
        bool paused = false;
    };
    ServerClockManager();

    void tick(bool advanceTime);
    int64_t gameTime() const { return gameTime_; }
    void setGameTime(int64_t t) { gameTime_ = t; }
    // By minecraft:world_clock entry name.
    Clock* byName(std::string_view name);
    std::string_view name(int id) const;
    Clock& clock(int id) { return clocks_[id]; }
    int overworldClock() const { return overworld_; }
    // The day timeline's gameplay/sky_light_level at `totalTicks` into the day (keyframes
    // interpolated linearly, wrapping around the period).
    static float skyLightMultiplier(int64_t totalTicks);
    int count() const { return static_cast<int>(clocks_.size()); }

private:
    std::vector<Clock> clocks_;
    int overworld_ = 0;
    int64_t gameTime_ = 0;
};

} // namespace mc
