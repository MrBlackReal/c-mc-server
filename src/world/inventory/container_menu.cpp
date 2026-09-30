#include "world/inventory/container_menu.hpp"

#include <algorithm>
#include <cmath>

#include "generated/packet_ids.hpp"
#include "network/connection.hpp"
#include "server/level/server_player.hpp"
#include "world/item/bundle.hpp"

namespace mc {

namespace cb = packets::play_cb;

// ---------------------------------------------------------------- Container, Slot

ItemStack Container::removeItem(int slot, int amount) {
    if (slot < 0 || slot >= size() || item(slot).isEmpty() || amount <= 0) return {};
    ItemStack out = item(slot).split(amount);
    item(slot).clearIfEmpty();
    if (!out.isEmpty()) setChanged();
    return out;
}

std::optional<ItemStack> Slot::tryRemove(int amount, int maxAmount, ServerPlayer& player) {
    if (!mayPickup(player)) return std::nullopt;
    if (!allowModification(player) && maxAmount < item().count) return std::nullopt;
    ItemStack result = remove(std::min(amount, maxAmount));
    if (result.isEmpty()) return std::nullopt;
    if (item().isEmpty()) setByPlayer(ItemStack{}, result);
    return result;
}

ItemStack Slot::safeTake(int amount, int maxAmount, ServerPlayer& player) {
    auto result = tryRemove(amount, maxAmount, player);
    if (result) onTake(player, *result);
    return result.value_or(ItemStack{});
}

ItemStack Slot::safeInsert(ItemStack stack, int amount) {
    if (stack.isEmpty() || !mayPlace(stack)) return stack;
    ItemStack current = item();
    int transfer = std::min(std::min(amount, stack.count), maxStackSize(stack) - (current.isEmpty() ? 0 : current.count));
    if (transfer <= 0) return stack;
    if (current.isEmpty()) {
        setByPlayer(stack.split(transfer));
    } else if (ItemStack::isSameItemSameComponents(current, stack)) {
        stack.shrink(transfer);
        current.grow(transfer);
        setByPlayer(current);
    }
    stack.clearIfEmpty();
    return stack;
}

void placeItemBackInInventory(ServerPlayer& player, ItemStack stack) {
    if (player.disconnected) { // AbstractContainerMenu.dropOrPlaceInInventory
        player.drop(std::move(stack), false);
        return;
    }
    Inventory& inv = player.inventory;
    while (!stack.isEmpty()) {
        int slot = -1; // getSlotWithRemainingSpace: the selected slot, the offhand, then the main inventory
        auto hasSpace = [&](ItemStack& s) { return !s.isEmpty() && ItemStack::isSameItemSameComponents(s, stack) && s.isStackable() && s.count < s.maxStackSize(); };
        if (hasSpace(inv.selectedItem())) slot = inv.selected();
        else if (hasSpace(inv.equipment(EquipmentSlot::OffHand))) slot = Inventory::kOffhandSlot;
        else
            for (int i = 0; i < Inventory::kMainSize && slot < 0; i++)
                if (hasSpace(inv.item(i))) slot = i;
        if (slot < 0) slot = inv.getFreeSlot();
        if (slot < 0) {
            player.drop(std::move(stack), false);
            return;
        }
        ItemStack& target = inv.slot(slot);
        int space = stack.maxStackSize() - (target.isEmpty() ? 0 : target.count);
        ItemStack part = stack.split(space);
        if (target.isEmpty()) target = part;
        else target.grow(part.count);
        stack.clearIfEmpty();
    }
}

// ---------------------------------------------------------------- AbstractContainerMenu

bool AbstractContainerMenu::RemoteSlot::matches(const ItemStack& local) {
    if (stack) return ItemStack::matches(*stack, local);
    if (hash && hash->matches(local)) {
        stack = local;
        return true;
    }
    return false;
}

Slot& AbstractContainerMenu::addSlot(std::unique_ptr<Slot> slot) {
    slot->index = static_cast<int>(slots.size());
    slots.push_back(std::move(slot));
    remoteSlots_.emplace_back();
    return *slots.back();
}

void AbstractContainerMenu::addStandardInventorySlots(Container& inventory) {
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 9; x++) addSlot(std::make_unique<Slot>(inventory, x + (y + 1) * 9));
    for (int x = 0; x < 9; x++) addSlot(std::make_unique<Slot>(inventory, x));
}

void AbstractContainerMenu::setSynchronizer(Connection& conn) {
    conn_ = &conn;
    sendAllDataToRemote();
}

