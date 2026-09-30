#include "world/item/crafting/recipes.hpp"

#include <algorithm>
#include <functional>

#include "generated/behaviour.hpp"
#include "core/registries.hpp"
#include "nbt/json_nbt.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc::recipes {

namespace {

using json::Value;

Value g_recipes;                          // id -> recipe JSON
std::vector<const Value*> g_crafting;     // crafting recipe definitions, in id order
std::vector<std::string_view> g_craftingIds;

bool isCraftingType(std::string_view t) { return t.starts_with("minecraft:crafting_"); }

// Ingredient.test
bool test(const Value& ingredient, const ItemStack& stack) {
    if (stack.isEmpty()) return false;
    if (ingredient.isArray()) {
        for (const auto& e : ingredient.array())
            if (items::matchesItemRef(stack.item, e.string())) return true;
        return false;
    }
    return items::matchesItemRef(stack.item, ingredient.string());
}

// ItemStackTemplate from a recipe's "result".
ItemStack resultStack(const Value& r, int32_t fallbackItem = 0, int extraCount = 0) {
    int32_t item = fallbackItem;
    if (r.has("id")) item = items::byName(r["id"].string()).value_or(0);
    if (item == 0) return {};
    ItemStack stack(item, static_cast<int>(r["count"].number(1)) + extraCount);
    if (r.has("components"))
        for (const auto& [type, value] : r["components"].object()) items::setComponentValue(stack, type, nbt::fromJson(value));
    return stack;
}

// ShapedRecipePattern: the pattern with empty rows and columns removed, and its key.
struct Shaped {
    int width = 0, height = 0;
    std::vector<const Value*> cells; // nullptr: must be empty
    int ingredientCount = 0;
    bool symmetrical = true;
};

Shaped unpackShaped(const Value& r) {
    std::vector<std::string> rows;
    for (const auto& p : r["pattern"].array()) rows.push_back(p.string());
    // ShapedRecipePattern.shrink
    size_t first = SIZE_MAX, last = 0;
    int top = -1, bottom = -1;
    for (size_t y = 0; y < rows.size(); y++) {
        size_t f = rows[y].find_first_not_of(' '), l = rows[y].find_last_not_of(' ');
        if (f == std::string::npos) continue;
        first = std::min(first, f), last = std::max(last, l);
        if (top < 0) top = static_cast<int>(y);
        bottom = static_cast<int>(y);
    }
    Shaped s;
    if (top < 0) return s;
    s.width = static_cast<int>(last - first + 1);
    s.height = bottom - top + 1;
    for (int y = top; y <= bottom; y++) {
        for (size_t x = first; x <= last; x++) {
            char c = x < rows[y].size() ? rows[y][x] : ' ';
            const Value* ing = c == ' ' ? nullptr : &r["key"][std::string(1, c)];
            s.cells.push_back(ing);
            s.ingredientCount += ing != nullptr;
        }
    }
    for (int y = 0; y < s.height; y++) // Util.isSymmetrical
        for (int x = 0; x < s.width / 2; x++)
            if (s.cells[x + y * s.width] != s.cells[s.width - 1 - x + y * s.width]) s.symmetrical = false; // (same key: same entry)
    return s;
}

bool matchesShaped(const Shaped& s, const CraftingInput& in, bool flip) {
    for (int y = 0; y < s.height; y++) {
        for (int x = 0; x < s.width; x++) {
            const Value* expected = s.cells[(flip ? s.width - x - 1 : x) + y * s.width];
            const ItemStack& actual = in.at(x, y);
            if (expected ? !test(*expected, actual) : !actual.isEmpty()) return false; // Ingredient.testOptionalIngredient
        }
    }
    return true;
}

// ShapelessRecipe: every ingredient gets a distinct item (StackedItemContents.canCraft).
bool matchesShapeless(const Value& r, const CraftingInput& in) {
    const auto& ingredients = r["ingredients"].array();
    if (in.ingredientCount != static_cast<int>(ingredients.size())) return false;
    std::vector<const ItemStack*> stacks;
    for (const auto& s : in.items)
        if (!s.isEmpty()) stacks.push_back(&s);
    std::vector<bool> used(stacks.size());
    std::function<bool(size_t)> assign = [&](size_t i) {
        if (i == ingredients.size()) return true;
        for (size_t j = 0; j < stacks.size(); j++) {
            if (used[j] || !test(ingredients[i], *stacks[j])) continue;
            used[j] = true;
            if (assign(i + 1)) return true;
            used[j] = false;
        }
        return false;
    };
    return assign(0);
}

// TransmuteRecipe: one input item plus 1-8 (or material_count) materials.
std::pair<int, int> materialBounds(const Value& r) {
    return {static_cast<int>(r["material_count"]["min"].number(1)), static_cast<int>(r["material_count"]["max"].number(8))};
}
ItemStack transmuteResult(const Value& r, const ItemStack& input, int materialCount) {
    // TransmuteResult.resolve (empty "result": keep the input's item), with the input's components.
    const Value& res = r["result"];
    int32_t item = res.isString() ? items::byName(res.string()).value_or(0) : res.has("id") ? items::byName(res["id"].string()).value_or(0) : input.item;
    int count = (res.isObject() ? static_cast<int>(res["count"].number(1)) : 1) + (r["add_material_count_to_result"].boolean() ? materialCount : 0);
    ItemStack out = input;
    out.item = item;
    out.count = count;
    items::normalizeComponents(out); // (the input's patch over the result item's prototype)
    if (res.isObject() && res.has("components"))
        for (const auto& [type, value] : res["components"].object()) items::setComponentValue(out, type, nbt::fromJson(value));
    return out;
}
bool matchesTransmute(const Value& r, const CraftingInput& in) {
    auto [minCount, maxCount] = materialBounds(r);
    if (in.ingredientCount < minCount + 1 || in.ingredientCount > maxCount + 1) return false;
    const ItemStack* found = nullptr;
    int materials = 0;
    for (const auto& s : in.items) {
        if (s.isEmpty()) continue;
        if (test(r["input"], s)) {
            if (found) return false;
            found = &s;
        } else {
            if (!test(r["material"], s) || ++materials > maxCount) return false;
        }
    }
    if (!found || materials < minCount || materials > maxCount) return false;
    int resultCount = (r["result"].isObject() ? static_cast<int>(r["result"]["count"].number(1)) : 1) +
                      (r["add_material_count_to_result"].boolean() ? materials : 0);
    if (resultCount != 1) return true;
    ItemStack result = transmuteResult(r, *found, 0);
    return !result.isEmpty() && !ItemStack::isSameItemSameComponents(*found, result);
}

// DyeRecipe (armor, wolf armor, ...): the target mixed with the dyes (DyedItemColor.applyDyes).
constexpr int kDyeDiffuse[16] = {16383998, 16351261, 13061821, 3847130, 16701501, 8439583, 15961002, 4673362,
                                 10329495, 1481884,  8991416,  3949738, 8606770,  6192150, 11546150, 1908001};
constexpr std::string_view kDyeNames[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
std::optional<int> dyeOf(const ItemStack& s) {
    std::string name;
    if (auto v = items::componentValue(s, "minecraft:dye"); v && v->asString()) name = *v->asString();
    else name = std::string(s.info().dye);
    for (int i = 0; i < 16; i++)
        if (kDyeNames[i] == name) return i;
    return std::nullopt;
}
bool matchesDye(const Value& r, const CraftingInput& in) {
    if (in.ingredientCount < 2) return false;
    bool target = false, dyes = false;
    for (const auto& s : in.items) {
        if (s.isEmpty()) continue;
        if (test(r["target"], s)) {
            if (target) return false;
            target = true;
        } else {
            if (!test(r["dye"], s) || !dyeOf(s)) return false;
            dyes = true;
        }
    }
    return target && dyes;
}
ItemStack assembleDye(const Value& r, const CraftingInput& in) {
    const ItemStack* target = nullptr;
    std::vector<int> colors;
    for (const auto& s : in.items) {
        if (s.isEmpty()) continue;
        if (test(r["target"], s)) target = &s;
        else colors.push_back(kDyeDiffuse[dyeOf(s).value_or(0)]);
    }
    if (!target || colors.empty()) return {};
    int red = 0, green = 0, blue = 0, intensity = 0, n = 0;
    auto add = [&](int rgb) {
        int rr = rgb >> 16 & 255, gg = rgb >> 8 & 255, bb = rgb & 255;
        intensity += std::max(rr, std::max(gg, bb));
        red += rr, green += gg, blue += bb, n++;
    };
    if (auto current = items::componentValue(*target, "minecraft:dyed_color"); current && current->isNumeric()) add(static_cast<int>(current->asLong()));
    for (int c : colors) add(c);
    red /= n, green /= n, blue /= n;
    float average = static_cast<float>(intensity) / n;
    float result = static_cast<float>(std::max(red, std::max(green, blue)));
    red = static_cast<int>(red * average / result), green = static_cast<int>(green * average / result), blue = static_cast<int>(blue * average / result);
    ItemStack out = transmuteResult(r, *target, 0); // createWithOriginalComponents
    out.count = static_cast<int>(r["result"]["count"].number(1));
    items::setComponentValue(out, "minecraft:dyed_color", (red << 16) | (green << 8) | blue);
    return out;
}

// ---------------------------------------------------------------- special crafting recipes

constexpr int kFireworkColors[16] = {15790320, 15435844, 12801229, 6719955, 14602026, 4312372, 14188952, 4408131,
                                     11250603, 2651799,  8073150,  2437522, 5320730,  3887386, 11743532, 1973019};

std::vector<const ItemStack*> nonEmpty(const CraftingInput& in) {
    std::vector<const ItemStack*> out;
    for (const auto& s : in.items)
        if (!s.isEmpty()) out.push_back(&s);
    return out;
}
size_t patternCount(const ItemStack& s) { // BANNER_PATTERNS layers
    auto v = items::componentValue(s, "minecraft:banner_patterns");
    return v && v->asList() ? v->asList()->items.size() : 0;
}
std::optional<int> bannerColor(const ItemStack& s) { // BannerItem.getColor: "<color>_banner"
    if (s.info().itemClass != "BannerItem") return std::nullopt;
    std::string_view name = items::name(s.item);
    for (int i = 0; i < 16; i++)
        if (name == "minecraft:" + std::string(kDyeNames[i]) + "_banner") return i;
    return std::nullopt;
}

// RepairItemRecipe: two worn items of the same kind become one (plus 5%), keeping only curses.
std::optional<std::pair<const ItemStack*, const ItemStack*>> repairPair(const CraftingInput& in) {
    auto items = nonEmpty(in);
    if (items.size() != 2) return std::nullopt;
    const ItemStack& a = *items[0];
    const ItemStack& b = *items[1];
    if (a.item != b.item || a.count != 1 || b.count != 1 || items::maxDamage(a) <= 0 || items::maxDamage(b) <= 0) return std::nullopt;
    return std::pair{&a, &b};
}
ItemStack assembleRepair(const CraftingInput& in) {
    auto pair = repairPair(in);
    if (!pair) return {};
    const ItemStack& a = *pair->first;
    const ItemStack& b = *pair->second;
    int durability = std::max(items::maxDamage(a), items::maxDamage(b));
    int remaining = (items::maxDamage(a) - items::damage(a)) + (items::maxDamage(b) - items::damage(b)) + durability * 5 / 100;
    ItemStack out(a.item, 1);
    if (durability != a.info().maxDamage) items::setComponentValue(out, "minecraft:max_damage", durability);
    items::setDamage(out, std::max(durability - remaining, 0));
    // EnchantmentHelper.getEnchantmentsForCrafting (stored ones for books) -> curses only
    std::string_view type = items::name(a.item) == "minecraft:enchanted_book" ? "minecraft:stored_enchantments" : "minecraft:enchantments";
    auto curses = loot::registryTagEntries("minecraft:enchantment", "minecraft:curse");
    nbt::Compound kept;
    for (const ItemStack* s : {&a, &b}) {
        auto v = items::componentValue(*s, type);
        if (!v || !v->asCompound()) continue;
        for (const auto& [id, lvl] : v->asCompound()->entries)
            if (std::find(curses.begin(), curses.end(), id) != curses.end() && lvl.asLong() > kept.getLong(id)) kept.put(id, lvl);
    }
    if (!kept.empty()) items::setComponentValue(out, type, kept);
    return out;
}

bool matchesSpecial(const Value& r, const CraftingInput& in) {
    std::string_view type = r["type"].string();
    auto items = nonEmpty(in);
    if (type == "minecraft:crafting_special_repairitem") return repairPair(in).has_value();
    if (type == "minecraft:crafting_decorated_pot") {
        if (in.width != 3 || in.height != 3 || in.ingredientCount != 4) return false;
        return test(r["back"], in.at(1, 0)) && test(r["left"], in.at(0, 1)) && test(r["right"], in.at(2, 1)) && test(r["front"], in.at(1, 2));
    }
    if (type == "minecraft:crafting_special_bannerduplicate") {
        if (in.ingredientCount != 2) return false;
        std::optional<int> color;
        bool target = false, source = false;
        for (const ItemStack* s : items) {
            auto c = bannerColor(*s);
            if (!test(r["banner"], *s) || !c) return false;
            if (color && *color != *c) return false;
            color = c;
            size_t n = patternCount(*s);
            if (n > 6) return false;
            bool& slot = n > 0 ? source : target;
            if (slot) return false;
            slot = true;
        }
        return source && target;
    }
    if (type == "minecraft:crafting_special_shielddecoration") {
        if (in.ingredientCount != 2) return false;
        bool target = false, banner = false;
        for (const ItemStack* s : items) {
            if (test(r["banner"], *s) && bannerColor(*s)) {
                if (banner) return false;
                banner = true;
            } else {
                if (!test(r["target"], *s) || target || patternCount(*s) > 0) return false;
                target = true;
            }
        }
        return target && banner;
    }
    if (type == "minecraft:crafting_special_bookcloning") {
        if (in.ingredientCount < 2) return false;
        bool material = false, source = false;
        for (const ItemStack* s : items) {
            if (test(r["source"], *s)) {
                auto content = items::componentValue(*s, "minecraft:written_book_content");
                if (!content || !content->asCompound() || content->asCompound()->getInt("generation") >= 2 || source) return false;
                source = true;
            } else {
                if (!test(r["material"], *s)) return false;
                material = true;
            }
        }
        return source && material;
    }
    if (type == "minecraft:crafting_imbue") {
        if (in.width != 3 || in.height != 3 || in.ingredientCount != 9) return false;
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                if (!test(x == 1 && y == 1 ? r["source"] : r["material"], in.at(x, y))) return false;
        return true;
    }
    if (type == "minecraft:crafting_special_firework_rocket") {
        if (in.ingredientCount < 2) return false;
        bool shell = false;
        int fuel = 0;
        for (const ItemStack* s : items) {
            if (test(r["shell"], *s)) {
                if (shell) return false;
                shell = true;
            } else if (test(r["fuel"], *s)) {
                if (++fuel > 3) return false;
            } else if (!test(r["star"], *s)) {
                return false;
            }
        }
        return shell && fuel >= 1;
    }
    if (type == "minecraft:crafting_special_firework_star") {
        if (in.ingredientCount < 2) return false;
        bool fuel = false, dye = false, shape = false, trail = false, twinkle = false;
        for (const ItemStack* s : items) {
            if (test(r["twinkle"], *s)) {
                if (twinkle) return false;
                twinkle = true;
            } else if (test(r["trail"], *s)) {
                if (trail) return false;
                trail = true;
            } else if (test(r["fuel"], *s)) {
                if (fuel) return false;
                fuel = true;
            } else if (test(r["dye"], *s) && dyeOf(*s)) {
                dye = true;
            } else {
                bool isShape = false;
                for (const auto& [name, ing] : r["shapes"].object()) isShape = isShape || test(ing, *s);
                if (!isShape || shape) return false;
                shape = true;
            }
        }
        return fuel && dye;
    }
    if (type == "minecraft:crafting_special_firework_star_fade") {
        if (in.ingredientCount < 2) return false;
        bool dye = false, target = false;
        for (const ItemStack* s : items) {
            if (test(r["dye"], *s) && dyeOf(*s)) {
                dye = true;
            } else {
                if (!test(r["target"], *s) || target) return false;
                target = true;
            }
        }
        return target && dye;
    }
    // TODO: map extending (needs map data: MapExtendingRecipe).
    return false;
}

ItemStack assembleSpecial(const Value& r, const CraftingInput& in) {
    std::string_view type = r["type"].string();
    auto items = nonEmpty(in);
    if (type == "minecraft:crafting_special_repairitem") return assembleRepair(in);
    if (type == "minecraft:crafting_decorated_pot") {
        ItemStack out = resultStack(r["result"]);
        nbt::List sherds{nbt::TagType::String, {}};
        bool allBricks = true;
        for (const ItemStack* s : {&in.at(1, 0), &in.at(0, 1), &in.at(2, 1), &in.at(1, 2)}) { // back, left, right, front
            std::string_view name = items::name(s->item);
            allBricks = allBricks && name == "minecraft:brick";
            sherds.items.emplace_back(std::string(name));
        }
        if (!allBricks) items::setComponentValue(out, "minecraft:pot_decorations", sherds); // (all bricks: the default)
        return out;
    }
    if (type == "minecraft:crafting_special_bannerduplicate") {
        for (const ItemStack* s : items)
            if (size_t n = patternCount(*s); n > 0 && n <= 6) return transmuteResult(r, *s, 0);
        return {};
    }
    if (type == "minecraft:crafting_special_shielddecoration") {
        const ItemStack* banner = nullptr;
        const ItemStack* target = nullptr;
        for (const ItemStack* s : items) {
            if (test(r["banner"], *s) && bannerColor(*s)) banner = s;
            else if (test(r["target"], *s)) target = s;
        }
        if (!banner || !target) return {};
        ItemStack out = transmuteResult(r, *target, 0);
        if (auto patterns = items::componentValue(*banner, "minecraft:banner_patterns")) items::setComponentValue(out, "minecraft:banner_patterns", *patterns);
        items::setComponentValue(out, "minecraft:base_color", std::string(kDyeNames[*bannerColor(*banner)]));
        return out;
    }
    if (type == "minecraft:crafting_special_bookcloning") {
        const ItemStack* source = nullptr;
        int copies = 0;
        for (const ItemStack* s : items) {
            if (test(r["source"], *s) && items::componentValue(*s, "minecraft:written_book_content")) source = s;
            else copies++;
        }
        if (!source) return {};
        auto content = items::componentValue(*source, "minecraft:written_book_content");
        nbt::Compound copied = *content->asCompound(); // WrittenBookContent.craftCopy: one generation further
        copied.put("generation", copied.getInt("generation") + 1);
        ItemStack out = transmuteResult(r, *source, copies - 1);
        items::setComponentValue(out, "minecraft:written_book_content", copied);
        return out;
    }
    if (type == "minecraft:crafting_imbue") {
        ItemStack out = resultStack(r["result"]);
        if (auto potion = items::componentValue(in.at(1, 1), "minecraft:potion_contents")) items::setComponentValue(out, "minecraft:potion_contents", *potion);
        return out;
    }
    if (type == "minecraft:crafting_special_firework_rocket") {
        int fuel = 0;
        nbt::List explosions{nbt::TagType::Compound, {}};
        for (const ItemStack* s : items) {
            if (test(r["fuel"], *s)) fuel++;
            else if (test(r["star"], *s))
                if (auto e = items::componentValue(*s, "minecraft:firework_explosion")) explosions.items.push_back(*e);
        }
        ItemStack out = resultStack(r["result"]);
        nbt::Compound fireworks;
        fireworks.put("flight_duration", static_cast<int8_t>(fuel));
        fireworks.put("explosions", std::move(explosions));
        items::setComponentValue(out, "minecraft:fireworks", fireworks);
        return out;
    }
    if (type == "minecraft:crafting_special_firework_star") {
        std::string shape = "small_ball";
        bool twinkle = false, trail = false;
        std::vector<int32_t> colors;
        for (const ItemStack* s : items) {
            std::string found;
            for (const auto& [name, ing] : r["shapes"].object())
                if (found.empty() && test(ing, *s)) found = name;
            if (!found.empty()) shape = found;
            else if (test(r["twinkle"], *s)) twinkle = true;
            else if (test(r["trail"], *s)) trail = true;
            else if (test(r["dye"], *s)) colors.push_back(kFireworkColors[dyeOf(*s).value_or(0)]);
        }
        ItemStack out = resultStack(r["result"]);
        nbt::Compound explosion;
        explosion.put("shape", shape);
        explosion.put("colors", colors);
        explosion.put("fade_colors", std::vector<int32_t>{});
        explosion.put("has_trail", nbt::byteTag(trail));
        explosion.put("has_twinkle", nbt::byteTag(twinkle));
        items::setComponentValue(out, "minecraft:firework_explosion", explosion);
        return out;
    }
    if (type == "minecraft:crafting_special_firework_star_fade") {
        std::vector<int32_t> colors;
        const ItemStack* target = nullptr;
        for (const ItemStack* s : items) {
            if (test(r["dye"], *s)) colors.push_back(kFireworkColors[dyeOf(*s).value_or(0)]);
            else if (test(r["target"], *s)) target = s;
        }
        if (!target || colors.empty()) return {};
        ItemStack out = transmuteResult(r, *target, 0);
        nbt::Compound explosion; // FireworkExplosion.DEFAULT, then withFadeColors
        explosion.put("shape", std::string("small_ball"));
        if (auto e = items::componentValue(*target, "minecraft:firework_explosion"); e && e->asCompound()) explosion = *e->asCompound();
        explosion.put("fade_colors", colors);
        items::setComponentValue(out, "minecraft:firework_explosion", explosion);
        return out;
    }
    return {};
}

bool matches(const Value& r, const CraftingInput& in) {
    std::string_view type = r["type"].string();
    if (type == "minecraft:crafting_shaped") {
        Shaped s = unpackShaped(r);
        if (in.ingredientCount != s.ingredientCount || in.width != s.width || in.height != s.height) return false;
        return (!s.symmetrical && matchesShaped(s, in, true)) || matchesShaped(s, in, false);
    }
    if (type == "minecraft:crafting_shapeless") return matchesShapeless(r, in);
    if (type == "minecraft:crafting_transmute") return matchesTransmute(r, in);
    if (type == "minecraft:crafting_dye") return matchesDye(r, in);
    return matchesSpecial(r, in);
}

} // namespace

// The items an ingredient accepts.
void addItems(const Value& ingredient, std::vector<int32_t>& out) {
    auto one = [&](std::string_view ref) {
        if (!ref.empty() && ref[0] == '#') {
            for (int32_t i : items::tagItems(ref.substr(1))) out.push_back(i);
        } else if (auto id = items::byName(ref)) {
            out.push_back(*id);
        }
    };
    if (ingredient.isArray()) {
        for (const auto& e : ingredient.array()) one(e.string());
    } else if (ingredient.isString()) {
        one(ingredient.string());
    } else if (ingredient.isObject() && ingredient.has("item")) { // PotionIngredient
        addItems(ingredient["item"], out);
    }
}

// Ingredient.CONTENTS_STREAM_CODEC (ByteBufCodecs.holderSet): a tag, or the item ids.
void writeIngredient(ByteWriter& out, const Value& ingredient) {
    if (ingredient.isString() && !ingredient.string().empty() && ingredient.string()[0] == '#') {
        out.writeVarInt(0);
        std::string tag(ingredient.string().substr(1));
        out.writeUtf(tag.find(':') == std::string::npos ? "minecraft:" + tag : tag);
        return;
    }
    std::vector<int32_t> ids;
    addItems(ingredient, ids);
    out.writeVarInt(static_cast<int32_t>(ids.size()) + 1);
    for (int32_t id : ids) out.writeVarInt(id);
}

std::vector<std::pair<std::string, const Value*>> g_stonecutting; // id, recipe

void bootstrap() {
    try {
        g_recipes = json::parse(gen::kRecipesJson);
    } catch (const std::exception& e) {
        log::error("Failed to parse recipes: {}", e.what());
        return;
    }
    for (const auto& [id, r] : g_recipes.object()) {
        if (r["type"].string() == "minecraft:stonecutting") g_stonecutting.emplace_back(id, &r);
        if (!isCraftingType(r["type"].string())) continue;
        g_crafting.push_back(&r);
        g_craftingIds.push_back(id);
    }
}

std::vector<Stonecutting> stonecuttingFor(const ItemStack& input) {
    std::vector<Stonecutting> out;
    if (input.isEmpty()) return out;
    for (const auto& [id, r] : g_stonecutting)
        if (test((*r)["ingredient"], input)) out.push_back({id, resultStack((*r)["result"])});
    return out;
}

bool acceptsStonecutting(const ItemStack& input) {
    if (input.isEmpty()) return false;
    for (const auto& [id, r] : g_stonecutting)
        if (test((*r)["ingredient"], input)) return true;
    return false;
}

namespace {
// Ingredient.testOptionalIngredient: no ingredient means the slot must be empty.
bool testOptional(const Value& r, const char* field, const ItemStack& stack) {
    return r.has(field) ? test(r[field], stack) : stack.isEmpty();
}
} // namespace

std::optional<std::string> findSmithing(const ItemStack& templ, const ItemStack& base, const ItemStack& addition) {
    if (templ.isEmpty() && base.isEmpty() && addition.isEmpty()) return std::nullopt; // SmithingRecipeInput.isEmpty
    for (const auto& [id, r] : g_recipes.object()) {
        std::string_view type = r["type"].string();
        if (type != "minecraft:smithing_transform" && type != "minecraft:smithing_trim") continue;
        if (testOptional(r, "template", templ) && test(r["base"], base) && testOptional(r, "addition", addition)) return id;
    }
    return std::nullopt;
}

ItemStack assembleSmithing(const std::string& id, const ItemStack& base, const ItemStack& addition) {
    const Value& r = g_recipes[id];
    if (r["type"].string() == "minecraft:smithing_transform") { // TransmuteRecipe.createWithOriginalComponents
        const Value& res = r["result"];
        ItemStack out = base;
        out.item = items::byName(res["id"].string()).value_or(0);
        out.count = static_cast<int>(res["count"].number(1));
        items::normalizeComponents(out);
        if (res.has("components"))
            for (const auto& [type, value] : res["components"].object()) items::setComponentValue(out, type, nbt::fromJson(value));
        return out;
    }
    // SmithingTrimRecipe.applyTrim
    auto material = items::effectiveComponent(addition, "minecraft:provides_trim_material");
    if (!material || !material->asString()) return {};
    nbt::Compound trim;
    trim.put("material", *material->asString());
    trim.put("pattern", r["pattern"].string());
    auto existing = items::effectiveComponent(base, "minecraft:trim");
    if (existing && existing->asCompound() && existing->asCompound()->getString("material") == *material->asString() &&
        existing->asCompound()->getString("pattern") == r["pattern"].string())
        return {};
    ItemStack out = base.copyWithCount(1);
    items::setComponentValue(out, "minecraft:trim", trim);
    return out;
}

bool inPropertySet(std::string_view set, const ItemStack& stack) {
    if (stack.isEmpty()) return false;
    struct Def {
        std::string_view name;
        std::vector<std::string_view> types;
        const char* field;
    };
    static const Def kDefs[] = {{"minecraft:smithing_addition", {"minecraft:smithing_transform", "minecraft:smithing_trim"}, "addition"},
                                {"minecraft:smithing_base", {"minecraft:smithing_transform", "minecraft:smithing_trim"}, "base"},
                                {"minecraft:smithing_template", {"minecraft:smithing_transform", "minecraft:smithing_trim"}, "template"},
                                {"minecraft:brewing_input", {"minecraft:brewing"}, "input"},
                                {"minecraft:brewing_reagent", {"minecraft:brewing"}, "reagent"}};
    for (const auto& d : kDefs) {
        if (d.name != set) continue;
        for (const auto& [id, r] : g_recipes.object()) {
            if (std::find(d.types.begin(), d.types.end(), r["type"].string()) == d.types.end() || !r.has(d.field)) continue;
            const Value& ingredient = r[d.field].isObject() ? r[d.field]["item"] : r[d.field]; // (a PotionIngredient's item)
            if (test(ingredient, stack)) return true;
        }
    }
    return false;
}

namespace {
// PotionIngredient.test: the item, and the potion (PotionsPredicate on minecraft:potion_contents).
bool testPotionIngredient(const Value& p, const ItemStack& stack) {
    if (!test(p["item"], stack)) return false;
    if (!p.has("potion_contents")) return true;
    const Value& pred = p["potion_contents"];
    if (!pred.has("potions")) return true; // (effects predicates: no brewing recipe uses them)
    auto contents = items::effectiveComponent(stack, "minecraft:potion_contents");
    std::string potion;
    if (contents && contents->asString()) potion = *contents->asString();
    else if (contents && contents->asCompound()) potion = contents->asCompound()->getString("potion");
    if (potion.empty()) return false;
    const Value& set = pred["potions"];
    if (set.isArray()) {
        for (const auto& e : set.array())
            if (e.string() == potion) return true;
        return false;
    }
    std::string_view ref = set.string();
    if (!ref.empty() && ref[0] == '#') return registries::hasTag("minecraft:potion", potion, ref.substr(1));
    return ref == potion;
}
} // namespace

std::optional<ItemStack> findBrewing(const ItemStack& input, const ItemStack& reagent) {
    if (input.isEmpty() || reagent.isEmpty()) return std::nullopt;
    for (const auto& [id, r] : g_recipes.object())
        if (r["type"].string() == "minecraft:brewing" && testPotionIngredient(r["input"], input) && testPotionIngredient(r["reagent"], reagent))
            return resultStack(r["output"]);
    return std::nullopt;
}

void writeUpdateRecipes(ByteWriter& out) {
    // RecipeManager.RECIPE_PROPERTY_SETS
    struct Set {
        const char* name;
        std::vector<std::string_view> types;
        const char* field;
    };
    const Set sets[] = {{"minecraft:smithing_addition", {"minecraft:smithing_transform", "minecraft:smithing_trim"}, "addition"},
                        {"minecraft:smithing_base", {"minecraft:smithing_transform", "minecraft:smithing_trim"}, "base"},
                        {"minecraft:smithing_template", {"minecraft:smithing_transform", "minecraft:smithing_trim"}, "template"},
                        {"minecraft:furnace_input", {"minecraft:smelting"}, "ingredient"},
                        {"minecraft:blast_furnace_input", {"minecraft:blasting"}, "ingredient"},
                        {"minecraft:smoker_input", {"minecraft:smoking"}, "ingredient"},
                        {"minecraft:campfire_input", {"minecraft:campfire_cooking"}, "ingredient"},
                        {"minecraft:brewing_input", {"minecraft:brewing"}, "input"},
                        {"minecraft:brewing_reagent", {"minecraft:brewing"}, "reagent"}};
    out.writeVarInt(static_cast<int32_t>(std::size(sets)));
    for (const auto& set : sets) {
        std::vector<int32_t> ids;
        for (const auto& [id, r] : g_recipes.object())
            if (std::find(set.types.begin(), set.types.end(), r["type"].string()) != set.types.end() && r.has(set.field)) addItems(r[set.field], ids);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        out.writeUtf(set.name);
        out.writeVarInt(static_cast<int32_t>(ids.size()));
        for (int32_t id : ids) out.writeVarInt(id);
    }
    // SelectableRecipe.SingleInputSet.noRecipeCodec: (ingredient, SlotDisplay.ItemStackSlotDisplay(result))
    out.writeVarInt(static_cast<int32_t>(g_stonecutting.size()));
    for (const auto& [id, r] : g_stonecutting) {
        writeIngredient(out, (*r)["ingredient"]);
        ItemStack result = resultStack((*r)["result"]);
        out.writeVarInt(5); // minecraft:item_stack slot display
        out.writeVarInt(result.item);
        out.writeVarInt(result.count);
        out.writeVarInt(0); // (no components)
        out.writeVarInt(0);
    }
}

PositionedInput positioned(int width, int height, const std::vector<ItemStack>& grid) {
    int left = width - 1, right = 0, top = height - 1, bottom = 0;
    for (int y = 0; y < height; y++) {
        bool rowEmpty = true;
        for (int x = 0; x < width; x++) {
            if (grid[x + y * width].isEmpty()) continue;
            left = std::min(left, x), right = std::max(right, x);
            rowEmpty = false;
        }
        if (!rowEmpty) top = std::min(top, y), bottom = std::max(bottom, y);
    }
    PositionedInput out;
    int w = right - left + 1, h = bottom - top + 1;
    if (w <= 0 || h <= 0) return out;
    out.left = left, out.top = top;
    out.input.width = w, out.input.height = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const ItemStack& s = grid[x + left + (y + top) * width];
            out.input.items.push_back(s);
            out.input.ingredientCount += !s.isEmpty();
        }
    return out;
}

