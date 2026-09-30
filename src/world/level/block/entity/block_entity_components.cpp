#include "world/level/block/entity/block_entity_components.hpp"

#include <set>
#include <string>

#include "world/level/block/block_state.hpp"
#include "core/component/data_components.hpp"
#include "nbt/snbt.hpp"

namespace mc::block_entity {
namespace {

bool isOneOf(std::string_view type, std::initializer_list<std::string_view> types) {
    for (auto t : types)
        if (t == type) return true;
    return false;
}

std::string typeOf(const nbt::Compound& be) {
    std::string id = be.getString("id");
    return id.find(':') == std::string::npos && !id.empty() ? "minecraft:" + id : id;
}

// BaseContainerBlockEntity subclasses (name, lock and "Items").
bool isBaseContainer(std::string_view t) {
    return isOneOf(t, {"minecraft:barrel", "minecraft:blast_furnace", "minecraft:brewing_stand", "minecraft:chest", "minecraft:crafter",
                       "minecraft:dispenser", "minecraft:dropper", "minecraft:furnace", "minecraft:hopper", "minecraft:shulker_box",
                       "minecraft:smoker", "minecraft:trapped_chest"});
}
// RandomizableContainerBlockEntity subclasses (container_loot).
bool isRandomizable(std::string_view t) {
    return isOneOf(t, {"minecraft:barrel", "minecraft:chest", "minecraft:crafter", "minecraft:dispenser", "minecraft:dropper",
                       "minecraft:hopper", "minecraft:shulker_box", "minecraft:trapped_chest"});
}
// Other block entities whose items are stored under "Items".
bool hasItemList(std::string_view t) { return isBaseContainer(t) || isOneOf(t, {"minecraft:chiseled_bookshelf", "minecraft:shelf", "minecraft:campfire"}); }

// Key of the custom name in the block entity's NBT, if it has one.
std::string_view nameKey(std::string_view t) {
    if (t == "minecraft:skull") return "custom_name";
    if (isBaseContainer(t) || isOneOf(t, {"minecraft:beacon", "minecraft:enchanting_table", "minecraft:command_block", "minecraft:banner"}))
        return "CustomName";
    return {};
}
bool hasLock(std::string_view t) { return isBaseContainer(t) || t == "minecraft:beacon"; }

// ContainerHelper "Items" [{Slot, id, count, components}] <-> ItemContainerContents [{slot, item}].
nbt::Tag itemsToContainer(const nbt::List& items) {
    std::vector<nbt::Tag> slots;
    for (const auto& e : items.items) {
        const nbt::Compound* it = e.asCompound();
        if (!it || it->getString("id").empty() || it->getInt("count", 1) <= 0) continue;
        nbt::Compound item;
        item.put("id", it->getString("id"));
        if (int count = it->getInt("count", 1); count != 1) item.put("count", count);
        if (const nbt::Compound* comps = it->getCompound("components"); comps && !comps->empty()) item.put("components", *comps);
        nbt::Compound slot;
        slot.put("slot", it->getInt("Slot") & 0xFF);
        slot.put("item", std::move(item));
        slots.emplace_back(std::move(slot));
    }
    return codec::makeList(std::move(slots));
}

nbt::List containerToItems(const nbt::Tag& container) {
    nbt::List items;
    items.elementType = nbt::TagType::Compound;
    std::vector<nbt::Tag> slots;
    try {
        slots = codec::listItems(container);
    } catch (const std::exception&) {
        return items;
    }
    for (const auto& e : slots) {
        const nbt::Compound* slot = e.asCompound();
        if (!slot) continue;
        nbt::Compound it;
        if (const nbt::Compound* item = slot->getCompound("item")) {
            it = *item;
        } else if (const std::string* id = slot->get("item") ? slot->get("item")->asString() : nullptr) {
            it.put("id", *id); // ItemStackTemplate's short form
        } else {
            continue;
        }
        if (!it.contains("count")) it.put("count", 1);
        it.put("Slot", static_cast<int8_t>(slot->getInt("slot")));
        items.items.emplace_back(std::move(it));
    }
    return items;
}

// A saved ItemStack (as in a decorated pot's "item") as a one-slot container.
nbt::Tag itemToContainer(const nbt::Compound& item) {
    nbt::List list;
    nbt::Compound withSlot = item;
    withSlot.put("Slot", static_cast<int8_t>(0));
    list.items.emplace_back(std::move(withSlot));
    return itemsToContainer(list);
}

void setOrRemove(nbt::Compound& be, std::string_view key, const nbt::Tag* value) {
    if (value) be.put(std::string(key), *value);
    else be.remove(key);
}

void merge(nbt::Compound& into, const nbt::Compound& from) {
    // CompoundTag.merge: nested compounds merge, everything else is replaced.
    for (const auto& [k, v] : from.entries) {
        nbt::Compound* existing = into.getCompound(k);
        if (existing && v.asCompound()) merge(*existing, *v.asCompound());
        else into.put(k, v);
    }
}

} // namespace

nbt::Compound collectComponents(const nbt::Compound& be) {
    const std::string t = typeOf(be);
    nbt::Compound out;
    auto copy = [&](std::string_view key, std::string_view component) {
        if (const nbt::Tag* v = be.get(key)) out.put(std::string(component), *v);
    };
    // BlockEntity.components: whatever the placed item carried that the block entity didn't use.
    if (const nbt::Compound* leftovers = be.getCompound("components"))
        for (const auto& [k, v] : leftovers->entries) out.put(k, v);
    if (auto key = nameKey(t); !key.empty()) copy(key, "minecraft:custom_name");
    if (hasLock(t)) copy("lock", "minecraft:lock");
    if (hasItemList(t)) {
        if (const nbt::List* items = be.getList("Items")) {
            nbt::Tag container = itemsToContainer(*items);
            if (!codec::listItems(container).empty()) out.put("minecraft:container", std::move(container));
        }
    }
    if (isRandomizable(t) && be.contains("LootTable")) {
        nbt::Compound loot;
        loot.put("loot_table", be.getString("LootTable"));
        if (int64_t seed = be.getLong("LootTableSeed"); seed != 0) loot.put("seed", seed);
        out.put("minecraft:container_loot", std::move(loot));
    }
    if (t == "minecraft:banner") copy("patterns", "minecraft:banner_patterns");
    if (t == "minecraft:skull") {
        copy("profile", "minecraft:profile");
        copy("note_block_sound", "minecraft:note_block_sound");
    }
    if (t == "minecraft:beehive") copy("bees", "minecraft:bees");
    if (t == "minecraft:decorated_pot") {
        copy("sherds", "minecraft:pot_decorations");
        if (const nbt::Compound* item = be.getCompound("item")) out.put("minecraft:container", itemToContainer(*item));
    }
    if (t == "minecraft:sign" || t == "minecraft:hanging_sign") {
        copy("front_text", "minecraft:sign_text_front");
        copy("back_text", "minecraft:sign_text_back");
        if (be.getBool("is_waxed")) out.put("minecraft:waxed", nbt::Compound{});
    }
    return out;
}

void applyItem(nbt::Compound& be, const ItemStack& stack, bool allowOpData) {
    const std::string t = typeOf(be);
    // The item's components in their saved form (the patch's added entries).
    nbt::Compound comps = stack.diskComponents;
    for (const auto& c : stack.components) {
        if (c.removed) continue;
        const components::ComponentType* type = components::byId(c.type);
        if (!type) continue;
        if (auto disk = components::toDisk(c.type, c.value)) comps.put(std::string(type->name), std::move(*disk));
    }

    // BlockItem.updateCustomBlockEntityTag
    if (const nbt::Compound* data = comps.getCompound("minecraft:block_entity_data")) {
        std::string id = data->getString("id");
        if (id.find(':') == std::string::npos) id = "minecraft:" + id;
        bool opOnly = isOneOf(t, {"minecraft:command_block", "minecraft:lectern", "minecraft:sign", "minecraft:hanging_sign", "minecraft:mob_spawner",
                                  "minecraft:trial_spawner"});
        if (id == t && (!opOnly || allowOpData)) {
            nbt::Compound tag = *data;
            tag.remove("id");
            tag.remove("x");
            tag.remove("y");
            tag.remove("z");
            merge(be, tag);
        }
    }

    // BlockEntity.applyComponents -> applyImplicitComponents; `used` tracks what was looked at.
    std::set<std::string, std::less<>> used{"minecraft:block_entity_data", "minecraft:block_state"};
    auto get = [&](std::string_view name) -> const nbt::Tag* {
        used.emplace(name);
        return comps.get(name);
    };
    if (auto key = nameKey(t); !key.empty()) setOrRemove(be, key, get("minecraft:custom_name"));
    if (hasLock(t)) setOrRemove(be, "lock", get("minecraft:lock"));
    if (hasItemList(t)) {
        const nbt::Tag* container = get("minecraft:container");
        be.put("Items", container ? containerToItems(*container) : nbt::List{nbt::TagType::Compound, {}});
    }
    if (isRandomizable(t)) {
        if (const nbt::Compound* loot = get("minecraft:container_loot") ? comps.getCompound("minecraft:container_loot") : nullptr) {
            be.put("LootTable", loot->getString("loot_table"));
            if (int64_t seed = loot->getLong("seed"); seed != 0) be.put("LootTableSeed", seed);
            else be.remove("LootTableSeed");
        }
    }
    if (t == "minecraft:banner") setOrRemove(be, "patterns", get("minecraft:banner_patterns"));
    if (t == "minecraft:skull") {
        setOrRemove(be, "profile", get("minecraft:profile"));
        setOrRemove(be, "note_block_sound", get("minecraft:note_block_sound"));
    }
    if (t == "minecraft:beehive") setOrRemove(be, "bees", get("minecraft:bees"));
    if (t == "minecraft:decorated_pot") {
        setOrRemove(be, "sherds", get("minecraft:pot_decorations"));
        be.remove("item");
        if (const nbt::Tag* container = get("minecraft:container")) {
            nbt::List items = containerToItems(*container);
            if (!items.items.empty()) {
                nbt::Compound first = *items.items[0].asCompound();
                first.remove("Slot");
                be.put("item", std::move(first));
            }
        }
    }
    if (t == "minecraft:sign" || t == "minecraft:hanging_sign") {
        const nbt::Tag* front = get("minecraft:sign_text_front");
        const nbt::Tag* back = get("minecraft:sign_text_back");
        be.put("front_text", front ? *front : nbt::Tag(emptySignText()));
        be.put("back_text", back ? *back : nbt::Tag(emptySignText()));
        be.put("is_waxed", nbt::byteTag(get("minecraft:waxed") != nullptr));
    }
    // RandomizableContainerBlockEntity.trySaveLootTable: a loot table replaces the items; shulker
    // boxes don't save an empty item list (ContainerHelper.saveAllItems(..., false)).
    if (const nbt::List* items = be.getList("Items"))
        if ((isRandomizable(t) && be.contains("LootTable")) || (t == "minecraft:shulker_box" && items->items.empty())) be.remove("Items");

    nbt::Compound leftovers;
    for (const auto& [k, v] : comps.entries)
        if (!used.contains(k)) leftovers.put(k, v);
    be.put("components", std::move(leftovers));
}

nbt::Compound emptySignText() {
    nbt::List messages{nbt::TagType::String, {}};
    for (int i = 0; i < 4; i++) messages.items.emplace_back(std::string());
    nbt::Compound text;
    text.put("color", std::string("black"));
    text.put("has_glowing_text", nbt::byteTag(false));
    text.put("messages", std::move(messages));
    return text;
}

nbt::Compound createDefault(uint32_t block, BlockPos pos) {
    const auto& beh = blocks::behaviour(block);
    nbt::Compound be;
    if (!beh.blockEntityDefault.empty())
        if (auto parsed = nbt::parseSnbt(beh.blockEntityDefault); parsed && parsed->asCompound()) be = *parsed->asCompound();
    // BlockEntity.saveMetadata
    be.put("id", std::string(beh.blockEntity));
    be.put("x", pos.x);
    be.put("y", pos.y);
    be.put("z", pos.z);
    return be;
}

bool sendsUpdates(std::string_view t) {
    return isOneOf(t, {"minecraft:banner", "minecraft:beacon", "minecraft:brushable_block", "minecraft:campfire", "minecraft:conduit",
                       "minecraft:creaking_heart", "minecraft:decorated_pot", "minecraft:jigsaw", "minecraft:skull", "minecraft:shelf",
                       "minecraft:sign", "minecraft:hanging_sign", "minecraft:mob_spawner", "minecraft:structure_block",
                       "minecraft:test_block", "minecraft:test_instance_block", "minecraft:end_gateway", "minecraft:trial_spawner"});
}

nbt::Compound getUpdateTag(const nbt::Compound& be) {
    const std::string t = typeOf(be);
    nbt::Compound custom = be; // saveCustomOnly: without the metadata and the stored components
    for (const char* k : {"id", "x", "y", "z", "keepPacked", "components"}) custom.remove(k);
    auto only = [&](std::initializer_list<const char*> keys) {
        nbt::Compound out;
        for (const char* k : keys)
            if (const nbt::Tag* v = be.get(k)) out.put(k, *v);
        return out;
    };
    if (t == "minecraft:banner") { // saveWithoutMetadata: with components
        if (const nbt::Tag* c = be.get("components")) custom.put("components", *c);
        return custom;
    }
    if (t == "minecraft:mob_spawner") {
        custom.remove("SpawnPotentials");
        return custom;
    }
    if (t == "minecraft:campfire") return only({"Items"});
    if (t == "minecraft:shelf") return only({"Items", "align_items_to_bottom"});
    if (t == "minecraft:brushable_block") return only({"item", "hit_direction"});
    if (t == "minecraft:trial_spawner") return only({"next_mob_spawns_at", "spawn_data"}); // next_mob_spawns_at only while active
    if (sendsUpdates(t)) return custom;
    return {};
}

} // namespace mc::block_entity
