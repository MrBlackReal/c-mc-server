#pragma once

#include <memory>
#include <optional>

#include "core/direction.hpp"
#include "world/entity/player/inventory.hpp"
#include "world/inventory/container_menu.hpp"

namespace mc {

class ServerLevel;

// The player's Inventory as a Container (Inventory.getItem/setItem): 0-35 main, 36-39 armor
// (feet..head), 40 offhand.
class PlayerInventoryContainer : public Container {
public:
    explicit PlayerInventoryContainer(Inventory& inventory) : inventory_(inventory) {}
    int size() const override { return 41; }
    ItemStack& item(int slot) override { return inventory_.slot(slot); }

private:
    Inventory& inventory_;
};

// TransientCraftingContainer: the crafting grid; every change recomputes the menu's result.
class CraftingContainer : public SimpleContainer {
public:
    CraftingContainer(AbstractContainerMenu& menu, int width, int height) : SimpleContainer(width * height), width(width), height(height), menu_(menu) {}
    const int width, height;
    ItemStack removeItem(int slot, int amount) override {
        ItemStack out = Container::removeItem(slot, amount);
        if (!out.isEmpty()) menu_.slotsChanged(*this);
        return out;
    }
    void setItem(int slot, ItemStack stack) override {
        items_[slot] = std::move(stack);
        menu_.slotsChanged(*this);
    }
    void setChanged() override {} // (TransientCraftingContainer.setChanged does nothing)

private:
    AbstractContainerMenu& menu_;
};

// AbstractCraftingMenu: a crafting grid with a result slot.
class AbstractCraftingMenu : public AbstractContainerMenu {
public:
    AbstractCraftingMenu(int menuType, int containerId, int width, int height)
        : AbstractContainerMenu(menuType, containerId), craftSlots(*this, width, height) {}
    CraftingContainer craftSlots;
    ResultContainer resultSlots;

protected:
    void addResultSlot(ServerPlayer& player);
    void addCraftingGridSlots();
    // CraftingMenu.slotChangedCraftingGrid: find the recipe and show its result.
    void slotChangedCraftingGrid(ServerPlayer& player);
};

// InventoryMenu (container 0): 0 crafting result, 1-4 crafting grid, 5-8 armor (head..feet),
// 9-35 main, 36-44 hotbar, 45 offhand.
class InventoryMenu : public AbstractCraftingMenu {
public:
    InventoryMenu(ServerPlayer& owner, Inventory& inventory);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer&) override { return true; }
    void removed(ServerPlayer& player) override;
    void slotsChanged(Container& container) override;
    bool canTakeItemForPickAll(const ItemStack& carried, Slot& target) override { return &target.container != &resultSlots; }
    PlayerInventoryContainer inventoryContainer;

private:
    ServerPlayer& owner_;
};

// CraftingMenu (crafting table): 0 result, 1-9 grid, 10-36 inventory, 37-45 hotbar.
class CraftingMenu : public AbstractCraftingMenu {
public:
    CraftingMenu(int containerId, ServerPlayer& player, BlockPos pos);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;
    void slotsChanged(Container& container) override;
    bool canTakeItemForPickAll(const ItemStack& carried, Slot& target) override { return &target.container != &resultSlots; }

private:
    ServerPlayer& player_;
    BlockPos pos_;
    PlayerInventoryContainer inventory_;
};

// ChestMenu (generic_9xN), ShulkerBoxMenu, HopperMenu, DispenserMenu: a container grid over the
// player's inventory. `container` stays alive with the menu.
class ContainerGridMenu : public AbstractContainerMenu {
public:
    enum class Kind { Chest, ShulkerBox, Hopper, Dispenser };
    ContainerGridMenu(Kind kind, int menuType, int containerId, ServerPlayer& player, std::shared_ptr<Container> container);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override { return container_->stillValid(player); }
    void removed(ServerPlayer& player) override;

private:
    Kind kind_;
    std::shared_ptr<Container> container_;
    PlayerInventoryContainer inventory_;
};

class BlockEntityContainer;

// AbstractFurnaceMenu (furnace, smoker, blast furnace): 0 ingredient, 1 fuel, 2 result, 3-29
// inventory, 30-38 hotbar; data: lit time, lit duration, cooking progress, cooking time.
class FurnaceMenu : public AbstractContainerMenu {
public:
    FurnaceMenu(int menuType, int containerId, ServerPlayer& player, std::shared_ptr<BlockEntityContainer> furnace);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;

private:
    bool canSmelt(const ItemStack& stack) const;
    std::shared_ptr<BlockEntityContainer> furnace_;
    PlayerInventoryContainer inventory_;
};

// AbstractFurnaceMenu.isFuel: the item has minecraft:cooking_fuel.
bool isFurnaceFuel(const ItemStack& stack);

// BrewingStandMenu: 0-2 bottles, 3 ingredient, 4 fuel, 5-40 inventory; data: brew time, fuel,
// total brew time, total fuel.
class BrewingStandMenu : public AbstractContainerMenu {
public:
    BrewingStandMenu(int containerId, ServerPlayer& player, std::shared_ptr<BlockEntityContainer> stand);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;

private:
    std::shared_ptr<BlockEntityContainer> stand_;
    PlayerInventoryContainer inventory_;
};
// PotionIngredient.isPotionInput / BrewingStandMenu.FuelSlot.mayPlaceItem / the reagent property set.
bool isPotionInput(const ItemStack& stack);
bool isBrewingFuel(const ItemStack& stack);
bool isBrewingReagent(const ItemStack& stack);

// LecternMenu: the book (slot 0) and the page (data 0); buttons turn pages and take the book.
class LecternMenu : public AbstractContainerMenu {
public:
    LecternMenu(int containerId, ServerPlayer& player, BlockPos pos);
    ItemStack quickMoveStack(ServerPlayer&, int) override { return {}; }
    bool stillValid(ServerPlayer& player) override;
    bool clickMenuButton(ServerPlayer& player, int buttonId) override;

private:
    void setPage(int page);
    std::shared_ptr<Container> lectern_;
    ServerLevel& level_;
    BlockPos pos_;
};

// BeaconMenu: the payment slot, 1-36 inventory; data: levels, primary and secondary effect
// (mob_effect id + 1). ServerboundSetBeaconPacket picks the effects (updateEffects).
class BeaconMenu : public AbstractContainerMenu {
public:
    BeaconMenu(int containerId, ServerPlayer& player, BlockPos pos);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;
    // False (and the player is disconnected) if the choice isn't valid for the beacon's levels.
    bool updateEffects(std::optional<int> primary, std::optional<int> secondary);

private:
    nbt::Compound* beacon();
    SimpleContainer payment_;
    ServerPlayer& player_;
    BlockPos pos_;
    PlayerInventoryContainer inventory_;
};

// CrafterMenu: 0-8 the grid, 9-44 inventory, 45 the result preview; data: 0-8 disabled slots,
// 9 triggered. ServerboundContainerSlotStateChangedPacket toggles empty slots.
class CrafterMenu : public AbstractContainerMenu {
public:
    CrafterMenu(int containerId, ServerPlayer& player, std::shared_ptr<BlockEntityContainer> crafter);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;
    void slotsChanged(Container& container) override;
    bool isSlotDisabled(int slot);
    // CrafterBlockEntity.setSlotState
    void setSlotState(int slot, bool enabled);

private:
    void refreshRecipeResult();
    std::shared_ptr<BlockEntityContainer> crafter_;
    ResultContainer result_;
    PlayerInventoryContainer inventory_;
};

} // namespace mc
