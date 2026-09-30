#include "server/players/player_data.hpp"

#include "core/registries.hpp"
#include "generated/registries.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/chunk/storage/chunk_serializer.hpp"

namespace mc::player_data {
namespace {
struct EquipName {
    EquipmentSlot slot;
    const char* name;
};
// EntityEquipment keys (EquipmentSlot serialized names); the main hand lives in Inventory.
constexpr EquipName kEquipment[] = {{EquipmentSlot::OffHand, "offhand"}, {EquipmentSlot::Feet, "feet"}, {EquipmentSlot::Legs, "legs"},
                                    {EquipmentSlot::Chest, "chest"}, {EquipmentSlot::Head, "head"}};

nbt::List doubles(std::initializer_list<double> v) {
    nbt::List l{nbt::TagType::Double, {}};
    for (double d : v) l.items.push_back(d);
    return l;
}
nbt::List floats(std::initializer_list<float> v) {
    nbt::List l{nbt::TagType::Float, {}};
    for (float f : v) l.items.push_back(f);
    return l;
}
} // namespace

void load(ServerPlayer& p, const nbt::Compound& tag) {
    p.savedTag = tag;
    if (const nbt::List* pos = tag.getList("Pos"); pos && pos->items.size() == 3) {
        p.x = pos->items[0].asDouble(), p.y = pos->items[1].asDouble(), p.z = pos->items[2].asDouble();
    }
    if (const nbt::List* rot = tag.getList("Rotation"); rot && rot->items.size() == 2) {
        p.yRot = static_cast<float>(rot->items[0].asDouble());
        p.xRot = static_cast<float>(rot->items[1].asDouble());
    }
    if (tag.contains("playerGameType")) p.gameMode = static_cast<GameType>(std::clamp(tag.getInt("playerGameType"), 0, 3));
    p.inventory.setSelected(std::clamp(tag.getInt("SelectedItemSlot"), 0, Inventory::kHotbarSize - 1));
    if (const nbt::List* inv = tag.getList("Inventory")) {
        for (const auto& e : inv->items) {
            const nbt::Compound* c = e.asCompound();
            if (!c) continue;
            int slot = c->getInt("Slot", -1) & 0xFF;
            if (slot >= Inventory::kMainSize) continue;
            if (auto stack = item_nbt::read(*c)) p.inventory.item(slot) = std::move(*stack);
        }
    }
    if (const nbt::Compound* eq = tag.getCompound("equipment")) {
        for (const auto& [slot, name] : kEquipment)
            if (const nbt::Compound* c = eq->getCompound(name))
                if (auto stack = item_nbt::read(*c)) p.inventory.equipment(slot) = std::move(*stack);
    }
    if (const nbt::List* ender = tag.getList("EnderItems")) { // PlayerEnderChestContainer.fromSlots
        for (const auto& e : ender->items) {
            const nbt::Compound* c = e.asCompound();
            if (!c) continue;
            int slot = c->getInt("Slot") & 0xFF;
            if (slot < p.enderChest.size())
                if (auto stack = item_nbt::read(*c)) p.enderChest.item(slot) = std::move(*stack);
        }
    }
    if (const nbt::Compound* r = tag.getCompound("respawn")) { // RespawnConfig
        const std::vector<int32_t>* pos = r->getIntArray("pos");
        if (pos && pos->size() == 3)
            p.respawn = ServerPlayer::RespawnConfig{r->getString("dimension", "minecraft:overworld"), {(*pos)[0], (*pos)[1], (*pos)[2]},
                                                    static_cast<float>(r->getDouble("yaw")), static_cast<float>(r->getDouble("pitch")),
                                                    r->getBool("forced")};
    }
    p.experienceProgress = static_cast<float>(tag.getDouble("XpP"));
    p.experienceLevel = tag.getInt("XpLevel");
    p.totalExperience = tag.getInt("XpTotal");
    p.enchantmentSeed = tag.getInt("XpSeed");
    p.score = tag.getInt("Score");
    // LivingEntity "active_effects" (MobEffectInstance.CODEC)
    if (const nbt::List* effects = tag.getList("active_effects")) {
        for (const auto& e : effects->items) {
            const nbt::Compound* c = e.asCompound();
            if (!c) continue;
            auto id = registries::id("minecraft:mob_effect", c->getString("id"));
            if (!id) continue;
            ServerPlayer::EffectInstance inst;
            inst.amplifier = c->getInt("amplifier") & 0xFF;
            inst.duration = c->getInt("duration");
            inst.ambient = c->getBool("ambient");
            inst.visible = c->getBool("show_particles", true);
            inst.showIcon = c->getBool("show_icon", inst.visible);
            p.effects[*id] = inst;
        }
    }
}

nbt::Compound save(const ServerPlayer& constPlayer) {
    auto& p = const_cast<ServerPlayer&>(constPlayer); // Inventory accessors are non-const
    nbt::Compound tag = p.savedTag;
    tag.put("DataVersion", kDataVersion);
    tag.put("Pos", doubles({p.x, p.y, p.z}));
    tag.put("Rotation", floats({p.yRot, p.xRot}));
    if (!tag.contains("Motion")) tag.put("Motion", doubles({0, 0, 0}));
    tag.put("OnGround", nbt::byteTag(p.onGround));
    tag.put("Dimension", std::string(p.level().dimension()));
    const UUID& id = p.profile().id;
    tag.put("UUID", std::vector<int32_t>{static_cast<int32_t>(id.most >> 32), static_cast<int32_t>(id.most),
                                         static_cast<int32_t>(id.least >> 32), static_cast<int32_t>(id.least)});
    tag.put("playerGameType", static_cast<int>(p.gameMode));
    if (!tag.contains("Health")) tag.put("Health", 20.0f);
    if (!tag.contains("foodLevel")) tag.put("foodLevel", 20);

    // GameType.updatePlayerAbilities
    nbt::Compound abilities;
    if (const nbt::Compound* old = tag.getCompound("abilities")) abilities = *old;
    bool creative = p.gameMode == GameType::Creative, spectator = p.gameMode == GameType::Spectator;
    abilities.put("invulnerable", nbt::byteTag(creative || spectator));
    abilities.put("mayfly", nbt::byteTag(creative || spectator));
    abilities.put("instabuild", nbt::byteTag(creative));
    abilities.put("mayBuild", nbt::byteTag(p.gameMode == GameType::Survival || creative));
    abilities.put("flying", nbt::byteTag(spectator || (creative && abilities.getBool("flying"))));
    if (!abilities.contains("flySpeed")) abilities.put("flySpeed", 0.05f);
    if (!abilities.contains("walkSpeed")) abilities.put("walkSpeed", 0.1f);
    tag.put("abilities", std::move(abilities));

    nbt::List ender{nbt::TagType::Compound, {}};
    for (int i = 0; i < p.enderChest.size(); i++) {
        if (p.enderChest.item(i).isEmpty()) continue;
        nbt::Compound c = item_nbt::write(p.enderChest.item(i));
        c.put("Slot", static_cast<int8_t>(i));
        ender.items.push_back(std::move(c));
    }
    tag.put("EnderItems", std::move(ender));
    tag.remove("respawn");
    if (p.respawn) {
        nbt::Compound r;
        r.put("dimension", p.respawn->dimension);
        r.put("pos", std::vector<int32_t>{p.respawn->pos.x, p.respawn->pos.y, p.respawn->pos.z});
        float yaw = std::fmod(p.respawn->yaw, 360.0f); // LevelData.RespawnData.of: Mth.wrapDegrees, pitch clamped
        if (yaw >= 180.0f) yaw -= 360.0f;
        if (yaw < -180.0f) yaw += 360.0f;
        r.put("yaw", yaw);
        r.put("pitch", std::clamp(p.respawn->pitch, -90.0f, 90.0f));
        if (p.respawn->forced) r.put("forced", nbt::byteTag(true));
        tag.put("respawn", std::move(r));
    }
    tag.put("XpP", p.experienceProgress);
    tag.put("XpLevel", p.experienceLevel);
    tag.put("XpTotal", p.totalExperience);
    tag.put("XpSeed", p.enchantmentSeed);
    tag.put("Score", p.score);
    tag.put("SelectedItemSlot", p.inventory.selected());
    nbt::List inv{nbt::TagType::Compound, {}};
    for (int i = 0; i < Inventory::kMainSize; i++) {
        const ItemStack& s = p.inventory.item(i);
        if (s.isEmpty()) continue;
        nbt::Compound c = item_nbt::write(s);
        c.put("Slot", static_cast<int8_t>(i));
        inv.items.push_back(std::move(c));
    }
    tag.put("Inventory", std::move(inv));
    nbt::Compound eq;
    for (const auto& [slot, name] : kEquipment) {
        const ItemStack& s = p.inventory.equipment(slot);
        if (!s.isEmpty()) eq.put(name, item_nbt::write(s));
    }
    tag.put("equipment", std::move(eq));
    tag.remove("active_effects");
    if (!p.effects.empty()) {
        nbt::List effects{nbt::TagType::Compound, {}};
        for (const auto& [id, e] : p.effects) {
            nbt::Compound c;
            for (const auto& r : gen::kStaticRegistries)
                if (r.name == "minecraft:mob_effect") c.put("id", std::string(r.entries[id]));
            if (e.amplifier != 0) c.put("amplifier", static_cast<int8_t>(e.amplifier));
            if (e.duration != 0) c.put("duration", e.duration);
            if (e.ambient) c.put("ambient", nbt::byteTag(true));
            if (!e.visible) c.put("show_particles", nbt::byteTag(false));
            c.put("show_icon", nbt::byteTag(e.showIcon));
            effects.items.push_back(std::move(c));
        }
        tag.put("active_effects", std::move(effects));
    }
    return tag;
}

} // namespace mc::player_data
