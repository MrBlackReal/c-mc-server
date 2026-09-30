#include "world/inventory/workstation_menus.hpp"

#include <cmath>

#include "core/registries.hpp"
#include "generated/blocks.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "world/entity/experience_orb.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/item/item_logic.hpp"

namespace mc {

namespace {

int menuId(std::string_view name) { return registries::id("minecraft:menu", name).value_or(0); }
int32_t soundId(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }
int32_t itemId(std::string_view name) { return items::byName(name).value_or(-1); }

// A slot nothing can be put into, with a callback when its stack is taken.
class TakeOnlySlot : public Slot {
public:
    TakeOnlySlot(Container& container, int slot, std::function<void(ServerPlayer&, const ItemStack&)> onTake)
        : Slot(container, slot), onTake_(std::move(onTake)) {}
    bool mayPlace(const ItemStack&) override { return false; }
    void onTake(ServerPlayer& player, const ItemStack& carried) override { onTake_(player, carried); }

private:
    std::function<void(ServerPlayer&, const ItemStack&)> onTake_;
};

// A slot with a placement rule (and optionally a stack limit).
class FilteredSlot : public Slot {
public:
    FilteredSlot(Container& container, int slot, std::function<bool(const ItemStack&)> mayPlace, int maxStack = -1)
        : Slot(container, slot), mayPlace_(std::move(mayPlace)), maxStack_(maxStack) {}
    bool mayPlace(const ItemStack& stack) override { return !mayPlace_ || mayPlace_(stack); }
    int maxStackSize() override { return maxStack_ > 0 ? maxStack_ : Slot::maxStackSize(); }

private:
    std::function<bool(const ItemStack&)> mayPlace_;
    int maxStack_;
};

// The end of every quickMoveStack: an emptied slot is cleared (setByPlayer), a changed one marked,
// and onTake runs when something moved. Returns the original stack, or empty if nothing moved.
ItemStack finishQuickMove(ServerPlayer& player, Slot& slot, ItemStack& stack, const ItemStack& clicked) {
    if (stack.isEmpty()) slot.setByPlayer(ItemStack{});
    else slot.setChanged();
    if (stack.count == clicked.count) return {};
    ItemStack left = stack;
    slot.onTake(player, left);
    return clicked;
}

bool isBlank(std::string_view s) { // StringUtil.isBlank
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    return true;
}

// StringUtil.filterText: without § and control characters. Returns the filtered text and its
// length in UTF-16 units (String.length).
std::pair<std::string, size_t> filterText(std::string_view in) {
    std::string out;
    size_t units = 0;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        len = std::min(len, in.size() - i);
        uint32_t cp = c;
        if (len == 2) cp = (c & 0x1F) << 6 | (in[i + 1] & 0x3F);
        else if (len == 3) cp = (c & 0x0F) << 12 | (in[i + 1] & 0x3F) << 6 | (in[i + 2] & 0x3F);
        else if (len == 4) cp = (c & 0x07) << 18 | (in[i + 1] & 0x3F) << 12 | (in[i + 2] & 0x3F) << 6 | (in[i + 3] & 0x3F);
        if (cp != 0xA7 && cp >= 0x20 && cp != 0x7F) { // StringUtil.isAllowedChatCharacter
            out.append(in.substr(i, len));
            units += cp > 0xFFFF ? 2 : 1;
        }
        i += len;
    }
    return {out, units};
}

// The plain text of a text component in saved form (a string, or {text, extra}).
std::string plainText(const nbt::Tag& t) {
    if (const std::string* s = t.asString()) return *s;
    std::string out;
    if (const nbt::Compound* c = t.asCompound()) {
        out = c->getString("text");
        if (const nbt::Tag* extra = c->get("extra"))
            if (const nbt::List* l = extra->asList())
                for (const auto& e : l->items) out += plainText(e);
    }
    return out;
}

// ItemStack.getHoverName().getString(), as far as we can tell without translations: the custom
// name, else the item name's key.
std::string hoverNameString(const ItemStack& stack) {
    if (auto name = items::componentValue(stack, "minecraft:custom_name")) return plainText(*name);
    if (auto name = items::effectiveComponent(stack, "minecraft:item_name")) {
        if (const nbt::Compound* c = name->asCompound(); c && c->get("translate")) return c->getString("translate");
        return plainText(*name);
    }
    return std::string(items::name(stack.item));
}

// ItemStack.transmuteCopy: the same stack as another item.
ItemStack transmuteCopy(const ItemStack& stack, int32_t item) {
    ItemStack out = stack;
    out.item = item;
    items::normalizeComponents(out);
    return out;
}

int repairCost(const ItemStack& stack) { // getOrDefault(REPAIR_COST, 0)
    auto v = items::effectiveComponent(stack, "minecraft:repair_cost");
    return v && v->isNumeric() ? static_cast<int>(v->asLong()) : 0;
}
void setRepairCost(ItemStack& stack, int cost) { items::setComponentValue(stack, "minecraft:repair_cost", nbt::Tag(cost)); }
void setMaxDamage(ItemStack& stack, int value) { items::setComponentValue(stack, "minecraft:max_damage", nbt::Tag(value)); }
void setDamageValue(ItemStack& stack, int value) { items::setDamage(stack, std::clamp(value, 0, items::maxDamage(stack))); }

// AnvilMenu.calculateIncreasedRepairCost
int increasedRepairCost(int cost) { return static_cast<int>(std::min<int64_t>(cost * 2LL + 1, INT32_MAX)); }

} // namespace

