#pragma once

#include <optional>

#include "core/direction.hpp"
#include "world/level/block/block_state.hpp"
#include "world/phys/aabb.hpp"

namespace mc {
class ServerLevel;

// The redstone overrides of block behaviour (redstone_blocks.cpp), called from block_behaviour's
// dispatch. Functions returning bool/optional say whether the block had such an override.
namespace block_behaviour::redstone_blocks {

bool onPlace(ServerLevel& level, BlockPos pos, BlockStateId state, BlockStateId old, bool movedByPiston);
void affectNeighborsAfterRemoval(ServerLevel& level, BlockPos pos, BlockStateId state, bool movedByPiston);
bool neighborChanged(ServerLevel& level, BlockPos pos, BlockStateId state, uint32_t sourceBlock, bool movedByPiston);
bool hasTick(uint32_t block);
bool tick(ServerLevel& level, BlockPos pos, BlockStateId state);
// The block's own updateShape, if it's a redstone one (diodes: support and repeater locks;
// observers: start a pulse when watched blocks change).
std::optional<BlockStateId> updateShape(BlockStateId state, ServerLevel& level, BlockPos pos, Direction toNeighbour, BlockStateId neighbour);
// getStateForPlacement's signal-dependent parts (powered doors, lit lamps, locked repeaters, ...).
BlockStateId placementState(uint32_t block, BlockStateId state, ServerLevel& level, BlockPos pos);
// Block.setPlacedBy: diodes powered from the start tick right away.
void setPlacedBy(ServerLevel& level, BlockPos pos, BlockStateId state);
// useWithoutItem: true = consumed, false = pass, empty = not a redstone block.
std::optional<bool> use(ServerLevel& level, BlockPos pos, BlockStateId state, int playerId, bool mayBuild);
// BlockBehaviour.entityInside (pressure plates, tripwire) for an entity with box `entityBox`.
void entityInside(ServerLevel& level, BlockPos pos, BlockStateId state, const AABB& entityBox);
// LecternBlock.tick and signalPageChange.
void lecternTick(ServerLevel& level, BlockPos pos, BlockStateId state);
void lecternSignalPageChange(ServerLevel& level, BlockPos pos, BlockStateId state);
// DaylightDetectorBlock.updateSignalStrength: the power follows the sky light and the sun.
void daylightUpdate(ServerLevel& level, BlockPos pos, BlockStateId state);
// Drops the per-level redstone torch burnout history.
void forgetLevel(const ServerLevel& level);

} // namespace block_behaviour::redstone_blocks
} // namespace mc
