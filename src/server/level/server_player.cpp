#include "server/level/server_player.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

#include "core/component/codec.hpp"
#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "generated/packet_ids.hpp"
#include "network/connection.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/item/item_logic.hpp"

namespace mc {

Component ServerPlayer::displayName() const {
    Component name = Component::literal(profile().name);
    nbt::Compound click;
    click.put("action", std::string("suggest_command"));
    click.put("command", "/tell " + profile().name + " ");
    nbt::Compound hover;
    hover.put("action", std::string("show_entity"));
    hover.put("id", std::string("minecraft:player"));
    const UUID& u = profile().id;
    hover.put("uuid", std::vector<int32_t>{static_cast<int32_t>(u.most >> 32), static_cast<int32_t>(u.most), static_cast<int32_t>(u.least >> 32),
                                           static_cast<int32_t>(u.least)});
    hover.put("name", Component::literal(profile().name).toNbt());
    name.clickEvent = std::move(click);
    name.hoverEvent = std::move(hover);
    name.insertion = profile().name;
    return name;
}

void writeProfileProperties(ByteWriter& out, const GameProfile& profile) {
    out.writeVarInt(static_cast<int32_t>(profile.properties.size()));
    for (const auto& p : profile.properties) {
        out.writeUtf(p.name);
        out.writeUtf(p.value);
        out.writeBool(p.signature.has_value());
        if (p.signature) out.writeUtf(*p.signature);
    }
}

GameType gameTypeByName(std::string_view name, GameType fallback) {
    if (name == "survival" || name == "0") return GameType::Survival;
    if (name == "creative" || name == "1") return GameType::Creative;
    if (name == "adventure" || name == "2") return GameType::Adventure;
    if (name == "spectator" || name == "3") return GameType::Spectator;
    return fallback;
}

// ---------------------------------------------------------------- PlayerChunkSender

void PlayerChunkSender::sendNextChunks(ServerPlayer& player) {
    if (unacknowledgedBatches_ >= maxUnacknowledgedBatches_) return;
    float maxBatchSize = std::max(1.0f, desiredChunksPerTick_);
    batchQuota_ = std::min(batchQuota_ + desiredChunksPerTick_, maxBatchSize);
    if (batchQuota_ < 1.0f || pending_.empty()) return;

    ChunkPos center = player.chunkPosition();
    std::vector<ChunkPos> chunks;
    chunks.reserve(pending_.size());
    for (int64_t packed : pending_)
        chunks.push_back({static_cast<int32_t>(packed & 0xffffffff), static_cast<int32_t>(packed >> 32)});
    size_t count = std::min(chunks.size(), static_cast<size_t>(std::floor(batchQuota_)));
    std::partial_sort(chunks.begin(), chunks.begin() + static_cast<std::ptrdiff_t>(count), chunks.end(),
                      [&](ChunkPos a, ChunkPos b) { return center.distanceSquared(a) < center.distanceSquared(b); });
    chunks.resize(count);

    Connection& conn = player.connection();
    unacknowledgedBatches_++;
    conn.send(Packet(packets::play_cb::CHUNK_BATCH_START));
    ServerLevel& level = player.level();
    for (ChunkPos pos : chunks) {
        // Chunks are sent once lit; lighting one can change its neighbours' light, which players
        // who don't have this chunk yet hear about first (it's still on their border).
        LevelChunk& chunk = level.getLitChunk(pos);
        if (level.onLightWork) level.onLightWork();
        pending_.erase(pos.pack());
        Packet p(packets::play_cb::LEVEL_CHUNK_WITH_LIGHT);
        chunk.writeChunkWithLight(p, [&](ByteWriter& out) { level.writeLightData(out, pos); });
        conn.send(p);
    }
    Packet done(packets::play_cb::CHUNK_BATCH_FINISHED);
    done.writeVarInt(static_cast<int32_t>(chunks.size()));
    conn.send(done);
    batchQuota_ -= static_cast<float>(chunks.size());
}

void PlayerChunkSender::onBatchReceived(float desiredChunksPerTick) {
    unacknowledgedBatches_ = std::max(0, unacknowledgedBatches_ - 1);
    desiredChunksPerTick_ = std::isnan(desiredChunksPerTick) ? 0.01f : std::clamp(desiredChunksPerTick, 0.01f, 64.0f);
    if (unacknowledgedBatches_ == 0) batchQuota_ = 1.0f;
    maxUnacknowledgedBatches_ = 10;
}

// ---------------------------------------------------------------- ServerPlayer

ServerPlayer::ServerPlayer(int entityId, GameProfile profile, ServerLevel& level, Connection& connection, ClientInformation info)
    : clientInfo(std::move(info)), entityId_(entityId), profile_(std::move(profile)), level_(&level), connection_(&connection) {
    inventoryMenu = std::make_unique<InventoryMenu>(*this, inventory);
    containerMenu = inventoryMenu.get();
}

ServerPlayer::~ServerPlayer() = default;

void ServerPlayer::openMenu(std::unique_ptr<AbstractContainerMenu> menu, int menuType, const nbt::Tag& title) {
    if (containerMenu != inventoryMenu.get()) closeContainer();
    Packet p(packets::play_cb::OPEN_SCREEN);
    p.writeVarInt(menu->containerId);
    p.writeVarInt(menuType);
    nbt::writeNetwork(p, title);
    connection().send(p);
    menu->setSynchronizer(connection()); // initMenu
    openMenu_ = std::move(menu);
    containerMenu = openMenu_.get();
}

void ServerPlayer::closeContainer() {
    Packet p(packets::play_cb::CONTAINER_CLOSE);
    p.writeVarInt(containerMenu->containerId);
    connection().send(p);
    doCloseContainer();
}

void ServerPlayer::doCloseContainer() {
    containerMenu->removed(*this);
    if (containerMenu != inventoryMenu.get()) inventoryMenu->transferState(*containerMenu);
    containerMenu = inventoryMenu.get();
    openMenu_.reset();
}

bool ServerPlayer::isWithinBlockInteractionRange(BlockPos pos, double buffer) const {
    // Distance from the eyes to the block's bounding box, squared.
    auto axisDist = [](double v, int lo) { return v < lo ? lo - v : v > lo + 1 ? v - (lo + 1) : 0.0; };
    double dx = axisDist(x, pos.x), dy = axisDist(eyeY(), pos.y), dz = axisDist(z, pos.z);
    double range = blockInteractionRange() + buffer;
    return dx * dx + dy * dy + dz * dz < range * range;
}

ItemEntity* ServerPlayer::drop(ItemStack stack, bool thrownFromHand) {
    if (stack.isEmpty()) return nullptr;
    ServerLevel& lvl = level();
    auto& rng = lvl.random();
    std::uniform_real_distribution<float> f(0.0f, 1.0f);
    auto item = std::make_unique<ItemEntity>(lvl.nextEntityId(), UUID::random(), x, eyeY() - 0.3f, z, std::move(stack));
    item->pickupDelay = 40;
    if (thrownFromHand) item->thrower = profile_.id;
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    float sinX = std::sin(xRot * kDeg), cosX = std::cos(xRot * kDeg);
    float sinY = std::sin(yRot * kDeg), cosY = std::cos(yRot * kDeg);
    float dir = f(rng) * 6.2831855f;
    float pow2 = 0.02f * f(rng);
    item->vx = -sinY * cosX * 0.3f + std::cos(dir) * pow2;
    item->vy = -sinX * 0.3f + 0.1f + (f(rng) - f(rng)) * 0.1f;
    item->vz = cosY * cosX * 0.3f + std::sin(dir) * pow2;
    return lvl.addFreshEntity(std::move(item));
}

int ServerPlayer::effectiveViewDistance(int serverViewDistance) const {
    return std::clamp(clientInfo.viewDistance, 2, serverViewDistance);
}

namespace {
// ChunkTrackingView.isWithinDistance (without neighbours).
bool withinDistance(ChunkPos center, int viewDistance, int x, int z) {
    long dx = std::max(0, std::abs(x - center.x) - 1);
    long dz = std::max(0, std::abs(z - center.z) - 1);
    return dx * dx + dz * dz < static_cast<long>(viewDistance) * viewDistance;
}
} // namespace

void ServerPlayer::updateChunkTracking(int serverViewDistance) {
    ChunkPos center = chunkPosition();
    int view = effectiveViewDistance(serverViewDistance);
    if (trackingStarted_ && center == lastCenter_ && view == lastViewDistance_) return;

    if (!trackingStarted_ || !(center == lastCenter_)) {
        Packet p(packets::play_cb::SET_CHUNK_CACHE_CENTER);
        p.writeVarInt(center.x);
        p.writeVarInt(center.z);
        connection_->send(p);
    }

    // Drop chunks that left the view.
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        ChunkPos pos{static_cast<int32_t>(*it & 0xffffffff), static_cast<int32_t>(*it >> 32)};
        if (withinDistance(center, view, pos.x, pos.z)) {
            ++it;
            continue;
        }
        if (!chunkSender_.dropPending(pos)) {
            Packet p(packets::play_cb::FORGET_LEVEL_CHUNK);
            p.writeLong(pos.pack());
            connection_->send(p);
        }
        it = tracked_.erase(it);
    }
    // Start tracking chunks that entered the view.
    for (int x = center.x - view - 1; x <= center.x + view + 1; x++) {
        for (int z = center.z - view - 1; z <= center.z + view + 1; z++) {
            if (!withinDistance(center, view, x, z)) continue;
            ChunkPos pos{x, z};
            if (tracked_.insert(pos.pack()).second) chunkSender_.markPending(pos);
        }
    }
    trackingStarted_ = true;
    lastCenter_ = center;
    lastViewDistance_ = view;
}