// ---------------------------------------------------------------- BlockMenu

BlockMenu::BlockMenu(int menuType, int containerId, ServerPlayer& player, BlockPos pos)
    : AbstractContainerMenu(menuType, containerId), player_(player), pos_(pos), inventory_(player.inventory) {}

bool BlockMenu::stillValidIf(ServerPlayer& player, const std::function<bool(BlockStateId)>& isValidBlock) {
    return isValidBlock(player.level().getBlockState(pos_)) && player.isWithinBlockInteractionRange(pos_, 4.0);
}

void BlockMenu::playTakeSound(std::string_view sound) {
    ServerLevel& level = player_.level();
    if (lastSoundTime_ == level.gameTime()) return;
    level.playBlockSound(-1, pos_, soundId(sound), 1.0f, 1.0f);
    lastSoundTime_ = level.gameTime();
}

// ---------------------------------------------------------------- stonecutter

class StonecutterResultSlot : public Slot {
public:
    StonecutterResultSlot(StonecutterMenu& menu) : Slot(menu.result_, 0), menu_(menu) {}
    bool mayPlace(const ItemStack&) override { return false; }
    void onTake(ServerPlayer& player, const ItemStack& carried) override {
        // (onCraftedBy and awardUsedRecipes: stats and the recipe book aren't ported)
        ItemStack removed = menu_.slot(0).remove(1);
        if (!removed.isEmpty()) menu_.setupResultSlot(menu_.selected_);
        menu_.playTakeSound("minecraft:ui.stonecutter.take_result");
        Slot::onTake(player, carried);
    }

private:
    StonecutterMenu& menu_;
};

StonecutterMenu::StonecutterMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : BlockMenu(menuId("minecraft:stonecutter"), containerId, player, pos), container_(1, [this](Container& c) { slotsChanged(c); }) {
    addSlot(std::make_unique<Slot>(container_, 0));
    addSlot(std::make_unique<StonecutterResultSlot>(*this));
    addStandardInventorySlots(inventory_);
    addDataSlot([this] { return selected_; });
}

bool StonecutterMenu::stillValid(ServerPlayer& player) {
    return stillValidIf(player, [](BlockStateId s) { return blocks::is(s, gen::block::STONECUTTER); });
}

bool StonecutterMenu::clickMenuButton(ServerPlayer&, int buttonId) {
    if (selected_ == buttonId) return false;
    if (isValidRecipeIndex(buttonId)) {
        selected_ = buttonId;
        setupResultSlot(buttonId);
    }
    return true;
}

void StonecutterMenu::slotsChanged(Container&) {
    const ItemStack& input = slot(0).item();
    auto itemOf = [](const ItemStack& s) { return s.isEmpty() ? 0 : s.item; };
    if (itemOf(input) != itemOf(input_)) { // !input.is(this.input.getItem())
        input_ = input;
        setupRecipeList(input);
    }
}

void StonecutterMenu::setupRecipeList(const ItemStack& input) {
    selected_ = -1;
    slot(1).set(ItemStack{});
    recipes_ = input.isEmpty() ? std::vector<recipes::Stonecutting>{} : recipes::stonecuttingFor(input);
}

void StonecutterMenu::setupResultSlot(int index) {
    if (!recipes_.empty() && isValidRecipeIndex(index)) slot(1).set(recipes_[index].result); // SingleItemRecipe.assemble
    else slot(1).set(ItemStack{});
    broadcastChanges();
}

ItemStack StonecutterMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    if (index == 1) {
        if (!moveItemStackTo(stack, 2, 38, true)) return {};
        s.onQuickCraft(stack, clicked);
    } else if (index == 0) {
        if (!moveItemStackTo(stack, 2, 38, false)) return {};
    } else if (recipes::acceptsStonecutting(stack)) {
        if (!moveItemStackTo(stack, 0, 1, false)) return {};
    } else if (index >= 2 && index < 29) {
        if (!moveItemStackTo(stack, 29, 38, false)) return {};
    } else if (index >= 29 && index < 38 && !moveItemStackTo(stack, 2, 29, false)) {
        return {};
    }
    if (stack.isEmpty()) s.setByPlayer(ItemStack{});
    s.setChanged();
    if (stack.count == clicked.count) return {};
    ItemStack left = stack;
    s.onTake(player, left);
    if (index == 1) player.drop(left, false);
    broadcastChanges();
    return clicked;
}

void StonecutterMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    result_.item(0) = {};
    clearContainer(player, container_);
}

// ---------------------------------------------------------------- grindstone

namespace {
bool grindable(const ItemStack& s) { return items::isDamageable(s) || enchantments::hasAnyEnchantments(s); }

// GrindstoneMenu.removeNonCursesFrom
ItemStack removeNonCurses(ItemStack item) {
    auto list = enchantments::forCrafting(item);
    bool storable = enchantments::canStoreEnchantments(item);
    std::erase_if(list, [](const auto& e) { return !enchantments::isCurse(e.first); });
    if (storable) enchantments::setEnchantments(item, list); // EnchantmentHelper.updateEnchantments
    static const int32_t kEnchantedBook = itemId("minecraft:enchanted_book"), kBook = itemId("minecraft:book");
    if (item.item == kEnchantedBook && list.empty()) item = transmuteCopy(item, kBook);
    int cost = 0;
    for (size_t i = 0; i < list.size(); i++) cost = increasedRepairCost(cost);
    setRepairCost(item, cost);
    return item;
}
} // namespace

