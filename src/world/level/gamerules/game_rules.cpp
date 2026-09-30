#include "world/level/gamerules/game_rules.hpp"

#include <limits>

namespace mc {
namespace {
constexpr int kIntMax = std::numeric_limits<int>::max();
constexpr GameRules::Info b(std::string_view id, bool def) { return {id, true, def ? 1 : 0, 0, 1}; }
constexpr GameRules::Info i(std::string_view id, int def, int min, int max = kIntMax) { return {id, false, def, min, max}; }

constexpr GameRules::Info kRules[] = {
    b("advance_time", true),
    b("advance_weather", true),
    b("allow_entering_nether_using_portals", true),
    b("block_drops", true),
    b("block_explosion_drop_decay", true),
    b("command_blocks_work", true),
    b("command_block_output", true),
    b("drowning_damage", true),
    b("elytra_movement_check", true),
    b("ender_pearls_vanish_on_death", true),
    b("entity_drops", true),
    b("fall_damage", true),
    b("fire_damage", true),
    i("fire_spread_radius_around_player", 128, -1),
    b("forgive_dead_players", true),
    b("freeze_damage", true),
    b("global_sound_events", true),
    b("immediate_respawn", false),
    b("keep_inventory", false),
    b("lava_source_conversion", false),
    b("limited_crafting", false),
    b("locator_bar", true),
    b("log_admin_commands", true),
    i("max_block_modifications", 32768, 1),
    i("max_command_forks", 65536, 0),
    i("max_command_sequence_length", 65536, 0),
    i("max_entity_cramming", 24, 0),
    i("max_snow_accumulation_height", 1, 0, 8),
    b("mob_drops", true),
    b("mob_explosion_drop_decay", true),
    b("mob_griefing", true),
    b("natural_health_regeneration", true),
    b("player_movement_check", true),
    i("players_nether_portal_creative_delay", 0, 0),
    i("players_nether_portal_default_delay", 80, 0),
    i("players_sleeping_percentage", 100, 0),
    b("projectiles_can_break_blocks", true),
    b("pvp", true),
    b("raids", true),
    i("random_tick_speed", 3, 0),
    b("reduced_debug_info", false),
    i("respawn_radius", 10, 0),
    b("send_command_feedback", true),
    b("show_advancement_messages", true),
    b("show_death_messages", true),
    b("spawner_blocks_work", true),
    b("spawn_mobs", true),
    b("spawn_monsters", true),
    b("spawn_patrols", true),
    b("spawn_phantoms", true),
    b("spawn_wandering_traders", true),
    b("spawn_wardens", true),
    b("spectators_generate_chunks", true),
    b("spread_vines", true),
    b("tnt_explodes", true),
    b("tnt_explosion_drop_decay", false),
    b("universal_anger", false),
    b("water_source_conversion", true),
};
static_assert(std::size(kRules) == GameRules::Count);
} // namespace

std::span<const GameRules::Info> GameRules::infos() { return kRules; }

std::optional<GameRules::Rule> GameRules::byName(std::string_view name) {
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    for (int r = 0; r < Count; r++)
        if (kRules[r].id == name) return static_cast<Rule>(r);
    return std::nullopt;
}

GameRules::GameRules() {
    for (int r = 0; r < Count; r++) values_[r] = kRules[r].defaultValue;
}

std::string GameRules::serialize(Rule rule) const {
    if (kRules[rule].isBool) return values_[rule] ? "true" : "false";
    return std::to_string(values_[rule]);
}

void GameRules::load(const nbt::Compound& data) {
    for (const auto& [key, value] : data.entries)
        if (!byName(key) || !key.starts_with("minecraft:")) unknown_.put(key, value);
    for (int r = 0; r < Count; r++) {
        std::string key = "minecraft:" + std::string(kRules[r].id);
        if (!data.contains(key)) continue;
        if (kRules[r].isBool) {
            values_[r] = data.getBool(key, kRules[r].defaultValue != 0) ? 1 : 0;
        } else {
            int v = data.getInt(key, kRules[r].defaultValue);
            if (v >= kRules[r].min && v <= kRules[r].max) values_[r] = v; // Codec.intRange
        }
    }
}

nbt::Compound GameRules::save() const {
    nbt::Compound data;
    for (int r = 0; r < Count; r++) {
        std::string key = "minecraft:" + std::string(kRules[r].id);
        if (kRules[r].isBool) data.put(key, nbt::byteTag(values_[r] != 0));
        else data.put(key, values_[r]);
    }
    for (const auto& [key, value] : unknown_.entries) data.put(key, value);
    return data;
}

} // namespace mc
