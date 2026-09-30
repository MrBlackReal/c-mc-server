#include "world/level/storage/loot/loot_tables.hpp"

#include <cmath>
#include <functional>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "world/item/item_logic.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/entity/block_entity_components.hpp"

namespace mc::loot {
namespace {

using json::Value;
Value g_root;
Value g_contextIntProviders;   // data/context_int_providers.json, by id
Value g_contextFloatProviders; // data/context_float_providers.json, by id
bool conditions(const Value& v, BlockContext& ctx);

float nextFloat(std::mt19937& r) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(r); }
int nextInt(std::mt19937& r, int bound) { return bound <= 0 ? 0 : std::uniform_int_distribution<int>(0, bound - 1)(r); }
int nextIntBetween(std::mt19937& r, int lo, int hi) { return hi < lo ? lo : std::uniform_int_distribution<int>(lo, hi)(r); }

// Iterates a JSON value that may be a single element or a list (the 26.x "modifier" and
// "condition" fields accept both).
void forEach(const Value& v, const std::function<void(const Value&)>& fn) {
    if (v.isNull()) return;
    if (v.isArray()) {
        for (const auto& e : v.array()) fn(e);
    } else {
        fn(v);
    }
}

// ContextIntProvider / ContextFloatProvider (constant, uniform, binomial, weighted_list,
// number_dispatcher), inline or by minecraft:context_int_provider id.
float numberFloat(const Value& v, BlockContext& ctx) {
    if (v.isNumber()) return static_cast<float>(v.number());
    if (v.isString()) {
        std::string id(v.string());
        if (id.find(':') == std::string::npos) id = "minecraft:" + id;
        const Value& ref = g_contextIntProviders.has(id) ? g_contextIntProviders[id] : g_contextFloatProviders[id];
        return numberFloat(ref, ctx);
    }
    std::string_view type = v["type"].string();
    if (type == "minecraft:conditional") return numberFloat(conditions(v["condition"], ctx) ? v["on_true"] : v["on_false"], ctx);
    if (type == "minecraft:div") { // integer division (Quotient); as floats for the float providers
        float l = numberFloat(v["left"], ctx), r = numberFloat(v["right"], ctx);
        if (r == 0) return 0;
        return l == std::floor(l) && r == std::floor(r) ? static_cast<float>(static_cast<int>(l) / static_cast<int>(r)) : l / r;
    }
    if (type == "minecraft:add") return numberFloat(v["left"], ctx) + numberFloat(v["right"], ctx);
    if (type == "minecraft:sub") return numberFloat(v["left"], ctx) - numberFloat(v["right"], ctx);
    if (type == "minecraft:mul") return numberFloat(v["left"], ctx) * numberFloat(v["right"], ctx);
    if (type == "minecraft:weighted_list") { // WeightedList.getRandom
        int total = 0;
        for (const auto& e : v["distribution"].array()) total += static_cast<int>(e["weight"].number());
        int selection = nextInt(ctx.random, total);
        for (const auto& e : v["distribution"].array()) {
            selection -= static_cast<int>(e["weight"].number());
            if (selection < 0) return numberFloat(e["data"], ctx);
        }
        return 0;
    }
    if (type == "minecraft:number_dispatcher") { // the first case whose condition holds, else the default
        for (const auto& c : v["cases"].array())
            if (conditions(c["condition"], ctx)) return numberFloat(c["value"], ctx);
        return numberFloat(v["default"], ctx);
    }
    if (type == "minecraft:constant") return static_cast<float>(v["value"].number());
    if (type == "minecraft:uniform" || (type.empty() && v.has("min"))) {
        float lo = numberFloat(v["min"], ctx), hi = numberFloat(v["max"], ctx);
        return lo + nextFloat(ctx.random) * (hi - lo);
    }
    if (type == "minecraft:binomial") {
        int n = static_cast<int>(numberFloat(v["n"], ctx));
        float p = numberFloat(v["p"], ctx);
        int result = 0;
        for (int i = 0; i < n; i++) result += nextFloat(ctx.random) < p;
        return static_cast<float>(result);
    }
    log::debug("unsupported loot number provider {}", type);
    return 0;
}

int numberInt(const Value& v, BlockContext& ctx) {
    if (v.isNumber()) return static_cast<int>(std::floor(v.number()));
    if (v.isString()) return static_cast<int>(std::floor(numberFloat(v, ctx)));
    std::string_view type = v["type"].string();
    if (type == "minecraft:uniform" || (type.empty() && v.has("min") && v.has("max"))) {
        return nextIntBetween(ctx.random, numberInt(v["min"], ctx), numberInt(v["max"], ctx)); // Mth.nextInt
    }
    return static_cast<int>(std::floor(numberFloat(v, ctx)));
}

// "minecraft:x", "#minecraft:tag" or a list of those, against a block.
bool matchesBlocks(const Value& ref, uint32_t block) {
    bool match = false;
    forEach(ref, [&](const Value& e) {
        std::string_view s = e.string();
        if (!s.empty() && s[0] == '#') match = match || blocks::hasTag(block, s.substr(1));
        else match = match || s == gen::kBlocks[block].name;
    });
    return match;
}

// StatePropertiesPredicate: {prop: "value"} or {prop: {min, max}}.
bool matchesState(const Value& props, BlockStateId state) {
    for (const auto& [prop, want] : props.object()) {
        auto have = blocks::getValue(state, prop);
        if (!have) return false;
        if (want.isString()) {
            if (*have != want.string()) return false;
        } else if (want.isObject()) {
            // Range match on integer properties; bounds may be written as numbers or strings.
            auto bound = [](const Value& b) { return b.isNumber() ? b.number() : std::atof(b.string().c_str()); };
            double v = std::atof(std::string(*have).c_str());
            if (want.has("min") && v < bound(want["min"])) return false;
            if (want.has("max") && v > bound(want["max"])) return false;
        } else if (want.isNumber() || want.isBool()) {
            std::string s = want.isBool() ? (want.boolean() ? "true" : "false") : std::to_string(static_cast<int>(want.number()));
            if (*have != s) return false;
        }
    }
    return true;
}

bool blockPredicate(const Value& pred, BlockStateId state) {
    if (pred.has("blocks") && !matchesBlocks(pred["blocks"], blocks::blockOf(state))) return false;
    if (pred.has("state") && !matchesState(pred["state"], state)) return false;
    return true;
}

// Entry names in a synced/static registry tag (e.g. enchantments in #minecraft:exclusive_set/armor).
std::vector<std::string_view> registryTag(std::string_view registry, std::string_view tag) {
    std::vector<std::string_view> out;
    const gen::RegistryInfo* info = nullptr;
    for (const auto& r : gen::kSyncedRegistries)
        if (r.name == registry) info = &r;
    for (const auto& r : gen::kStaticRegistries)
        if (r.name == registry) info = &r;
    if (!info) return out;
    std::string full = tag.find(':') == std::string_view::npos ? "minecraft:" + std::string(tag) : std::string(tag);
    for (const auto& reg : gen::kTags) {
        if (reg.registry != registry) continue;
        for (const auto& t : reg.tags)
            if (t.name == full || t.name == tag)
                for (int32_t id : t.ids)
                    if (id >= 0 && static_cast<size_t>(id) < info->entries.size()) out.push_back(info->entries[id]);
    }
    return out;
}

// A registry holder set as written in data files: "id", "#tag" or a list of ids.
bool inHolderSet(const Value& set, std::string_view registry, std::string_view entry) {
    bool match = false;
    forEach(set, [&](const Value& e) {
        std::string_view s = e.string();
        if (!s.empty() && s[0] == '#') {
            for (auto m : registryTag(registry, s.substr(1))) match = match || m == entry;
        } else {
            match = match || s == entry || (s.find(':') == std::string_view::npos && "minecraft:" + std::string(s) == entry);
        }
    });
    return match;
}

// MinMaxBounds: a number or {min, max}.
bool inBounds(const Value& b, double v) {
    if (b.isNull()) return true;
    if (b.isNumber()) return v == b.number();
    if (b.has("min") && v < b["min"].number()) return false;
    if (b.has("max") && v > b["max"].number()) return false;
    return true;
}

// ItemPredicate: items, and enchantment sub-predicates.
bool itemPredicate(const Value& pred, const ItemStack* tool) {
    if (pred.has("items")) {
        if (!tool || tool->isEmpty()) return false;
        bool match = false;
        forEach(pred["items"], [&](const Value& e) { match = match || items::matchesItemRef(tool->item, e.string()); });
        if (!match) return false;
    }
    if (pred.has("count")) {
        int count = tool ? tool->count : 0;
        const Value& c = pred["count"];
        if (c.isNumber() && count != c.number()) return false;
        if (c["min"].isNumber() && count < c["min"].number()) return false;
        if (c["max"].isNumber() && count > c["max"].number()) return false;
    }
    const Value& sub = pred["predicates"];
    if (sub.has("minecraft:enchantments")) {
        for (const auto& e : sub["minecraft:enchantments"].array()) {
            int best = 0;
            forEach(e["enchantments"], [&](const Value& name) {
                if (!tool) return;
                std::string_view n = name.string();
                if (n.empty() || n[0] != '#') {
                    best = std::max(best, items::enchantmentLevel(*tool, n));
                    return;
                }
                for (const auto& ench : registryTag("minecraft:enchantment", n.substr(1))) best = std::max(best, items::enchantmentLevel(*tool, ench));
            });
            const Value& levels = e["levels"];
            int lo = levels.isNumber() ? static_cast<int>(levels.number()) : static_cast<int>(levels["min"].number(1));
            int hi = levels.isNumber() ? lo : static_cast<int>(levels["max"].number(255));
            if (best < lo || best > hi) return false;
        }
    }
    return true;
}

// LocationPredicate.matches at (x, y, z).
bool locationPredicate(const Value& p, BlockContext& ctx, double x, double y, double z) {
    if (p.isNull()) return true;
    BlockPos pos{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z))};
    for (const auto& [key, v] : p.object()) {
        if (key == "position") {
            if (!inBounds(v["x"], x) || !inBounds(v["y"], y) || !inBounds(v["z"], z)) return false;
        } else if (key == "biomes") {
            uint32_t biome = ctx.level.getBiome(pos);
            std::string_view name;
            for (const auto& r : gen::kSyncedRegistries)
                if (r.name == "minecraft:worldgen/biome" && biome < r.entries.size()) name = r.entries[biome];
            if (!inHolderSet(v, "minecraft:worldgen/biome", name)) return false;
        } else if (key == "dimension") {
            if (v.string() != ctx.level.dimension()) return false;
        } else if (key == "smokey") {
            if (block_behaviour::isSmokeyPos(ctx.level, pos) != v.boolean()) return false;
        } else if (key == "block") {
            if (!blockPredicate(v, ctx.level.getBlockState(pos))) return false;
        } else if (key == "fluid") {
            // FluidPredicate: fluids (water/flowing_water/lava/flowing_lava) and state (level, falling).
            BlockStateId st = ctx.level.getBlockState(pos);
            uint32_t f = blocks::flags(st);
            bool source = f & blocks::kFluidSource;
            std::string fluid = (f & blocks::kWater) ? (source ? "minecraft:water" : "minecraft:flowing_water")
                              : (f & blocks::kLava)  ? (source ? "minecraft:lava" : "minecraft:flowing_lava")
                                                     : "minecraft:empty";
            if (v.has("fluids") && !inHolderSet(v["fluids"], "minecraft:fluid", fluid)) return false;
            if (v.has("state") && !matchesState(v["state"], st)) return false; // approximated by the block state
        } else if (key == "light") { // LightPredicate: getMaxLocalRawBrightness, with the sky darkened for the time
            int light = ctx.level.rawBrightness(pos, ctx.level.skyDarken());
            if (!inBounds(v["light"], light)) return false;
        } else if (key == "can_see_sky") {
            if (ctx.level.canSeeSky(pos) != v.boolean()) return false;
        } else {
            // TODO: structures need worldgen.
            log::debug("unsupported location predicate field {}", key);
            return false;
        }
    }
    return true;
}