class GrindstoneResultSlot : public Slot {
public:
    explicit GrindstoneResultSlot(GrindstoneMenu& menu) : Slot(menu.result_, 0), menu_(menu) {}
    bool mayPlace(const ItemStack&) override { return false; }
    void onTake(ServerPlayer& player, const ItemStack&) override {
        ServerLevel& level = player.level();
        BlockPos pos = menu_.pos_;
        ExperienceOrb::award(level, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, experience(level));
        level.levelEvent(1042, pos, 0);
        menu_.repair_.setItem(0, ItemStack{});
        menu_.repair_.setItem(1, ItemStack{});
    }

private:
    int experience(ServerLevel& level) {
        int amount = fromItem(menu_.repair_.item(0)) + fromItem(menu_.repair_.item(1));
        if (amount <= 0) return 0;
        int half = static_cast<int>(std::ceil(amount / 2.0));
        return half + std::uniform_int_distribution<int>(0, half - 1)(level.random());
    }
    static int fromItem(const ItemStack& item) {
        int amount = 0;
        for (const auto& [id, lvl] : enchantments::forCrafting(item))
            if (!enchantments::isCurse(id)) amount += enchantments::minCost(id, lvl);
        return amount;
    }
    GrindstoneMenu& menu_;
};

GrindstoneMenu::GrindstoneMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : BlockMenu(menuId("minecraft:grindstone"), containerId, player, pos), repair_(2, [this](Container& c) { slotsChanged(c); }) {
    addSlot(std::make_unique<FilteredSlot>(repair_, 0, grindable));
    addSlot(std::make_unique<FilteredSlot>(repair_, 1, grindable));
    addSlot(std::make_unique<GrindstoneResultSlot>(*this));
    addStandardInventorySlots(inventory_);
}

bool GrindstoneMenu::stillValid(ServerPlayer& player) {
    return stillValidIf(player, [](BlockStateId s) { return blocks::is(s, gen::block::GRINDSTONE); });
}

void GrindstoneMenu::slotsChanged(Container& container) {
    AbstractContainerMenu::slotsChanged(container);
    if (&container == &repair_) createResult();
}

void GrindstoneMenu::createResult() {
    result_.setItem(0, computeResult(repair_.item(0), repair_.item(1)));
    broadcastChanges();
}

ItemStack GrindstoneMenu::computeResult(const ItemStack& input, const ItemStack& additional) {
    if (input.isEmpty() && additional.isEmpty()) return {};
    if (input.count > 1 || additional.count > 1) return {};
    if (input.isEmpty() || additional.isEmpty()) {
        const ItemStack& item = !input.isEmpty() ? input : additional;
        return enchantments::hasAnyEnchantments(item) ? removeNonCurses(item) : ItemStack{};
    }
    return mergeItems(input, additional);
}

ItemStack GrindstoneMenu::mergeItems(const ItemStack& input, const ItemStack& additional) {
    if (input.item != additional.item) return {};
    int durability = std::max(items::maxDamage(input), items::maxDamage(additional));
    int remaining1 = items::maxDamage(input) - items::damage(input);
    int remaining2 = items::maxDamage(additional) - items::damage(additional);
    int remaining = remaining1 + remaining2 + durability * 5 / 100;
    int count = 1;
    if (!items::isDamageable(input)) {
        if (input.maxStackSize() < 2 || !ItemStack::matches(input, additional)) return {};
        count = 2;
    }
    ItemStack item = input.copyWithCount(count);
    if (items::isDamageable(item)) {
        setMaxDamage(item, durability);
        setDamageValue(item, std::max(durability - remaining, 0));
    }
    // mergeEnchantsFrom
    if (enchantments::canStoreEnchantments(item)) {
        auto list = enchantments::forCrafting(item);
        for (const auto& [id, lvl] : enchantments::forCrafting(additional))
            if (!enchantments::isCurse(id) || enchantments::levelOf(list, id) == 0) enchantments::upgrade(list, id, lvl);
        enchantments::setEnchantments(item, list);
    }
    return removeNonCurses(std::move(item));
}

void GrindstoneMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    clearContainer(player, repair_);
}

ItemStack GrindstoneMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    bool bothFull = !repair_.item(0).isEmpty() && !repair_.item(1).isEmpty();
    if (index == 2) {
        if (!moveItemStackTo(stack, 3, 39, true)) return {};
        s.onQuickCraft(stack, clicked);
    } else if (index != 0 && index != 1) {
        if (bothFull) {
            if (index >= 3 && index < 30) {
                if (!moveItemStackTo(stack, 30, 39, false)) return {};
            } else if (index >= 30 && index < 39 && !moveItemStackTo(stack, 3, 30, false)) {
                return {};
            }
        } else if (!moveItemStackTo(stack, 0, 2, false)) {
            return {};
        }
    } else if (!moveItemStackTo(stack, 3, 39, false)) {
        return {};
    }
    return finishQuickMove(player, s, stack, clicked);
}

