#pragma once

#include <memory>
#include <optional>

#include "core/direction.hpp"
#include "world/item/item_stack.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
class ServerLevel;
class Container;
class ItemEntity;

// Port of HopperBlockEntity: hoppers moving items between containers, and the container lookup
// and insertion droppers share with them.
namespace hopper {

// HopperBlockEntity.getContainerAt (block containers; there are no container entities yet):
// chests (both halves of a double chest), barrels, hoppers, dispensers, droppers, shulker boxes,
// furnaces, brewing stands, crafters and composters.
std::shared_ptr<Container> getContainerAt(ServerLevel& level, BlockPos pos);
// HopperBlockEntity.addItem(from, container, stack, face): what's left of `stack` after putting
// as much as fits into `container` (through `face`, for worldly containers).
ItemStack addItem(ServerLevel& level, Container* from, Container& container, ItemStack stack, std::optional<Direction> face);
// HopperBlockEntity.pushItemsTick
void pushItemsTick(ServerLevel& level, BlockPos pos, BlockStateId state);
// HopperBlockEntity.entityInside: an item entity in the hopper's suck area goes in.
void entityInside(ServerLevel& level, BlockPos pos, BlockStateId state, ItemEntity& entity);

} // namespace hopper
} // namespace mc
