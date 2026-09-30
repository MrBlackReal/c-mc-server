#include "world/item/enchantment/enchantment_helper.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "core/component/data_components.hpp"
#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/level/server_level.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc::enchantments {
namespace {

using json::Value;
Value g_defs;

// LevelBasedValue: a number, or {type: linear|levels_squared|clamped|fraction|exponent|lookup, ...}.
float levelBased(const Value& v, int level) {
    if (v.isNumber()) return static_cast<float>(v.number());
    std::string_view type = v["type"].string();
    if (type == "minecraft:linear") return static_cast<float>(v["base"].number()) + static_cast<float>(v["per_level_above_first"].number()) * (level - 1);
    if (type == "minecraft:levels_squared") return static_cast<float>(level * level) + static_cast<float>(v["added"].number());
    if (type == "minecraft:clamped")
        return std::clamp(levelBased(v["value"], level), static_cast<float>(v["min"].number()), static_cast<float>(v["max"].number()));
    if (type == "minecraft:fraction") {
        float d = levelBased(v["denominator"], level);
        return d == 0.0f ? 0.0f : levelBased(v["numerator"], level) / d;
    }
    if (type == "minecraft:exponent") return static_cast<float>(std::pow(levelBased(v["base"], level), levelBased(v["power"], level)));
    if (type == "minecraft:lookup") {
        const auto& values = v["values"].array();
        return level <= static_cast<int>(values.size()) ? static_cast<float>(values[level - 1].number()) : levelBased(v["fallback"], level);
    }
    log::debug("unsupported level-based value {}", type);
    return 0.0f;
}

// EnchantmentValueEffect.process
float valueEffect(const Value& e, int level, std::mt19937& rng, float value) {
    std::string_view type = e["type"].string();
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    if (type == "minecraft:add") return value + levelBased(e["value"], level);
    if (type == "minecraft:multiply") return value * levelBased(e["factor"], level);
    if (type == "minecraft:set") return levelBased(e["value"], level);
    if (type == "minecraft:exponential") return static_cast<float>(value * std::pow(levelBased(e["base"], level), levelBased(e["exponent"], level)));
    if (type == "minecraft:all_of") {
        for (const auto& sub : e["effects"].array()) value = valueEffect(sub, level, rng, value);
        return value;
    }
    if (type == "minecraft:remove_binomial") {
        float p = levelBased(e["chance"], level);
        int drop = 0;
        if (!(value <= 128.0f) && !(value * p < 20.0f) && !(value * (1.0f - p) < 20.0f)) {
            double mu = std::floor(value * p), sigma = std::sqrt(value * p * (1.0f - p));
            drop = static_cast<int>(std::clamp<double>(std::round(mu + std::normal_distribution<double>(0.0, 1.0)(rng) * sigma), 0, value));
        } else {
            for (int i = 0; i < static_cast<int>(value); i++) drop += unit(rng) < p;
        }
        return value - drop;
    }
    log::debug("unsupported enchantment value effect {}", type);
    return value;
}

// Enchantment.matchingSlot: the definition's "slots" (EquipmentSlotGroup names) include `slot`.
bool matchingSlot(const Value& def, EquipmentSlot slot) {
    std::string_view name;
    switch (slot) {
    case EquipmentSlot::MainHand: name = "mainhand"; break;
    case EquipmentSlot::OffHand: name = "offhand"; break;
    case EquipmentSlot::Feet: name = "feet"; break;
    case EquipmentSlot::Legs: name = "legs"; break;
    case EquipmentSlot::Chest: name = "chest"; break;
    case EquipmentSlot::Head: name = "head"; break;
    }
    bool hand = slot == EquipmentSlot::MainHand || slot == EquipmentSlot::OffHand;
    for (const auto& g : def["slots"].array()) {
        std::string_view group = g.string();
        if (group == "any" || group == name || (group == "hand" && hand) || (group == "armor" && !hand)) return true;
    }
    return false;
}

// The stack's enchantments (the minecraft:enchantments component) as (id, level).
std::vector<std::pair<std::string, int>> enchantmentsOf(const ItemStack& stack) {
    std::vector<std::pair<std::string, int>> out;
    auto v = items::componentValue(stack, "minecraft:enchantments");
    if (!v || !v->asCompound()) return out;
    for (const auto& [id, level] : v->asCompound()->entries)
        if (level.isNumeric()) out.emplace_back(id, static_cast<int>(level.asLong()));
    return out;
}

} // namespace

void bootstrap() {
    try {
        g_defs = json::parse(gen::kEnchantmentsJson);
    } catch (const std::exception& e) {
        log::error("Failed to parse enchantments: {}", e.what());
    }
}