// ---------------------------------------------------------------- item combiners

class CombinerResultSlot : public Slot {
public:
    explicit CombinerResultSlot(ItemCombinerMenu& menu) : Slot(menu.resultSlots_, 0), menu_(menu) {}
    bool mayPlace(const ItemStack&) override { return false; }
    bool mayPickup(ServerPlayer& player) override { return menu_.mayPickupResult(player, hasItem()); }
    void onTake(ServerPlayer& player, const ItemStack& carried) override { menu_.onTakeResult(player, carried); }

private:
    ItemCombinerMenu& menu_;
};

ItemCombinerMenu::ItemCombinerMenu(int menuType, int containerId, ServerPlayer& player, BlockPos pos,
                                   std::vector<std::function<bool(const ItemStack&)>> inputs)
    : BlockMenu(menuType, containerId, player, pos), inputSlots_(static_cast<int>(inputs.size()), [this](Container& c) { slotsChanged(c); }),
      resultSlots_([this](Container& c) { slotsChanged(c); }), resultSlot_(static_cast<int>(inputs.size())) {
    for (int i = 0; i < static_cast<int>(inputs.size()); i++) addSlot(std::make_unique<FilteredSlot>(inputSlots_, i, inputs[i]));
    addSlot(std::make_unique<CombinerResultSlot>(*this));
    addStandardInventorySlots(inventory_);
}

void ItemCombinerMenu::slotsChanged(Container& container) {
    AbstractContainerMenu::slotsChanged(container);
    if (&container == &inputSlots_) createResult();
}

void ItemCombinerMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    clearContainer(player, inputSlots_);
}

bool ItemCombinerMenu::stillValid(ServerPlayer& player) {
    return stillValidIf(player, [this](BlockStateId s) { return isValidBlock(s); });
}

ItemStack ItemCombinerMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    int invStart = resultSlot_ + 1, invEnd = invStart + 27, useRowStart = invEnd, useRowEnd = useRowStart + 9;
    if (index == resultSlot_) {
        if (!moveItemStackTo(stack, invStart, useRowEnd, true)) return {};
        s.onQuickCraft(stack, clicked);
    } else if (index >= 0 && index < resultSlot_) {
        if (!moveItemStackTo(stack, invStart, useRowEnd, false)) return {};
    } else if (canMoveIntoInputSlots(stack) && index >= invStart && index < useRowEnd) {
        if (!moveItemStackTo(stack, 0, resultSlot_, false)) return {};
    } else if (index >= invStart && index < invEnd) {
        if (!moveItemStackTo(stack, useRowStart, useRowEnd, false)) return {};
    } else if (index >= useRowStart && index < useRowEnd && !moveItemStackTo(stack, invStart, invEnd, false)) {
        return {};
    }
    return finishQuickMove(player, s, stack, clicked);
}

// ---------------------------------------------------------------- smithing

SmithingMenu::SmithingMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : ItemCombinerMenu(menuId("minecraft:smithing"), containerId, player, pos,
                       {[](const ItemStack& s) { return recipes::inPropertySet("minecraft:smithing_template", s); },
                        [](const ItemStack& s) { return recipes::inPropertySet("minecraft:smithing_base", s); },
                        [](const ItemStack& s) { return recipes::inPropertySet("minecraft:smithing_addition", s); }}) {
    addDataSlot([this] { return hasRecipeError_; });
}

bool SmithingMenu::isValidBlock(BlockStateId state) { return blocks::is(state, gen::block::SMITHING_TABLE); }

void SmithingMenu::onTakeResult(ServerPlayer&, const ItemStack&) {
    shrinkStackInSlot(0);
    shrinkStackInSlot(1);
    shrinkStackInSlot(2);
    player_.level().levelEvent(1044, pos_, 0);
}

void SmithingMenu::shrinkStackInSlot(int slot) {
    ItemStack stack = inputSlots_.item(slot);
    if (stack.isEmpty()) return;
    stack.shrink(1);
    stack.clearIfEmpty();
    inputSlots_.setItem(slot, stack);
}

void SmithingMenu::slotsChanged(Container& container) {
    ItemCombinerMenu::slotsChanged(container);
    hasRecipeError_ = slot(0).hasItem() && slot(1).hasItem() && slot(2).hasItem() && !slot(resultSlot()).hasItem();
}

void SmithingMenu::createResult() {
    const ItemStack &t = inputSlots_.item(0), &b = inputSlots_.item(1), &a = inputSlots_.item(2);
    auto recipe = recipes::findSmithing(t, b, a);
    resultSlots_.setItem(0, recipe ? recipes::assembleSmithing(*recipe, b, a) : ItemStack{});
}

bool SmithingMenu::canMoveIntoInputSlots(const ItemStack& stack) {
    if (recipes::inPropertySet("minecraft:smithing_template", stack) && !slot(0).hasItem()) return true;
    if (recipes::inPropertySet("minecraft:smithing_base", stack) && !slot(1).hasItem()) return true;
    return recipes::inPropertySet("minecraft:smithing_addition", stack) && !slot(2).hasItem();
}

// ---------------------------------------------------------------- anvil

AnvilMenu::AnvilMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : ItemCombinerMenu(menuId("minecraft:anvil"), containerId, player, pos, {nullptr, nullptr}) {
    addDataSlot([this] { return cost_; });
}

