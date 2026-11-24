#include "world/inventory/menus.hpp"

#include "core/registries.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "world/item/crafting/recipes.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/item/item_logic.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/block/entity/block_entity_ticker.hpp"
#include "world/level/block/redstone_blocks.hpp"

namespace mc {

namespace {

// ResultSlot: taking the result consumes one of each ingredient (leaving crafting remainders).
class ResultSlot : public Slot {
public:
    ResultSlot(ServerPlayer& player, CraftingContainer& craftSlots, Container& result) : Slot(result, 0), player_(player), craftSlots_(craftSlots) {}
    bool mayPlace(const ItemStack&) override { return false; }
    void onTake(ServerPlayer& player, const ItemStack& /*carried*/) override {
        // (checkTakeAchievements: stats and the recipe book aren't ported)
        std::vector<ItemStack> grid(craftSlots_.items());
        recipes::PositionedInput positioned = recipes::positioned(craftSlots_.width, craftSlots_.height, grid);
        const recipes::CraftingInput& input = positioned.input;
        auto recipe = recipes::findCrafting(input);
        // ResultSlot.getRemainingItems: without a recipe every ingredient stays.
        std::vector<ItemStack> remaining = recipe ? recipes::remainingItems(&*recipe, input) : input.items;
        for (int y = 0; y < input.height; y++) {
            for (int x = 0; x < input.width; x++) {
                int slot = x + positioned.left + (y + positioned.top) * craftSlots_.width;
                ItemStack replacement = remaining[x + y * input.width];
                if (!craftSlots_.item(slot).isEmpty()) craftSlots_.removeItem(slot, 1);
                ItemStack current = craftSlots_.item(slot);
                if (replacement.isEmpty()) continue;
                if (current.isEmpty()) {
                    craftSlots_.setItem(slot, replacement);
                } else if (ItemStack::isSameItemSameComponents(current, replacement)) {
                    replacement.grow(current.count);
                    craftSlots_.setItem(slot, replacement);
                } else if (!player.inventory.add(replacement) && !replacement.isEmpty()) {
                    player.drop(std::move(replacement), false);
                }
            }
        }
    }

private:
    ServerPlayer& player_;
    CraftingContainer& craftSlots_;
};

// ArmorSlot: only armor for that slot, one at a time; Curse of Binding keeps it on.
class ArmorSlot : public Slot {
public:
    ArmorSlot(Container& inventory, int slot, EquipmentSlot equipment) : Slot(inventory, slot), equipment_(equipment) {}
    int maxStackSize() override { return 1; }
    bool mayPlace(const ItemStack& stack) override { return stack.info().equipSlot == static_cast<int8_t>(equipment_); }
    bool mayPickup(ServerPlayer& player) override {
        const ItemStack& s = item();
        return s.isEmpty() || player.gameMode == GameType::Creative || !enchantments::hasEffect(s, "minecraft:prevent_armor_change");
    }

private:
    EquipmentSlot equipment_;
};

// ShulkerBoxSlot: no shulker boxes inside shulker boxes.
class ShulkerBoxSlot : public Slot {
public:
    using Slot::Slot;
    bool mayPlace(const ItemStack& stack) override { return stack.info().fitsInContainers; }
};

// The quickMoveStack pattern shared by the grid menus: returns the moved stack's original, or empty.
template <class Move>
ItemStack quickMove(AbstractContainerMenu& menu, ServerPlayer& player, int index, bool takeCallback, Move move) {
    Slot& s = menu.slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    if (!move(stack)) return {};
    if (stack.isEmpty()) s.setByPlayer(ItemStack{}, clicked);
    else s.setChanged();
    if (takeCallback) {
        if (stack.count == clicked.count) return {};
        ItemStack left = stack;
        s.onTake(player, left);
    }
    return clicked;
}

} // namespace

// ---------------------------------------------------------------- crafting

void AbstractCraftingMenu::addResultSlot(ServerPlayer& player) { addSlot(std::make_unique<ResultSlot>(player, craftSlots, resultSlots)); }

void AbstractCraftingMenu::addCraftingGridSlots() {
    for (int y = 0; y < craftSlots.height; y++)
        for (int x = 0; x < craftSlots.width; x++) addSlot(std::make_unique<Slot>(craftSlots, x + y * craftSlots.width));
}

void AbstractCraftingMenu::slotChangedCraftingGrid(ServerPlayer& /*player*/) {
    std::vector<ItemStack> grid(craftSlots.items());
    recipes::CraftingInput input = recipes::positioned(craftSlots.width, craftSlots.height, grid).input;
    ItemStack result;
    if (auto recipe = recipes::findCrafting(input)) result = recipes::assemble(*recipe, input); // (limited crafting is off)
    resultSlots.setItem(0, result);
    setRemoteSlot(0, result);
    sendSlot(0, result);
}

// ---------------------------------------------------------------- InventoryMenu

InventoryMenu::InventoryMenu(ServerPlayer& owner, Inventory& inventory)
    : AbstractCraftingMenu(-1, 0, 2, 2), inventoryContainer(inventory), owner_(owner) {
    addResultSlot(owner);
    addCraftingGridSlots();
    constexpr EquipmentSlot kArmor[4] = {EquipmentSlot::Head, EquipmentSlot::Chest, EquipmentSlot::Legs, EquipmentSlot::Feet};
    for (int i = 0; i < 4; i++) addSlot(std::make_unique<ArmorSlot>(inventoryContainer, 39 - i, kArmor[i]));
    addStandardInventorySlots(inventoryContainer);
    addSlot(std::make_unique<Slot>(inventoryContainer, Inventory::kOffhandSlot));
}

void InventoryMenu::slotsChanged(Container&) { slotChangedCraftingGrid(owner_); }

void InventoryMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    resultSlots.item(0) = {};
    clearContainer(player, craftSlots);
}

