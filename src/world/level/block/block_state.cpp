#include "world/level/block/block_state.hpp"

#include <string>
#include <unordered_map>
#include <vector>

#include "generated/registries.hpp"

namespace mc::blocks {
namespace {
std::vector<uint32_t> g_stateToBlock;
std::unordered_map<std::string_view, uint32_t> g_byName;
std::unordered_map<std::string_view, std::vector<bool>> g_tags;

// States enumerate properties (sorted by name) with the last one varying fastest.
struct Digit {
    size_t index;
    uint32_t stride;
};

std::optional<Digit> locate(const gen::BlockInfo& info, std::string_view property) {
    uint32_t stride = 1;
    for (size_t i = info.properties.size(); i-- > 0;) {
        if (info.properties[i].name == property) return Digit{i, stride};
        stride *= static_cast<uint32_t>(info.properties[i].values.size());
    }
    return std::nullopt;
}
} // namespace

void bootstrap() {
    g_stateToBlock.resize(gen::kBlockStateCount);
    for (uint32_t b = 0; b < gen::kBlocks.size(); b++) {
        const auto& info = gen::kBlocks[b];
        g_byName.emplace(info.name, b);
        for (uint32_t s = info.first_state; s < info.first_state + info.state_count; s++) g_stateToBlock[s] = b;
    }
    for (const auto& reg : gen::kTags) {
        if (reg.registry != "minecraft:block") continue;
        for (const auto& tag : reg.tags) {
            auto& bits = g_tags[tag.name];
            bits.assign(gen::kBlocks.size(), false);
            for (int32_t id : tag.ids)
                if (id >= 0 && static_cast<size_t>(id) < bits.size()) bits[id] = true;
        }
    }
}

uint32_t blockOf(BlockStateId state) { return state < g_stateToBlock.size() ? g_stateToBlock[state] : 0; }

std::optional<uint32_t> byName(std::string_view name) {
    std::string full = name.contains(':') ? std::string(name) : "minecraft:" + std::string(name);
    auto it = g_byName.find(full);
    if (it == g_byName.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string_view> getValue(BlockStateId state, std::string_view property) {
    const auto& info = gen::kBlocks[blockOf(state)];
    auto d = locate(info, property);
    if (!d) return std::nullopt;
    const auto& values = info.properties[d->index].values;
    return values[((state - info.first_state) / d->stride) % values.size()];
}

bool hasProperty(BlockStateId state, std::string_view property) {
    return locate(gen::kBlocks[blockOf(state)], property).has_value();
}

BlockStateId setValue(BlockStateId state, std::string_view property, std::string_view value) {
    const auto& info = gen::kBlocks[blockOf(state)];
    auto d = locate(info, property);
    if (!d) return state;
    const auto& values = info.properties[d->index].values;
    for (uint32_t v = 0; v < values.size(); v++) {
        if (values[v] != value) continue;
        uint32_t rel = state - info.first_state;
        uint32_t current = (rel / d->stride) % values.size();
        return info.first_state + rel - current * d->stride + v * d->stride;
    }
    return state;
}

bool isInstance(uint32_t block, std::string_view javaClass) {
    std::string_view chain = gen::kBlockBehaviour[block].classes;
    while (!chain.empty()) {
        size_t bar = chain.find('|');
        if (chain.substr(0, bar) == javaClass) return true;
        if (bar == std::string_view::npos) break;
        chain.remove_prefix(bar + 1);
    }
    return false;
}

bool hasTag(uint32_t block, std::string_view tag) {
    auto it = g_tags.find(tag);
    return it != g_tags.end() && block < it->second.size() && it->second[block];
}

} // namespace mc::blocks
