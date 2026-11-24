#pragma once

#include <optional>

#include "core/direction.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {

class ServerLevel;
class Container;

// Redstone signals: the BlockBehaviour signal methods (isSignalSource, getSignal, getDirectSignal,
// hasAnalogOutputSignal, getAnalogOutputSignal) dispatched by block class, and SignalGetter's
// level-wide queries built on them.
namespace redstone {

// Redstone.SIGNAL_MAX
inline constexpr int kSignalMax = 15;

// BlockState.isSignalSource (redstone wire stops being one while it measures its own input).
bool isSignalSource(BlockStateId state);
// BlockState.getSignal / getDirectSignal: the weak and strong power `state` at `pos` gives towards
// `direction` (the direction from the neighbour asking to `pos`, reversed: vanilla's convention).
int getSignal(ServerLevel& level, BlockPos pos, BlockStateId state, Direction direction);
int getDirectSignal(ServerLevel& level, BlockPos pos, BlockStateId state, Direction direction);
// BlockBehaviour.ownSignal
int ownSignal(ServerLevel& level, BlockPos pos, BlockStateId state);
// BlockState.shouldRedstoneWireConnectTo
bool shouldWireConnectTo(BlockStateId state, std::optional<Direction> direction);
// BlockState.hasAnalogOutputSignal / getAnalogOutputSignal (what comparators read).
bool hasAnalogOutputSignal(BlockStateId state);
int getAnalogOutputSignal(ServerLevel& level, BlockPos pos, BlockStateId state, Direction direction);
// AbstractContainerMenu.getRedstoneSignalFromContainer
int signalFromContainer(Container* container);

// SignalGetter
int getDirectSignal(ServerLevel& level, BlockPos pos, Direction direction);
int getDirectSignalTo(ServerLevel& level, BlockPos pos);
int getControlInputSignal(ServerLevel& level, BlockPos pos, Direction direction, bool onlyDiodes);
int getSignal(ServerLevel& level, BlockPos pos, Direction direction);
inline bool hasSignal(ServerLevel& level, BlockPos pos, Direction direction) { return getSignal(level, pos, direction) > 0; }
bool hasNeighborSignal(ServerLevel& level, BlockPos pos);
int getBestNeighborSignal(ServerLevel& level, BlockPos pos);

// RedstoneWireBlock.getBlockSignal: the strongest signal into `pos`, wires not counted.
int wireBlockSignal(ServerLevel& level, BlockPos pos);

// DiodeBlock.isDiode
bool isDiode(BlockStateId state);

// RedstoneWireBlock's connections (getConnectionState and friends), shared with shape updates.
namespace wire {
bool connected(BlockStateId state, Direction d);
bool isDot(BlockStateId state);
bool isCross(BlockStateId state);
BlockStateId crossState(BlockStateId state); // crossState with `state`'s power
BlockStateId connectionState(ServerLevel& level, BlockStateId state, BlockPos pos);
std::string_view connectingSide(ServerLevel& level, BlockPos pos, Direction direction);
bool canSurviveOn(BlockStateId below);
} // namespace wire

} // namespace redstone
} // namespace mc
