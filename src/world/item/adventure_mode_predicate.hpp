#pragma once

#include <string_view>

#include "core/direction.hpp"
#include "nbt/tag.hpp"
#include "world/item/item_stack.hpp"

namespace mc {

class ServerLevel;

// Port of AdventureModePredicate / BlockPredicate.matches(BlockInWorld): which blocks an item may be
// placed on (minecraft:can_place_on) or may break (minecraft:can_break) in adventure mode.
namespace adventure {

// ItemStack.canPlaceOnBlockInAdventureMode / canBreakBlockInAdventureMode for `component`.
bool test(const ItemStack& stack, std::string_view component, ServerLevel& level, BlockPos pos);

// NbtUtils.compareNbt: `expected` is contained in `actual` (compounds by key, lists partially).
bool compareNbt(const nbt::Tag& expected, const nbt::Tag& actual, bool partialListMatches);

} // namespace adventure
} // namespace mc
