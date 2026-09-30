#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "generated/registries.hpp"

namespace mc::registries {

void bootstrap();

// Network id of `entry` in `registry` (static or synchronised), e.g.
// id("minecraft:worldgen/biome", "minecraft:plains").
std::optional<int> id(std::string_view registry, std::string_view entry);
// The entry with network id `id`, or empty.
std::string_view name(std::string_view registry, int id);
int size(std::string_view registry);
// Tags as sent to clients: whether `entry` is in `tag` (without '#'), and the entries of a tag in
// its order.
bool hasTag(std::string_view registry, std::string_view entry, std::string_view tag);
std::vector<std::string_view> tagEntries(std::string_view registry, std::string_view tag);
bool tagExists(std::string_view registry, std::string_view tag);
// The tags of a registry (without '#').
std::vector<std::string_view> tags(std::string_view registry);

} // namespace mc::registries