ItemStack InventoryMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    int8_t equip = clicked.info().equipSlot;
    auto armorSlotFor = [](int8_t e) { // 8 - EquipmentSlot.getIndex: feet 8, legs 7, chest 6, head 5
        switch (static_cast<EquipmentSlot>(e)) {
        case EquipmentSlot::Feet: return 8;
        case EquipmentSlot::Legs: return 7;
        case EquipmentSlot::Chest: return 6;
        case EquipmentSlot::Head: return 5;
        default: return -1;
        }
    };
    int armor = armorSlotFor(equip);
    bool moved;
    if (index == 0) {
        moved = moveItemStackTo(stack, 9, 45, true);
        if (moved) s.onQuickCraft(stack, clicked);
    } else if (index < 9) {
        moved = moveItemStackTo(stack, 9, 45, false);
    } else if (armor >= 0 && !slot(armor).hasItem()) {
        moved = moveItemStackTo(stack, armor, armor + 1, false);
    } else if (equip == static_cast<int8_t>(EquipmentSlot::OffHand) && !slot(45).hasItem()) {
        moved = moveItemStackTo(stack, 45, 46, false);
    } else if (index < 36) {
        moved = moveItemStackTo(stack, 36, 45, false);
    } else if (index < 45) {
        moved = moveItemStackTo(stack, 9, 36, false);
    } else {
        moved = moveItemStackTo(stack, 9, 45, false);
    }
    if (!moved) return {};
    if (stack.isEmpty()) s.setByPlayer(ItemStack{}, clicked);
    else s.setChanged();
    if (stack.count == clicked.count) return {};
    ItemStack left = stack;
    s.onTake(player, left);
    if (index == 0 && !left.isEmpty()) player.drop(left, false);
    return clicked;
}

// ---------------------------------------------------------------- CraftingMenu

CraftingMenu::CraftingMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : AbstractCraftingMenu(registries::id("minecraft:menu", "minecraft:crafting").value_or(0), containerId, 3, 3),
      player_(player), pos_(pos), inventory_(player.inventory) {
    addResultSlot(player);
    addCraftingGridSlots();
    addStandardInventorySlots(inventory_);
}

void CraftingMenu::slotsChanged(Container&) { slotChangedCraftingGrid(player_); }

