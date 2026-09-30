#include "core/registries.hpp"

#include <string>
#include <unordered_map>

namespace mc::registries {
namespace {
struct Registry {
    std::unordered_map<std::string_view, int> ids;
    std::span<const std::string_view> entries;
    int size = 0;
};
std::unordered_map<std::string_view, Registry> g_registries;

void add(const gen::RegistryInfo& info) {
    Registry& r = g_registries[info.name];
    r.size = static_cast<int>(info.entries.size());
    r.entries = info.entries;
    for (int i = 0; i < r.size; i++) r.ids.emplace(info.entries[i], i);
}
} // namespace

void bootstrap() {
    for (const auto& r : gen::kStaticRegistries) add(r);
    for (const auto& r : gen::kSyncedRegistries) add(r);
}

std::optional<int> id(std::string_view registry, std::string_view entry) {
    auto it = g_registries.find(registry);
    if (it == g_registries.end()) return std::nullopt;
    auto e = it->second.ids.find(entry);
    if (e == it->second.ids.end()) return std::nullopt;
    return e->second;
}

std::string_view name(std::string_view registry, int id) {
    auto it = g_registries.find(registry);
    if (it == g_registries.end() || id < 0 || id >= it->second.size) return {};
    return it->second.entries[id];
}

int size(std::string_view registry) {
    auto it = g_registries.find(registry);
    return it == g_registries.end() ? 0 : it->second.size;
}

namespace {
const gen::TagInfo* findTag(std::string_view registry, std::string_view tag) {
    for (const auto& reg : gen::kTags) {
        if (reg.registry != registry) continue;
        for (const auto& t : reg.tags)
            if (t.name == tag) return &t;
    }
    return nullptr;
}
} // namespace

bool hasTag(std::string_view registry, std::string_view entry, std::string_view tag) {
    const gen::TagInfo* t = findTag(registry, tag);
    auto want = id(registry, entry);
    if (!t || !want) return false;
    for (int32_t i : t->ids)
        if (i == *want) return true;
    return false;
}

std::vector<std::string_view> tagEntries(std::string_view registry, std::string_view tag) {
    std::vector<std::string_view> out;
    if (const gen::TagInfo* t = findTag(registry, tag))
        for (int32_t i : t->ids) out.push_back(name(registry, i));
    return out;
}

bool tagExists(std::string_view registry, std::string_view tag) { return findTag(registry, tag) != nullptr; }

std::vector<std::string_view> tags(std::string_view registry) {
    std::vector<std::string_view> out;
    for (const auto& reg : gen::kTags)
        if (reg.registry == registry)
            for (const auto& t : reg.tags) out.push_back(t.name);
    return out;
}

} // namespace mc::registries
