#include "world/item/item_logic.hpp"

#include <unordered_map>
#include <vector>

#include "core/component/data_components.hpp"
#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "generated/registries.hpp"
#include "nbt/json_nbt.hpp"
#include "util/json.hpp"

namespace mc::items {
namespace {
std::unordered_map<std::string_view, std::vector<bool>> g_tags;
const gen::RegistryInfo* g_items = nullptr;

bool blockMatchesRule(uint32_t block, std::string_view blocks) {
    if (!blocks.empty() && blocks[0] == '#') return blocks::hasTag(block, blocks.substr(1));
    std::string_view blockName = gen::kBlocks[block].name;
    while (!blocks.empty()) {
        size_t comma = blocks.find(',');
        if (blocks.substr(0, comma) == blockName) return true;
        if (comma == std::string_view::npos) break;
        blocks.remove_prefix(comma + 1);
    }
    return false;
}

std::optional<int> componentId(std::string_view type) { return registries::id("minecraft:data_component_type", type); }

// Item.components(), parsed on first use.
const json::Value& prototype(int32_t item) {
    static std::vector<std::optional<json::Value>> cache(gen::kItems.size());
    static const json::Value kEmpty;
    if (item < 0 || static_cast<size_t>(item) >= cache.size()) return kEmpty;
    auto& slot = cache[item];
    if (!slot) {
        std::string_view text = gen::kItems[item].defaults;
        try {
            slot = text.empty() ? json::Value{} : json::parse(text);
        } catch (const std::exception&) {
            slot = json::Value{};
        }
    }
    return *slot;
}

// The default value's network bytes (for patch normalization), cached per item and type.
const std::optional<std::vector<uint8_t>>& defaultBytes(int32_t item, int32_t type) {
    static std::unordered_map<uint64_t, std::optional<std::vector<uint8_t>>> cache;
    uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(item)) << 32 | static_cast<uint32_t>(type);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::optional<std::vector<uint8_t>> bytes;
    std::string_view name = registries::name("minecraft:data_component_type", type);
    const json::Value& proto = prototype(item);
    if (!name.empty() && proto.isObject() && proto.has(std::string(name))) bytes = components::toNetwork(type, nbt::fromJson(proto[std::string(name)]));
    return cache.emplace(key, std::move(bytes)).first->second;
}
} // namespace

std::optional<nbt::Tag> defaultComponent(int32_t item, std::string_view type) {
    const json::Value& proto = prototype(item);
    if (!proto.isObject() || !proto.has(std::string(type))) return std::nullopt;
    return nbt::fromJson(proto[std::string(type)]);
}

std::optional<nbt::Tag> effectiveComponent(const ItemStack& stack, std::string_view type) {
    if (auto id = componentId(type)) {
        for (const auto& c : stack.components)
            if (c.type == *id) return c.removed ? std::nullopt : components::toDisk(c.type, c.value);
    }
    if (const nbt::Tag* t = stack.diskComponents.get(type)) return *t;
    return defaultComponent(stack.item, type);
}

bool hasComponent(const ItemStack& stack, std::string_view type) { return effectiveComponent(stack, type).has_value(); }

void normalizeComponents(ItemStack& stack) {
    std::erase_if(stack.components, [&](const ItemStack::Component& c) {
        const auto& def = defaultBytes(stack.item, c.type);
        if (c.removed) return !defaultComponent(stack.item, registries::name("minecraft:data_component_type", c.type)).has_value();
        return def && *def == c.value;
    });
}

void removeComponent(ItemStack& stack, std::string_view type) {
    auto id = componentId(type);
    stack.diskComponents.remove(type);
    if (!id) return;
    std::erase_if(stack.components, [&](const ItemStack::Component& c) { return c.type == *id; });
    if (defaultComponent(stack.item, type)) stack.components.push_back({*id, true, {}});
}

