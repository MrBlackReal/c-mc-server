#pragma once

#include <optional>

#include "nbt/tag.hpp"
#include "world/item/item_stack.hpp"

namespace mc::item_nbt {

// ItemStack.CODEC: {id, count, components}. Components are converted between the disk (Codec)
// and network (StreamCodec) forms by core/component/data_components. Values that don't convert
// are preserved: unreadable disk values in ItemStack::diskComponents (not sent to clients),
// undecodable network values under "cmc:network_components".
nbt::Compound write(const ItemStack& stack);
std::optional<ItemStack> read(const nbt::Compound& tag);

} // namespace mc::item_nbt
