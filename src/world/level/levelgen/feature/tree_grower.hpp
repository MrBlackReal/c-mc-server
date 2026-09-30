#pragma once

#include <optional>

#include "core/direction.hpp"
#include "util/legacy_random.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
class ServerLevel;

// Port of TreeGrower and SaplingBlock.advanceTree.
namespace tree_grower {

// SaplingBlock.advanceTree: stage 0 -> 1, then the tree grows (TreeGrower.growTree).
void advanceTree(ServerLevel& level, BlockPos pos, BlockStateId sapling, LegacyRandom& random);
// TreeGrower.growTree for the sapling's grower (2x2 mega trees first). True if a tree grew.
bool growTree(ServerLevel& level, BlockPos pos, BlockStateId sapling, LegacyRandom& random);
// TreeGrower.getMinimumHeight: the shortest tree's trunk base height (none for 2x2-only growers).
std::optional<int> minimumHeight(BlockStateId sapling);
// TreeGrower.canGrow (for bone meal): a 2x2-only grower needs its 2x2.
bool canGrow(ServerLevel& level, BlockPos pos, BlockStateId sapling);

} // namespace tree_grower
} // namespace mc