bool AnvilMenu::isValidBlock(BlockStateId state) { return blocks::stateHasTag(state, "minecraft:anvil"); }

bool AnvilMenu::mayPickupResult(ServerPlayer& player, bool) { return (player.hasInfiniteMaterials() || player.experienceLevel >= cost_) && cost_ > 0; }

void AnvilMenu::onTakeResult(ServerPlayer& player, const ItemStack&) {
    if (!player.hasInfiniteMaterials()) player.giveExperienceLevels(-cost_);
    if (repairItemCountCost_ > 0) {
        ItemStack addition = inputSlots_.item(1);
        if (!addition.isEmpty() && addition.count > repairItemCountCost_) {
            addition.shrink(repairItemCountCost_);
            inputSlots_.setItem(1, addition);
        } else {
            inputSlots_.setItem(1, ItemStack{});
        }
    } else if (!onlyRenaming_) {
        inputSlots_.setItem(1, ItemStack{});
    }
    cost_ = 0;
    inputSlots_.setItem(0, ItemStack{});
    ServerLevel& level = player.level();
    BlockStateId state = level.getBlockState(pos_);
    std::uniform_real_distribution<float> uf(0.0f, 1.0f);
    if (!player.hasInfiniteMaterials() && blocks::stateHasTag(state, "minecraft:anvil") && uf(level.random()) < 0.12F) {
        // AnvilBlock.damage: anvil -> chipped -> damaged -> gone
        uint32_t block = blocks::blockOf(state);
        uint32_t next = block == gen::block::ANVIL ? gen::block::CHIPPED_ANVIL : block == gen::block::CHIPPED_ANVIL ? gen::block::DAMAGED_ANVIL : 0;
        if (next == 0) {
            level.setBlock(pos_, blocks::defaultState(gen::block::AIR));
            level.levelEvent(1029, pos_, 0);
        } else {
            BlockStateId damaged = blocks::setValue(blocks::defaultState(next), "facing", blocks::getValue(state, "facing").value_or("north"));
            level.setBlock(pos_, damaged, ServerLevel::kUpdateClients);
            level.levelEvent(1030, pos_, 0);
        }
    } else {
        level.levelEvent(1030, pos_, 0);
    }
}

void AnvilMenu::createResult() {
    const ItemStack& input = inputSlots_.item(0);
    onlyRenaming_ = false;
    cost_ = 1;
    int price = 0;
    int64_t tax = 0;
    int namingCost = 0;
    auto fail = [&] {
        resultSlots_.setItem(0, ItemStack{});
        cost_ = 0;
    };
    if (input.isEmpty() || !enchantments::canStoreEnchantments(input)) return fail();
    ItemStack result = input;
    const ItemStack& addition = inputSlots_.item(1);
    auto list = enchantments::forCrafting(result);
    tax += static_cast<int64_t>(repairCost(input)) + repairCost(addition);
    repairItemCountCost_ = 0;
    if (!addition.isEmpty()) {
        bool usingBook = items::hasComponent(addition, "minecraft:stored_enchantments");
        bool validRepairItem = false; // ItemStack.isValidRepairItem
        if (auto repairable = items::effectiveComponent(input, "minecraft:repairable"); repairable && repairable->asCompound()) {
            if (const nbt::Tag* its = repairable->asCompound()->get("items")) {
                if (const std::string* ref = its->asString()) validRepairItem = items::matchesItemRef(addition.item, *ref);
                else if (const nbt::List* l = its->asList())
                    for (const auto& e : l->items)
                        if (e.asString() && items::matchesItemRef(addition.item, *e.asString())) validRepairItem = true;
            }
        }
        if (items::isDamageable(result) && validRepairItem) {
            int repairAmount = std::min(items::damage(result), items::maxDamage(result) / 4);
            if (repairAmount <= 0) return fail();
            int count = 0;
            for (; repairAmount > 0 && count < addition.count; count++) {
                setDamageValue(result, items::damage(result) - repairAmount);
                price++;
                repairAmount = std::min(items::damage(result), items::maxDamage(result) / 4);
            }
            repairItemCountCost_ = count;
        } else {
            if (!usingBook && (result.item != addition.item || !items::isDamageable(result))) return fail();
            if (items::isDamageable(result) && !usingBook) {
                int remaining1 = items::maxDamage(input) - items::damage(input);
                int remaining2 = items::maxDamage(addition) - items::damage(addition);
                int additional = remaining2 + items::maxDamage(result) * 12 / 100;
                int resultDamage = std::max(items::maxDamage(result) - (remaining1 + additional), 0);
                if (resultDamage < items::damage(result)) {
                    setDamageValue(result, resultDamage);
                    price += 2;
                }
            }
            bool anyCompatible = false, anyIncompatible = false;
            static const int32_t kEnchantedBook = itemId("minecraft:enchanted_book");
            for (const auto& [id, addLevel] : enchantments::forCrafting(addition)) {
                int current = enchantments::levelOf(list, id);
                int level = current == addLevel ? addLevel + 1 : std::max(addLevel, current);
                bool compatible = enchantments::canEnchant(id, input);
                if (player_.hasInfiniteMaterials() || input.item == kEnchantedBook) compatible = true;
                for (const auto& [other, l] : list) {
                    if (other != id && !enchantments::areCompatible(id, other)) {
                        compatible = false;
                        price++;
                    }
                }
                if (!compatible) {
                    anyIncompatible = true;
                } else {
                    anyCompatible = true;
                    level = std::min(level, enchantments::maxLevel(id));
                    enchantments::set(list, id, level);
                    int fee = enchantments::anvilCost(id);
                    if (usingBook) fee = std::max(1, fee / 2);
                    price += fee * level;
                    if (input.count > 1) price = 40;
                }
            }
            if (anyIncompatible && !anyCompatible) return fail();
        }
    }
    if (itemName_ && !isBlank(*itemName_)) {
        if (*itemName_ != hoverNameString(input)) {
            namingCost = 1;
            price += namingCost;
            items::setComponentValue(result, "minecraft:custom_name", nbt::Tag(*itemName_)); // Component.literal
        }
    } else if (items::hasComponent(input, "minecraft:custom_name")) {
        namingCost = 1;
        price += namingCost;
        items::removeComponent(result, "minecraft:custom_name");
    }
    cost_ = price <= 0 ? 0 : static_cast<int>(std::clamp<int64_t>(tax + price, 0, INT32_MAX));
    if (price <= 0) result = {};
    if (namingCost == price && namingCost > 0) {
        if (cost_ >= 40) cost_ = 39;
        onlyRenaming_ = true;
    }
    if (cost_ >= 40 && !player_.hasInfiniteMaterials()) result = {};
    if (!result.isEmpty()) {
        int baseCost = std::max(repairCost(result), repairCost(addition));
        if (namingCost != price || namingCost == 0) baseCost = increasedRepairCost(baseCost);
        setRepairCost(result, baseCost);
        enchantments::setEnchantments(result, list);
    }
    resultSlots_.setItem(0, result);
    broadcastChanges();
}

