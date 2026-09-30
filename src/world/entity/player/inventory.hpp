#pragma once

#include <array>

#include "world/item/item_stack.hpp"

namespace mc {

// EquipmentSlot ordinals, as used by ClientboundSetEquipmentPacket and ItemInfo::equipSlot.
enum class EquipmentSlot : int8_t { MainHand = 0, OffHand = 1, Feet = 2, Legs = 3, Chest = 4, Head = 5 };

// Port of net.minecraft.world.entity.player.Inventory.
class Inventory {
public:
    static constexpr int kMainSize = 36;  // 0-8 hotbar, 9-35 main
    static constexpr int kHotbarSize = 9;
    static constexpr int kOffhandSlot = 40; // Inventory slot index used by SWAP clicks

    ItemStack& item(int i) { return items_[i]; }
    // Inventory.getItem/setItem: 0-35 main, 36-39 armor (feet..head), 40 offhand.
    ItemStack& slot(int i);
    ItemStack& equipment(EquipmentSlot s);

    int selected() const { return selected_; }
    void setSelected(int s) { selected_ = s; }
    ItemStack& selectedItem() { return items_[selected_]; }

    int getFreeSlot() const;
    int findSlotMatchingItem(const ItemStack& stack) const;
    int getSuitableHotbarSlot() const;
    void addAndPickItem(const ItemStack& stack);
    void pickSlot(int slot);
    // Inventory.add: merges into existing stacks, then free slots; `stack` keeps what didn't fit.
    // Returns false if something was left.
    bool add(ItemStack& stack);

private:
    std::array<ItemStack, kMainSize> items_{};
    std::array<ItemStack, 4> armor_{}; // feet, legs, chest, head
    ItemStack offhand_;
    int selected_ = 0;
};

} // namespace mc
