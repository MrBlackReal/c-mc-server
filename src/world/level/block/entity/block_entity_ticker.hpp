#pragma once

#include <string>
#include <string_view>

#include "core/direction.hpp"
#include "nbt/tag.hpp"
#include "world/item/item_stack.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
class ServerLevel;
}

// BlockEntityTicker ports, working on the block entity's saved NBT, plus the block entity
// behaviour the tickers share with interactions (jukebox songs, ...).
namespace mc::block_entity {

// Whether block entities of `type` tick (EntityBlock.getTicker) in our port.
bool hasTicker(std::string_view type);
// One tick of the block entity `be` at `pos`. Returns true if its NBT changed.
bool tick(ServerLevel& level, BlockPos pos, BlockStateId state, nbt::Compound& be);

// AbstractFurnaceBlockEntity.getRecipesToAwardAndPopExperience: experience orbs at (x, y, z) for
// everything the furnace cooked since, then forgets it.
// A beacon was removed: forget its beam state.
void forgetBeacon(ServerLevel& level, BlockPos pos);

void awardFurnaceExperience(ServerLevel& level, nbt::Compound& be, double x, double y, double z);

namespace jukebox {
// JukeboxSong.fromStack: the disc's minecraft:jukebox_song, or empty.
std::string songOf(const ItemStack& stack);
// JukeboxBlockEntity.setTheItem with a disc: stores it and starts its song.
void insert(ServerLevel& level, BlockPos pos, BlockStateId state, nbt::Compound& be, const ItemStack& disc);
// JukeboxBlockEntity.popOutTheItem: the disc jumps out and the song stops.
void popOut(ServerLevel& level, BlockPos pos, nbt::Compound& be);
// JukeboxBlockEntity.getComparatorOutput for the disc `stack`.
int comparatorOutput(const ItemStack& stack);
} // namespace jukebox

} // namespace mc::block_entity
