#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "world/item/item_stack.hpp"

namespace mc {

class Connection;
class ServerPlayer;

// Port of net.minecraft.world.Container: numbered item slots.
class Container {
public:
    virtual ~Container() = default;
    virtual int size() const = 0;
    virtual ItemStack& item(int slot) = 0;
    virtual int maxStackSize() const { return 99; }
    int maxStackSize(const ItemStack& stack) const { return std::min(maxStackSize(), stack.maxStackSize()); }
    virtual void setChanged() {}
    virtual bool stillValid(ServerPlayer& /*player*/) { return true; }
    virtual void startOpen(ServerPlayer& /*player*/) {}
    virtual void stopOpen(ServerPlayer& /*player*/) {}
    // Container.removeItem (ContainerHelper.removeItem + setChanged when something came off).
    virtual ItemStack removeItem(int slot, int amount);
    virtual void setItem(int slot, ItemStack stack) {
        item(slot) = std::move(stack);
        setChanged();
    }
};

// SimpleContainer: its own list of slots.
class SimpleContainer : public Container {
public:
    explicit SimpleContainer(int size) : items_(size) {}
    int size() const override { return static_cast<int>(items_.size()); }
    ItemStack& item(int slot) override { return items_[slot]; }
    std::vector<ItemStack>& items() { return items_; }

protected:
    std::vector<ItemStack> items_;
};

// A SimpleContainer that reports changes (the anonymous SimpleContainer subclasses menus use for
// their input slots: setChanged -> menu.slotsChanged).
class ListeningContainer : public SimpleContainer {
public:
    ListeningContainer(int size, std::function<void(Container&)> onChanged) : SimpleContainer(size), onChanged_(std::move(onChanged)) {}
    void setChanged() override {
        if (onChanged_) onChanged_(*this);
    }

private:
    std::function<void(Container&)> onChanged_;
};

// ResultContainer: one slot whose setItem doesn't count as a change and whose removeItem takes the
// whole stack (ContainerHelper.takeItem). `onChanged` runs on setChanged (a Slot's setChanged).
class ResultContainer : public SimpleContainer {
public:
    explicit ResultContainer(std::function<void(Container&)> onChanged = {}) : SimpleContainer(1), onChanged_(std::move(onChanged)) {}
    ItemStack removeItem(int slot, int /*amount*/) override {
        if (slot != 0) return {};
        ItemStack out = std::move(items_[0]);
        items_[0] = {};
        return out;
    }
    void setItem(int slot, ItemStack stack) override {
        if (slot == 0) items_[0] = std::move(stack);
    }
    void setChanged() override {
        if (onChanged_) onChanged_(*this);
    }

private:
    std::function<void(Container&)> onChanged_;
};

// Port of net.minecraft.world.inventory.Slot: one slot of a menu, viewing a container slot.
class Slot {
public:
    Slot(Container& container, int containerSlot) : container(container), containerSlot(containerSlot) {}
    virtual ~Slot() = default;

    Container& container;
    const int containerSlot;
    int index = 0; // in the menu

    ItemStack& item() { return container.item(containerSlot); }
    bool hasItem() { return !item().isEmpty(); }

    virtual bool mayPlace(const ItemStack& /*stack*/) { return true; }
    virtual bool mayPickup(ServerPlayer& /*player*/) { return true; }
    virtual int maxStackSize() { return container.maxStackSize(); }
    virtual int maxStackSize(const ItemStack& stack) { return std::min(maxStackSize(), stack.maxStackSize()); }
    virtual void setByPlayer(ItemStack stack, const ItemStack& /*previous*/) { set(std::move(stack)); }
    void setByPlayer(ItemStack stack) {
        ItemStack previous = item();
        setByPlayer(std::move(stack), previous);
    }
    virtual void set(ItemStack stack) {
        container.setItem(containerSlot, std::move(stack));
        setChanged();
    }
    virtual void setChanged() { container.setChanged(); }
    virtual ItemStack remove(int amount) { return container.removeItem(containerSlot, amount); }
    virtual void onTake(ServerPlayer& /*player*/, const ItemStack& /*carried*/) { setChanged(); }
    void onQuickCraft(const ItemStack& picked, const ItemStack& original) {
        int count = original.count - picked.count;
        if (count > 0) onQuickCraft(original, count);
    }
    virtual void onQuickCraft(const ItemStack& /*picked*/, int /*count*/) {}
    virtual void onSwapCraft(int /*count*/) {}
    virtual ItemStack safeClone(ServerPlayer& /*player*/) { return item().copyWithCount(item().maxStackSize()); }

    std::optional<ItemStack> tryRemove(int amount, int maxAmount, ServerPlayer& player);
    ItemStack safeTake(int amount, int maxAmount, ServerPlayer& player);
    ItemStack safeInsert(ItemStack stack, int amount);
    ItemStack safeInsert(ItemStack stack) {
        int n = stack.count;
        return safeInsert(std::move(stack), n);
    }
    bool allowModification(ServerPlayer& player) { return mayPickup(player) && mayPlace(item()); }
};

// Port of net.minecraft.world.inventory.AbstractContainerMenu: slots, clicks (doClick) and keeping
// the client's view in sync (ServerPlayer's ContainerSynchronizer, RemoteSlot.Synchronized).
class AbstractContainerMenu {
public:
    enum class ClickType { Pickup = 0, QuickMove = 1, Swap = 2, Clone = 3, Throw = 4, QuickCraft = 5, PickupAll = 6 };

