#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "world/entity/player/inventory.hpp"
#include "util/legacy_random.hpp"
#include "world/item/item_stack.hpp"

namespace mc {

class ServerLevel;

// Port of the parts of EnchantmentHelper the server uses, driven by the enchantment definitions
// from the jar (data/enchantments.json): LevelBasedValue, value effects and attribute effects.
namespace enchantments {

void bootstrap();

// EnchantmentHelper.runIterationOnItem with a value effect component: `value` after every
// enchantment's `component` effects (e.g. "minecraft:block_experience", "minecraft:repair_with_xp").
float processValue(ServerLevel& level, const ItemStack& stack, std::string_view component, float value);
// EnchantmentHelper.processDurabilityChange: minecraft:item_damage effects (Unbreaking).
int processDurabilityChange(ServerLevel& level, const ItemStack& stack, int amount);
// EnchantmentHelper.has: one of the stack's enchantments has `component` effects.
bool hasEffect(const ItemStack& stack, std::string_view component);
// Whether one of the stack's enchantments has `component` effects and applies in `slot`
// (EnchantmentHelper.getRandomItemWith's filter).
bool hasEffectInSlot(const ItemStack& stack, std::string_view component, EquipmentSlot slot);

// EnchantmentHelper.forEachModifier: attribute modifiers of the stack's enchantments that apply
// in `slot`. The callback gets (attribute, modifier id, amount, operation 0-2).
void forEachModifier(const ItemStack& stack, EquipmentSlot slot,
                     const std::function<void(std::string_view attribute, std::string id, double amount, int operation)>& fn);

// ItemEnchantments as (enchantment id, level), in the component's order.
using Enchantments = std::vector<std::pair<std::string, int>>;

// EnchantmentHelper.getComponentType: stored_enchantments for enchanted books, else enchantments.
std::string_view componentTypeFor(const ItemStack& stack);
// EnchantmentHelper.canStoreEnchantments / getEnchantmentsForCrafting / setEnchantments.
bool canStoreEnchantments(const ItemStack& stack);
Enchantments forCrafting(const ItemStack& stack);
void setEnchantments(ItemStack& stack, const Enchantments& list);
// EnchantmentHelper.hasAnyEnchantments: enchantments or stored_enchantments (defaults included).
bool hasAnyEnchantments(const ItemStack& stack);
// ItemEnchantments.Mutable.set / upgrade.
void set(Enchantments& list, const std::string& id, int level);
void upgrade(Enchantments& list, const std::string& id, int level);
int levelOf(const Enchantments& list, std::string_view id);

// Enchantment definition queries.
bool isCurse(std::string_view id);
bool canEnchant(std::string_view id, const ItemStack& stack);   // supported_items
bool isPrimaryItem(std::string_view id, const ItemStack& stack); // supported and primary_items
bool areCompatible(std::string_view a, std::string_view b);      // Enchantment.areCompatible
int maxLevel(std::string_view id);
int anvilCost(std::string_view id);
int minCost(std::string_view id, int level);

// EnchantmentHelper.getEnchantmentCost and selectEnchantment (the enchanting table), over the
// minecraft:in_enchanting_table tag.
int enchantmentCost(LegacyRandom& random, int slot, int bookcases, const ItemStack& stack);
Enchantments selectEnchantment(LegacyRandom& random, const ItemStack& stack, int cost);

} // namespace enchantments
} // namespace mc