bool AnvilMenu::setItemName(const std::string& name) {
    auto [filtered, length] = filterText(name);
    if (length > 50 || filtered == itemName_) return false; // validateName
    itemName_ = filtered;
    if (slot(2).hasItem()) {
        ItemStack& stack = slot(2).item();
        if (isBlank(filtered)) items::removeComponent(stack, "minecraft:custom_name");
        else items::setComponentValue(stack, "minecraft:custom_name", nbt::Tag(filtered));
    }
    createResult();
    return true;
}

// ---------------------------------------------------------------- loom

namespace {
bool isBanner(const ItemStack& s) { return !s.isEmpty() && s.info().itemClass == "BannerItem"; }
bool isDyeItem(const ItemStack& s) { return !s.isEmpty() && items::hasTag(s.item, "minecraft:loom_dyes") && items::hasComponent(s, "minecraft:dye"); }
bool isPatternItem(const ItemStack& s) {
    return !s.isEmpty() && items::hasTag(s.item, "minecraft:loom_patterns") && items::hasComponent(s, "minecraft:provides_banner_patterns");
}
} // namespace

class LoomResultSlot : public Slot {
public:
    explicit LoomResultSlot(LoomMenu& menu) : Slot(menu.output_, 0), menu_(menu) {}
    bool mayPlace(const ItemStack&) override { return false; }
    void onTake(ServerPlayer& player, const ItemStack& carried) override {
        menu_.slot(0).remove(1);
        menu_.slot(1).remove(1);
        if (!menu_.slot(0).hasItem() || !menu_.slot(1).hasItem()) menu_.selected_ = -1;
        menu_.playTakeSound("minecraft:ui.loom.take_result");
        Slot::onTake(player, carried);
    }

private:
    LoomMenu& menu_;
};

LoomMenu::LoomMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : BlockMenu(menuId("minecraft:loom"), containerId, player, pos), input_(3, [this](Container& c) { slotsChanged(c); }), output_(1) {
    addSlot(std::make_unique<FilteredSlot>(input_, 0, isBanner));
    addSlot(std::make_unique<FilteredSlot>(input_, 1, isDyeItem));
    addSlot(std::make_unique<FilteredSlot>(input_, 2, isPatternItem));
    addSlot(std::make_unique<LoomResultSlot>(*this));
    addStandardInventorySlots(inventory_);
    addDataSlot([this] { return selected_; });
}

bool LoomMenu::stillValid(ServerPlayer& player) {
    return stillValidIf(player, [](BlockStateId s) { return blocks::is(s, gen::block::LOOM); });
}

bool LoomMenu::clickMenuButton(ServerPlayer&, int buttonId) {
    if (buttonId < 0 || buttonId >= static_cast<int>(patterns_.size())) return false;
    selected_ = buttonId;
    setupResultSlot(patterns_[buttonId]);
    return true;
}

std::vector<std::string> LoomMenu::selectablePatterns(const ItemStack& pattern) const {
    std::vector<std::string> out;
    auto addTag = [&](std::string_view tag) {
        for (std::string_view e : registries::tagEntries("minecraft:banner_pattern", tag)) out.emplace_back(e);
    };
    if (pattern.isEmpty()) {
        addTag("minecraft:no_item_required");
        return out;
    }
    auto set = items::effectiveComponent(pattern, "minecraft:provides_banner_patterns");
    if (!set) return out;
    if (const std::string* s = set->asString()) {
        if (!s->empty() && (*s)[0] == '#') addTag(std::string_view(*s).substr(1));
        else out.push_back(*s);
    } else if (const nbt::List* l = set->asList()) {
        for (const auto& e : l->items)
            if (e.asString()) out.push_back(*e.asString());
    }
    return out;
}