bool CraftingMenu::stillValid(ServerPlayer& player) { // AbstractContainerMenu.stillValid(access, player, block)
    return blocks::blockOf(player.level().getBlockState(pos_)) == gen::block::CRAFTING_TABLE && player.isWithinBlockInteractionRange(pos_, 4.0);
}

void CraftingMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    clearContainer(player, craftSlots);
}

ItemStack CraftingMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    bool moved;
    if (index == 0) {
        moved = moveItemStackTo(stack, 10, 46, true);
        if (moved) s.onQuickCraft(stack, clicked);
    } else if (index >= 10 && index < 46) {
        moved = moveItemStackTo(stack, 1, 10, false) ||
                (index < 37 ? moveItemStackTo(stack, 37, 46, false) : moveItemStackTo(stack, 10, 37, false));
    } else {
        moved = moveItemStackTo(stack, 10, 46, false);
    }
    if (!moved) return {};
    if (stack.isEmpty()) s.setByPlayer(ItemStack{});
    else s.setChanged();
    if (stack.count == clicked.count) return {};
    ItemStack left = stack;
    s.onTake(player, left);
    if (index == 0 && !left.isEmpty()) player.drop(left, false);
    return clicked;
}

// ---------------------------------------------------------------- container grids

ContainerGridMenu::ContainerGridMenu(Kind kind, int menuType, int containerId, ServerPlayer& player, std::shared_ptr<Container> container)
    : AbstractContainerMenu(menuType, containerId), kind_(kind), container_(std::move(container)), inventory_(player.inventory) {
    container_->startOpen(player);
    for (int i = 0; i < container_->size(); i++) {
        if (kind == Kind::ShulkerBox) addSlot(std::make_unique<ShulkerBoxSlot>(*container_, i));
        else addSlot(std::make_unique<Slot>(*container_, i));
    }
    addStandardInventorySlots(inventory_);
}

void ContainerGridMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    container_->stopOpen(player);
}

ItemStack ContainerGridMenu::quickMoveStack(ServerPlayer& player, int index) {
    int size = container_->size();
    int total = static_cast<int>(slots.size());
    // ChestMenu / ShulkerBoxMenu / HopperMenu don't call onTake; DispenserMenu does.
    return quickMove(*this, player, index, kind_ == Kind::Dispenser, [&](ItemStack& stack) {
        return index < size ? moveItemStackTo(stack, size, total, true) : moveItemStackTo(stack, 0, size, false);
    });
}

// ---------------------------------------------------------------- furnaces

bool isFurnaceFuel(const ItemStack& stack) {
    return !stack.isEmpty() && (!stack.info().fuelBurnTime.empty() || items::componentValue(stack, "minecraft:cooking_fuel"));
}

namespace {
// FurnaceFuelSlot: fuel, or one empty bucket (for a wet sponge).
class FurnaceFuelSlot : public Slot {
public:
    using Slot::Slot;
    static bool isBucket(const ItemStack& s) {
        static const int32_t kBucket = items::byName("minecraft:bucket").value_or(-1);
        return !s.isEmpty() && s.item == kBucket;
    }
    bool mayPlace(const ItemStack& stack) override { return isFurnaceFuel(stack) || isBucket(stack); }
    int maxStackSize(const ItemStack& stack) override { return isBucket(stack) ? 1 : Slot::maxStackSize(stack); }
};

// FurnaceResultSlot: taking the result awards the experience of what was cooked.
class FurnaceResultSlot : public Slot {
public:
    FurnaceResultSlot(Container& container, BlockEntityContainer& furnace) : Slot(container, 2), furnace_(furnace) {}
    bool mayPlace(const ItemStack&) override { return false; }
    void onTake(ServerPlayer& player, const ItemStack& carried) override {
        award(player);
        Slot::onTake(player, carried);
    }
    void onQuickCraft(const ItemStack&, int) override {} // (the award happens in onTake, which follows)

private:
    void award(ServerPlayer& player) { // checkTakeAchievements -> awardUsedRecipesAndPopExperience
        if (nbt::Compound* tag = furnace_.tag()) block_entity::awardFurnaceExperience(player.level(), *tag, player.x, player.y, player.z);
    }
    BlockEntityContainer& furnace_;
};
} // namespace