// EntityPredicate for the breaking player.
bool entityPredicate(const Value& p, BlockContext& ctx) {
    const ServerPlayer* e = ctx.entity;
    if (!e) return false;
    if (p.isNull()) return true;
    for (const auto& [key, v] : p.object()) {
        if (key == "type") {
            if (!inHolderSet(v, "minecraft:entity_type", "minecraft:player")) return false;
        } else if (key == "flags") {
            // EntityFlagsPredicate; players here are never burning, swimming, babies or gliding.
            bool inWater = blocks::flags(ctx.level.getBlockState(static_cast<int>(std::floor(e->x)), static_cast<int>(std::floor(e->y)),
                                                                  static_cast<int>(std::floor(e->z)))) & blocks::kWater;
            const std::pair<std::string_view, bool> actual[] = {
                {"is_on_ground", e->onGround}, {"is_sneaking", e->sneaking}, {"is_sprinting", e->sprinting}, {"is_flying", e->flying},
                {"is_in_water", inWater},      {"is_on_fire", false},        {"is_swimming", false},        {"is_baby", false},
                {"is_fall_flying", false}};
            for (const auto& [flag, want] : v.object()) {
                bool known = false;
                for (auto [name, have] : actual) {
                    if (name != flag) continue;
                    known = true;
                    if (have != want.boolean()) return false;
                }
                if (!known) return false;
            }
        } else if (key == "equipment") {
            static const std::pair<std::string_view, EquipmentSlot> slots[] = {
                {"mainhand", EquipmentSlot::MainHand}, {"offhand", EquipmentSlot::OffHand}, {"head", EquipmentSlot::Head},
                {"chest", EquipmentSlot::Chest},       {"legs", EquipmentSlot::Legs},       {"feet", EquipmentSlot::Feet}};
            auto& inv = const_cast<ServerPlayer*>(e)->inventory;
            for (const auto& [slotName, pred] : v.object()) {
                const ItemStack* item = nullptr;
                for (auto [name, slot] : slots)
                    if (name == slotName) item = &inv.equipment(slot);
                if (!item || !itemPredicate(pred, item)) return false;
            }
        } else if (key == "location") {
            // LocationWrapper: where the entity is, the block it stands on, the block affecting movement.
            if (v.has("located") && !locationPredicate(v["located"], ctx, e->x, e->y, e->z)) return false;
            if (v.has("stepping_on") && !locationPredicate(v["stepping_on"], ctx, e->x, e->y - 0.2, e->z)) return false;
            if (v.has("affects_movement") && !locationPredicate(v["affects_movement"], ctx, e->x, e->y - 0.500001, e->z)) return false;
        } else if (key == "effects") {
            // MobEffectsPredicate: {effect: {amplifier, duration, ambient, visible}}
            for (const auto& [effectId, want] : v.object()) {
                auto id = registries::id("minecraft:mob_effect", effectId);
                auto it = id ? e->effects.find(*id) : e->effects.end();
                if (it == e->effects.end()) return false;
                if (!inBounds(want["amplifier"], it->second.amplifier) || !inBounds(want["duration"], it->second.duration)) return false;
                if (want.has("ambient") && want["ambient"].boolean() != it->second.ambient) return false;
                if (want.has("visible") && want["visible"].boolean() != it->second.visible) return false;
            }
        } else {
            log::debug("unsupported entity predicate field {}", key);
            return false;
        }
    }
    return true;
}

