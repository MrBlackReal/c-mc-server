#pragma once

#include <memory>
#include <string>

#include "core/direction.hpp"
#include "world/inventory/container_menu.hpp"

namespace mc {

class ServerLevel;

// A block entity's item slots ("Items") as a live Container: chests, barrels, shulker boxes,
// hoppers, dispensers, droppers, furnaces, ... Menus and tickers share one per block entity
// (ServerLevel::blockEntityContainer); every change is written back to the block entity's NBT,
// which stays what is saved, dropped and copied into items.
class BlockEntityContainer : public SimpleContainer {
public:
    BlockEntityContainer(ServerLevel& level, BlockPos pos, std::string type, int size);

    const BlockPos pos;
    const std::string type;
    bool valid = true; // false once the block entity is gone

    void setChanged() override;
    // AbstractFurnaceBlockEntity.setItem: a new ingredient restarts the cooking.
    void setItem(int slot, ItemStack stack) override;
    // The block entity's saved NBT (null once it's gone).
    nbt::Compound* tag();
    // Container.stillValidBlockEntity: still there, and the player within reach.
    bool stillValid(ServerPlayer& player) override;
    // ContainerOpenersCounter / ShulkerBoxBlockEntity.startOpen: lids, barrel doors and sounds.
    void startOpen(ServerPlayer& player) override;
    void stopOpen(ServerPlayer& player) override;
    int openers = 0;

    static bool isFurnace(std::string_view type) {
        return type == "minecraft:furnace" || type == "minecraft:smoker" || type == "minecraft:blast_furnace";
    }
    // The recipe type a furnace cooks with.
    static std::string_view cookingType(std::string_view type) {
        return type == "minecraft:smoker" ? "minecraft:smoking" : type == "minecraft:blast_furnace" ? "minecraft:blasting" : "minecraft:smelting";
    }
    // The number of item slots a block entity type keeps, 0 if it isn't a container.
    static int sizeFor(std::string_view type);

    // The rules of the block entity's own Container / WorldlyContainer implementation.
    bool canPlaceItem(int slot, const ItemStack& stack) override;
    bool isWorldly() const override;
    std::vector<int> slotsForFace(Direction face) override;
    bool canPlaceItemThroughFace(int slot, const ItemStack& stack, std::optional<Direction> face) override;
    bool canTakeItemThroughFace(int slot, const ItemStack& stack, Direction face) override;

private:
    void openerCountChanged(bool opened);
    ServerLevel& level_;
};

// PlayerEnderChestContainer: the player's ender chest items, shown at an ender chest (whose lid
// opens while someone looks inside).
class EnderChestContainer : public Container {
public:
    EnderChestContainer(SimpleContainer& items, std::shared_ptr<BlockEntityContainer> chest) : items_(items), chest_(std::move(chest)) {}
    int size() const override { return items_.size(); }
    ItemStack& item(int slot) override { return items_.item(slot); }
    bool stillValid(ServerPlayer& player) override { return chest_->stillValid(player); }
    void startOpen(ServerPlayer& player) override { chest_->startOpen(player); }
    void stopOpen(ServerPlayer& player) override { chest_->stopOpen(player); }

private:
    SimpleContainer& items_;
    std::shared_ptr<BlockEntityContainer> chest_;
};

// CompoundContainer: both halves of a double chest as one (first = the right half, whose slots come first).
class CompoundContainer : public Container {
public:
    CompoundContainer(std::shared_ptr<Container> first, std::shared_ptr<Container> second) : first_(std::move(first)), second_(std::move(second)) {}
    int size() const override { return first_->size() + second_->size(); }
    ItemStack& item(int slot) override { return slot < first_->size() ? first_->item(slot) : second_->item(slot - first_->size()); }
    int maxStackSize() const override { return first_->maxStackSize(); }
    void setChanged() override {
        first_->setChanged();
        second_->setChanged();
    }
    ItemStack removeItem(int slot, int amount) override {
        return slot < first_->size() ? first_->removeItem(slot, amount) : second_->removeItem(slot - first_->size(), amount);
    }
    void setItem(int slot, ItemStack stack) override {
        if (slot < first_->size()) first_->setItem(slot, std::move(stack));
        else second_->setItem(slot - first_->size(), std::move(stack));
    }
    bool canPlaceItem(int slot, const ItemStack& stack) override {
        return slot < first_->size() ? first_->canPlaceItem(slot, stack) : second_->canPlaceItem(slot - first_->size(), stack);
    }
    bool stillValid(ServerPlayer& player) override { return first_->stillValid(player) && second_->stillValid(player); }
    void startOpen(ServerPlayer& player) override {
        first_->startOpen(player);
        second_->startOpen(player);
    }
    void stopOpen(ServerPlayer& player) override {
        first_->stopOpen(player);
        second_->stopOpen(player);
    }

private:
    std::shared_ptr<Container> first_, second_;
};

} // namespace mc
