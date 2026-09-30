#pragma once

#include <memory>
#include <string>

#include "nbt/tag.hpp"
#include "world/level/chunk/level_chunk.hpp"

namespace mc {

inline constexpr int kDataVersion = 5023; // SharedConstants.getCurrentVersion().dataVersion()

// Port of SerializableChunkData (region/*.mca chunk tags).
namespace chunk_serializer {

struct LoadResult {
    std::unique_ptr<LevelChunk> chunk;
    std::string problem; // non-empty: chunk marked read-only, with the reason
};

LoadResult read(const nbt::Compound& tag, ChunkPos expected, const DimensionInfo& dim, uint32_t defaultBiome, int biomeCount);
// `gameTime` becomes LastUpdate.
// `light`: where the chunk's light comes from (null: none saved).
nbt::Compound write(const LevelChunk& chunk, int64_t gameTime, const light::LevelLightEngine* light);

// Block state <-> NBT as BlockState.CODEC encodes it in 26.x: a plain name for a block's default
// state, otherwise {id, properties}. Also accepts the older {Name, Properties} form.
nbt::Tag writeBlockState(BlockStateId state);
std::optional<BlockStateId> readBlockState(const nbt::Tag& tag);

} // namespace chunk_serializer
} // namespace mc