bool condition(const Value& c, BlockContext& ctx);

bool conditions(const Value& v, BlockContext& ctx) {
    if (v.isString()) { // a predicate by id (data/minecraft/predicate)
        std::string id(v.string());
        if (id.find(':') == std::string::npos) id = "minecraft:" + id;
        return conditions(g_root["predicates"][id], ctx);
    }
    bool ok = true;
    forEach(v, [&](const Value& c) { ok = ok && condition(c, ctx); });
    return ok;
}

bool condition(const Value& c, BlockContext& ctx) {
    if (c.isString()) { // reference to data/minecraft/predicate/*.json
        const Value& ref = g_root["predicates"][c.string()];
        if (ref.isNull()) {
            log::debug("unknown loot predicate {}", c.string());
            return false;
        }
        return conditions(ref, ctx);
    }
    std::string_view type = c["type"].string();
    if (type == "minecraft:survives_explosion") return true; // no explosions yet
    if (type == "minecraft:inverted") return !conditions(c["term"], ctx);
    if (type == "minecraft:all_of") {
        for (const auto& t : c["terms"].array())
            if (!conditions(t, ctx)) return false;
        return true;
    }
    if (type == "minecraft:any_of") {
        for (const auto& t : c["terms"].array())
            if (conditions(t, ctx)) return true;
        return false;
    }
    if (type == "minecraft:random_chance") return nextFloat(ctx.random) < numberFloat(c["chance"], ctx);
    if (type == "minecraft:match_block") return blockPredicate(c, ctx.state);
    if (type == "minecraft:match_tool") return itemPredicate(c["predicate"], ctx.tool);
    if (type == "minecraft:entity_properties") return c["entity"].string() == "this" && entityPredicate(c["predicate"], ctx);
    if (type == "minecraft:table_bonus") {
        int level = ctx.tool ? items::enchantmentLevel(*ctx.tool, c["enchantment"].string()) : 0;
        const auto& chances = c["chances"].array();
        if (chances.empty()) return false;
        return nextFloat(ctx.random) < chances[std::min<size_t>(static_cast<size_t>(level), chances.size() - 1)].number();
    }
    if (type == "minecraft:location_check") {
        BlockPos p{ctx.pos.x + static_cast<int>(c["offsetX"].number()), ctx.pos.y + static_cast<int>(c["offsetY"].number()),
                   ctx.pos.z + static_cast<int>(c["offsetZ"].number())};
        // LootContextParams.ORIGIN for block drops is the block's center.
        return locationPredicate(c["predicate"], ctx, p.x + 0.5, p.y + 0.5, p.z + 0.5);
    }
    log::debug("unsupported loot condition {}", type);
    return false;
}

