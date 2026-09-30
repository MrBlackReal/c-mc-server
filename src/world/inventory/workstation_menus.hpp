#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/direction.hpp"
#include "util/legacy_random.hpp"
#include "world/inventory/menus.hpp"
#include "world/item/crafting/recipes.hpp"
#include "world/level/block/block_state.hpp"

// The menus of blocks without an inventory of their own (ContainerLevelAccess menus): stonecutter,
// grindstone, smithing table, anvil, loom and enchanting table. Their input slots belong to the menu
// and go back to the player when it closes.
namespace mc {

// A menu tied to a block position (ContainerLevelAccess.create(level, pos)).
class BlockMenu : public AbstractContainerMenu {
protected:
    BlockMenu(int menuType, int containerId, ServerPlayer& player, BlockPos pos);
    // AbstractContainerMenu.stillValid(access, player, block): the block is still there and the
    // player within reach.
    bool stillValidIf(ServerPlayer& player, const std::function<bool(BlockStateId)>& isValidBlock);
    // Plays `sound` at the block, at most once per game tick (the stonecutter's and loom's take sound).
    void playTakeSound(std::string_view sound);

    ServerPlayer& player_;
    BlockPos pos_;
    PlayerInventoryContainer inventory_;

private:
    int64_t lastSoundTime_ = -1;
};

// StonecutterMenu: 0 input, 1 result, 2-37 inventory; data: the selected recipe (a button).
class StonecutterMenu : public BlockMenu {
public:
    StonecutterMenu(int containerId, ServerPlayer& player, BlockPos pos);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;
    void slotsChanged(Container& container) override;
    bool clickMenuButton(ServerPlayer& player, int buttonId) override;
    bool canTakeItemForPickAll(const ItemStack&, Slot& target) override { return &target.container != &result_; }

private:
    friend class StonecutterResultSlot;
    void setupRecipeList(const ItemStack& input);
    void setupResultSlot(int index);
    bool isValidRecipeIndex(int index) const { return index >= 0 && index < static_cast<int>(recipes_.size()); }

    ListeningContainer container_;
    ResultContainer result_;
    int selected_ = -1;
    std::vector<recipes::Stonecutting> recipes_;
    ItemStack input_;
};

// GrindstoneMenu: 0 input, 1 additional, 2 result, 3-38 inventory.
class GrindstoneMenu : public BlockMenu {
public:
    GrindstoneMenu(int containerId, ServerPlayer& player, BlockPos pos);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;
    void slotsChanged(Container& container) override;

private:
    friend class GrindstoneResultSlot;
    void createResult();
    ItemStack computeResult(const ItemStack& input, const ItemStack& additional);
    ItemStack mergeItems(const ItemStack& input, const ItemStack& additional);

    ListeningContainer repair_;
    ResultContainer result_;
};

// ItemCombinerMenu: input slots, a result slot, then the inventory (smithing table, anvil).
class ItemCombinerMenu : public BlockMenu {
public:
    void slotsChanged(Container& container) override;
    void removed(ServerPlayer& player) override;
    bool stillValid(ServerPlayer& player) override;
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    virtual void createResult() = 0;

protected:
    ItemCombinerMenu(int menuType, int containerId, ServerPlayer& player, BlockPos pos, std::vector<std::function<bool(const ItemStack&)>> inputs);
    virtual bool mayPickupResult(ServerPlayer& /*player*/, bool /*hasItem*/) { return true; }
    virtual void onTakeResult(ServerPlayer& player, const ItemStack& carried) = 0;
    virtual bool isValidBlock(BlockStateId state) = 0;
    virtual bool canMoveIntoInputSlots(const ItemStack& /*stack*/) { return true; }
    int resultSlot() const { return resultSlot_; }

    ListeningContainer inputSlots_;
    ResultContainer resultSlots_;

private:
    friend class CombinerResultSlot;
    int resultSlot_;
};

// SmithingMenu: 0 template, 1 base, 2 addition, 3 result; data: whether the recipe is invalid.
class SmithingMenu : public ItemCombinerMenu {
public:
    SmithingMenu(int containerId, ServerPlayer& player, BlockPos pos);
    void slotsChanged(Container& container) override;
    void createResult() override;
    bool canTakeItemForPickAll(const ItemStack&, Slot& target) override { return &target.container != &resultSlots_; }

protected:
    void onTakeResult(ServerPlayer& player, const ItemStack& carried) override;
    bool isValidBlock(BlockStateId state) override;
    bool canMoveIntoInputSlots(const ItemStack& stack) override;

private:
    void shrinkStackInSlot(int slot);
    int hasRecipeError_ = 0;
};

// AnvilMenu: 0 input, 1 addition, 2 result; data: the level cost. Renaming comes in through
// ServerboundRenameItemPacket (setItemName).
class AnvilMenu : public ItemCombinerMenu {
public:
    AnvilMenu(int containerId, ServerPlayer& player, BlockPos pos);
    void createResult() override;
    bool setItemName(const std::string& name);
    int cost() const { return cost_; }

protected:
    bool mayPickupResult(ServerPlayer& player, bool hasItem) override;
    void onTakeResult(ServerPlayer& player, const ItemStack& carried) override;
    bool isValidBlock(BlockStateId state) override;

private:
    int cost_ = 0;
    int repairItemCountCost_ = 0;
    std::optional<std::string> itemName_;
    bool onlyRenaming_ = false;
};

// LoomMenu: 0 banner, 1 dye, 2 pattern, 3 result, 4-39 inventory; data: the selected pattern.
class LoomMenu : public BlockMenu {
public:
    LoomMenu(int containerId, ServerPlayer& player, BlockPos pos);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;
    void slotsChanged(Container& container) override;
    bool clickMenuButton(ServerPlayer& player, int buttonId) override;

private:
    friend class LoomResultSlot;
    std::vector<std::string> selectablePatterns(const ItemStack& pattern) const;
    void setupResultSlot(const std::string& pattern);

    ListeningContainer input_;
    SimpleContainer output_;
    int selected_ = -1;
    std::vector<std::string> patterns_;
};

// EnchantmentMenu: 0 item, 1 lapis, 2-37 inventory; data: the three costs, the seed, the three
// enchantment clues and their levels.
class EnchantmentMenu : public BlockMenu {
public:
    EnchantmentMenu(int containerId, ServerPlayer& player, BlockPos pos);
    ItemStack quickMoveStack(ServerPlayer& player, int index) override;
    bool stillValid(ServerPlayer& player) override;
    void removed(ServerPlayer& player) override;
    void slotsChanged(Container& container) override;
    bool clickMenuButton(ServerPlayer& player, int buttonId) override;

private:
    std::vector<std::pair<std::string, int>> enchantmentList(const ItemStack& stack, int slot, int cost);

    ListeningContainer slots_;
    LegacyRandom random_;
    int seed_ = 0;
    int costs_[3] = {0, 0, 0};
    int enchantClue_[3] = {-1, -1, -1};
    int levelClue_[3] = {-1, -1, -1};
};

} // namespace mc