float processValue(ServerLevel& level, const ItemStack& stack, std::string_view component, float value) {
    for (const auto& [id, lvl] : enchantmentsOf(stack)) {
        const Value& def = g_defs[id];
        for (const auto& effect : def["effects"][component].array()) {
            // ConditionalEffect: requirements are loot conditions in the enchanted-item context.
            if (effect.has("requirements")) {
                loot::BlockContext ctx{level, level.random(), 0, {}, &stack, nullptr, nullptr};
                if (!loot::testCondition(effect["requirements"], ctx)) continue;
            }
            value = valueEffect(effect["effect"], lvl, level.random(), value);
        }
    }
    return value;
}

int processDurabilityChange(ServerLevel& level, const ItemStack& stack, int amount) {
    return static_cast<int>(processValue(level, stack, "minecraft:item_damage", static_cast<float>(amount))); // MutableFloat.intValue
}

bool hasEffect(const ItemStack& stack, std::string_view component) {
    for (const auto& [id, lvl] : enchantmentsOf(stack))
        if (g_defs[id]["effects"].has(component)) return true;
    return false;
}

bool hasEffectInSlot(const ItemStack& stack, std::string_view component, EquipmentSlot slot) {
    for (const auto& [id, lvl] : enchantmentsOf(stack)) {
        const Value& def = g_defs[id];
        if (def["effects"].has(component) && matchingSlot(def, slot)) return true;
    }
    return false;
}

void forEachModifier(const ItemStack& stack, EquipmentSlot slot,
                     const std::function<void(std::string_view, std::string, double, int)>& fn) {
    for (const auto& [id, lvl] : enchantmentsOf(stack)) {
        const Value& def = g_defs[id];
        if (!matchingSlot(def, slot)) continue;
        for (const auto& a : def["effects"]["minecraft:attributes"].array()) {
            std::string_view op = a["operation"].string();
            int operation = op == "add_multiplied_base" ? 1 : op == "add_multiplied_total" ? 2 : 0;
            // EnchantmentAttributeEffect.idForSlot: "<id>/<slot>"
            std::string_view slotName = slot == EquipmentSlot::MainHand ? "mainhand" : slot == EquipmentSlot::OffHand ? "offhand"
                                      : slot == EquipmentSlot::Feet     ? "feet"     : slot == EquipmentSlot::Legs    ? "legs"
                                      : slot == EquipmentSlot::Chest    ? "chest"    : "head";
            fn(a["attribute"].string(), a["id"].string() + "/" + std::string(slotName), levelBased(a["amount"], lvl), operation);
        }
    }
}

// ---------------------------------------------------------------- crafting (anvil, grindstone, table)

namespace {
// An item holder set: "#tag", an id, or a list of ids.
bool itemSetContains(const Value& set, const ItemStack& stack) {
    if (set.isArray()) {
        for (const auto& e : set.array())
            if (items::matchesItemRef(stack.item, e.string())) return true;
        return false;
    }
    return set.isString() && items::matchesItemRef(stack.item, set.string());
}
// An enchantment holder set.
bool enchantmentSetContains(const Value& set, std::string_view id) {
    if (set.isArray()) {
        for (const auto& e : set.array())
            if (e.string() == id) return true;
        return false;
    }
    if (!set.isString()) return false;
    std::string_view s = set.string();
    if (!s.empty() && s[0] == '#') return registries::hasTag("minecraft:enchantment", id, s.substr(1));
    return s == id;
}
// Enchantment.Cost.calculate
int cost(const Value& c, int level) { return static_cast<int>(c["base"].number(0) + c["per_level_above_first"].number(0) * (level - 1)); }
int maxCost(std::string_view id, int level) { return cost(g_defs[id]["max_cost"], level); }
} // namespace

std::string_view componentTypeFor(const ItemStack& stack) {
    static const int32_t kEnchantedBook = items::byName("minecraft:enchanted_book").value_or(-1);
    return stack.item == kEnchantedBook ? "minecraft:stored_enchantments" : "minecraft:enchantments";
}

bool canStoreEnchantments(const ItemStack& stack) { return items::hasComponent(stack, componentTypeFor(stack)); }

namespace {
Enchantments listOf(const std::optional<nbt::Tag>& v) {
    Enchantments out;
    if (!v || !v->asCompound()) return out;
    for (const auto& [id, level] : v->asCompound()->entries)
        if (level.isNumeric()) out.emplace_back(id, static_cast<int>(level.asLong()));
    return out;
}
} // namespace

Enchantments forCrafting(const ItemStack& stack) { return listOf(items::effectiveComponent(stack, componentTypeFor(stack))); }

void setEnchantments(ItemStack& stack, const Enchantments& list) {
    nbt::Compound c;
    for (const auto& [id, level] : list) c.put(id, level);
    items::setComponentValue(stack, componentTypeFor(stack), c);
}

