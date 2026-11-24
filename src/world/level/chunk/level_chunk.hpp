#pragma once

#include <array>
#include <optional>
#include <string>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "nbt/tag.hpp"
#include "world/level/block/block_state.hpp"
#include "world/level/chunk/paletted_container.hpp"
#include "world/level/lighting/level_light_engine.hpp"

namespace mc {

struct ChunkPos {
    int x = 0, z = 0;

    static ChunkPos ofBlock(double bx, double bz) {
        return {static_cast<int>(std::floor(bx)) >> 4, static_cast<int>(std::floor(bz)) >> 4};
    }
    int64_t pack() const {
        return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(x))) |
                                    (static_cast<uint64_t>(static_cast<uint32_t>(z)) << 32));
    }
    int distanceSquared(const ChunkPos& o) const {
        int dx = x - o.x, dz = z - o.z;
        return dx * dx + dz * dz;
    }
    bool operator==(const ChunkPos&) const = default;
};

struct DimensionInfo {
    int minY = -64;
    int height = 384;
    int sectionCount() const { return height / 16; }
};

class LevelChunkSection {
public:
    LevelChunkSection(uint32_t biome, int biomeCount);

    BlockStateId getBlockState(int x, int y, int z) const { return states_.get(x, y, z); }
    BlockStateId setBlockState(int x, int y, int z, BlockStateId state);
    uint32_t getBiome(int qx, int qy, int qz) const { return biomes_.get(qx, qy, qz); }
    void setBiome(int qx, int qy, int qz, uint32_t biome) { biomes_.set(qx, qy, qz, biome); }
    void fill(BlockStateId state);

    bool hasOnlyAir() const { return nonEmptyBlockCount_ == 0; }
    void write(ByteWriter& out) const;
    PalettedContainer& states() { return states_; }
    const PalettedContainer& states() const { return states_; }
    PalettedContainer& biomes() { return biomes_; }
    const PalettedContainer& biomes() const { return biomes_; }
    void recalcBlockCounts();

private:
    int16_t nonEmptyBlockCount_ = 0;
    int16_t fluidCount_ = 0;
    PalettedContainer states_;
    PalettedContainer biomes_;
};

// Heightmap.Types (network id = value). A full chunk keeps the four that aren't worldgen-only.
enum class HeightmapType : int { WorldSurfaceWG = 0, WorldSurface = 1, OceanFloorWG = 2, OceanFloor = 3, MotionBlocking = 4, MotionBlockingNoLeaves = 5 };

// Port of net.minecraft.world.level.levelgen.Heightmap: per column, the first y above the highest
// block that counts for the type (stored relative to minY, packed like SimpleBitStorage).
class Heightmap {
public:
    explicit Heightmap(HeightmapType type) : type(type) {}
    const HeightmapType type;
    // Heightmap.Types.isOpaque
    static bool isOpaque(HeightmapType type, BlockStateId state);
    // Heightmap.Types.getSerializationKey
    static const char* serializationKey(HeightmapType type);
    int firstAvailable(int x, int z, int minY) const { return values_[x + z * 16] + minY; }
    void setHeight(int x, int z, int height, int minY) { values_[x + z * 16] = static_cast<int16_t>(height - minY); }
    // SimpleBitStorage long array with ceillog2(height + 1) bits per column, and back.
    std::vector<uint64_t> raw(int worldHeight) const;
    bool setRaw(const std::vector<int64_t>& data, int worldHeight);

private:
    std::array<int16_t, 256> values_{};
};

class LevelChunk {
public:
    LevelChunk(ChunkPos pos, const DimensionInfo& dim, uint32_t defaultBiome, int biomeCount);

    ChunkPos pos() const { return pos_; }
    BlockStateId getBlockState(int x, int y, int z) const;
    BlockStateId setBlockState(int x, int y, int z, BlockStateId state); // local x/z, world y
    LevelChunkSection& section(int index) { return sections_[index]; }