void AbstractContainerMenu::sendSlot(int slot, const ItemStack& stack) {
    if (!conn_) return;
    Packet p(cb::CONTAINER_SET_SLOT);
    p.writeVarInt(containerId);
    p.writeVarInt(incrementStateId());
    p.writeShort(static_cast<int16_t>(slot));
    stack.write(p);
    conn_->send(p);
}

void AbstractContainerMenu::sendAllDataToRemote() {
    for (size_t i = 0; i < slots.size(); i++) remoteSlots_[i].force(slots[i]->item());
    remoteCarried_.force(carried);
    for (size_t i = 0; i < dataSlots_.size(); i++) remoteData_[i] = dataSlots_[i]();
    if (!conn_) return;
    Packet p(cb::CONTAINER_SET_CONTENT);
    p.writeVarInt(containerId);
    p.writeVarInt(incrementStateId());
    p.writeVarInt(static_cast<int32_t>(slots.size()));
    for (auto& s : slots) s->item().write(p);
    carried.write(p);
    conn_->send(p);
    for (size_t i = 0; i < dataSlots_.size(); i++) {
        Packet d(cb::CONTAINER_SET_DATA);
        d.writeVarInt(containerId);
        d.writeShort(static_cast<int16_t>(i));
        d.writeShort(static_cast<int16_t>(remoteData_[i]));
        conn_->send(d);
    }
}

void AbstractContainerMenu::broadcastChanges() {
    if (suppressRemote_ || !conn_) return;
    for (size_t i = 0; i < slots.size(); i++) {
        const ItemStack& current = slots[i]->item();
        if (remoteSlots_[i].matches(current)) continue;
        remoteSlots_[i].force(current);
        sendSlot(static_cast<int>(i), current);
    }
    if (!remoteCarried_.matches(carried)) {
        remoteCarried_.force(carried);
        Packet p(cb::SET_CURSOR_ITEM);
        carried.write(p);
        conn_->send(p);
    }
    for (size_t i = 0; i < dataSlots_.size(); i++) {
        int value = dataSlots_[i]();
        if (remoteData_[i] == value) continue;
        remoteData_[i] = value;
        Packet d(cb::CONTAINER_SET_DATA);
        d.writeVarInt(containerId);
        d.writeShort(static_cast<int16_t>(i));
        d.writeShort(static_cast<int16_t>(value));
        conn_->send(d);
    }
}

void AbstractContainerMenu::transferState(AbstractContainerMenu& other) {
    for (size_t i = 0; i < slots.size(); i++)
        for (size_t j = 0; j < other.slots.size(); j++)
            if (&other.slots[j]->container == &slots[i]->container && other.slots[j]->containerSlot == slots[i]->containerSlot)
                remoteSlots_[i] = other.remoteSlots_[j];
}

void AbstractContainerMenu::removed(ServerPlayer& player) {
    if (!carried.isEmpty()) placeItemBackInInventory(player, std::exchange(carried, ItemStack{}));
}

void AbstractContainerMenu::setSelectedBundleItemIndex(int slotIndex, int selected) {
    if (slotIndex >= 0 && slotIndex < static_cast<int>(slots.size())) bundle::toggleSelectedItem(slots[slotIndex]->item(), selected);
}

void AbstractContainerMenu::clearContainer(ServerPlayer& player, Container& container) {
    for (int i = 0; i < container.size(); i++) {
        ItemStack s = std::exchange(container.item(i), ItemStack{}); // removeItemNoUpdate
        if (!s.isEmpty()) placeItemBackInInventory(player, std::move(s));
    }
}

bool AbstractContainerMenu::moveItemStackTo(ItemStack& stack, int start, int end, bool backwards) {
    bool changed = false;
    if (stack.isStackable()) {
        for (int i = backwards ? end - 1 : start; !stack.isEmpty() && (backwards ? i >= start : i < end); i += backwards ? -1 : 1) {
            Slot& s = *slots[i];
            ItemStack& target = s.item();
            if (target.isEmpty() || !ItemStack::isSameItemSameComponents(stack, target)) continue;
            int total = target.count + stack.count;
            int max = s.maxStackSize(target);
            if (total <= max) {
                stack.count = 0;
                target.count = total;
                s.setChanged();
                changed = true;
            } else if (target.count < max) {
                stack.shrink(max - target.count);
                target.count = max;
                s.setChanged();
                changed = true;
            }
        }
    }
    if (!stack.isEmpty()) {
        for (int i = backwards ? end - 1 : start; backwards ? i >= start : i < end; i += backwards ? -1 : 1) {
            Slot& s = *slots[i];
            if (s.item().isEmpty() && s.mayPlace(stack)) {
                s.setByPlayer(stack.split(std::min(stack.count, s.maxStackSize(stack))));
                s.setChanged();
                changed = true;
                break;
            }
        }
    }
    stack.clearIfEmpty();
    return changed;
}