bool hasAnyEnchantments(const ItemStack& stack) {
    return !listOf(items::effectiveComponent(stack, "minecraft:enchantments")).empty() ||
           !listOf(items::effectiveComponent(stack, "minecraft:stored_enchantments")).empty();
}

int levelOf(const Enchantments& list, std::string_view id) {
    for (const auto& [e, l] : list)
        if (e == id) return l;
    return 0;
}

void set(Enchantments& list, const std::string& id, int level) {
    auto it = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == id; });
    if (level <= 0) {
        if (it != list.end()) list.erase(it);
    } else if (it != list.end()) {
        it->second = std::min(level, 255);
    } else {
        list.emplace_back(id, std::min(level, 255));
    }
}

void upgrade(Enchantments& list, const std::string& id, int level) {
    if (level > 0) set(list, id, std::max(levelOf(list, id), std::min(level, 255)));
}

bool isCurse(std::string_view id) { return registries::hasTag("minecraft:enchantment", id, "minecraft:curse"); }
bool canEnchant(std::string_view id, const ItemStack& stack) { return itemSetContains(g_defs[id]["supported_items"], stack); }
bool isPrimaryItem(std::string_view id, const ItemStack& stack) {
    const Value& def = g_defs[id];
    return canEnchant(id, stack) && (!def.has("primary_items") || itemSetContains(def["primary_items"], stack));
}
bool areCompatible(std::string_view a, std::string_view b) {
    return a != b && !enchantmentSetContains(g_defs[a]["exclusive_set"], b) && !enchantmentSetContains(g_defs[b]["exclusive_set"], a);
}
int maxLevel(std::string_view id) { return static_cast<int>(g_defs[id]["max_level"].number(1)); }
int anvilCost(std::string_view id) { return static_cast<int>(g_defs[id]["anvil_cost"].number(0)); }
int minCost(std::string_view id, int level) { return cost(g_defs[id]["min_cost"], level); }

namespace {
int enchantableValue(const ItemStack& stack) {
    auto v = items::effectiveComponent(stack, "minecraft:enchantable");
    if (!v || !v->asCompound()) return -1;
    return v->asCompound()->getInt("value");
}
} // namespace

int enchantmentCost(LegacyRandom& random, int slot, int bookcases, const ItemStack& stack) {
    if (enchantableValue(stack) < 0) return 0;
    bookcases = std::min(bookcases, 15);
    int selected = random.nextInt(8) + 1 + (bookcases >> 1) + random.nextInt(bookcases + 1);
    if (slot == 0) return std::max(selected / 3, 1);
    return slot == 1 ? selected * 2 / 3 + 1 : std::max(selected, bookcases * 2);
}

Enchantments selectEnchantment(LegacyRandom& random, const ItemStack& stack, int enchantmentCost) {
    Enchantments results;
    int enchantable = enchantableValue(stack);
    if (enchantable < 0) return results;
    enchantmentCost += 1 + random.nextInt(enchantable / 4 + 1) + random.nextInt(enchantable / 4 + 1);
    float span = (random.nextFloat() + random.nextFloat() - 1.0F) * 0.15F;
    enchantmentCost = std::max(1, static_cast<int>(std::floor(enchantmentCost + enchantmentCost * span + 0.5f))); // Math.round(float)
    // getAvailableEnchantmentResults
    static const int32_t kBook = items::byName("minecraft:book").value_or(-1);
    bool isBook = stack.item == kBook;
    Enchantments available;
    for (std::string_view id : registries::tagEntries("minecraft:enchantment", "minecraft:in_enchanting_table")) {
        if (!isBook && !isPrimaryItem(id, stack)) continue;
        for (int level = maxLevel(id); level >= 1; level--) {
            if (enchantmentCost >= minCost(id, level) && enchantmentCost <= maxCost(id, level)) {
                available.emplace_back(std::string(id), level);
                break;
            }
        }
    }
    // WeightedRandom.getRandomItem
    auto pick = [&]() {
        int total = 0;
        for (const auto& [id, l] : available) total += static_cast<int>(g_defs[id]["weight"].number(1));
        if (total <= 0) return;
        int r = random.nextInt(total);
        for (const auto& e : available) {
            r -= static_cast<int>(g_defs[e.first]["weight"].number(1));
            if (r < 0) {
                results.push_back(e);
                return;
            }
        }
    };
    if (available.empty()) return results;
    pick();
    while (random.nextInt(50) <= enchantmentCost) {
        if (!results.empty()) {
            const std::string last = results.back().first;
            std::erase_if(available, [&](const auto& e) { return !areCompatible(last, e.first); });
        }
        if (available.empty()) break;
        pick();
        enchantmentCost /= 2;
    }
    return results;
}

} // namespace mc::enchantments