void setCount(ItemStack& s, int n) {
    s.count = n;
}

// LootItemFunction implementations.
void applyModifier(const Value& m, ItemStack& stack, BlockContext& ctx) {
    if (m.has("condition") && !conditions(m["condition"], ctx)) return;
    std::string_view type = m["type"].string();
    if (type == "minecraft:set_count") {
        int base = m["add"].boolean() ? stack.count : 0;
        setCount(stack, base + numberInt(m["count"], ctx));
    } else if (type == "minecraft:limit_count") {
        const Value& lim = m["limit"];
        if (lim.has("min")) stack.count = std::max(stack.count, numberInt(lim["min"], ctx));
        if (lim.has("max")) stack.count = std::min(stack.count, numberInt(lim["max"], ctx));
    } else if (type == "minecraft:apply_bonus") {
        int level = ctx.tool ? items::enchantmentLevel(*ctx.tool, m["enchantment"].string()) : 0;
        std::string_view formula = m["formula"].string();
        const Value& params = m["parameters"];
        if (formula == "minecraft:binomial_with_bonus_count") {
            int extra = static_cast<int>(params["extra"].number());
            float p = static_cast<float>(params["probability"].number());
            for (int i = 0; i < level + extra; i++) stack.count += nextFloat(ctx.random) < p;
        } else if (formula == "minecraft:ore_drops") {
            if (level > 0) stack.count *= std::max(0, nextInt(ctx.random, level + 2) - 1) + 1;
        } else if (formula == "minecraft:uniform_bonus_count") {
            stack.count += nextInt(ctx.random, static_cast<int>(params["bonusMultiplier"].number()) * level + 1);
        }
    } else if (type == "minecraft:explosion_decay") {
        // Only affects explosion drops.
    } else if (type == "minecraft:copy_state") {
        // BlockItemStateProperties: map of property name -> value.
        ByteWriter w;
        std::vector<std::pair<std::string, std::string>> props;
        for (const auto& p : m["properties"].array())
            if (auto v = blocks::getValue(ctx.state, p.string())) props.emplace_back(p.string(), std::string(*v));
        w.writeVarInt(static_cast<int32_t>(props.size()));
        for (const auto& [k, v] : props) {
            w.writeUtf(k);
            w.writeUtf(v);
        }
        items::setComponent(stack, "minecraft:block_state", w.data());
    } else if (type == "minecraft:copy_components") {
        // CopyComponentsFunction with the block entity source (BlockEntity.collectComponents).
        if (m["source"].string() != "block_entity" || !ctx.blockEntity) return;
        nbt::Compound collected = block_entity::collectComponents(*ctx.blockEntity);
        auto listed = [](const Value& list, std::string_view name) {
            for (const auto& e : list.array())
                if (e.string() == name) return true;
            return false;
        };
        for (const auto& [name, value] : collected.entries) {
            if (m.has("include") && !listed(m["include"], name)) continue;
            if (m.has("exclude") && listed(m["exclude"], name)) continue;
            items::setComponentValue(stack, name, value);
        }
    } else {
        log::debug("unsupported loot modifier {}", type);
    }
}

