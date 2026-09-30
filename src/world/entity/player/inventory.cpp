#include "world/entity/player/inventory.hpp"

#include <algorithm>

namespace mc {

ItemStack& Inventory::slot(int i) {
    if (i < kMainSize) return items_[i];
    if (i < kMainSize + 4) return armor_[i - kMainSize];
    return offhand_;
}

ItemStack& Inventory::equipment(EquipmentSlot s) {
    switch (s) {
    case EquipmentSlot::MainHand: return items_[selected_];
    case EquipmentSlot::OffHand: return offhand_;
    case EquipmentSlot::Feet: return armor_[0];
    case EquipmentSlot::Legs: return armor_[1];
    case EquipmentSlot::Chest: return armor_[2];
    case EquipmentSlot::Head: return armor_[3];
    }
    return offhand_;
}

int Inventory::getFreeSlot() const {
    for (int i = 0; i < kMainSize; i++)
        if (items_[i].isEmpty()) return i;
    return -1;
}

int Inventory::findSlotMatchingItem(const ItemStack& stack) const {
    for (int i = 0; i < kMainSize; i++)
        if (!items_[i].isEmpty() && ItemStack::isSameItemSameComponents(stack, items_[i])) return i;
    return -1;
}

int Inventory::getSuitableHotbarSlot() const {
    for (int i = 0; i < kHotbarSize; i++) {
        int index = (selected_ + i) % kHotbarSize;
        if (items_[index].isEmpty()) return index;
    }
    // Vanilla then prefers a non-enchanted slot; we don't decode enchantments yet.
    return selected_;
}

void Inventory::addAndPickItem(const ItemStack& stack) {
    selected_ = getSuitableHotbarSlot();
    if (!items_[selected_].isEmpty()) {
        int free = getFreeSlot();
        if (free != -1) items_[free] = items_[selected_];
    }
    items_[selected_] = stack;
}

void Inventory::pickSlot(int slot) {
    selected_ = getSuitableHotbarSlot();
    std::swap(items_[selected_], items_[slot]);
}

bool Inventory::add(ItemStack& stack) {
    if (stack.isEmpty()) return true;
    // Selected slot and offhand first, then everything else (Inventory.getSlotWithRemainingSpace).
    auto merge = [&](ItemStack& target) {
        if (target.isEmpty() || !ItemStack::isSameItemSameComponents(target, stack)) return;
        int room = target.maxStackSize() - target.count;
        int moved = std::min(room, stack.count);
        if (moved <= 0) return;
        target.grow(moved);
        stack.shrink(moved);
    };
    if (stack.isStackable()) {
        merge(items_[selected_]);
        merge(offhand_);
        for (int i = 0; i < kMainSize && !stack.isEmpty(); i++) merge(items_[i]);
    }
    while (!stack.isEmpty()) {
        int free = getFreeSlot();
        if (free < 0) break;
        items_[free] = stack.split(stack.maxStackSize());
    }
    stack.clearIfEmpty();
    return stack.isEmpty();
}

} // namespace mc