FurnaceMenu::FurnaceMenu(int menuType, int containerId, ServerPlayer& player, std::shared_ptr<BlockEntityContainer> furnace)
    : AbstractContainerMenu(menuType, containerId), furnace_(std::move(furnace)), inventory_(player.inventory) {
    furnace_->startOpen(player);
    addSlot(std::make_unique<Slot>(*furnace_, 0));
    addSlot(std::make_unique<FurnaceFuelSlot>(*furnace_, 1));
    addSlot(std::make_unique<FurnaceResultSlot>(*furnace_, *furnace_));
    addStandardInventorySlots(inventory_);
    for (const char* key : {"lit_time_remaining", "lit_total_time", "cooking_time_spent", "cooking_total_time"})
        addDataSlot([f = furnace_.get(), key] {
            nbt::Compound* tag = f->tag();
            return tag ? tag->getInt(key) : 0;
        });
}

bool FurnaceMenu::stillValid(ServerPlayer& player) { return furnace_->stillValid(player); }

void FurnaceMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    furnace_->stopOpen(player);
}

bool FurnaceMenu::canSmelt(const ItemStack& stack) const {
    return recipes::findCooking(BlockEntityContainer::cookingType(furnace_->type), stack).has_value(); // (the recipe property set)
}

ItemStack FurnaceMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    bool moved;
    if (index == 2) {
        moved = moveItemStackTo(stack, 3, 39, true);
        if (moved) s.onQuickCraft(stack, clicked);
    } else if (index != 1 && index != 0) {
        if (canSmelt(stack)) moved = moveItemStackTo(stack, 0, 1, false);
        else if (isFurnaceFuel(stack)) moved = moveItemStackTo(stack, 1, 2, false);
        else if (index >= 3 && index < 30) moved = moveItemStackTo(stack, 30, 39, false);
        else moved = index >= 30 && index < 39 ? moveItemStackTo(stack, 3, 30, false) : true;
    } else {
        moved = moveItemStackTo(stack, 3, 39, false);
    }
    if (!moved) return {};
    if (stack.isEmpty()) s.setByPlayer(ItemStack{});
    else s.setChanged();
    if (stack.count == clicked.count) return {};
    ItemStack left = stack;
    s.onTake(player, left);
    return clicked;
}

// ---------------------------------------------------------------- brewing stand

bool isPotionInput(const ItemStack& stack) {
    return !stack.isEmpty() && (recipes::inPropertySet("minecraft:brewing_input", stack) || items::hasTag(stack.item, "minecraft:brewing_potion_inputs"));
}
bool isBrewingFuel(const ItemStack& stack) { return !stack.isEmpty() && items::hasComponent(stack, "minecraft:brewing_fuel"); }
bool isBrewingReagent(const ItemStack& stack) { return recipes::inPropertySet("minecraft:brewing_reagent", stack); }

namespace {
class PotionSlot : public Slot {
public:
    using Slot::Slot;
    bool mayPlace(const ItemStack& stack) override { return isPotionInput(stack); }
    int maxStackSize() override { return 1; }
};
class BrewingSlot : public Slot {
public:
    BrewingSlot(Container& container, int slot, bool (*mayPlace)(const ItemStack&)) : Slot(container, slot), mayPlace_(mayPlace) {}
    bool mayPlace(const ItemStack& stack) override { return mayPlace_(stack); }

private:
    bool (*mayPlace_)(const ItemStack&);
};
int tagInt(BlockEntityContainer& c, const char* key, int def = 0) {
    nbt::Compound* tag = c.tag();
    return tag ? tag->getInt(key, def) : def;
}
} // namespace

