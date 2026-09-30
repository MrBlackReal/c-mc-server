#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/entity/player/inventory.hpp"

// Port of AttributeInstance / AttributeModifier for the values the server computes itself.
namespace mc::attributes {

struct Modifier {
    std::string id;
    double amount;
    int operation; // AttributeModifier.Operation: 0 add_value, 1 add_multiplied_base, 2 add_multiplied_total
    bool operator==(const Modifier&) const = default;
};

std::optional<int> byName(std::string_view name); // minecraft:attribute id
// The player's base value (DefaultAttributes for the player), or the attribute's default.
double playerBase(int attribute);
// AttributeInstance.calculateValue, sanitized to the attribute's range.
double calculate(int attribute, double base, const std::vector<Modifier>& modifiers);
// EquipmentSlotGroup.test
bool slotGroupMatches(std::string_view group, EquipmentSlot slot);

} // namespace mc::attributes
