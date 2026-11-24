#pragma once

#include "core/direction.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
class ServerLevel;

// DispenserBlock and DropperBlock: triggered by a rising signal, they dispense 4 ticks later.
namespace block_behaviour::dispenser {

// DispenserBlock.neighborChanged: the triggered state follows the signal (also from above).
void neighborChanged(ServerLevel& level, BlockPos pos, BlockStateId state);
// DispenserBlock.tick -> dispenseFrom (DropperBlock: into a container in front, or out).
void tick(ServerLevel& level, BlockPos pos, BlockStateId state);

} // namespace block_behaviour::dispenser
} // namespace mc
