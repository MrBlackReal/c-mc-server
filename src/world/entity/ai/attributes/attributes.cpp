#include "world/entity/ai/attributes/attributes.hpp"

#include <algorithm>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"

namespace mc::attributes {

std::optional<int> byName(std::string_view name) { return registries::id("minecraft:attribute", name); }

double playerBase(int attribute) {
    const auto& a = gen::kAttributes[attribute];
    return a.player ? a.playerBase : a.defaultValue;
}

double calculate(int attribute, double base, const std::vector<Modifier>& modifiers) {
    for (const auto& m : modifiers)
        if (m.operation == 0) base += m.amount;
    double result = base;
    for (const auto& m : modifiers)
        if (m.operation == 1) result += base * m.amount;
    for (const auto& m : modifiers)
        if (m.operation == 2) result *= 1.0 + m.amount;
    const auto& a = gen::kAttributes[attribute];
    return std::clamp(result, a.min, a.max); // RangedAttribute.sanitizeValue
}

bool slotGroupMatches(std::string_view group, EquipmentSlot slot) {
    bool hand = slot == EquipmentSlot::MainHand || slot == EquipmentSlot::OffHand;
    if (group == "any") return true;
    if (group == "hand") return hand;
    if (group == "armor") return !hand;
    switch (slot) {
    case EquipmentSlot::MainHand: return group == "mainhand";
    case EquipmentSlot::OffHand: return group == "offhand";
    case EquipmentSlot::Feet: return group == "feet";
    case EquipmentSlot::Legs: return group == "legs";
    case EquipmentSlot::Chest: return group == "chest";
    case EquipmentSlot::Head: return group == "head";
    }
    return false;
}

} // namespace mc::attributes