BrewingStandMenu::BrewingStandMenu(int containerId, ServerPlayer& player, std::shared_ptr<BlockEntityContainer> stand)
    : AbstractContainerMenu(registries::id("minecraft:menu", "minecraft:brewing_stand").value_or(0), containerId), stand_(std::move(stand)),
      inventory_(player.inventory) {
    stand_->startOpen(player);
    for (int i = 0; i < 3; i++) addSlot(std::make_unique<PotionSlot>(*stand_, i));
    addSlot(std::make_unique<BrewingSlot>(*stand_, 3, isBrewingReagent));
    addSlot(std::make_unique<BrewingSlot>(*stand_, 4, isBrewingFuel));
    auto* c = stand_.get();
    addDataSlot([c] { return tagInt(*c, "BrewTime"); });
    addDataSlot([c] { return tagInt(*c, "Fuel"); });
    addDataSlot([c] { return tagInt(*c, "total_brew_time", 400); });
    addDataSlot([c] { return tagInt(*c, "total_fuel", 20); });
    addStandardInventorySlots(inventory_);
}

bool BrewingStandMenu::stillValid(ServerPlayer& player) { return stand_->stillValid(player); }

void BrewingStandMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    stand_->stopOpen(player);
}

ItemStack BrewingStandMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    if ((index < 0 || index > 2) && index != 3 && index != 4) {
        if (isBrewingFuel(clicked)) {
            if (moveItemStackTo(stack, 4, 5, false) || (slot(3).mayPlace(stack) && !moveItemStackTo(stack, 3, 4, false))) return {};
        } else if (slot(3).mayPlace(stack)) {
            if (!moveItemStackTo(stack, 3, 4, false)) return {};
        } else if (isPotionInput(clicked)) {
            if (!moveItemStackTo(stack, 0, 3, false)) return {};
        } else if (index >= 5 && index < 32) {
            if (!moveItemStackTo(stack, 32, 41, false)) return {};
        } else if (index >= 32 && index < 41) {
            if (!moveItemStackTo(stack, 5, 32, false)) return {};
        } else if (!moveItemStackTo(stack, 5, 41, false)) {
            return {};
        }
    } else {
        if (!moveItemStackTo(stack, 5, 41, true)) return {};
        s.onQuickCraft(stack, clicked);
    }
    if (stack.isEmpty()) s.setByPlayer(ItemStack{});
    else s.setChanged();
    if (stack.count == clicked.count) return {};
    s.onTake(player, clicked);
    return clicked;
}

// ---------------------------------------------------------------- lectern

namespace {
// LecternBlockEntity.bookAccess over the block entity's "Book" and "Page".
class LecternContainer : public Container {
public:
    LecternContainer(ServerLevel& level, BlockPos pos) : level_(level), pos_(pos) {}
    int size() const override { return 1; }
    int maxStackSize() const override { return 1; }
    ItemStack& item(int) override {
        const nbt::Compound* tag = be();
        const nbt::Compound* book = tag ? tag->getCompound("Book") : nullptr;
        book_ = book ? item_nbt::read(*book).value_or(ItemStack{}) : ItemStack{};
        return book_;
    }
    ItemStack removeItem(int slot, int amount) override {
        if (slot != 0) return {};
        ItemStack book = item(0);
        ItemStack out = book.split(amount);
        book.clearIfEmpty();
        if (book.isEmpty()) onBookItemRemove();
        else if (nbt::Compound* tag = be()) tag->put("Book", item_nbt::write(book));
        return out;
    }
    // removeItemNoUpdate
    ItemStack take() {
        ItemStack book = item(0);
        onBookItemRemove();
        return book;
    }
    void setItem(int, ItemStack) override {}
    void setChanged() override { level_.blockEntitySetChanged(pos_); }
    bool stillValid(ServerPlayer& player) override { // stillValidBlockEntity && hasBook
        const ItemStack& book = item(0);
        return be() && player.isWithinBlockInteractionRange(pos_, 4.0) &&
               (items::hasComponent(book, "minecraft:writable_book_content") || items::hasComponent(book, "minecraft:written_book_content"));
    }
    int page() {
        nbt::Compound* tag = be();
        return tag ? tag->getInt("Page") : 0;
    }
    int pageCount() { // LecternBlockEntity.getPageCount
        const ItemStack& book = item(0);
        for (const char* type : {"minecraft:written_book_content", "minecraft:writable_book_content"}) {
            auto v = items::effectiveComponent(book, type);
            if (!v || !v->asCompound()) continue;
            const nbt::Tag* pages = v->asCompound()->get("pages");
            return pages && pages->asList() ? static_cast<int>(pages->asList()->items.size()) : 0;
        }
        return 0;
    }
    nbt::Compound* be() {
        LevelChunk* chunk = level_.getChunkIfLoaded({pos_.x >> 4, pos_.z >> 4});
        auto* e = chunk ? chunk->blockEntityAt(pos_.x, pos_.y, pos_.z) : nullptr;
        return e && e->tag.getString("id") == "minecraft:lectern" ? &e->tag : nullptr;
    }

private:
    void onBookItemRemove() {
        if (nbt::Compound* tag = be()) {
            tag->remove("Book");
            tag->remove("Page");
        }
        BlockStateId state = level_.getBlockState(pos_); // LecternBlock.resetBookState
        if (blocks::hasProperty(state, "has_book")) {
            level_.setBlock(pos_, blocks::setBool(blocks::setBool(state, "powered", false), "has_book", false));
            level_.updateNeighborsAt(pos_.below(), blocks::blockOf(state));
        }
    }
    ServerLevel& level_;
    BlockPos pos_;
    ItemStack book_;
};
} // namespace