void applyModifiers(const Value& v, ItemStack& stack, BlockContext& ctx) {
    forEach(v, [&](const Value& m) { applyModifier(m, stack, ctx); });
}

struct Candidate {
    const Value* entry;
    int weight;
    int32_t item = -1; // an expanded tag entry's item
};

// LootPoolEntryContainer.expand: alternatives/group/sequence composites down to singleton entries.
bool expand(const Value& e, BlockContext& ctx, std::vector<Candidate>& out) {
    if (e.has("condition") && !conditions(e["condition"], ctx)) return false;
    std::string_view type = e["type"].string();
    if (type == "minecraft:alternatives") {
        for (const auto& child : e["children"].array())
            if (expand(child, ctx, out)) return true;
        return false;
    }
    if (type == "minecraft:group") {
        for (const auto& child : e["children"].array()) expand(child, ctx, out);
        return true;
    }
    if (type == "minecraft:sequence") {
        for (const auto& child : e["children"].array())
            if (!expand(child, ctx, out)) break;
        return true;
    }
    int weight = std::max(0, static_cast<int>(e["weight"].number(1))); // quality needs luck (0)
    if (type == "minecraft:tag" && e["expand"].boolean()) {
        // TagEntry.expandTag: every item in the tag is a separate choice with the entry's weight.
        auto members = items::tagItems(e["name"].string());
        for (int32_t item : members) out.push_back({&e, weight, item});
        return !members.empty();
    }
    if (type == "minecraft:item" || type == "minecraft:empty" || type == "minecraft:loot_table" || type == "minecraft:tag" ||
        type == "minecraft:dynamic") {
        out.push_back({&e, weight});
        return true;
    }
    log::debug("unsupported loot entry {}", type);
    return false;
}

