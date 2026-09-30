#pragma once

#include <optional>
#include <random>

#include "core/direction.hpp"
#include "util/json.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
class ServerLevel;
}

// The world-gen BlockPredicate and BlockStateProvider types, interpreting their JSON (used by
// data-driven block transformers now, and by features once world generation is ported).
namespace mc::levelgen {

// A HolderSet<Block> (RegistryCodecs.holderSet): "minecraft:x", "#minecraft:tag" or a list of ids.
bool matchesBlocks(const json::Value& set, uint32_t block);

// BlockPredicate.test at `pos` (offsets are relative to it).
bool testBlockPredicate(const json::Value& predicate, ServerLevel& level, BlockPos pos);

// BlockStateProvider.getOptionalState: the state to place at `pos`, or nothing.
std::optional<BlockStateId> optionalState(const json::Value& provider, ServerLevel& level, BlockPos pos, std::mt19937& random);

// BlockState.CODEC: {"id"/"Name": block, "properties"/"Properties": {...}} over the default state.
std::optional<BlockStateId> parseBlockState(const json::Value& v);

} // namespace mc::levelgen
