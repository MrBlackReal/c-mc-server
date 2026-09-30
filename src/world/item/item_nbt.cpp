#include "world/item/item_nbt.hpp"

#include "core/component/data_components.hpp"
#include "core/registries.hpp"
#include "generated/registries.hpp"

namespace mc::item_nbt {
namespace {
constexpr std::string_view kNetworkKey = "cmc:network_components";
} // namespace

nbt::Compound write(const ItemStack& stack) {
    nbt::Compound tag;
    for (const auto& r : gen::kStaticRegistries)
        if (r.name == "minecraft:item") tag.put("id", std::string(r.entries[stack.item]));
    tag.put("count", stack.count);

    nbt::Compound components = stack.diskComponents;
    nbt::List untranslated;
    for (const auto& c : stack.components) {
        const components::ComponentType* type = components::byId(c.type);
        if (!type || !type->persistent) continue; // transient components (creative_slot_lock, ...) aren't saved
        if (c.removed) {
            components.put("!" + std::string(type->name), nbt::Compound{});
            continue;
        }
        if (auto disk = components::toDisk(c.type, c.value)) {
            components.put(std::string(type->name), std::move(*disk));
        } else {
            nbt::Compound raw;
            raw.put("type", std::string(type->name));
            raw.put("value", std::vector<int8_t>(c.value.begin(), c.value.end()));
            untranslated.items.push_back(std::move(raw));
        }
    }
    if (!components.empty()) tag.put("components", std::move(components));
    // Vanilla ignores this field. It keeps values we couldn't decode (e.g. an inline trim material
    // from a modified client) so that at least this server doesn't lose them.
    if (!untranslated.items.empty()) tag.put(std::string(kNetworkKey), std::move(untranslated));
    return tag;
}

std::optional<ItemStack> read(const nbt::Compound& tag) {
    auto id = registries::id("minecraft:item", tag.getString("id"));
    if (!id || *id == 0) return std::nullopt;
    ItemStack stack(*id, tag.getInt("count", 1));
    if (stack.count <= 0) return std::nullopt;
    if (const nbt::Compound* comps = tag.getCompound("components")) {
        for (const auto& [key, value] : comps->entries) {
            bool removed = !key.empty() && key[0] == '!';
            std::string name = removed ? key.substr(1) : key;
            if (name.find(':') == std::string::npos) name = "minecraft:" + name;
            const components::ComponentType* type = components::byName(name);
            if (type && removed) {
                stack.components.push_back({type->id, true, {}});
                continue;
            }
            auto bytes = type ? components::toNetwork(type->id, value) : std::nullopt;
            if (bytes) stack.components.push_back({type->id, false, std::move(*bytes)});
            else stack.diskComponents.put(key, value); // unknown or malformed: kept verbatim, not sent
        }
    }
    if (const nbt::List* raw = tag.getList(kNetworkKey)) {
        for (const auto& e : raw->items) {
            const nbt::Compound* c = e.asCompound();
            if (!c) continue;
            auto type = registries::id("minecraft:data_component_type", c->getString("type"));
            const auto* value = c->getByteArray("value");
            if (type && value) stack.components.push_back({*type, false, {value->begin(), value->end()}});
        }
    }
    return stack;
}

} // namespace mc::item_nbt