void createItems(const Candidate& pick, BlockContext& ctx, std::vector<ItemStack>& out, const Value& poolModifiers,
                 const Value& tableModifiers) {
    const Value& entry = *pick.entry;
    std::string_view type = entry["type"].string();
    std::vector<ItemStack> made;
    if (pick.item >= 0) {
        made.push_back(ItemStack(pick.item, 1));
    } else if (type == "minecraft:item") {
        auto id = items::byName(entry["name"].string());
        if (!id || *id == 0) return;
        made.push_back(ItemStack(*id, 1));
    } else if (type == "minecraft:loot_table") {
        if (entry["value"].isString()) made = getRandomItems(entry["value"].string(), ctx);
    } else if (type == "minecraft:tag") {
        for (int32_t item : items::tagItems(entry["name"].string())) made.push_back(ItemStack(item, 1)); // all of them
    } else if (type == "minecraft:dynamic" && entry["name"].string() == "minecraft:sherds" && ctx.blockEntity) {
        // DecoratedPotBlock: PotDecorations.ordered() (back, left, right, front), bricks where empty.
        const nbt::Compound* sherds = ctx.blockEntity->getCompound("sherds");
        for (const char* side : {"back", "left", "right", "front"}) {
            std::string id = "minecraft:brick";
            if (const nbt::Tag* t = sherds ? sherds->get(side) : nullptr) {
                if (const std::string* sId = t->asString()) id = *sId;
                else if (const nbt::Compound* c = t->asCompound()) id = c->getString("id", id);
            }
            if (auto item = items::byName(id)) made.push_back(ItemStack(*item, 1));
        }
    } else if (type == "minecraft:dynamic" && entry["name"].string() == "minecraft:sherds") {
        for (int i = 0; i < 4; i++)
            if (auto item = items::byName("minecraft:brick")) made.push_back(ItemStack(*item, 1));
    }
    for (auto& s : made) {
        applyModifiers(entry["modifier"], s, ctx);
        applyModifiers(poolModifiers, s, ctx);
        applyModifiers(tableModifiers, s, ctx);
        if (!s.isEmpty()) out.push_back(std::move(s));
    }
}

} // namespace