void bootstrap() {
    for (const auto& r : gen::kStaticRegistries)
        if (r.name == "minecraft:item") g_items = &r;
    for (const auto& reg : gen::kTags) {
        if (reg.registry != "minecraft:item") continue;
        for (const auto& tag : reg.tags) {
            auto& bits = g_tags[tag.name];
            bits.assign(gen::kItems.size(), false);
            for (int32_t id : tag.ids)
                if (id >= 0 && static_cast<size_t>(id) < bits.size()) bits[id] = true;
        }
    }
}

std::optional<int32_t> byName(std::string_view n) {
    auto id = registries::id("minecraft:item", n);
    if (id) return *id;
    return std::nullopt;
}

std::string_view name(int32_t item) {
    return g_items && item >= 0 && static_cast<size_t>(item) < g_items->entries.size() ? g_items->entries[item] : "";
}

bool hasTag(int32_t item, std::string_view tag) {
    auto it = g_tags.find(tag);
    return it != g_tags.end() && item >= 0 && static_cast<size_t>(item) < it->second.size() && it->second[item];
}

std::vector<int32_t> tagItems(std::string_view tag) {
    std::vector<int32_t> out;
    auto it = g_tags.find(tag);
    if (it == g_tags.end()) return out;
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i]) out.push_back(static_cast<int32_t>(i));
    return out;
}

bool matchesItemRef(int32_t item, std::string_view ref) {
    if (!ref.empty() && ref[0] == '#') return hasTag(item, ref.substr(1));
    return name(item) == ref;
}

const ItemStack::Component* findComponent(const ItemStack& stack, std::string_view type) {
    auto id = componentId(type);
    if (!id) return nullptr;
    for (const auto& c : stack.components)
        if (c.type == *id) return c.removed ? nullptr : &c;
    return nullptr;
}

void setComponent(ItemStack& stack, std::string_view type, std::vector<uint8_t> value) {
    auto id = componentId(type);
    if (!id) return;
    // PatchedDataComponentMap.set: the prototype's own value isn't a patch entry.
    if (const auto& def = defaultBytes(stack.item, *id); def && *def == value) {
        std::erase_if(stack.components, [&](const ItemStack::Component& c) { return c.type == *id; });
        return;
    }
    for (auto& c : stack.components) {
        if (c.type == *id) {
            c.removed = false;
            c.value = std::move(value);
            return;
        }
    }
    stack.components.push_back({*id, false, std::move(value)});
}

std::optional<nbt::Tag> componentValue(const ItemStack& stack, std::string_view type) {
    if (const auto* c = findComponent(stack, type)) return components::toDisk(c->type, c->value);
    if (const nbt::Tag* t = stack.diskComponents.get(type)) return *t;
    return std::nullopt;
}

void setComponentValue(ItemStack& stack, std::string_view type, const nbt::Tag& value) {
    const components::ComponentType* t = components::byName(type);
    auto bytes = t ? components::toNetwork(t->id, value) : std::nullopt;
    if (bytes) setComponent(stack, type, std::move(*bytes));
    else stack.diskComponents.put(std::string(type), value);
}

void resetComponent(ItemStack& stack, std::string_view type) {
    if (auto id = componentId(type)) std::erase_if(stack.components, [&](const ItemStack::Component& c) { return c.type == *id; });
    stack.diskComponents.remove(type);
}

int enchantmentLevel(const ItemStack& stack, std::string_view enchantment) {
    const auto* c = findComponent(stack, "minecraft:enchantments");
    if (!c) return 0;
    auto want = registries::id("minecraft:enchantment", enchantment);
    if (!want) return 0;
    try {
        ByteReader in(c->value);
        int n = in.readVarInt();
        for (int i = 0; i < n; i++) {
            int id = in.readVarInt(), level = in.readVarInt();
            if (id == *want) return level;
        }
    } catch (const std::exception&) {
    }
    return 0;
}