void LoomMenu::slotsChanged(Container&) {
    const ItemStack &banner = slot(0).item(), &dye = slot(1).item(), &pattern = slot(2).item();
    if (banner.isEmpty() || dye.isEmpty()) {
        slot(3).set(ItemStack{});
        patterns_.clear();
        selected_ = -1;
        return;
    }
    bool validIndex = selected_ >= 0 && selected_ < static_cast<int>(patterns_.size());
    std::vector<std::string> previous = std::move(patterns_);
    patterns_ = selectablePatterns(pattern);
    std::optional<std::string> display;
    if (patterns_.size() == 1) {
        selected_ = 0;
        display = patterns_[0];
    } else if (!validIndex) {
        selected_ = -1;
    } else {
        const std::string& value = previous[selected_];
        auto it = std::find(patterns_.begin(), patterns_.end(), value);
        if (it != patterns_.end()) {
            display = value;
            selected_ = static_cast<int>(it - patterns_.begin());
        } else {
            selected_ = -1;
        }
    }
    if (display) {
        auto layers = items::effectiveComponent(banner, "minecraft:banner_patterns");
        size_t count = layers && layers->asList() ? layers->asList()->items.size() : 0;
        if (count >= 6) {
            selected_ = -1;
            slot(3).set(ItemStack{});
        } else {
            setupResultSlot(*display);
        }
    } else {
        slot(3).set(ItemStack{});
    }
    broadcastChanges();
}

void LoomMenu::setupResultSlot(const std::string& pattern) {
    const ItemStack &banner = slot(0).item(), &dye = slot(1).item();
    ItemStack result;
    if (!banner.isEmpty() && !dye.isEmpty()) {
        auto color = items::effectiveComponent(dye, "minecraft:dye");
        if (color && color->asString()) {
            result = banner.copyWithCount(1);
            auto layers = items::effectiveComponent(result, "minecraft:banner_patterns");
            nbt::List list = layers && layers->asList() ? *layers->asList() : nbt::List{};
            nbt::Compound layer;
            layer.put("pattern", pattern);
            layer.put("color", *color->asString());
            list.items.push_back(std::move(layer));
            items::setComponentValue(result, "minecraft:banner_patterns", list);
        }
    }
    if (!ItemStack::matches(result, slot(3).item())) slot(3).set(result);
}

void LoomMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    clearContainer(player, input_);
}

ItemStack LoomMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    if (index == 3) {
        if (!moveItemStackTo(stack, 4, 40, true)) return {};
        s.onQuickCraft(stack, clicked);
    } else if (index != 1 && index != 0 && index != 2) {
        if (isBanner(stack)) {
            if (!moveItemStackTo(stack, 0, 1, false)) return {};
        } else if (isDyeItem(stack)) {
            if (!moveItemStackTo(stack, 1, 2, false)) return {};
        } else if (isPatternItem(stack)) {
            if (!moveItemStackTo(stack, 2, 3, false)) return {};
        } else if (index >= 4 && index < 31) {
            if (!moveItemStackTo(stack, 31, 40, false)) return {};
        } else if (index >= 31 && index < 40 && !moveItemStackTo(stack, 4, 31, false)) {
            return {};
        }
    } else if (!moveItemStackTo(stack, 4, 40, false)) {
        return {};
    }
    return finishQuickMove(player, s, stack, clicked);
}

// ---------------------------------------------------------------- enchanting table

EnchantmentMenu::EnchantmentMenu(int containerId, ServerPlayer& player, BlockPos pos)
    : BlockMenu(menuId("minecraft:enchantment"), containerId, player, pos), slots_(2, [this](Container& c) { slotsChanged(c); }) {
    addSlot(std::make_unique<FilteredSlot>(slots_, 0, nullptr, 1));
    static const int32_t kLapis = itemId("minecraft:lapis_lazuli");
    addSlot(std::make_unique<FilteredSlot>(slots_, 1, [](const ItemStack& s) { return s.item == kLapis; }));
    addStandardInventorySlots(inventory_);
    for (int i = 0; i < 3; i++) addDataSlot([this, i] { return costs_[i]; });
    seed_ = player.enchantmentSeed;
    addDataSlot([this] { return seed_; });
    for (int i = 0; i < 3; i++) addDataSlot([this, i] { return enchantClue_[i]; });
    for (int i = 0; i < 3; i++) addDataSlot([this, i] { return levelClue_[i]; });
}

bool EnchantmentMenu::stillValid(ServerPlayer& player) {
    return stillValidIf(player, [](BlockStateId s) { return blocks::is(s, gen::block::ENCHANTING_TABLE); });
}