LecternMenu::LecternMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : AbstractContainerMenu(registries::id("minecraft:menu", "minecraft:lectern").value_or(0), containerId),
      lectern_(std::make_shared<LecternContainer>(player.level(), pos)), level_(player.level()), pos_(pos) {
    addSlot(std::make_unique<Slot>(*lectern_, 0));
    auto* c = static_cast<LecternContainer*>(lectern_.get());
    addDataSlot([c] { return c->page(); });
}

bool LecternMenu::stillValid(ServerPlayer& player) { return lectern_->stillValid(player); }

void LecternMenu::setPage(int page) { // LecternBlockEntity.setPage, then broadcastChanges
    auto* c = static_cast<LecternContainer*>(lectern_.get());
    int newPage = std::clamp(page, 0, c->pageCount() - 1);
    if (newPage < 0) newPage = 0; // (Mth.clamp with an empty book: 0 wins)
    nbt::Compound* tag = c->be();
    if (tag && newPage != tag->getInt("Page")) {
        tag->put("Page", newPage);
        level_.blockEntitySetChanged(pos_);
        // LecternBlock.signalPageChange: a redstone pulse
        BlockStateId state = level_.getBlockState(pos_);
        if (blocks::hasProperty(state, "powered")) block_behaviour::redstone_blocks::lecternSignalPageChange(level_, pos_, state);
    }
    broadcastChanges();
}