    // `menuType`: minecraft:menu id (-1 for the player's inventory menu).
    AbstractContainerMenu(int menuType, int containerId) : menuType(menuType), containerId(containerId) {}
    virtual ~AbstractContainerMenu() = default;

    const int menuType;
    const int containerId;
    std::vector<std::unique_ptr<Slot>> slots;
    ItemStack carried;

    Slot& slot(int index) { return *slots[index]; }
    bool isValidSlotIndex(int index) const { return index == -1 || index == -999 || (index >= 0 && index < static_cast<int>(slots.size())); }
    int stateId() const { return stateId_; }
    int incrementStateId() { return stateId_ = (stateId_ + 1) & 32767; }

    virtual ItemStack quickMoveStack(ServerPlayer& player, int index) = 0;
    virtual bool stillValid(ServerPlayer& player) = 0;
    // AbstractContainerMenu.removed: the cursor stack goes back to the inventory.
    virtual void removed(ServerPlayer& player);
    virtual bool canTakeItemForPickAll(const ItemStack& /*carried*/, Slot& /*target*/) { return true; }
    virtual bool canDragTo(Slot& /*slot*/) { return true; }
    virtual void slotsChanged(Container& /*container*/) { broadcastChanges(); }
    // AbstractContainerMenu.clickMenuButton (ServerboundContainerButtonClickPacket): true if
    // something changed, which then gets broadcast.
    virtual bool clickMenuButton(ServerPlayer& /*player*/, int /*buttonId*/) { return false; }

    void clicked(int slotIndex, int button, ClickType type, ServerPlayer& player);
    // ServerboundSelectBundleItemPacket: the bundle in `slotIndex` points at item `selected`.
    void setSelectedBundleItemIndex(int slotIndex, int selected);

    // ServerPlayer.initMenu -> setSynchronizer: from now on changes go to `conn`.
    void setSynchronizer(Connection& conn);
    void broadcastChanges();
    void broadcastFullState() { sendAllDataToRemote(); }
    void sendAllDataToRemote();
    void suppressRemoteUpdates() { suppressRemote_ = true; }
    void resumeRemoteUpdates() { suppressRemote_ = false; }
    void setRemoteSlot(int slot, const ItemStack& stack) { remoteSlots_[slot].force(stack); }
    void setRemoteSlotUnsafe(int slot, HashedStack stack) {
        if (slot >= 0 && slot < static_cast<int>(remoteSlots_.size())) remoteSlots_[slot].receive(std::move(stack));
    }
    void setRemoteCarried(HashedStack stack) { remoteCarried_.receive(std::move(stack)); }
    // AbstractContainerMenu.transferState: take over what `other` knows the client has, for the
    // slots both show (ServerPlayer.doCloseContainer, back to the inventory menu).
    void transferState(AbstractContainerMenu& other);
    // ClientboundContainerSetSlotPacket for one slot right now.
    void sendSlot(int slot, const ItemStack& stack);

protected:
    Slot& addSlot(std::unique_ptr<Slot> slot);
    void addDataSlot(std::function<int()> get) {
        dataSlots_.push_back(std::move(get));
        remoteData_.push_back(0);
    }
    // addStandardInventorySlots: the player's main inventory (27) then hotbar (9).
    void addStandardInventorySlots(Container& inventory);
    bool moveItemStackTo(ItemStack& stack, int start, int end, bool backwards);
    // AbstractContainerMenu.clearContainer: everything back to the player (or dropped).
    void clearContainer(ServerPlayer& player, Container& container);
    Connection* conn_ = nullptr;

private:
    struct RemoteSlot {
        std::optional<ItemStack> stack;
        std::optional<HashedStack> hash;
        void force(const ItemStack& s) {
            stack = s;
            hash.reset();
        }
        void receive(HashedStack h) {
            stack.reset();
            hash = std::move(h);
        }
        bool matches(const ItemStack& local);
    };
    void resetQuickCraft() {
        quickcraftStatus_ = 0;
        quickcraftSlots_.clear();
    }

    int stateId_ = 0;
    std::vector<RemoteSlot> remoteSlots_;
    RemoteSlot remoteCarried_;
    std::vector<std::function<int()>> dataSlots_;
    std::vector<int> remoteData_;
    bool suppressRemote_ = false;
    int quickcraftType_ = -1;
    int quickcraftStatus_ = 0;
    std::vector<int> quickcraftSlots_; // (a set in vanilla; kept in insertion order)
};

// Inventory.placeItemBackInInventory: fills matching stacks, then free slots, then drops.
void placeItemBackInInventory(ServerPlayer& player, ItemStack stack);

} // namespace mc