void bootstrap() {
    try {
        g_root = json::parse(gen::kLootTablesJson);
        g_contextIntProviders = json::parse(gen::kContextIntProvidersJson);
        g_contextFloatProviders = json::parse(gen::kContextFloatProvidersJson);
    } catch (const std::exception& e) {
        log::error("Failed to parse loot tables: {}", e.what());
    }
}

std::vector<std::string_view> registryTagEntries(std::string_view registry, std::string_view tag) { return registryTag(registry, tag); }

bool testCondition(const json::Value& c, BlockContext& ctx) { return conditions(c, ctx); }

float contextFloat(std::string_view provider, BlockContext& ctx) {
    if (provider.empty()) return 0;
    if (provider.front() == '{' || provider.front() == '[' || std::isdigit(static_cast<unsigned char>(provider.front())))
        return numberFloat(json::parse(provider), ctx);
    Value id;
    id.v = std::string(provider);
    return numberFloat(id, ctx);
}

int contextInt(std::string_view provider, BlockContext& ctx) {
    if (provider.empty()) return 0;
    if (provider.front() == '{' || provider.front() == '[' || std::isdigit(static_cast<unsigned char>(provider.front())))
        return numberInt(json::parse(provider), ctx);
    Value id;
    id.v = std::string(provider);
    return numberInt(id, ctx);
}

std::vector<ItemStack> getRandomItems(std::string_view tableName, BlockContext& ctx) {
    std::vector<ItemStack> raw;
    const Value& table = g_root["tables"][tableName];
    if (table.isNull()) return raw;
    for (const auto& pool : table["pools"].array()) {
        if (pool.has("condition") && !conditions(pool["condition"], ctx)) continue;
        int rolls = numberInt(pool["rolls"], ctx); // + bonus_rolls * luck (0)
        for (int r = 0; r < rolls; r++) {
            std::vector<Candidate> candidates;
            int total = 0;
            for (const auto& e : pool["entries"].array()) expand(e, ctx, candidates);
            for (const auto& c : candidates) total += c.weight;
            if (candidates.empty() || total == 0) continue;
            const Candidate* pick = &candidates[0];
            if (candidates.size() > 1) {
                int roll = nextInt(ctx.random, total);
                for (const auto& c : candidates) {
                    roll -= c.weight;
                    if (roll < 0) {
                        pick = &c;
                        break;
                    }
                }
            }
            createItems(*pick, ctx, raw, pool["modifier"], table["modifier"]);
        }
    }
    // LootTable.createStackSplitter: oversized stacks become several valid ones.
    std::vector<ItemStack> out;
    for (auto& s : raw) {
        while (s.count > s.maxStackSize()) {
            out.push_back(s.copyWithCount(s.maxStackSize()));
            s.count -= s.maxStackSize();
        }
        if (s.count > 0) out.push_back(std::move(s));
    }
    return out;
}

} // namespace mc::loot
