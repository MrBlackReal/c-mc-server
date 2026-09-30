#include "world/level/levelgen/block_predicate.hpp"

#include "server/level/server_level.hpp"
#include "util/log.hpp"
#include "world/level/block/block_behaviour.hpp"

namespace mc::levelgen {

namespace {

namespace bs = blocks;

BlockPos offset(const json::Value& p, BlockPos pos) {
    if (!p.has("offset")) return pos;
    const auto& o = p["offset"].array();
    return {pos.x + static_cast<int>(o[0].number()), pos.y + static_cast<int>(o[1].number()), pos.z + static_cast<int>(o[2].number())};
}

} // namespace

bool matchesBlocks(const json::Value& set, uint32_t block) {
    auto one = [&](std::string_view s) {
        if (!s.empty() && s[0] == '#') return bs::hasTag(block, s.substr(1));
        auto b = bs::byName(s);
        return b && *b == block;
    };
    if (set.isArray()) {
        for (const auto& e : set.array())
            if (one(e.string())) return true;
        return false;
    }
    return one(set.string());
}

bool testBlockPredicate(const json::Value& p, ServerLevel& level, BlockPos pos) {
    std::string_view type = p["type"].string();
    if (type == "minecraft:true") return true;
    if (type == "minecraft:all_of" || type == "minecraft:any_of") {
        bool any = type == "minecraft:any_of";
        for (const auto& e : p["predicates"].array())
            if (testBlockPredicate(e, level, pos) == any) return any;
        return !any;
    }
    if (type == "minecraft:not") return !testBlockPredicate(p["predicate"], level, pos);
    BlockPos at = offset(p, pos);
    BlockStateId state = level.getBlockState(at);
    if (type == "minecraft:matching_blocks") return matchesBlocks(p["blocks"], bs::blockOf(state));
    if (type == "minecraft:matching_block_tag") return bs::stateHasTag(state, p["tag"].string()[0] == '#' ? p["tag"].string().substr(1) : p["tag"].string());
    if (type == "minecraft:solid") return bs::isSolid(state);
    if (type == "minecraft:replaceable") return bs::canBeReplaced(state);
    if (type == "minecraft:inside_world_bounds") return at.y >= level.minY() && at.y <= level.maxY();
    if (type == "minecraft:has_sturdy_face") return bs::isFaceSturdy(state, direction::byName(p["direction"].string()));
    if (type == "minecraft:would_survive") {
        auto s = parseBlockState(p["state"]);
        return s && block_behaviour::canSurvive(*s, level, at);
    }
    log::debug("unsupported block predicate {}", type);
    return false;
}

std::optional<BlockStateId> parseBlockState(const json::Value& v) {
    const json::Value& id = v.has("id") ? v["id"] : v["Name"];
    auto block = bs::byName(id.string());
    if (!block) return std::nullopt;
    BlockStateId state = bs::defaultState(*block);
    const json::Value& props = v.has("properties") ? v["properties"] : v["Properties"];
    if (props.isObject())
        for (const auto& [k, val] : props.object()) state = bs::setValue(state, k, val.string());
    return state;
}

std::optional<BlockStateId> optionalState(const json::Value& p, ServerLevel& level, BlockPos pos, std::mt19937& random) {
    std::string_view type = p["type"].string();
    if (type.empty()) return parseBlockState(p); // a plain state (SimpleStateProvider)
    if (type == "minecraft:simple_state_provider") return parseBlockState(p["state"]);
    if (type == "minecraft:rule_based") {
        for (const auto& rule : p["rules"].array())
            if (testBlockPredicate(rule["if_true"], level, pos))
                if (auto s = optionalState(rule["then"], level, pos, random)) return s;
        if (p.has("fallback")) return optionalState(p["fallback"], level, pos, random);
        return std::nullopt;
    }
    if (type == "minecraft:copy_properties") { // the source state with the properties of what's there
        auto source = optionalState(p["source"], level, pos, random);
        if (!source) return std::nullopt;
        return block_behaviour::withPropertiesOf(bs::blockOf(*source), level.getBlockState(pos));
    }
    log::debug("unsupported block state provider {}", type);
    return std::nullopt;
}

} // namespace mc::levelgen
