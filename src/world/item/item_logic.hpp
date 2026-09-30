#pragma once

#include <optional>
#include <string_view>

#include "world/item/item_stack.hpp"
#include "world/level/block/block_state.hpp"

namespace mc::items {

void bootstrap();

std::optional<int32_t> byName(std::string_view name);
std::string_view name(int32_t item);
// Item tags as sent to clients, e.g. "minecraft:swords".
bool hasTag(int32_t item, std::string_view tag);
// Items in an item tag (TagKey<Item>), in id order.
std::vector<int32_t> tagItems(std::string_view tag);
// "minecraft:x", "#minecraft:tag" or a list of those (loot table / predicate item matchers).
bool matchesItemRef(int32_t item, std::string_view ref);

// Component access on the network-encoded patch.
const ItemStack::Component* findComponent(const ItemStack& stack, std::string_view type);
void setComponent(ItemStack& stack, std::string_view type, std::vector<uint8_t> value);
// The same in the saved (Codec NBT) form, converted through core/component/data_components.
std::optional<nbt::Tag> componentValue(const ItemStack& stack, std::string_view type);
void setComponentValue(ItemStack& stack, std::string_view type, const nbt::Tag& value);
// Drops `type` from the patch, back to the item's default (ItemStack.remove for components the
// prototype doesn't have).
void resetComponent(ItemStack& stack, std::string_view type);
// ItemStack.remove: no value at all (a removal entry if the prototype has one).
void removeComponent(ItemStack& stack, std::string_view type);
// The item's default value (Item.components()), in saved form; empty if it has none.
std::optional<nbt::Tag> defaultComponent(int32_t item, std::string_view type);
// ItemStack.get: the patched value, else the default (empty if removed or absent).
std::optional<nbt::Tag> effectiveComponent(const ItemStack& stack, std::string_view type);
bool hasComponent(const ItemStack& stack, std::string_view type);
// PatchedDataComponentMap.applyPatch against the stack's (new) item: entries equal to the
// prototype's value, and removals of components it doesn't have, drop out of the patch.
void normalizeComponents(ItemStack& stack);
int enchantmentLevel(const ItemStack& stack, std::string_view enchantment);

// Durability (DataComponents.DAMAGE / MAX_DAMAGE).
int maxDamage(const ItemStack& stack);
int damage(const ItemStack& stack);
void setDamage(ItemStack& stack, int damage);
// ItemStack.isDamageableItem: max_damage and damage, and not unbreakable.
bool isDamageable(const ItemStack& stack);

// Tool component, patched or default (Tool.getMiningSpeed / isCorrectForDrops / damagePerBlock).
float miningSpeed(const ItemStack& stack, BlockStateId state);
bool isCorrectForDrops(const ItemStack& stack, BlockStateId state);
int toolDamagePerBlock(const ItemStack& stack); // 0 if not a tool

} // namespace mc::items