void EnchantmentMenu::slotsChanged(Container& container) {
    if (&container != &slots_) return;
    const ItemStack& stack = slots_.item(0);
    // ItemStack.isEnchantable: enchantable, with an empty enchantments component
    auto enchants = stack.isEmpty() ? std::nullopt : items::effectiveComponent(stack, "minecraft:enchantments");
    if (!enchants || !items::hasComponent(stack, "minecraft:enchantable") || !enchants->asCompound() || !enchants->asCompound()->entries.empty()) {
        for (int i = 0; i < 3; i++) costs_[i] = 0, enchantClue_[i] = -1, levelClue_[i] = -1;
        return;
    }
    ServerLevel& level = player_.level();
    int bookcases = 0; // EnchantingTableBlock.BOOKSHELF_OFFSETS / isValidBookShelf
    for (int y = 0; y <= 1; y++)
        for (int x = -2; x <= 2; x++)
            for (int z = -2; z <= 2; z++) {
                if (std::abs(x) != 2 && std::abs(z) != 2) continue;
                BlockPos shelf{pos_.x + x, pos_.y + y, pos_.z + z}, between{pos_.x + x / 2, pos_.y + y, pos_.z + z / 2};
                if (blocks::stateHasTag(level.getBlockState(shelf), "minecraft:enchantment_power_provider") &&
                    blocks::stateHasTag(level.getBlockState(between), "minecraft:enchantment_power_transmitter"))
                    bookcases++;
            }
    random_.setSeed(seed_);
    for (int i = 0; i < 3; i++) {
        costs_[i] = enchantments::enchantmentCost(random_, i, bookcases, stack);
        enchantClue_[i] = -1;
        levelClue_[i] = -1;
        if (costs_[i] < i + 1) costs_[i] = 0;
    }
    for (int i = 0; i < 3; i++) {
        if (costs_[i] <= 0) continue;
        auto list = enchantmentList(stack, i, costs_[i]);
        if (list.empty()) continue;
        const auto& ench = list[random_.nextInt(static_cast<int>(list.size()))];
        enchantClue_[i] = registries::id("minecraft:enchantment", ench.first).value_or(-1);
        levelClue_[i] = ench.second;
    }
    broadcastChanges();
}

std::vector<std::pair<std::string, int>> EnchantmentMenu::enchantmentList(const ItemStack& stack, int slot, int cost) {
    random_.setSeed(static_cast<int64_t>(seed_) + slot);
    auto list = enchantments::selectEnchantment(random_, stack, cost);
    static const int32_t kBook = itemId("minecraft:book");
    if (stack.item == kBook && list.size() > 1) list.erase(list.begin() + random_.nextInt(static_cast<int>(list.size())));
    return list;
}

bool EnchantmentMenu::clickMenuButton(ServerPlayer& player, int buttonId) {
    if (buttonId < 0 || buttonId >= 3) return false;
    ItemStack& stack = slots_.item(0);
    ItemStack& currency = slots_.item(1);
    int levels = buttonId + 1;
    if ((currency.isEmpty() || currency.count < levels) && !player.hasInfiniteMaterials()) return false;
    if (costs_[buttonId] <= 0 || stack.isEmpty() ||
        ((player.experienceLevel < levels || player.experienceLevel < costs_[buttonId]) && !player.hasInfiniteMaterials()))
        return false;
    auto list = enchantmentList(stack, buttonId, costs_[buttonId]);
    if (list.empty()) return true;
    // Player.onEnchantmentPerformed
    player.experienceLevel -= levels;
    if (player.experienceLevel < 0) player.experienceLevel = 0, player.experienceProgress = 0, player.totalExperience = 0;
    player.enchantmentSeed = std::uniform_int_distribution<int32_t>(INT32_MIN, INT32_MAX)(player.level().random());
    static const int32_t kBook = itemId("minecraft:book"), kEnchantedBook = itemId("minecraft:enchanted_book");
    if (stack.item == kBook) slots_.setItem(0, transmuteCopy(stack, kEnchantedBook));
    ItemStack& item = slots_.item(0);
    auto enchants = enchantments::forCrafting(item);
    for (const auto& [id, level] : list) enchantments::upgrade(enchants, id, level); // ItemStack.enchant
    enchantments::setEnchantments(item, enchants);
    if (!player.hasInfiniteMaterials()) currency.shrink(levels); // ItemStack.consume
    if (currency.isEmpty()) slots_.setItem(1, ItemStack{});
    slots_.setChanged();
    seed_ = player.enchantmentSeed;
    slotsChanged(slots_);
    std::uniform_real_distribution<float> uf(0.0f, 1.0f);
    player.level().playBlockSound(-1, pos_, soundId("minecraft:block.enchantment_table.use"), 1.0f, uf(player.level().random()) * 0.1f + 0.9f);
    return true;
}

void EnchantmentMenu::removed(ServerPlayer& player) {
    AbstractContainerMenu::removed(player);
    clearContainer(player, slots_);
}

ItemStack EnchantmentMenu::quickMoveStack(ServerPlayer& player, int index) {
    Slot& s = slot(index);
    if (!s.hasItem()) return {};
    ItemStack& stack = s.item();
    ItemStack clicked = stack;
    static const int32_t kLapis = itemId("minecraft:lapis_lazuli");
    if (index == 0 || index == 1) {
        if (!moveItemStackTo(stack, 2, 38, true)) return {};
    } else if (stack.item == kLapis) {
        if (!moveItemStackTo(stack, 1, 2, true)) return {};
    } else {
        if (slot(0).hasItem() || !slot(0).mayPlace(stack)) return {};
        ItemStack single = stack.copyWithCount(1);
        stack.shrink(1);
        slot(0).setByPlayer(single);
    }
    return finishQuickMove(player, s, stack, clicked);
}

} // namespace mc
