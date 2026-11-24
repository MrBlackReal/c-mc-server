#pragma once

#include <optional>

#include "core/direction.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
class ServerLevel;

// Pistons: PistonBaseBlock, PistonHeadBlock, MovingPistonBlock, PistonStructureResolver and
// PistonMovingBlockEntity (whose live state ServerLevel keeps as MovingPiston).
namespace block_behaviour::piston {

bool isPistonBase(uint32_t block);
// PistonBaseBlock.checkIfExtend: queue the block event that extends or retracts it.
void checkIfExtend(ServerLevel& level, BlockPos pos, BlockStateId state);
// PistonBaseBlock.triggerEvent: the extension or retraction itself (true if clients should play it).
bool triggerEvent(ServerLevel& level, BlockPos pos, BlockStateId state, int b0, int b1);
// PistonHeadBlock
bool headCanSurvive(ServerLevel& level, BlockPos pos, BlockStateId state);
void headNeighborChanged(ServerLevel& level, BlockPos pos, BlockStateId state, uint32_t sourceBlock);
void headAffectNeighborsAfterRemoval(ServerLevel& level, BlockPos pos, BlockStateId state);
// PistonMovingBlockEntity.tick and finalTick.
void tickMoving(ServerLevel& level, BlockPos pos);
void finalTick(ServerLevel& level, BlockPos pos);

} // namespace block_behaviour::piston
} // namespace mc
