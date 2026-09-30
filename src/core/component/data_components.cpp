#include "core/component/data_components.hpp"

#include <algorithm>
#include <unordered_map>

#include "core/registries.hpp"
#include "generated/registries.hpp"
#include "nbt/snbt.hpp"

namespace mc::components {
using namespace codec;

namespace {

std::vector<ComponentType> g_types; // by id
std::unordered_map<std::string_view, const ComponentType*> g_byName;

const nbt::Compound& asComp(const nbt::Tag& t) {
    const nbt::Compound* c = t.asCompound();
    if (!c) throw CodecError("expected a compound");
    return *c;
}

// ---------------------------------------------------------------- DataComponentPatch
class Patch final : public Schema {
public:
    nbt::Tag read(ByteReader& in) const override {
        int32_t added = in.readVarInt(), removed = in.readVarInt();
        if (added < 0 || removed < 0 || added + removed > 256) throw DecoderException("bad component patch");
        nbt::Compound out;
        for (int i = 0; i < added; i++) {
            const ComponentType* t = byId(in.readVarInt());
            if (!t) throw DecoderException("unknown component type");
            nbt::Tag v = t->schema->read(in);
            if (t->persistent) out.put(std::string(t->name), std::move(v)); // transient ones aren't saved
        }
        for (int i = 0; i < removed; i++) {
            const ComponentType* t = byId(in.readVarInt());
            if (!t) throw DecoderException("unknown component type");
            if (t->persistent) out.put("!" + std::string(t->name), nbt::Compound{});
        }
        return out;
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        std::vector<std::pair<const ComponentType*, const nbt::Tag*>> added;
        std::vector<const ComponentType*> removed;
        for (const auto& [key, value] : asComp(v).entries) {
            bool isRemoved = !key.empty() && key[0] == '!';
            const ComponentType* t = lookup(isRemoved ? std::string_view(key).substr(1) : std::string_view(key));
            if (isRemoved) removed.push_back(t);
            else added.emplace_back(t, &value);
        }
        out.writeVarInt(static_cast<int32_t>(added.size()));
        out.writeVarInt(static_cast<int32_t>(removed.size()));
        for (auto [t, value] : added) {
            out.writeVarInt(t->id);
            t->schema->write(out, t->schema->normalize(*value));
        }
        for (auto* t : removed) out.writeVarInt(t->id);
    }
    uint32_t hash(const nbt::Tag& v) const override {
        std::vector<std::pair<uint32_t, uint32_t>> es;
        for (const auto& [key, value] : asComp(v).entries) {
            bool isRemoved = !key.empty() && key[0] == '!';
            const ComponentType* t = lookup(isRemoved ? std::string_view(key).substr(1) : std::string_view(key));
            std::string name = (isRemoved ? "!" : "") + std::string(t->name);
            es.emplace_back(hash::ofString(name), isRemoved ? hash::ofMap({}) : t->schema->hash(t->schema->normalize(value)));
        }
        return hash::ofMap(std::move(es));
    }

private:
    static const ComponentType* lookup(std::string_view name) {
        std::string full = name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name) : std::string(name);
        const ComponentType* t = byName(full);
        if (!t || !t->persistent) throw CodecError("unknown component " + full);
        return t;
    }
};

// ---------------------------------------------------------------- ItemContainerContents
// Network: a list of optional templates (empty slots included); disk: [{slot, item}] of filled ones.
class Container final : public Schema {
public:
    nbt::Tag read(ByteReader& in) const override {
        int32_t n = in.readVarInt();
        if (n < 0 || n > 256) throw DecoderException("container too large");
        std::vector<nbt::Tag> slots;
        for (int i = 0; i < n; i++) {
            if (!in.readBool()) continue;
            nbt::Compound slot;
            slot.put("slot", i);
            slot.put("item", itemStackTemplate()->read(in));
            slots.emplace_back(std::move(slot));
        }
        return makeList(std::move(slots));
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        auto slots = bySlot(v);
        out.writeVarInt(static_cast<int32_t>(slots.size()));
        for (const auto& item : slots) {
            out.writeBool(item.has_value());
            if (item) itemStackTemplate()->write(out, itemStackTemplate()->normalize(*item));
        }
    }
    uint32_t hash(const nbt::Tag& v) const override {
        std::vector<uint32_t> hs;
        for (const auto& e : listItems(v)) {
            const nbt::Compound& c = asComp(e);
            const nbt::Tag* item = c.get("item");
            if (!item) throw CodecError("slot without item");
            hs.push_back(hash::ofMap({{hash::ofString("slot"), hash::ofInt(c.getInt("slot"))},
                                      {hash::ofString("item"), itemStackTemplate()->hash(itemStackTemplate()->normalize(*item))}}));
        }
        return hash::ofList(hs);
    }

private:
    // ItemContainerContents.fromSlots: as many slots as the highest index + 1.
    static std::vector<std::optional<nbt::Tag>> bySlot(const nbt::Tag& v) {
        std::vector<std::optional<nbt::Tag>> out;
        for (const auto& e : listItems(v)) {
            const nbt::Compound& c = asComp(e);
            const nbt::Tag* slot = c.get("slot");
            const nbt::Tag* item = c.get("item");
            if (!slot || !slot->isNumeric() || !item) throw CodecError("bad container slot");
            int64_t index = slot->asLong();
            if (index < 0 || index > 255) throw CodecError("bad container slot index");
            if (static_cast<size_t>(index) >= out.size()) out.resize(index + 1);
            out[index] = *item;
        }
        return out;
    }
};

// ---------------------------------------------------------------- TypedEntityData
// Disk: the entity/block entity NBT with its type under "id"; network: type id + NBT without it.
class TypedEntityData final : public Schema {
public:
    explicit TypedEntityData(std::string_view registry) : type_(registryId(registry)) {}
    nbt::Tag read(ByteReader& in) const override {
        nbt::Tag type = type_->read(in);
        nbt::Tag tag = compoundTag()->read(in);
        tag.asCompound()->put("id", std::move(type));
        return tag;
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        nbt::Compound c = asComp(v);
        const nbt::Tag* id = c.get("id");
        if (!id) throw CodecError("entity data without id");
        type_->write(out, type_->normalize(*id));
        c.remove("id");
        nbt::writeNetwork(out, c);
    }
    uint32_t hash(const nbt::Tag& v) const override {
        const nbt::Compound& c = asComp(v);
        if (!c.get("id")) throw CodecError("entity data without id");
        return hash::ofTag(v);
    }

private:
    Ptr type_;
};

// ---------------------------------------------------------------- ResolvableProfile
class Profile final : public Schema {
public:
    nbt::Tag read(ByteReader& in) const override {
        nbt::Compound out;
        if (in.readBool()) { // ByteBufCodecs.GAME_PROFILE
            out.put("id", uuid()->read(in));
            out.put("name", in.readUtf(16));
            nbt::Tag props = readProperties(in);
            if (!listItems(props).empty()) out.put("properties", std::move(props));
        } else { // ResolvableProfile.Partial
            if (in.readBool()) out.put("name", in.readUtf(16));
            if (in.readBool()) out.put("id", uuid()->read(in));
            nbt::Tag props = readProperties(in);
            if (!listItems(props).empty()) out.put("properties", std::move(props));
        }
        for (const char* key : {"texture", "cape", "elytra"})
            if (in.readBool()) out.put(key, in.readUtf());
        if (in.readBool()) out.put("model", std::string(in.readBool() ? "slim" : "wide"));
        return out;
    }
    void write(ByteWriter& out, const nbt::Tag& value) const override {
        nbt::Tag normalized = normalize(value);
        const nbt::Compound& c = asComp(normalized);
        const nbt::Tag* id = c.get("id");
        const nbt::Tag* name = c.get("name");
        const nbt::Tag* props = c.get("properties");
        bool full = id && name; // Codec.mapEither tries the full profile first
        out.writeBool(full);
        if (full) {
            uuid()->write(out, *id);
            out.writeUtf(*requireString(*name));
        } else {
            out.writeBool(name != nullptr);
            if (name) out.writeUtf(*requireString(*name));
            out.writeBool(id != nullptr);
            if (id) uuid()->write(out, *id);
        }
        writeProperties(out, props);
        for (const char* key : {"texture", "cape", "elytra"}) {
            const nbt::Tag* t = c.get(key);
            out.writeBool(t != nullptr);
            if (t) out.writeUtf(*requireString(*t));
        }
        const nbt::Tag* model = c.get("model");
        out.writeBool(model != nullptr);
        if (model) out.writeBool(*requireString(*model) == "slim");
    }
    uint32_t hash(const nbt::Tag& value) const override {
        nbt::Tag normalized = normalize(value);
        const nbt::Compound& c = asComp(normalized);
        std::vector<std::pair<uint32_t, uint32_t>> es;
        for (const auto& [k, v] : c.entries) {
            uint32_t h;
            if (k == "id") h = uuid()->hash(v);
            else if (k == "properties") h = propertiesHash(v);
            else h = hash::ofTag(v);
            es.emplace_back(hash::ofString(k), h);
        }
        return hash::ofMap(std::move(es));
    }
    nbt::Tag normalize(const nbt::Tag& v) const override {
        if (const std::string* s = v.asString()) { // Codec.withAlternative(..., PLAYER_NAME)
            nbt::Compound c;
            c.put("name", *s);
            return c;
        }
        return v;
    }

private:
    static const std::string* requireString(const nbt::Tag& t) {
        if (!t.asString()) throw CodecError("expected a string");
        return t.asString();
    }
    static nbt::Tag readProperties(ByteReader& in) {
        int32_t n = in.readVarInt();
        if (n < 0 || n > 16) throw DecoderException("too many profile properties");
        std::vector<nbt::Tag> out;
        for (int i = 0; i < n; i++) {
            nbt::Compound p;
            p.put("name", in.readUtf(64));
            p.put("value", in.readUtf(32767));
            if (in.readBool()) p.put("signature", in.readUtf(1024));
            out.emplace_back(std::move(p));
        }
        return makeList(std::move(out));
    }
    // ExtraCodecs.PROPERTY_MAP: a list of {name, value, signature?}, or a map name -> [values].
    static std::vector<nbt::Compound> properties(const nbt::Tag* t) {
        std::vector<nbt::Compound> out;
        if (!t) return out;
        if (const nbt::Compound* m = t->asCompound()) {
            for (const auto& [name, values] : m->entries)
                for (const auto& v : listItems(values)) {
                    nbt::Compound p;
                    p.put("name", name);
                    p.put("value", *requireString(v));
                    out.push_back(std::move(p));
                }
            return out;
        }
        for (const auto& e : listItems(*t)) out.push_back(asComp(e));
        return out;
    }
    static void writeProperties(ByteWriter& out, const nbt::Tag* t) {
        auto props = properties(t);
        if (props.size() > 16) throw CodecError("too many profile properties");
        out.writeVarInt(static_cast<int32_t>(props.size()));
        for (const auto& p : props) {
            out.writeUtf(p.getString("name"));
            out.writeUtf(p.getString("value"));
            const nbt::Tag* sig = p.get("signature");
            out.writeBool(sig != nullptr);
            if (sig) out.writeUtf(*requireString(*sig));
        }
    }
    static uint32_t propertiesHash(const nbt::Tag& t) {
        std::vector<uint32_t> hs;
        for (const auto& p : properties(&t)) {
            std::vector<std::pair<uint32_t, uint32_t>> es{{hash::ofString("name"), hash::ofString(p.getString("name"))},
                                                          {hash::ofString("value"), hash::ofString(p.getString("value"))}};
            if (p.contains("signature")) es.emplace_back(hash::ofString("signature"), hash::ofString(p.getString("signature")));
            hs.push_back(hash::ofMap(std::move(es)));
        }
        return hash::ofList(hs);
    }
};

// ---------------------------------------------------------------- DataComponentMatchers pieces
// DataComponentExactPredicate: disk {component: value}; network [(type, value)].
class ExactComponents final : public Schema {
public:
    nbt::Tag read(ByteReader& in) const override {
        int32_t n = in.readVarInt();
        if (n < 0 || n > 256) throw DecoderException("too many components");
        nbt::Compound out;
        for (int i = 0; i < n; i++) {
            const ComponentType* t = byId(in.readVarInt());
            if (!t) throw DecoderException("unknown component type");
            nbt::Tag v = t->schema->read(in);
            out.put(std::string(t->name), std::move(v));
        }
        return out;
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        const nbt::Compound& c = asComp(v);
        out.writeVarInt(static_cast<int32_t>(c.entries.size()));
        for (const auto& [name, value] : c.entries) {
            const ComponentType* t = byName(name.find(':') == std::string::npos ? "minecraft:" + name : name);
            if (!t) throw CodecError("unknown component " + name);
            out.writeVarInt(t->id);
            t->schema->write(out, t->schema->normalize(value));
        }
    }
    uint32_t hash(const nbt::Tag& v) const override {
        std::vector<std::pair<uint32_t, uint32_t>> es;
        for (const auto& [name, value] : asComp(v).entries) {
            const ComponentType* t = byName(name.find(':') == std::string::npos ? "minecraft:" + name : name);
            if (!t) throw CodecError("unknown component " + name);
            es.emplace_back(hash::ofString(t->name), t->schema->hash(t->schema->normalize(value)));
        }
        return hash::ofMap(std::move(es));
    }
};

// DataComponentPredicate map: only the empty map is supported (partial predicates such as
// enchantment or damage checks aren't ported; items using them keep their raw encoding).
class NoPartialPredicates final : public Schema {
public:
    nbt::Tag read(ByteReader& in) const override {
        if (in.readVarInt() != 0) throw DecoderException("component predicates not supported");
        return nbt::Compound{};
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        if (!asComp(v).empty()) throw CodecError("component predicates not supported");
        out.writeVarInt(0);
    }
    uint32_t hash(const nbt::Tag& v) const override {
        if (!asComp(v).empty()) throw CodecError("component predicates not supported");
        return hash::ofMap({});
    }
};

nbt::Compound compound(std::initializer_list<std::pair<std::string, nbt::Tag>> entries) {
    nbt::Compound c;
    for (const auto& [k, v] : entries) c.put(k, v);
    return c;
}

// ---------------------------------------------------------------- shared definitions
struct Defs {
    Ptr dye = enumeration("DyeColor");
    Ptr soundEvent = holder("minecraft:sound_event", record({req("sound_id", identifier()), opt("range", float32())}));
    Ptr mobEffect = registryId("minecraft:mob_effect");
    Ptr text = codec::text();
    Ptr rgb = alternative(int32(), [](const nbt::Tag& t) -> std::optional<nbt::Tag> {
        // ExtraCodecs.RGB_COLOR_CODEC alternative: [r, g, b] floats -> ARGB.colorFromFloat(1, r, g, b).
        try {
            auto v = listItems(t);
            if (v.size() != 3) return std::nullopt;
            auto c = [](const nbt::Tag& x) { return static_cast<int32_t>(std::clamp(x.asDouble(), 0.0, 1.0) * 255.0f + 0.5f) & 0xFF; };
            return nbt::Tag(static_cast<int32_t>(0xFF000000u | (c(v[0]) << 16) | (c(v[1]) << 8) | c(v[2])));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    });
    Ptr filterableString(size_t max) {
        return alternative(record({req("raw", string(max)), opt("filtered", string(max))}), [](const nbt::Tag& t) -> std::optional<nbt::Tag> {
            if (!t.asString()) return std::nullopt;
            return nbt::Tag(compound({{"raw", t}}));
        });
    }
    Ptr filterableText() {
        return alternative(record({req("raw", text), opt("filtered", text)}), [](const nbt::Tag& t) -> std::optional<nbt::Tag> {
            if (t.asCompound() && t.asCompound()->contains("raw")) return std::nullopt;
            return nbt::Tag(compound({{"raw", t}}));
        });
    }

    Ptr details = lazy([this] { return detailsSchema; });
    Ptr detailsSchema;
    std::vector<Field> detailsFields() {
        return {def("amplifier", unsignedByte(), static_cast<int8_t>(0)), def("duration", varInt(), 0), def("ambient", boolean(), nbt::byteTag(false)),
                def("show_particles", boolean(), nbt::byteTag(true)), always("show_icon", boolean(), nbt::byteTag(true)),
                opt("hidden_effect", details)};
    }
    Ptr mobEffectInstance;
    Ptr consumeEffect;
    Ptr blockPredicate;
    Ptr resolvableInt = either(int32(), identifier());
    Ptr resolvableFloat = either(float32(), identifier());

    Defs() {
        detailsSchema = record(detailsFields());
        mobEffectInstance = record(fields({{req("id", mobEffect)}, detailsFields()}));
        consumeEffect = dispatchRegistry(
            "minecraft:consume_effect_type",
            {{"minecraft:apply_effects", {req("effects", list(mobEffectInstance)), def("probability", float32(), 1.0f)}},
             {"minecraft:remove_effects", {req("effects", holderSet("minecraft:mob_effect"))}},
             {"minecraft:clear_all_effects", {}},
             {"minecraft:teleport_randomly", {def("diameter", float32(), 16.0f), def("directional_particles", boolean(), nbt::byteTag(true))}},
             {"minecraft:play_sound", {req("sound", soundEvent)}}});
        Ptr statePredicate = map(string(), either(string(), record({opt("min", string()), opt("max", string())})));
        blockPredicate = record({opt("blocks", holderSet("minecraft:block")), opt("state", statePredicate), opt("nbt", snbtCompound()),
                                 def("components", std::make_shared<ExactComponents>(), nbt::Compound{}),
                                 def("predicates", std::make_shared<NoPartialPredicates>(), nbt::Compound{})});
    }
};

Ptr g_template;
Ptr g_patch;

void add(std::string_view name, Ptr schema, bool persistent = true) {
    auto id = registries::id("minecraft:data_component_type", name);
    if (!id) return; // not in this game version
    if (static_cast<size_t>(*id) >= g_types.size()) g_types.resize(*id + 1);
    g_types[*id] = {name, *id, std::move(schema), persistent};
}

} // namespace

codec::Ptr itemStackTemplate() { return g_template; }
codec::Ptr patch() { return g_patch; }

void bootstrap() {
    if (!g_types.empty()) return;
    static Defs d;
    g_patch = std::make_shared<Patch>();
    g_template = alternative(record({req("id", registryId("minecraft:item")), def("count", varInt(), 1), def("components", g_patch, nbt::Compound{})}),
                             [](const nbt::Tag& t) -> std::optional<nbt::Tag> {
                                 if (!t.asString()) return std::nullopt;
                                 return nbt::Tag(compound({{"id", t}}));
                             });
    Ptr tmpl = g_template;
    auto enchantments = map(registryId("minecraft:enchantment"), varInt());
    auto adventure = compactList(d.blockPredicate);
    auto swing = record({def("type", enumeration("SwingAnimationType"), "whack"), def("duration", varInt(), 6)});
    auto kineticCondition = record({req("max_duration_ticks", varInt()), def("min_speed", float32(), 0.0f), def("min_relative_speed", float32(), 0.0f)});
    auto signText = record({req("messages", fixedList(d.text, 4)), opt("filtered_messages", fixedList(d.text, 4)),
                            always("color", d.dye, "black"), always("has_glowing_text", boolean(), nbt::byteTag(false))});
    auto nbtOnly = [](Ptr codec) { return nbtOf(std::move(codec)); }; // persistent without a StreamCodec
    auto variant = [](std::string_view registry) { return registryId(registry); };
    nbt::List defaultReductions;
    defaultReductions.items.emplace_back(compound({{"base", 0.0f}, {"factor", 1.0f}}));

    add("minecraft:custom_data", nbtOnly(alternative(compoundTag(), [](const nbt::Tag& t) -> std::optional<nbt::Tag> {
            if (!t.asString()) return std::nullopt;
            auto parsed = nbt::parseSnbt(*t.asString()); // CustomData also accepts SNBT strings
            if (!parsed || !parsed->asCompound()) return std::nullopt;
            return parsed;
        })));
    add("minecraft:max_stack_size", varInt());
    add("minecraft:max_damage", varInt());
    add("minecraft:damage", varInt());
    add("minecraft:unbreakable", unit());
    add("minecraft:use_effects", record({def("can_sprint", boolean(), nbt::byteTag(false)), def("interact_vibrations", boolean(), nbt::byteTag(true)),
                                         def("speed_multiplier", float32(), 0.2f)}));
    add("minecraft:custom_name", d.text);
    add("minecraft:minimum_attack_charge", float32());
    add("minecraft:damage_type", registryId("minecraft:damage_type"));
    add("minecraft:item_name", d.text);
    add("minecraft:item_model", identifier());
    add("minecraft:lore", list(d.text, 256));
    add("minecraft:rarity", enumeration("Rarity"));
    add("minecraft:enchantments", enchantments);
    add("minecraft:can_place_on", adventure);
    add("minecraft:can_break", adventure);
    add("minecraft:attribute_modifiers",
        list(record(fields({{req("type", registryId("minecraft:attribute"))},
                            {req("id", identifier()), req("amount", float64()), req("operation", enumeration("AttributeModifier$Operation"))},
                            {def("slot", enumeration("EquipmentSlotGroup"), "any"),
                             def("display",
                                 dispatchEnum("ItemAttributeModifiers$Display$Type",
                                              {{"default", {}}, {"hidden", {}}, {"override", {req("value", d.text)}}}),
                                 compound({{"type", "default"}}))}}))));
    add("minecraft:custom_model_data", record({def("floats", list(float32()), nbt::List{}), def("flags", list(boolean()), nbt::List{}),
                                               def("strings", list(string()), nbt::List{}), def("colors", list(d.rgb), nbt::List{})}));
    add("minecraft:tooltip_display", record({def("hide_tooltip", boolean(), nbt::byteTag(false)),
                                             def("hidden_components", list(registryId("minecraft:data_component_type")), nbt::List{})}));
    add("minecraft:repair_cost", varInt());
    add("minecraft:creative_slot_lock", unit(), false);
    add("minecraft:enchantment_glint_override", boolean());
    add("minecraft:intangible_projectile", nbtOnly(unit()));
    add("minecraft:food", record({req("nutrition", varInt()), req("saturation", float32()), def("can_always_eat", boolean(), nbt::byteTag(false))}));
    add("minecraft:consumable", record({def("consume_seconds", float32(), 1.6f), def("animation", enumeration("ItemUseAnimation"), "eat"),
                                        def("sound", d.soundEvent, "minecraft:entity.generic.eat"),
                                        def("has_consume_particles", boolean(), nbt::byteTag(true)),
                                        def("on_consume_effects", list(d.consumeEffect), nbt::List{})}));
    add("minecraft:use_remainder", tmpl);
    add("minecraft:use_cooldown", record({req("seconds", float32()), opt("cooldown_group", identifier())}));
    add("minecraft:damage_resistant", record({req("types", holderSet("minecraft:damage_type"))}));
    add("minecraft:tool", record({req("rules", list(record({req("blocks", holderSet("minecraft:block")), opt("speed", float32()),
                                                           opt("correct_for_drops", boolean())}))),
                                  def("default_mining_speed", float32(), 1.0f), def("damage_per_block", varInt(), 1),
                                  def("can_destroy_blocks_in_creative", boolean(), nbt::byteTag(true))}));
    add("minecraft:weapon", record({def("item_damage_per_attack", varInt(), 1), def("disable_blocking_for_seconds", float32(), 0.0f)}));
    add("minecraft:attack_range", record({def("min_reach", float32(), 0.0f), def("max_reach", float32(), 3.0f), def("min_creative_reach", float32(), 0.0f),
                                          def("max_creative_reach", float32(), 5.0f), def("hitbox_margin", float32(), 0.3f),
                                          def("mob_factor", float32(), 1.0f)}));
    add("minecraft:enchantable", record({req("value", varInt())}));
    add("minecraft:equippable",
        record({req("slot", enumeration("EquipmentSlot")), def("equip_sound", d.soundEvent, "minecraft:item.armor.equip_generic"),
                opt("asset_id", identifier()), opt("camera_overlay", identifier()), opt("allowed_entities", holderSet("minecraft:entity_type")),
                def("dispensable", boolean(), nbt::byteTag(true)), def("swappable", boolean(), nbt::byteTag(true)),
                def("damage_on_hurt", boolean(), nbt::byteTag(true)), def("equip_on_interact", boolean(), nbt::byteTag(false)),
                def("can_be_sheared", boolean(), nbt::byteTag(false)), def("shearing_sound", d.soundEvent, "minecraft:item.shears.snip")}));
    add("minecraft:repairable", record({req("items", holderSet("minecraft:item"))}));
    add("minecraft:glider", unit());
    add("minecraft:tooltip_style", identifier());
    add("minecraft:death_protection", record({def("death_effects", list(d.consumeEffect), nbt::List{})}));
    add("minecraft:blocks_attacks",
        record({def("block_delay_seconds", float32(), 0.0f), def("disable_cooldown_scale", float32(), 1.0f),
                def("damage_reductions",
                    list(record({def("horizontal_blocking_angle", float32(), 90.0f), opt("type", holderSet("minecraft:damage_type")),
                                 req("base", float32()), req("factor", float32())})),
                    defaultReductions),
                def("item_damage", record({req("threshold", float32()), req("base", float32()), req("factor", float32())}),
                    compound({{"threshold", 1.0f}, {"base", 0.0f}, {"factor", 1.0f}})),
                opt("bypassed_by", holderSet("minecraft:damage_type")), opt("block_sound", d.soundEvent), opt("disabled_sound", d.soundEvent)}));
    add("minecraft:piercing_weapon", record({def("deals_knockback", boolean(), nbt::byteTag(true)), def("dismounts", boolean(), nbt::byteTag(false)),
                                             opt("sound", d.soundEvent), opt("hit_sound", d.soundEvent)}));
    add("minecraft:kinetic_weapon", record({def("contact_cooldown_ticks", varInt(), 10), def("delay_ticks", varInt(), 0),
                                            opt("dismount_conditions", kineticCondition), opt("knockback_conditions", kineticCondition),
                                            opt("damage_conditions", kineticCondition), def("forward_movement", float32(), 0.0f),
                                            def("damage_multiplier", float32(), 1.0f), opt("sound", d.soundEvent), opt("hit_sound", d.soundEvent)}));
    add("minecraft:attack_animation", swing);
    add("minecraft:interact_animation", swing);
    add("minecraft:additional_trade_cost", varInt(), false);
    add("minecraft:block_transformer", registryId("minecraft:block_transformer"));
    add("minecraft:villager_food", record({req("nutrition", varInt())}));
    add("minecraft:stored_enchantments", enchantments);
    add("minecraft:dye", d.dye);
    add("minecraft:dyed_color", d.rgb);
    add("minecraft:map_id", varInt());
    add("minecraft:map_decorations",
        nbtOnly(map(string(), record({req("type", registryId("minecraft:map_decoration_type")), req("x", float64()), req("z", float64()),
                                      req("rotation", float32())}))));
    add("minecraft:map_post_processing", enumeration("MapPostProcessing"), false);
    add("minecraft:charged_projectiles", list(tmpl, 1024));
    add("minecraft:bundle_contents", list(tmpl));
    add("minecraft:potion_contents",
        alternative(record({opt("potion", registryId("minecraft:potion")), opt("custom_color", int32()),
                            def("custom_effects", list(d.mobEffectInstance), nbt::List{}), opt("custom_name", string())}),
                    [](const nbt::Tag& t) -> std::optional<nbt::Tag> {
                        if (!t.asString()) return std::nullopt;
                        return nbt::Tag(compound({{"potion", t}}));
                    }));
    add("minecraft:potion_duration_scale", float32());
    add("minecraft:suspicious_stew_effects", list(record({req("id", d.mobEffect), def("duration", varInt(), 160)})));
    add("minecraft:writable_book_content", record({def("pages", list(d.filterableString(1024), 100), nbt::List{})}));
    add("minecraft:written_book_content",
        record({req("title", d.filterableString(32)), req("author", string()), def("generation", varInt(), 0),
                def("pages", list(d.filterableText()), nbt::List{}), def("resolved", boolean(), nbt::byteTag(false))}));
    add("minecraft:trim", record({req("material", holder("minecraft:trim_material")), req("pattern", holder("minecraft:trim_pattern"))}));
    add("minecraft:debug_stick_state", nbtOnly(map(registryId("minecraft:block"), string())));
    add("minecraft:entity_data", std::make_shared<TypedEntityData>("minecraft:entity_type"));
    add("minecraft:bucket_entity_data", compoundTag());
    add("minecraft:block_entity_data", std::make_shared<TypedEntityData>("minecraft:block_entity_type"));
    add("minecraft:instrument", holder("minecraft:instrument"));
    add("minecraft:provides_trim_material", holder("minecraft:trim_material"));
    add("minecraft:ominous_bottle_amplifier", varInt());
    add("minecraft:jukebox_playable", holder("minecraft:jukebox_song"));
    add("minecraft:provides_banner_patterns", holderSet("minecraft:banner_pattern"));
    add("minecraft:recipes", nbtOnly(list(identifier())));
    add("minecraft:lodestone_tracker", record({opt("target", record({req("dimension", identifier()), req("pos", blockPos())})),
                                               def("tracked", boolean(), nbt::byteTag(true))}));
    auto explosion = record({req("shape", enumeration("FireworkExplosion$Shape")), def("colors", list(int32()), nbt::List{}),
                             def("fade_colors", list(int32()), nbt::List{}), def("has_trail", boolean(), nbt::byteTag(false)),
                             def("has_twinkle", boolean(), nbt::byteTag(false))});
    add("minecraft:firework_explosion", explosion);
    add("minecraft:fireworks", record({def("flight_duration", unsignedByte(), static_cast<int8_t>(0)), def("explosions", list(explosion, 256), nbt::List{})}));
    add("minecraft:profile", std::make_shared<Profile>());
    add("minecraft:note_block_sound", identifier());
    add("minecraft:banner_patterns", list(record({req("pattern", holder("minecraft:banner_pattern")), req("color", d.dye)})));
    add("minecraft:base_color", d.dye);
    add("minecraft:pot_decorations", record({opt("back", tmpl), opt("left", tmpl), opt("right", tmpl), opt("front", tmpl)}));
    add("minecraft:container", std::make_shared<Container>());
    add("minecraft:block_state", map(string(), string()));
    add("minecraft:bees", list(record({req("entity_data", std::make_shared<TypedEntityData>("minecraft:entity_type")),
                                       req("ticks_in_hive", varInt()), req("min_ticks_in_hive", varInt())})));
    add("minecraft:sulfur_cube_content", tmpl);
    add("minecraft:lock", nbtOnly(anyTag()));
    add("minecraft:container_loot", nbtOnly(record({req("loot_table", identifier()), def("seed", int64(), static_cast<int64_t>(0))})));
    add("minecraft:break_sound", d.soundEvent);
    add("minecraft:compostable", record({req("layers", d.resolvableInt)}));
    add("minecraft:cooking_fuel", record({req("burn_time", d.resolvableInt), req("speed_multiplier", d.resolvableFloat)}));
    add("minecraft:brewing_fuel", record({req("uses", d.resolvableInt), req("speed_multiplier", d.resolvableFloat)}));
    add("minecraft:mob_visibility", record({req("targeting_entity_types", holderSet("minecraft:entity_type")), req("visibility", float32())}));
    add("minecraft:villager/variant", variant("minecraft:villager_type"));
    add("minecraft:wolf/variant", variant("minecraft:wolf_variant"));
    add("minecraft:wolf/sound_variant", variant("minecraft:wolf_sound_variant"));
    add("minecraft:wolf/collar", d.dye);
    add("minecraft:fox/variant", enumeration("Fox$Variant"));
    add("minecraft:salmon/size", enumeration("Salmon$Variant"));
    add("minecraft:parrot/variant", enumeration("Parrot$Variant"));
    add("minecraft:tropical_fish/pattern", enumeration("TropicalFish$Pattern"));
    add("minecraft:tropical_fish/base_color", d.dye);
    add("minecraft:tropical_fish/pattern_color", d.dye);
    add("minecraft:mooshroom/variant", enumeration("MushroomCow$Variant"));
    add("minecraft:rabbit/variant", enumeration("Rabbit$Variant"));
    add("minecraft:pig/variant", variant("minecraft:pig_variant"));
    add("minecraft:pig/sound_variant", variant("minecraft:pig_sound_variant"));
    add("minecraft:cow/variant", variant("minecraft:cow_variant"));
    add("minecraft:cow/sound_variant", variant("minecraft:cow_sound_variant"));
    add("minecraft:chicken/variant", variant("minecraft:chicken_variant"));
    add("minecraft:chicken/sound_variant", variant("minecraft:chicken_sound_variant"));
    add("minecraft:zombie_nautilus/variant", variant("minecraft:zombie_nautilus_variant"));
    add("minecraft:frog/variant", variant("minecraft:frog_variant"));
    add("minecraft:horse/variant", enumeration("Variant"));
    add("minecraft:painting/variant", holder("minecraft:painting_variant"));
    add("minecraft:llama/variant", enumeration("Llama$Variant"));
    add("minecraft:axolotl/variant", enumeration("Axolotl$Variant"));
    add("minecraft:cat/variant", variant("minecraft:cat_variant"));
    add("minecraft:cat/sound_variant", variant("minecraft:cat_sound_variant"));
    add("minecraft:cat/collar", d.dye);
    add("minecraft:sheep/color", d.dye);
    add("minecraft:shulker/color", d.dye);
    add("minecraft:provides_pottery_pattern", variant("minecraft:decorated_pot_pattern"));
    add("minecraft:sign_text_front", signText);
    add("minecraft:sign_text_back", signText);
    add("minecraft:waxed", unit());
    add("minecraft:cushion/color", d.dye);

    for (auto& t : g_types)
        if (t.schema) g_byName.emplace(t.name, &t);
}

const ComponentType* byId(int32_t id) {
    if (id < 0 || static_cast<size_t>(id) >= g_types.size() || !g_types[id].schema) return nullptr;
    return &g_types[id];
}

const ComponentType* byName(std::string_view name) {
    auto it = g_byName.find(name);
    return it == g_byName.end() ? nullptr : it->second;
}

std::optional<nbt::Tag> toDisk(int32_t type, std::span<const uint8_t> value) {
    const ComponentType* t = byId(type);
    if (!t || !t->persistent) return std::nullopt;
    try {
        ByteReader in(value);
        nbt::Tag out = t->schema->read(in);
        if (in.remaining() != 0) return std::nullopt;
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> toNetwork(int32_t type, const nbt::Tag& value) {
    const ComponentType* t = byId(type);
    if (!t) return std::nullopt;
    try {
        ByteWriter out;
        t->schema->write(out, t->schema->normalize(value));
        return std::move(out.data());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int32_t> hash(int32_t type, std::span<const uint8_t> value) {
    const ComponentType* t = byId(type);
    if (!t) return std::nullopt;
    try {
        ByteReader in(value);
        nbt::Tag v = t->schema->read(in);
        return static_cast<int32_t>(t->schema->hash(v));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace mc::components
