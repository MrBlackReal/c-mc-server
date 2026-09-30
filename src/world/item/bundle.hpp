#pragma once

#include "world/item/item_stack.hpp"

namespace mc {
class ServerPlayer;
class Slot;
}

// Port of BundleItem's inventory behaviour and BundleContents.Mutable: bundles take items clicked
// onto them (and give them back on a right click), weighing each stack as count / max stack size.
namespace mc::bundle {

// Item.overrideStackedOnOther: the carried stack `self` clicked onto `slot`. True if the bundle
// handled the click (the normal pickup/place logic is skipped).
bool overrideStackedOnOther(ItemStack& self, Slot& slot, bool primary, ServerPlayer& player);
// Item.overrideOtherStackedOnMe: `other` (the carried stack) clicked onto the bundle `self` in `slot`.
bool overrideOtherStackedOnMe(ItemStack& self, ItemStack& other, Slot& slot, bool primary, ServerPlayer& player);
// BundleItem.toggleSelectedItem
void toggleSelectedItem(ItemStack& stack, int selected);

} // namespace mc::bundle
