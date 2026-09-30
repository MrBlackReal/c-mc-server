#include "world/item/bundle.hpp"

#include <numeric>
#include <optional>
#include <random>
#include <vector>

#include "core/registries.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "world/inventory/menus.hpp"
#include "world/item/item_logic.hpp"
#include "world/item/item_nbt.hpp"

namespace mc::bundle {

namespace {

// org.apache.commons.lang3.math.Fraction, as far as bundles use it (always reduced).
struct Fraction {
    int64_t num = 0, den = 1;
    static Fraction of(int64_t n, int64_t d) {
        if (d < 0) n = -n, d = -d;
        int64_t g = std::gcd(n < 0 ? -n : n, d);
        return g > 1 ? Fraction{n / g, d / g} : Fraction{n, d};
    }
    Fraction operator+(Fraction o) const { return of(num * o.den + o.num * den, den * o.den); }
    Fraction operator-(Fraction o) const { return of(num * o.den - o.num * den, den * o.den); }
    Fraction operator*(int64_t k) const { return of(num * k, den); }
    Fraction operator/(Fraction o) const { return of(num * o.den, den * o.num); }
    int intValue() const { return static_cast<int>(num / den); }
    bool operator<=(Fraction o) const { return num * o.den <= o.num * den; }
};
const Fraction kOne{1, 1};

std::optional<nbt::List> contentsTag(const ItemStack& stack) {
    auto v = items::effectiveComponent(stack, "minecraft:bundle_contents");
    if (!v || !v->asList()) return std::nullopt;
    return *v->asList();
}

std::vector<ItemStack> itemsOf(const nbt::List& list) {
    std::vector<ItemStack> out;
    for (const auto& e : list.items)
        if (const nbt::Compound* c = e.asCompound())
            if (auto s = item_nbt::read(*c); s && !s->isEmpty()) out.push_back(*s);
    return out;
}

int maxStackSize(const ItemStack& stack) {
    auto v = items::effectiveComponent(stack, "minecraft:max_stack_size");
    return v && v->isNumeric() ? static_cast<int>(v->asLong()) : stack.maxStackSize();
}

Fraction contentWeight(const std::vector<ItemStack>& items);

// BundleContents.getWeight
Fraction weightOf(const ItemStack& stack) {
    if (auto nested = contentsTag(stack)) return contentWeight(itemsOf(*nested)) + Fraction::of(1, 16);
    auto bees = items::effectiveComponent(stack, "minecraft:bees");
    if (bees && bees->asList() && !bees->asList()->items.empty()) return kOne;
    return Fraction::of(1, maxStackSize(stack));
}

Fraction contentWeight(const std::vector<ItemStack>& items) {
    Fraction w;
    for (const auto& s : items) w = w + weightOf(s) * s.count;
    return w;
}

bool canItemBeInBundle(const ItemStack& s) { return !s.isEmpty() && s.info().fitsInContainers; }

// BundleContents.Mutable
struct Contents {
    std::vector<ItemStack> items;
    Fraction weight;
    int selected = -1;

    int maxAmountToAdd(Fraction itemWeight) const { return std::max(((kOne - weight) / itemWeight).intValue(), 0); }

    int tryInsert(ItemStack& toAdd) {
        if (!canItemBeInBundle(toAdd)) return 0;
        Fraction itemWeight = weightOf(toAdd);
        int amount = std::min(toAdd.count, maxAmountToAdd(itemWeight));
        if (amount == 0) return 0;
        weight = weight + itemWeight * amount;
        int index = -1;
        if (toAdd.isStackable())
            for (size_t i = 0; i < items.size(); i++)
                if (ItemStack::isSameItemSameComponents(items[i], toAdd)) {
                    index = static_cast<int>(i);
                    break;
                }
        if (index != -1) {
            ItemStack merged = items[index].copyWithCount(items[index].count + amount);
            items.erase(items.begin() + index);
            toAdd.shrink(amount);
            toAdd.clearIfEmpty();
            items.insert(items.begin(), merged);
        } else {
            items.insert(items.begin(), toAdd.split(amount));
            toAdd.clearIfEmpty();
        }
        return amount;
    }

    int tryTransfer(Slot& slot, ServerPlayer& player) {
        ItemStack other = slot.item();
        int max = maxAmountToAdd(weightOf(other));
        if (!canItemBeInBundle(other)) return 0;
        ItemStack taken = slot.safeTake(other.count, max, player);
        return tryInsert(taken);
    }