bool ServerPlayer::consumeMoved() {
    bool moved = x != sentX_ || y != sentY_ || z != sentZ_ || yRot != sentYRot_ || xRot != sentXRot_;
    sentX_ = x, sentY_ = y, sentZ_ = z, sentYRot_ = yRot, sentXRot_ = xRot;
    return moved;
}

} // namespace mc

// ---------------------------------------------------------------- mob effects and attributes

namespace mc {

void ServerPlayer::addEffect(int effect, EffectInstance e) {
    effects[effect] = e;
    // ClientboundUpdateMobEffectPacket
    Packet p(packets::play_cb::UPDATE_MOB_EFFECT);
    p.writeVarInt(id());
    p.writeVarInt(effect);
    p.writeVarInt(e.amplifier);
    p.writeVarInt(e.duration);
    p.writeByte(static_cast<uint8_t>((e.ambient ? 1 : 0) | (e.visible ? 2 : 0) | (e.showIcon ? 4 : 0)));
    connection().send(p);
}

void ServerPlayer::removeEffect(int effect) {
    if (!effects.erase(effect)) return;
    Packet p(packets::play_cb::REMOVE_MOB_EFFECT);
    p.writeVarInt(id());
    p.writeVarInt(effect);
    connection().send(p);
}

int ServerPlayer::xpNeededForNextLevel() const {
    if (experienceLevel >= 30) return 112 + (experienceLevel - 30) * 9;
    return experienceLevel >= 15 ? 37 + (experienceLevel - 15) * 5 : 7 + experienceLevel * 2;
}

void ServerPlayer::giveExperiencePoints(int amount) {
    if (amount == 0) return;
    score += amount; // Player.increaseScore
    experienceProgress += static_cast<float>(amount) / static_cast<float>(xpNeededForNextLevel());
    totalExperience = static_cast<int>(std::clamp<int64_t>(int64_t{totalExperience} + amount, 0, INT32_MAX));
    while (experienceProgress < 0.0f) {
        float remaining = experienceProgress * static_cast<float>(xpNeededForNextLevel());
        if (experienceLevel > 0) {
            giveExperienceLevels(-1);
            experienceProgress = 1.0f + remaining / static_cast<float>(xpNeededForNextLevel());
        } else {
            giveExperienceLevels(-1);
            experienceProgress = 0.0f;
        }
    }
    while (experienceProgress >= 1.0f) {
        experienceProgress = (experienceProgress - 1.0f) * static_cast<float>(xpNeededForNextLevel());
        giveExperienceLevels(1);
        experienceProgress /= static_cast<float>(xpNeededForNextLevel());
    }
    lastSentExp = -1;
}

void ServerPlayer::giveExperienceLevels(int amount) {
    if (amount == 0) return;
    experienceLevel = static_cast<int>(std::clamp<int64_t>(int64_t{experienceLevel} + amount, INT32_MIN, INT32_MAX));
    if (experienceLevel < 0) {
        experienceLevel = 0;
        experienceProgress = 0.0f;
        totalExperience = 0;
    }
    if (amount > 0 && experienceLevel % 5 == 0 && static_cast<float>(lastLevelUpTime) < static_cast<float>(gameTicks) - 100.0f) {
        float volume = experienceLevel > 30 ? 1.0f : static_cast<float>(experienceLevel) / 30.0f;
        static const int32_t kLevelUp = registries::id("minecraft:sound_event", "minecraft:entity.player.levelup").value_or(-1);
        level().playSound(-1, x, y, z, kLevelUp, 7 /* SoundSource.PLAYERS */, volume * 0.75f, 1.0f);
        lastLevelUpTime = static_cast<int64_t>(gameTicks);
    }
    lastSentExp = -1;
}

void ServerPlayer::tickExperience() {
    if (takeXpDelay > 0) takeXpDelay--;
    if (totalExperience != lastSentExp) {
        lastSentExp = totalExperience;
        Packet p(packets::play_cb::SET_EXPERIENCE);
        p.writeFloat(experienceProgress);
        p.writeVarInt(experienceLevel);
        p.writeVarInt(totalExperience);
        connection().send(p);
    }
}

void ServerPlayer::tickEffects() {
    std::vector<int> expired;
    for (auto& [effect, e] : effects) {
        if (e.duration == -1) continue; // infinite
        if (--e.duration <= 0) expired.push_back(effect);
    }
    for (int effect : expired) removeEffect(effect);
}

std::vector<attributes::Modifier> ServerPlayer::attributeModifiers(int attribute) {
    std::vector<attributes::Modifier> out;
    auto add = [&](attributes::Modifier m) { // AttributeInstance: one modifier per id
        for (auto& existing : out)
            if (existing.id == m.id) {
                existing = std::move(m);
                return;
            }
        out.push_back(std::move(m));
    };
    std::string_view attrName = gen::kAttributes[attribute].name;
    for (EquipmentSlot slot : {EquipmentSlot::MainHand, EquipmentSlot::OffHand, EquipmentSlot::Feet, EquipmentSlot::Legs,
                               EquipmentSlot::Chest, EquipmentSlot::Head}) {
        const ItemStack& stack = inventory.equipment(slot);
        if (stack.isEmpty() || (items::maxDamage(stack) > 0 && items::damage(stack) >= items::maxDamage(stack))) continue; // isBroken
        // ItemStack.forEachModifier: the attribute_modifiers component (patched or default)...
        bool removed = false;
        for (const auto& c : stack.components)
            if (c.removed && registries::id("minecraft:data_component_type", "minecraft:attribute_modifiers") == c.type) removed = true;
        if (auto patched = items::componentValue(stack, "minecraft:attribute_modifiers")) {
            try {
                for (const auto& e : codec::listItems(*patched)) {
                    const nbt::Compound* m = e.asCompound();
                    if (!m || m->getString("type") != attrName || !attributes::slotGroupMatches(m->getString("slot", "any"), slot)) continue;
                    std::string op = m->getString("operation");
                    add({m->getString("id"), m->getDouble("amount"), op == "add_multiplied_base" ? 1 : op == "add_multiplied_total" ? 2 : 0});
                }
            } catch (const std::exception&) {
            }
        } else if (!removed) {
            for (const auto& m : stack.info().attributes)
                if (m.attribute == attribute && attributes::slotGroupMatches(m.slot, slot)) add({std::string(m.id), m.amount, m.operation});
        }
        // ...and its enchantments' attribute effects.
        enchantments::forEachModifier(stack, slot, [&](std::string_view attr, std::string id, double amount, int op) {
            if (attr == attrName) add({std::move(id), amount, op});
        });
    }
    // MobEffect.addAttributeModifiers: amount * (amplifier + 1).
    for (const auto& [effect, e] : effects)
        for (const auto& m : gen::kEffectModifiers[effect])
            if (m.attribute == attribute) add({std::string(m.id), m.amount * (e.amplifier + 1), m.operation});
    return out;
}

double ServerPlayer::attributeValue(int attribute) {
    return attributes::calculate(attribute, attributes::playerBase(attribute), attributeModifiers(attribute));
}

void ServerPlayer::syncAttributes(bool all) {
    Packet p(packets::play_cb::UPDATE_ATTRIBUTES);
    p.writeVarInt(id());
    ByteWriter body;
    int count = 0;
    for (size_t a = 0; a < gen::kAttributes.size(); a++) {
        const auto& info = gen::kAttributes[a];
        if (!info.syncable || !info.player) continue;
        auto mods = attributeModifiers(static_cast<int>(a));
        auto it = sentAttributes.find(static_cast<int>(a));
        if (!all && ((it == sentAttributes.end() && mods.empty()) || (it != sentAttributes.end() && it->second == mods))) continue;
        // AttributeSnapshot: attribute, base value, modifiers (id, amount, operation)
        body.writeVarInt(static_cast<int32_t>(a));
        body.writeDouble(attributes::playerBase(static_cast<int>(a)));
        body.writeVarInt(static_cast<int32_t>(mods.size()));
        for (const auto& m : mods) {
            body.writeUtf(m.id);
            body.writeDouble(m.amount);
            body.writeVarInt(m.operation);
        }
        sentAttributes[static_cast<int>(a)] = std::move(mods);
        count++;
    }
    if (count == 0) return;
    p.writeVarInt(count);
    p.writeBytes(body.data());
    connection().send(p);
}

} // namespace mc