namespace {
std::optional<int> varIntComponent(const ItemStack& stack, std::string_view type) {
    const auto* c = findComponent(stack, type);
    if (!c) return std::nullopt;
    try {
        ByteReader in(c->value);
        return in.readVarInt();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
} // namespace

int maxDamage(const ItemStack& stack) { return varIntComponent(stack, "minecraft:max_damage").value_or(stack.info().maxDamage); }

bool isDamageable(const ItemStack& stack) {
    return hasComponent(stack, "minecraft:max_damage") && !hasComponent(stack, "minecraft:unbreakable") && hasComponent(stack, "minecraft:damage");
}

int damage(const ItemStack& stack) { return varIntComponent(stack, "minecraft:damage").value_or(0); }

void setDamage(ItemStack& stack, int value) {
    ByteWriter w;
    w.writeVarInt(value);
    setComponent(stack, "minecraft:damage", w.data());
}

namespace {
// The item's tool: a patched minecraft:tool component (disk form), or the default from the item.
struct ToolRuleView {
    std::string blocks; // "#tag", or ids separated by ','
    float speed;        // < 0: not set
    int correct;        // -1: not set
};
struct ToolView {
    float defaultSpeed;
    int damagePerBlock;
    std::vector<ToolRuleView> rules;
};
std::optional<ToolView> toolOf(const ItemStack& stack) {
    if (stack.isEmpty()) return std::nullopt;
    static const auto kTool = componentId("minecraft:tool");
    for (const auto& c : stack.components) {
        if (c.type != kTool) continue;
        if (c.removed) return std::nullopt;
        auto disk = components::toDisk(c.type, c.value);
        const nbt::Compound* t = disk ? disk->asCompound() : nullptr;
        if (!t) break;
        ToolView v{static_cast<float>(t->getDouble("default_mining_speed", 1.0)), t->getInt("damage_per_block", 1), {}};
        if (const nbt::List* rules = t->getList("rules")) {
            for (const auto& r : rules->items) {
                const nbt::Compound* rc = r.asCompound();
                if (!rc) continue;
                ToolRuleView rv{"", rc->contains("speed") ? static_cast<float>(rc->getDouble("speed")) : -1.0f,
                                rc->contains("correct_for_drops") ? (rc->getBool("correct_for_drops") ? 1 : 0) : -1};
                if (const nbt::Tag* b = rc->get("blocks")) {
                    if (const std::string* one = b->asString()) rv.blocks = *one;
                    else if (const nbt::List* many = b->asList())
                        for (const auto& e : many->items)
                            if (e.asString()) rv.blocks += (rv.blocks.empty() ? "" : ",") + *e.asString();
                }
                v.rules.push_back(std::move(rv));
            }
        }
        return v;
    }
    if (stack.info().toolDefaultSpeed < 0) return std::nullopt;
    ToolView v{stack.info().toolDefaultSpeed, stack.info().toolDamagePerBlock, {}};
    for (const auto& r : stack.info().toolRules) v.rules.push_back({std::string(r.blocks), r.speed, r.correctForDrops});
    return v;
}
} // namespace

float miningSpeed(const ItemStack& stack, BlockStateId state) {
    auto tool = toolOf(stack);
    if (!tool) return 1.0f;
    uint32_t block = blocks::blockOf(state);
    for (const auto& rule : tool->rules)
        if (rule.speed >= 0 && blockMatchesRule(block, rule.blocks)) return rule.speed;
    return tool->defaultSpeed;
}

bool isCorrectForDrops(const ItemStack& stack, BlockStateId state) {
    auto tool = toolOf(stack);
    if (!tool) return false;
    uint32_t block = blocks::blockOf(state);
    for (const auto& rule : tool->rules)
        if (rule.correct >= 0 && blockMatchesRule(block, rule.blocks)) return rule.correct == 1;
    return false;
}

int toolDamagePerBlock(const ItemStack& stack) {
    auto tool = toolOf(stack);
    return tool ? tool->damagePerBlock : 0;
}

} // namespace mc::items
