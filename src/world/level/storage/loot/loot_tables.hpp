#pragma once

#include <random>
#include <string_view>
#include <vector>

#include "core/direction.hpp"
#include "nbt/tag.hpp"
#include "world/item/item_stack.hpp"
#include "util/json.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {

class ServerLevel;
class ServerPlayer;

// Port of the loot table system for block drops (LootTable, LootPool, entries, conditions,
// modifiers), interpreting the vanilla JSON from data/loot_tables.json.
namespace loot {

struct BlockContext {
    ServerLevel& level;
    std::mt19937& random;
    BlockStateId state;
    BlockPos pos;
    const ItemStack* tool;                 // LootContextParams.TOOL
    const ServerPlayer* entity;            // THIS_ENTITY (the breaking player), may be null
    const nbt::Compound* blockEntity;      // BLOCK_ENTITY, as saved
};

void bootstrap();
// LootItemCondition.test (the JSON form, e.g. an enchantment effect's requirements).
bool testCondition(const json::Value& condition, BlockContext& ctx);
// ContextIntProvider.getInt for a minecraft:context_int_provider id or an inline provider's JSON
// (item components store either), e.g. a compostable item's layers.
int contextInt(std::string_view provider, BlockContext& ctx);
float contextFloat(std::string_view provider, BlockContext& ctx);
// Entry names in a registry tag, e.g. the enchantments in #minecraft:prevents_decorated_pot_shattering.
std::vector<std::string_view> registryTagEntries(std::string_view registry, std::string_view tag);
// LootTable.getRandomItems for `table` (e.g. "minecraft:blocks/stone"), split into valid stacks.
std::vector<ItemStack> getRandomItems(std::string_view table, BlockContext& ctx);

} // namespace loot
} // namespace mc
