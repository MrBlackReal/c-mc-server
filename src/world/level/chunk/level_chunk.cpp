#include "world/level/chunk/level_chunk.hpp"

#include <algorithm>
#include <bit>

#include "core/registries.hpp"
#include "world/level/block/entity/block_entity_components.hpp"

namespace mc {

// ---------------------------------------------------------------- LevelChunkSection

LevelChunkSection::LevelChunkSection(uint32_t biome, int biomeCount)
    : states_(PalettedContainer::blockStrategy(), blocks::defaultState(gen::block::AIR)),
      biomes_(PalettedContainer::biomeStrategy(biomeCount), biome) {}

BlockStateId LevelChunkSection::setBlockState(int x, int y, int z, BlockStateId state) {
    BlockStateId old = states_.set(x, y, z, state);
    if (old == state) return old;
    nonEmptyBlockCount_ += static_cast<int16_t>(!blocks::isAir(state)) - static_cast<int16_t>(!blocks::isAir(old));
    fluidCount_ += static_cast<int16_t>(blocks::hasFluid(state)) - static_cast<int16_t>(blocks::hasFluid(old));
    return old;
}

void LevelChunkSection::fill(BlockStateId state) {
    states_.fill(state);
    nonEmptyBlockCount_ = blocks::isAir(state) ? 0 : 4096;
    fluidCount_ = blocks::hasFluid(state) ? 4096 : 0;
}

void LevelChunkSection::recalcBlockCounts() {
    nonEmptyBlockCount_ = 0;
    fluidCount_ = 0;
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                BlockStateId s = states_.get(x, y, z);
                nonEmptyBlockCount_ += !blocks::isAir(s);
                fluidCount_ += blocks::hasFluid(s);
            }
}

void LevelChunkSection::write(ByteWriter& out) const {
    out.writeShort(nonEmptyBlockCount_);
    out.writeShort(fluidCount_);
    states_.write(out);
    biomes_.write(out);
}

// ---------------------------------------------------------------- LevelChunk

LevelChunk::LevelChunk(ChunkPos pos, const DimensionInfo& dim, uint32_t defaultBiome, int biomeCount)
    : skyLightSources(dim.minY), pos_(pos), dim_(dim) {
    sections_.reserve(dim.sectionCount());
    for (int i = 0; i < dim.sectionCount(); i++) sections_.emplace_back(defaultBiome, biomeCount);
}

BlockStateId LevelChunk::getBlockState(int x, int y, int z) const {
    int rel = y - dim_.minY;
    if (rel < 0 || rel >= dim_.height) return blocks::defaultState(gen::block::VOID_AIR);
    return sections_[rel >> 4].getBlockState(x, rel & 15, z);
}

BlockStateId LevelChunk::setBlockState(int x, int y, int z, BlockStateId state) {
    int rel = y - dim_.minY;
    if (rel < 0 || rel >= dim_.height) return state;
    BlockStateId old = sections_[rel >> 4].setBlockState(x, rel & 15, z, state);
    if (old != state) {
        dirty = true;
        // LevelChunk.setBlockState: a different block loses the old block entity, and a block
        // with one gets a fresh one.
        uint32_t block = blocks::blockOf(state);
        if (blocks::blockOf(old) != block) {
            int wx = pos_.x * 16 + x, wz = pos_.z * 16 + z;
            std::erase_if(blockEntities, [&](const BlockEntity& be) { return be.x == wx && be.y == y && be.z == wz; });
            if (!blocks::behaviour(block).blockEntity.empty())
                blockEntities.push_back({wx, y, wz, block, block_entity::createDefault(block, {wx, y, wz})});
        }
        for (auto& map : heightmaps_) updateHeightmap(map, x, y, z, state);
    }
    return old;
}

// ---------------------------------------------------------------- heightmaps

bool Heightmap::isOpaque(HeightmapType type, BlockStateId state) {
    switch (type) {
    case HeightmapType::WorldSurfaceWG:
    case HeightmapType::WorldSurface: return !blocks::isAir(state); // NOT_AIR
    case HeightmapType::OceanFloorWG:
    case HeightmapType::OceanFloor: { // MATERIAL_MOTION_BLOCKING: BlockStateBase.blocksMotion
        uint32_t block = blocks::blockOf(state);
        return block != gen::block::COBWEB && block != gen::block::BAMBOO_SAPLING && (blocks::flags(state) & blocks::kSolid);
    }
    case HeightmapType::MotionBlocking: return blocks::stateHasTag(state, "minecraft:blocks_motion_in_heightmap") || blocks::hasFluid(state);
    case HeightmapType::MotionBlockingNoLeaves:
        return blocks::stateHasTag(state, "minecraft:blocks_motion_in_heightmap_no_leaves") || blocks::hasFluid(state);
    }
    return false;
}

const char* Heightmap::serializationKey(HeightmapType type) {
    static const char* kKeys[] = {"WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG", "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"};
    return kKeys[static_cast<int>(type)];
}

