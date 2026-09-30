#include "world/item/adventure_mode_predicate.hpp"

#include <charconv>

#include "core/component/codec.hpp"
#include "generated/blocks.hpp"
#include "nbt/snbt.hpp"
#include "server/level/server_level.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/block/block_state.hpp"

namespace mc::adventure {
namespace {

// A holder set as saved: "id", "#tag" or a list of ids.
bool inBlockSet(const nbt::Tag& set, uint32_t block) {
    auto one = [&](const nbt::Tag& e) {
        const std::string* s = e.asString();
        if (!s) return false;
        std::string id = s->find(':') == std::string::npos && (*s)[0] != '#' ? "minecraft:" + *s : *s;
        if (!id.empty() && id[0] == '#') return blocks::hasTag(block, id.substr(1));
        return id == gen::kBlocks[block].name;
    };
    if (set.asString()) return one(set);
    try {
        for (const auto& e : codec::listItems(set))
            if (one(e)) return true;
    } catch (const std::exception&) {
    }
    return false;
}

// Property.getValue ordering, for RangedMatcher: numbers by value, false < true, enums by
// declaration order.
std::optional<int> ordinal(uint32_t block, std::string_view prop, std::string_view value) {
    int n = 0;
    auto r = std::from_chars(value.data(), value.data() + value.size(), n);
    if (r.ec == std::errc() && r.ptr == value.data() + value.size()) return n;
    if (value == "false") return 0;
    if (value == "true") return 1;
    for (const auto& p : gen::kBlocks[block].properties) {
        if (p.name != prop) continue;
        for (size_t i = 0; i < p.values.size(); i++)
            if (p.values[i] == value) return static_cast<int>(i);
    }
    return std::nullopt;
}

// StatePropertiesPredicate: {name: "value"} or {name: {min, max}}.
bool matchesState(const nbt::Compound& props, BlockStateId state) {
    uint32_t block = blocks::blockOf(state);
    for (const auto& [name, want] : props.entries) {
        auto have = blocks::getValue(state, name);
        if (!have) return false;
        if (const std::string* exact = want.asString()) {
            if (*have != *exact) return false;
            continue;
        }
        const nbt::Compound* range = want.asCompound();
        if (!range) return false;
        auto v = ordinal(block, name, *have);
        if (!v) return false;
        if (const nbt::Tag* lo = range->get("min"); lo && lo->asString()) {
            auto o = ordinal(block, name, *lo->asString());
            if (!o || *v < *o) return false;
        }
        if (const nbt::Tag* hi = range->get("max"); hi && hi->asString()) {
            auto o = ordinal(block, name, *hi->asString());
            if (!o || *v > *o) return false;
        }
    }
    return true;
}

// BlockPredicate.matches(BlockInWorld): blocks, state, then the block entity's saved NBT.
bool matches(const nbt::Compound& pred, ServerLevel& level, BlockPos pos) {
    BlockStateId state = level.getBlockState(pos);
    if (const nbt::Tag* set = pred.get("blocks"); set && !inBlockSet(*set, blocks::blockOf(state))) return false;
    if (const nbt::Compound* props = pred.getCompound("state"); props && !matchesState(*props, state)) return false;
    if (const nbt::Tag* nbtTag = pred.get("nbt")) {
        std::optional<nbt::Tag> expected;
        if (const std::string* snbt = nbtTag->asString()) expected = nbt::parseSnbt(*snbt);
        else if (nbtTag->asCompound()) expected = *nbtTag;
        auto* be = level.getChunk({pos.x >> 4, pos.z >> 4}).blockEntityAt(pos.x, pos.y, pos.z);
        if (!expected || !be || !compareNbt(*expected, nbt::Tag(be->tag), true)) return false;
    }
    return true;
}

} // namespace

bool compareNbt(const nbt::Tag& expected, const nbt::Tag& actual, bool partialListMatches) {
    if (expected.type() != actual.type()) return false;
    if (const nbt::Compound* e = expected.asCompound()) {
        const nbt::Compound* a = actual.asCompound();
        if (a->entries.size() < e->entries.size()) return false;
        for (const auto& [k, v] : e->entries) {
            const nbt::Tag* other = a->get(k);
            if (!other || !compareNbt(v, *other, partialListMatches)) return false;
        }
        return true;
    }
    if (const nbt::List* e = expected.asList(); e && partialListMatches) {
        const nbt::List* a = actual.asList();
        if (e->items.empty()) return a->items.empty();
        if (a->items.size() < e->items.size()) return false;
        for (const auto& want : e->items) {
            bool found = false;
            for (const auto& have : a->items)
                if (compareNbt(want, have, partialListMatches)) {
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    }
    return expected == actual;
}

bool test(const ItemStack& stack, std::string_view component, ServerLevel& level, BlockPos pos) {
    auto value = items::componentValue(stack, component);
    if (!value) return false;
    // compactListCodec: one predicate, or a list of them; any may match.
    if (const nbt::Compound* single = value->asCompound()) return matches(*single, level, pos);
    try {
        for (const auto& e : codec::listItems(*value))
            if (const nbt::Compound* p = e.asCompound(); p && matches(*p, level, pos)) return true;
    } catch (const std::exception&) {
    }
    return false;
}

} // namespace mc::adventure