    // Heightmap.primeHeightmaps for all four types, and one of them.
    void recalculateHeightmaps();
    Heightmap& heightmap(HeightmapType type);
    const Heightmap& heightmap(HeightmapType type) const;
    // Heightmap.getFirstAvailable in world coordinates (local x/z).
    int height(HeightmapType type, int x, int z) const { return heightmap(type).firstAvailable(x, z, dim_.minY); }
    // Index of the highest section with blocks, -1 if all air (ChunkAccess.getHighestFilledSectionIndex).
    int highestFilledSectionIndex() const;
    // ClientboundLevelChunkWithLightPacket body (after the packet id).
    // `writeLight` adds the ClientboundLightUpdatePacketData (from the level's light engine).
    void writeChunkWithLight(ByteWriter& out, const std::function<void(ByteWriter&)>& writeLight) const;

    const DimensionInfo& dimension() const { return dim_; }
    int sectionCount() const { return static_cast<int>(sections_.size()); }
    const LevelChunkSection& section(int index) const { return sections_[index]; }

    // Persistence state (SerializableChunkData).
    bool dirty = false;    // changed since last save
    bool readOnly = false; // loaded from data we couldn't fully understand: never written back
    std::string status = "minecraft:full";
    int64_t inhabitedTime = 0;
    std::optional<nbt::Compound> original; // tag it was loaded from, for fields we pass through
    // Light as loaded from disk, per light section (index 0 = one below the world), until the
    // level's light engine takes it (SerializableChunkData.read -> queueSectionData).
    struct StoredLight {
        std::optional<std::vector<int8_t>> sky, block;
    };
    std::vector<StoredLight> storedLight;
    // ChunkAccess.isLightCorrect ("isLightOn"): the saved light can be trusted. `lit`: the chunk
    // went through the LIGHT step here (its light sources were propagated, or its light loaded).
    bool lightCorrect = false;
    bool lit = false;
    light::ChunkSkyLightSources skyLightSources;
    // Block entities as their saved NBT (loaded from disk, or created with their block).
    struct BlockEntity {
        int x, y, z; // world coordinates
        uint32_t blockAtLoad;
        nbt::Compound tag;
    };
    std::vector<BlockEntity> blockEntities;
    BlockEntity* blockEntityAt(int x, int y, int z) {
        for (auto& be : blockEntities)
            if (be.x == x && be.y == y && be.z == z) return &be;
        return nullptr;
    }

    // LevelChunkTicks: scheduled block and fluid ticks in this chunk (absolute trigger times), and
    // saved ticks for blocks whose tick() isn't ported yet, kept as loaded.
    struct ScheduledTick {
        uint32_t block; // (the fluid type for fluid ticks)
        BlockPos pos;
        int64_t triggerTick;
        int priority;      // TickPriority value, lower runs first
        int64_t subTickOrder;
    };
    std::vector<ScheduledTick> blockTicks;
    std::vector<nbt::Compound> unportedBlockTicks;
    std::vector<ScheduledTick> fluidTicks;

    // Entity storage (entities/r.X.Z.mca). Entities we don't simulate yet are kept verbatim.
    std::vector<nbt::Compound> otherEntities;
    bool entitiesDirty = false;
    bool hadEntityData = false;

private:
    void updateHeightmap(Heightmap& map, int x, int y, int z, BlockStateId state);
    void writeHeightmaps(ByteWriter& out) const;

    ChunkPos pos_;
    DimensionInfo dim_;
    std::vector<LevelChunkSection> sections_;
    std::array<Heightmap, 4> heightmaps_{Heightmap(HeightmapType::WorldSurface), Heightmap(HeightmapType::OceanFloor),
                                         Heightmap(HeightmapType::MotionBlocking), Heightmap(HeightmapType::MotionBlockingNoLeaves)};
};

} // namespace mc