bool LecternMenu::clickMenuButton(ServerPlayer& player, int buttonId) {
    auto* c = static_cast<LecternContainer*>(lectern_.get());
    if (buttonId >= 100) {
        setPage(buttonId - 100);
        return true;
    }
    switch (buttonId) {
    case 1: setPage(c->page() - 1); return true;
    case 2: setPage(c->page() + 1); return true;
    case 3: {
        if (player.gameMode == GameType::Adventure || player.gameMode == GameType::Spectator) return false; // mayBuild
        ItemStack book = c->take();
        c->setChanged();
        if (!player.inventory.add(book) && !book.isEmpty()) player.drop(std::move(book), false);
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------- beacon

namespace {
// BeaconBlockEntity.BEACON_EFFECTS: the effects each pyramid level unlocks.
int requiredLevelsFor(std::optional<int> effect) {
    if (!effect) return 0;
    static const std::vector<std::vector<std::string_view>> kEffects = {
        {"minecraft:speed", "minecraft:haste"}, {"minecraft:resistance", "minecraft:jump_boost"}, {"minecraft:strength"}, {"minecraft:regeneration"}};
    std::string_view name = registries::name("minecraft:mob_effect", *effect);
    for (size_t i = 0; i < kEffects.size(); i++)
        for (auto e : kEffects[i])
            if (e == name) return static_cast<int>(i) + 1;
    return INT32_MAX;
}
class PaymentSlot : public Slot {
public:
    using Slot::Slot;
    bool mayPlace(const ItemStack& stack) override { return !stack.isEmpty() && items::hasTag(stack.item, "minecraft:beacon_payment_items"); }
    int maxStackSize() override { return 1; }
};
int encodeEffect(const nbt::Compound* be, const char* key) {
    if (!be || !be->get(key)) return 0;
    auto id = registries::id("minecraft:mob_effect", be->getString(key));
    return id && requiredLevelsFor(*id) != INT32_MAX ? *id + 1 : 0; // (filterEffect)
}
} // namespace

BeaconMenu::BeaconMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : AbstractContainerMenu(registries::id("minecraft:menu", "minecraft:beacon").value_or(0), containerId), payment_(1), player_(player), pos_(pos),
      inventory_(player.inventory) {
    addSlot(std::make_unique<PaymentSlot>(payment_, 0));
    addDataSlot([this] {
        nbt::Compound* be = beacon();
        return be ? be->getInt("Levels") : 0;
    });
    addDataSlot([this] { return encodeEffect(beacon(), "primary_effect"); });
    addDataSlot([this] { return encodeEffect(beacon(), "secondary_effect"); });
    addStandardInventorySlots(inventory_);
}

nbt::Compound* BeaconMenu::beacon() {
    LevelChunk* chunk = player_.level().getChunkIfLoaded({pos_.x >> 4, pos_.z >> 4});
    auto* e = chunk ? chunk->blockEntityAt(pos_.x, pos_.y, pos_.z) : nullptr;
    return e && e->tag.getString("id") == "minecraft:beacon" ? &e->tag : nullptr;
}

bool BeaconMenu::stillValid(ServerPlayer& player) {
    return blocks::is(player.level().getBlockState(pos_), gen::block::BEACON) && player.isWithinBlockInteractionRange(pos_, 4.0);
}

void BeaconMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    ItemStack stack = slot(0).remove(1);
    if (!stack.isEmpty()) player.drop(std::move(stack), false);
}

bool BeaconMenu::updateEffects(std::optional<int> primary, std::optional<int> secondary) {
    if (!slot(0).hasItem()) return true; // (nothing happens without a payment)
    nbt::Compound* be = beacon();
    int levels = be ? be->getInt("Levels") : 0;
    // BeaconBlockEntity.validateEffects
    int primaryLevel = requiredLevelsFor(primary), secondaryLevel = requiredLevelsFor(secondary);
    bool valid = !(secondary && levels < 4) && primaryLevel <= levels && secondaryLevel <= levels && primaryLevel < 4 &&
                 (secondaryLevel == 0 || secondaryLevel >= 4 || primary == secondary);
    if (!valid) return false;
    if (be) {
        if (levels > 0) // (playSound(BEACON_POWER_SELECT) when there's a beam; levels stand in for it)
            player_.level().playBlockSound(-1, pos_, registries::id("minecraft:sound_event", "minecraft:block.beacon.power_select").value_or(-1), 1.0f, 1.0f);
        auto put = [&](const char* key, std::optional<int> effect) {
            if (effect && requiredLevelsFor(effect) != INT32_MAX) be->put(key, std::string(registries::name("minecraft:mob_effect", *effect)));
            else be->remove(key);
        };
        put("primary_effect", primary);
        put("secondary_effect", secondary);
        player_.level().blockEntitySetChanged(pos_);
    }
    slot(0).remove(1);
    return true;
}

ItemStack BeaconMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    if (index == 0) {
        if (!moveItemStackTo(stack, 1, 37, true)) return {};
        s.onQuickCraft(stack, clicked);
    } else if (!slot(0).hasItem() && slot(0).mayPlace(stack) && stack.count == 1) {
        if (!moveItemStackTo(stack, 0, 1, false)) return {};
    } else if (index >= 1 && index < 28) {
        if (!moveItemStackTo(stack, 28, 37, false)) return {};
    } else if (index >= 28 && index < 37) {
        if (!moveItemStackTo(stack, 1, 28, false)) return {};
    } else if (!moveItemStackTo(stack, 1, 37, false)) {
        return {};
    }
    if (stack.isEmpty()) s.setByPlayer(ItemStack{});
    else s.setChanged();
    if (stack.count == clicked.count) return {};
    ItemStack left = stack;
    s.onTake(player, left);
    return clicked;
}

// ---------------------------------------------------------------- crafter

namespace {
class CrafterSlot : public Slot {
public:
    CrafterSlot(Container& container, int slot, CrafterMenu& menu) : Slot(container, slot), menu_(menu) {}
    bool mayPlace(const ItemStack& stack) override { return !menu_.isSlotDisabled(index) && Slot::mayPlace(stack); }
    void setChanged() override {
        Slot::setChanged();
        menu_.slotsChanged(container);
    }

private:
    CrafterMenu& menu_;
};
// NonInteractiveResultSlot: shows the result, can't be touched.
class PreviewSlot : public Slot {
public:
    using Slot::Slot;
    bool mayPlace(const ItemStack&) override { return false; }
    bool mayPickup(ServerPlayer&) override { return false; }
};
} // namespace

CrafterMenu::CrafterMenu(int containerId, ServerPlayer& player, std::shared_ptr<BlockEntityContainer> crafter)
    : AbstractContainerMenu(registries::id("minecraft:menu", "minecraft:crafter_3x3").value_or(0), containerId), crafter_(std::move(crafter)),
      inventory_(player.inventory) {
    crafter_->startOpen(player);
    for (int i = 0; i < 9; i++) addSlot(std::make_unique<CrafterSlot>(*crafter_, i, *this));
    addStandardInventorySlots(inventory_);
    addSlot(std::make_unique<PreviewSlot>(result_, 0));
    for (int i = 0; i < 9; i++) addDataSlot([this, i] { return isSlotDisabled(i) ? 1 : 0; });
    addDataSlot([c = crafter_.get()] { return tagInt(*c, "triggered"); });
    refreshRecipeResult();
}

bool CrafterMenu::isSlotDisabled(int slot) {
    nbt::Compound* tag = crafter_->tag();
    const std::vector<int32_t>* disabled = tag ? tag->getIntArray("disabled_slots") : nullptr;
    return disabled && std::find(disabled->begin(), disabled->end(), slot) != disabled->end();
}

void CrafterMenu::setSlotState(int slot, bool enabled) {
    if (slot < 0 || slot >= 9 || !crafter_->item(slot).isEmpty()) return; // slotCanBeDisabled
    nbt::Compound* tag = crafter_->tag();
    if (!tag) return;
    std::vector<int32_t> disabled;
    for (int i = 0; i < 9; i++)
        if (i == slot ? !enabled : isSlotDisabled(i)) disabled.push_back(i);
    tag->put("disabled_slots", disabled);
    crafter_->setChanged();
}

bool CrafterMenu::stillValid(ServerPlayer& player) { return crafter_->stillValid(player); }

void CrafterMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    crafter_->stopOpen(player);
}

void CrafterMenu::slotsChanged(Container&) { refreshRecipeResult(); }

void CrafterMenu::refreshRecipeResult() { // CrafterBlock.getPotentialResults
    std::vector<ItemStack> grid(9);
    for (int i = 0; i < 9; i++) grid[i] = crafter_->item(i);
    recipes::CraftingInput input = recipes::positioned(3, 3, grid).input;
    auto recipe = recipes::findCrafting(input);
    result_.setItem(0, recipe ? recipes::assemble(*recipe, input) : ItemStack{});
}

ItemStack CrafterMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem() || index >= 45) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    if (index < 9) {
        if (!moveItemStackTo(stack, 9, 45, true)) return {};
    } else if (!moveItemStackTo(stack, 0, 9, false)) {
        return {};
    }
    if (stack.isEmpty()) s.set(ItemStack{});
    else s.setChanged();
    if (stack.count == clicked.count) return {};
    ItemStack left = stack;
    s.onTake(player, left);
    return clicked;
}

} // namespace mc
