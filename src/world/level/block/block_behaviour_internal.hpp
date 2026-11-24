#pragma once

#include <optional>
#include <string_view>

#include "core/direction.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
class ServerLevel;

// Helpers block_behaviour.cpp shares with the other block behaviour files (random_tick.cpp,
// the tree features).
namespace block_behaviour::detail {

int intValue(BlockStateId s, std::string_view property, int def = 0);
// MultifaceBlock.canAttachTo: the face of the block next to `pos` towards it is full.
bool canAttachFace(ServerLevel& level, BlockPos pos, Direction towardsNeighbour);
// VineBlock.canSupportAtFace
bool vineSupportsFace(ServerLevel& level, BlockPos pos, Direction d);
bool isSpeleothemWithDirection(BlockStateId s, Direction tip);
bool isValidSpeleothemPlacement(uint32_t self, ServerLevel& level, BlockPos pos, Direction tip);
// ChorusPlantBlock.getStateWithConnections
BlockStateId chorusConnections(BlockStateId s, ServerLevel& level, BlockPos pos);
// LeavesBlock.updateDistance
BlockStateId leavesDistance(BlockStateId s, ServerLevel& level, BlockPos pos);

// NoteBlock.playNote (the block event, if the note block may sound)
void playNote(ServerLevel& level, BlockPos pos, BlockStateId state);
// RailState.place for the rail at `pos` (BaseRailBlock.updateDir); returns its new state.
BlockStateId railUpdateDir(ServerLevel& level, BlockPos pos, BlockStateId state, bool hasSignal, bool first);
// RailState.countPotentialConnections
int railPotentialConnections(ServerLevel& level, BlockPos pos, BlockStateId state);
// BaseRailBlock.shouldBeRemoved: no rigid support below (or for a slope, at its high end).
bool railShouldBeRemoved(ServerLevel& level, BlockPos pos, BlockStateId state);
// BellBlock.attemptToRing (with no ringing entity); `direction` defaults to the bell's facing.
void ringBell(ServerLevel& level, BlockPos pos, std::optional<Direction> direction);

// Scheduled ticks of the blocks in random_tick.cpp (cactus, farmland, leaves, cauldron drips, ...).
bool hasPlantTick(uint32_t block);
bool plantTick(ServerLevel& level, BlockPos pos, BlockStateId state); // false if not one of them

} // namespace block_behaviour::detail
} // namespace mc