std::vector<uint64_t> Heightmap::raw(int worldHeight) const {
    int bits = std::bit_width(static_cast<uint32_t>(worldHeight)); // Mth.ceillog2(height + 1)
    int perLong = 64 / bits;
    std::vector<uint64_t> packed((256 + perLong - 1) / perLong, 0);
    for (int i = 0; i < 256; i++) packed[i / perLong] |= static_cast<uint64_t>(values_[i]) << ((i % perLong) * bits);
    return packed;
}

bool Heightmap::setRaw(const std::vector<int64_t>& data, int worldHeight) {
    int bits = std::bit_width(static_cast<uint32_t>(worldHeight));
    int perLong = 64 / bits;
    if (static_cast<int>(data.size()) != (256 + perLong - 1) / perLong) return false; // (Heightmap.setRawData: a wrong size is re-primed)
    uint64_t mask = (1ull << bits) - 1;
    for (int i = 0; i < 256; i++) values_[i] = static_cast<int16_t>((static_cast<uint64_t>(data[i / perLong]) >> ((i % perLong) * bits)) & mask);
    return true;
}

Heightmap& LevelChunk::heightmap(HeightmapType type) {
    for (auto& m : heightmaps_)
        if (m.type == type) return m;
    return heightmaps_[0];
}
const Heightmap& LevelChunk::heightmap(HeightmapType type) const { return const_cast<LevelChunk*>(this)->heightmap(type); }

int LevelChunk::highestFilledSectionIndex() const {
    for (int i = static_cast<int>(sections_.size()) - 1; i >= 0; i--)
        if (!sections_[i].hasOnlyAir()) return i;
    return -1;
}

// Heightmap.update
void LevelChunk::updateHeightmap(Heightmap& map, int x, int y, int z, BlockStateId state) {
    int first = map.firstAvailable(x, z, dim_.minY);
    if (y <= first - 2) return;
    if (Heightmap::isOpaque(map.type, state)) {
        if (y >= first) map.setHeight(x, z, y + 1, dim_.minY);
    } else if (first - 1 == y) {
        for (int yy = y - 1; yy >= dim_.minY; yy--) {
            if (Heightmap::isOpaque(map.type, getBlockState(x, yy, z))) {
                map.setHeight(x, z, yy + 1, dim_.minY);
                return;
            }
        }
        map.setHeight(x, z, dim_.minY, dim_.minY);
    }
}

// Heightmap.primeHeightmaps
void LevelChunk::recalculateHeightmaps() {
    int top = highestFilledSectionIndex();
    int highest = (top < 0 ? dim_.minY : dim_.minY + top * 16) + 16; // getHighestSectionPosition + 16
    for (int x = 0; x < 16; x++) {
        for (int z = 0; z < 16; z++) {
            bool done[4] = {false, false, false, false};
            int remaining = 4;
            for (auto& m : heightmaps_) m.setHeight(x, z, dim_.minY, dim_.minY);
            for (int y = highest - 1; y >= dim_.minY && remaining > 0; y--) {
                BlockStateId state = getBlockState(x, y, z);
                if (blocks::blockOf(state) == gen::block::AIR) continue;
                for (int i = 0; i < 4; i++) {
                    if (done[i] || !Heightmap::isOpaque(heightmaps_[i].type, state)) continue;
                    heightmaps_[i].setHeight(x, z, y + 1, dim_.minY);
                    done[i] = true;
                    remaining--;
                }
            }
        }
    }
}

void LevelChunk::writeHeightmaps(ByteWriter& out) const {
    // The types with Usage.CLIENT, as a map of type id -> raw data.
    const HeightmapType types[] = {HeightmapType::WorldSurface, HeightmapType::MotionBlocking, HeightmapType::MotionBlockingNoLeaves};
    out.writeVarInt(3);
    for (auto t : types) {
        out.writeVarInt(static_cast<int>(t));
        out.writeLongArray(heightmap(t).raw(dim_.height));
    }
}

void LevelChunk::writeChunkWithLight(ByteWriter& out, const std::function<void(ByteWriter&)>& writeLight) const {
    out.writeInt(pos_.x);
    out.writeInt(pos_.z);
    writeHeightmaps(out);
    ByteWriter sectionData;
    for (const auto& s : sections_) s.write(sectionData);
    out.writeByteArray(sectionData.data());
    // ClientboundLevelChunkPacketData.BlockEntityInfo: type and BlockEntity.getUpdateTag.
    int count = 0;
    for (const auto& be : blockEntities) count += blocks::blockOf(getBlockState(be.x & 15, be.y, be.z & 15)) == be.blockAtLoad;
    out.writeVarInt(count);
    for (const auto& be : blockEntities) {
        if (blocks::blockOf(getBlockState(be.x & 15, be.y, be.z & 15)) != be.blockAtLoad) continue;
        out.writeByte(static_cast<uint8_t>((be.x & 15) << 4 | (be.z & 15)));
        out.writeShort(static_cast<int16_t>(be.y));
        out.writeVarInt(registries::id("minecraft:block_entity_type", be.tag.getString("id")).value_or(0));
        nbt::Compound data = block_entity::getUpdateTag(be.tag);
        if (data.empty()) {
            out.writeByte(0); // no tag
        } else {
            nbt::writeNetwork(out, nbt::Tag(std::move(data)));
        }
    }
    writeLight(out);
}

} // namespace mc