    void toggleSelected(int index) { selected = selected != index && index >= 0 && index < static_cast<int>(items.size()) ? index : -1; }

    std::optional<ItemStack> removeOne() {
        if (items.empty()) return std::nullopt;
        int index = selected < 0 || selected >= static_cast<int>(items.size()) ? 0 : selected;
        ItemStack stack = items[index];
        items.erase(items.begin() + index);
        weight = weight - weightOf(stack) * stack.count;
        toggleSelected(-1);
        return stack;
    }
};

// BundleContents.asMutable (null for a stack without bundle contents).
std::optional<Contents> contentsOf(const ItemStack& stack) {
    auto tag = contentsTag(stack);
    if (!tag) return std::nullopt;
    Contents c;
    c.items = itemsOf(*tag);
    c.weight = contentWeight(c.items);
    c.selected = stack.bundleSelected;
    return c;
}

// toImmutable + ItemStack.set
void store(ItemStack& stack, const Contents& c) {
    nbt::List list{nbt::TagType::Compound, {}};
    for (const auto& s : c.items) list.items.push_back(item_nbt::write(s));
    items::setComponentValue(stack, "minecraft:bundle_contents", list);
    stack.bundleSelected = c.selected;
}

void playSound(ServerPlayer& player, std::string_view sound, float volume, bool randomPitch) {
    std::uniform_real_distribution<float> uf(0.0f, 1.0f);
    float pitch = randomPitch ? 0.8f + uf(player.level().random()) * 0.4f : 1.0f;
    player.level().playSound(player.id(), player.x, player.y, player.z, registries::id("minecraft:sound_event", sound).value_or(-1),
                             ServerLevel::kSoundSourcePlayers, volume, pitch);
}
void insertSound(ServerPlayer& p) { playSound(p, "minecraft:item.bundle.insert", 0.8f, true); }
void insertFailSound(ServerPlayer& p) { playSound(p, "minecraft:item.bundle.insert_fail", 1.0f, false); }
void removeOneSound(ServerPlayer& p) { playSound(p, "minecraft:item.bundle.remove_one", 0.8f, true); }

// BundleItem.broadcastChangesOnContainerMenu
void broadcast(ServerPlayer& player) {
    PlayerInventoryContainer inventory(player.inventory);
    if (player.containerMenu) player.containerMenu->slotsChanged(inventory);
}

} // namespace

bool overrideStackedOnOther(ItemStack& self, Slot& slot, bool primary, ServerPlayer& player) {
    auto contents = contentsOf(self);
    if (!contents) return false;
    const ItemStack& other = slot.item();
    if (primary && !other.isEmpty()) {
        if (contents->tryTransfer(slot, player) > 0) insertSound(player);
        else insertFailSound(player);
    } else if (!primary && other.isEmpty()) {
        if (auto stack = contents->removeOne()) {
            ItemStack remainder = slot.safeInsert(*stack);
            if (remainder.count > 0) contents->tryInsert(remainder);
            else removeOneSound(player);
        }
    } else {
        return false;
    }
    store(self, *contents);
    broadcast(player);
    return true;
}

bool overrideOtherStackedOnMe(ItemStack& self, ItemStack& other, Slot& slot, bool primary, ServerPlayer& player) {
    if (primary && other.isEmpty()) {
        toggleSelectedItem(self, -1);
        return false;
    }
    auto contents = contentsOf(self);
    if (!contents) return false;
    if (primary && !other.isEmpty()) {
        if (slot.allowModification(player) && contents->tryInsert(other) > 0) insertSound(player);
        else insertFailSound(player);
    } else if (!primary && other.isEmpty()) {
        if (slot.allowModification(player)) {
            if (auto removed = contents->removeOne()) {
                removeOneSound(player);
                other = *removed; // carriedItem.set
            }
        }
    } else {
        toggleSelectedItem(self, -1);
        return false;
    }
    store(self, *contents);
    broadcast(player);
    return true;
}

void toggleSelectedItem(ItemStack& stack, int selected) {
    auto contents = contentsOf(stack);
    if (!contents) return;
    contents->toggleSelected(selected);
    store(stack, *contents);
}

} // namespace mc::bundle