namespace {
bool canItemQuickReplace(Slot* slot, const ItemStack& stack, bool ignoreSize) {
    bool empty = slot == nullptr || !slot->hasItem();
    if (!empty && ItemStack::isSameItemSameComponents(stack, slot->item()))
        return slot->item().count + (ignoreSize ? 0 : stack.count) <= stack.maxStackSize();
    return empty;
}
} // namespace

void AbstractContainerMenu::clicked(int slotIndex, int button, ClickType type, ServerPlayer& player) {
    Inventory& inventory = player.inventory;
    auto drop = [&](ItemStack s) { player.drop(std::move(s), true); };
    if (type == ClickType::QuickCraft) {
        int expected = quickcraftStatus_;
        quickcraftStatus_ = button & 3;
        if ((expected != 1 || quickcraftStatus_ != 2) && expected != quickcraftStatus_) {
            resetQuickCraft();
        } else if (carried.isEmpty()) {
            resetQuickCraft();
        } else if (quickcraftStatus_ == 0) {
            quickcraftType_ = (button >> 2) & 3;
            if (quickcraftType_ == 0 || quickcraftType_ == 1 || (quickcraftType_ == 2 && player.hasInfiniteMaterials())) {
                quickcraftStatus_ = 1;
                quickcraftSlots_.clear();
            } else {
                resetQuickCraft();
            }
        } else if (quickcraftStatus_ == 1) {
            if (slotIndex < 0 || slotIndex >= static_cast<int>(slots.size())) return;
            Slot& s = *slots[slotIndex];
            if (canItemQuickReplace(&s, carried, true) && s.mayPlace(carried) &&
                (quickcraftType_ == 2 || carried.count > static_cast<int>(quickcraftSlots_.size())) && canDragTo(s) &&
                std::find(quickcraftSlots_.begin(), quickcraftSlots_.end(), slotIndex) == quickcraftSlots_.end())
                quickcraftSlots_.push_back(slotIndex);
        } else if (quickcraftStatus_ == 2) {
            if (!quickcraftSlots_.empty()) {
                if (quickcraftSlots_.size() == 1) {
                    int only = quickcraftSlots_.front();
                    resetQuickCraft();
                    clicked(only, quickcraftType_, ClickType::Pickup, player);
                    return;
                }
                ItemStack source = carried;
                if (source.isEmpty()) {
                    resetQuickCraft();
                    return;
                }
                int remaining = carried.count;
                int n = static_cast<int>(quickcraftSlots_.size());
                for (int i : quickcraftSlots_) {
                    Slot& s = *slots[i];
                    if (!canItemQuickReplace(&s, carried, true) || !s.mayPlace(carried) || !(quickcraftType_ == 2 || carried.count >= n) ||
                        !canDragTo(s))
                        continue;
                    int carry = s.hasItem() ? s.item().count : 0;
                    int maxSize = std::min(source.maxStackSize(), s.maxStackSize(source));
                    int place = quickcraftType_ == 0   ? static_cast<int>(std::floor(static_cast<float>(source.count) / n))
                                : quickcraftType_ == 1 ? 1
                                : quickcraftType_ == 2 ? source.maxStackSize()
                                                       : source.count;
                    int newCount = std::min(place + carry, maxSize);
                    remaining -= newCount - carry;
                    s.setByPlayer(source.copyWithCount(newCount));
                }
                source.count = remaining;
                source.clearIfEmpty();
                carried = source;
            }
            resetQuickCraft();
        } else {
            resetQuickCraft();
        }
        return;
    }
    if (quickcraftStatus_ != 0) {
        resetQuickCraft();
        return;
    }

    if ((type == ClickType::Pickup || type == ClickType::QuickMove) && (button == 0 || button == 1)) {
        bool primary = button == 0;
        if (slotIndex == -999) { // outside the window: throw the cursor stack (or one item)
            if (!carried.isEmpty()) drop(primary ? std::exchange(carried, ItemStack{}) : carried.split(1));
            carried.clearIfEmpty();
            return;
        }
        if (slotIndex < 0) return;
        Slot& s = *slots[slotIndex];
        if (type == ClickType::QuickMove) {
            if (!s.mayPickup(player)) return;
            ItemStack moved = quickMoveStack(player, slotIndex);
            while (!moved.isEmpty() && ItemStack::isSameItem(s.item(), moved)) moved = quickMoveStack(player, slotIndex);
            return;
        }
        // tryItemClickBehaviourOverride: bundles handle clicks with or onto them.
        if (bundle::overrideStackedOnOther(carried, s, primary, player) || bundle::overrideOtherStackedOnMe(s.item(), carried, s, primary, player)) {
            carried.clearIfEmpty();
            s.setChanged();
            return;
        }
        ItemStack clickedStack = s.item();
        if (clickedStack.isEmpty()) {
            if (!carried.isEmpty()) carried = s.safeInsert(carried, primary ? carried.count : 1);
        } else if (s.mayPickup(player)) {
            if (carried.isEmpty()) {
                int amount = primary ? clickedStack.count : (clickedStack.count + 1) / 2;
                if (auto taken = s.tryRemove(amount, INT32_MAX, player)) {
                    carried = *taken;
                    s.onTake(player, *taken);
                }
            } else if (s.mayPlace(carried)) {
                if (ItemStack::isSameItemSameComponents(clickedStack, carried)) {
                    carried = s.safeInsert(carried, primary ? carried.count : 1);
                } else if (carried.count <= s.maxStackSize(carried)) {
                    ItemStack held = carried;
                    carried = clickedStack;
                    s.setByPlayer(held);
                }
            } else if (ItemStack::isSameItemSameComponents(clickedStack, carried)) {
                if (auto taken = s.tryRemove(clickedStack.count, carried.maxStackSize() - carried.count, player)) {
                    carried.grow(taken->count);
                    s.onTake(player, *taken);
                }
            }
        }
        s.setChanged();
    } else if (type == ClickType::Swap && ((button >= 0 && button < 9) || button == Inventory::kOffhandSlot)) {
        if (slotIndex < 0 || slotIndex >= static_cast<int>(slots.size())) return;
        ItemStack source = inventory.slot(button);
        Slot& target = *slots[slotIndex];
        ItemStack targetStack = target.item();
        if (source.isEmpty() && targetStack.isEmpty()) return;
        if (source.isEmpty()) {
            if (target.mayPickup(player)) {
                inventory.slot(button) = targetStack;
                target.onSwapCraft(targetStack.count);
                target.setByPlayer(ItemStack{});
                target.onTake(player, targetStack);
            }
        } else if (targetStack.isEmpty()) {
            if (target.mayPlace(source)) {
                int max = target.maxStackSize(source);
                if (source.count > max) {
                    target.setByPlayer(source.split(max));
                    inventory.slot(button) = source;
                } else {
                    inventory.slot(button) = ItemStack{};
                    target.setByPlayer(source);
                }
            }
        } else if (target.mayPickup(player) && target.mayPlace(source)) {
            int max = target.maxStackSize(source);
            if (source.count > max) {
                target.setByPlayer(source.split(max));
                inventory.slot(button) = source;
                target.onTake(player, targetStack);
                if (!inventory.add(targetStack) && !targetStack.isEmpty()) drop(targetStack);
            } else {
                inventory.slot(button) = targetStack;
                target.setByPlayer(source);
                target.onTake(player, targetStack);
            }
        }
    } else if (type == ClickType::Clone && player.hasInfiniteMaterials() && carried.isEmpty() && slotIndex >= 0) {
        Slot& s = *slots[slotIndex];
        if (s.hasItem()) carried = s.safeClone(player);
    } else if (type == ClickType::Throw && carried.isEmpty() && slotIndex >= 0) {
        Slot& s = *slots[slotIndex];
        int amount = button == 0 ? 1 : s.item().count;
        ItemStack thrown = s.safeTake(amount, INT32_MAX, player);
        if (!thrown.isEmpty()) drop(thrown);
        if (button == 1) {
            while (!thrown.isEmpty() && ItemStack::isSameItem(s.item(), thrown)) {
                thrown = s.safeTake(amount, INT32_MAX, player);
                if (!thrown.isEmpty()) drop(thrown);
            }
        }
    } else if (type == ClickType::PickupAll && slotIndex >= 0) {
        Slot& clickedSlot = *slots[slotIndex];
        if (!carried.isEmpty() && (!clickedSlot.hasItem() || !clickedSlot.mayPickup(player))) {
            int n = static_cast<int>(slots.size());
            int start = button == 0 ? 0 : n - 1;
            int step = button == 0 ? 1 : -1;
            for (int pass = 0; pass < 2; pass++) {
                for (int i = start; i >= 0 && i < n && carried.count < carried.maxStackSize(); i += step) {
                    Slot& t = *slots[i];
                    if (!t.hasItem() || !canItemQuickReplace(&t, carried, true) || !t.mayPickup(player) || !canTakeItemForPickAll(carried, t)) continue;
                    if (pass == 0 && t.item().count == t.item().maxStackSize()) continue;
                    ItemStack removed = t.safeTake(t.item().count, carried.maxStackSize() - carried.count, player);
                    carried.grow(removed.count);
                }
            }
        }
    }
}

} // namespace mc