std::optional<std::string> findCrafting(const CraftingInput& input) {
    if (input.ingredientCount == 0) return std::nullopt;
    for (size_t i = 0; i < g_crafting.size(); i++)
        if (matches(*g_crafting[i], input)) return std::string(g_craftingIds[i]);
    return std::nullopt;
}

ItemStack assemble(const std::string& id, const CraftingInput& input) {
    const Value& r = g_recipes[id];
    std::string_view type = r["type"].string();
    if (type == "minecraft:crafting_shaped" || type == "minecraft:crafting_shapeless") return resultStack(r["result"]);
    if (type == "minecraft:crafting_dye") return assembleDye(r, input);
    if (type == "minecraft:crafting_transmute") {
        const ItemStack* in = nullptr;
        int materials = 0;
        for (const auto& s : input.items) {
            if (s.isEmpty()) continue;
            if (test(r["input"], s)) in = &s;
            else if (test(r["material"], s)) materials++;
        }
        return in ? transmuteResult(r, *in, r["add_material_count_to_result"].boolean() ? materials : 0) : ItemStack{};
    }
    return assembleSpecial(r, input);
}

std::vector<ItemStack> remainingItems(const std::string* id, const CraftingInput& input) {
    // CraftingRecipe.defaultCraftingReminder: the items' crafting remainders; banner duplication
    // keeps the patterned banner and book cloning the original book.
    std::string_view type = id ? g_recipes[*id]["type"].string() : std::string_view{};
    std::vector<ItemStack> out(input.items.size());
    bool keptBook = false;
    for (size_t i = 0; i < input.items.size(); i++) {
        const ItemStack& s = input.items[i];
        if (s.isEmpty()) continue;
        if (s.info().craftRemainder > 0) out[i] = ItemStack(s.info().craftRemainder, 1);
        else if (type == "minecraft:crafting_special_bannerduplicate" && patternCount(s) > 0) out[i] = s.copyWithCount(1);
        else if (type == "minecraft:crafting_special_bookcloning" && !keptBook && items::componentValue(s, "minecraft:written_book_content")) {
            out[i] = s.copyWithCount(1);
            keptBook = true;
        }
    }
    return out;
}

float cookingExperience(std::string_view id) { return static_cast<float>(g_recipes[id]["experience"].number(0)); }

std::optional<Cooking> findCooking(std::string_view type, const ItemStack& input) {
    if (input.isEmpty()) return std::nullopt;
    for (const auto& [id, r] : g_recipes.object()) {
        if (r["type"].string() != type || !test(r["ingredient"], input)) continue;
        Cooking c;
        c.id = id;
        c.result = resultStack(r["result"]);
        c.experience = static_cast<float>(r["experience"].number(0));
        c.cookingTime = static_cast<int>(r["cookingtime"].number(type == "minecraft:campfire_cooking" ? 100 : type == "minecraft:smelting" ? 200 : 100));
        return c;
    }
    return std::nullopt;
}

} // namespace mc::recipes
