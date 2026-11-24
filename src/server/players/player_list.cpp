#include "server/players/player_list.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/registries.hpp"
#include "generated/packet_ids.hpp"
#include "server/commands/commands.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "server/players/player_data.hpp"
#include "util/log.hpp"
#include "world/entity/experience_orb.hpp"
#include "world/item/crafting/recipes.hpp"
#include "world/level/block/entity/block_entity_components.hpp"
#include "world/level/block/redstone_blocks.hpp"

namespace mc {

namespace pk = packets::play_cb;

namespace {
int8_t packDegrees(float angle) { return static_cast<int8_t>(static_cast<int>(std::floor(angle * 256.0f / 360.0f))); }

int difficultyId(std::string_view name) {
    if (name == "peaceful") return 0;
    if (name == "easy") return 1;
    if (name == "normal") return 2;
    if (name == "hard") return 3;
    return 1;
}
} // namespace

ServerPlayer* PlayerList::getPlayer(const UUID& id) const {
    for (const auto& p : players_)
        if (p->profile().id == id) return p.get();
    return nullptr;
}

ServerPlayer* PlayerList::getPlayerByName(std::string_view name) const {
    for (const auto& p : players_) {
        const auto& n = p->profile().name;
        if (n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
            return p.get();
    }
    return nullptr;
}

std::optional<Component> PlayerList::canPlayerLogin(const GameProfile& profile) const {
    if (playerCount() >= server_.properties().maxPlayers && !getPlayer(profile.id))
        return Component::translatable("multiplayer.disconnect.server_full");
    return std::nullopt;
}

void PlayerList::broadcastChatMessage(const chat::PlayerChatMessage& message, ServerPlayer* sender, const chat::ChatTypeBound& chatType) {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    bool trusted = message.signature && !message.hasExpiredServer(now); // verifyChatTrusted
    std::string decorated = chatType.decorate(message.decoratedContent()).getString(); // MinecraftServer.logChatMessage
    if (trusted) log::info("{}", decorated);
    else log::info("[Not Secure] {}", decorated);
    for (auto& p : players_) { // OutgoingChatMessage: disguised from the server, else player chat
        if (!p->game) continue;
        if (message.isSystem()) p->game->sendDisguisedChatMessage(message.decoratedContent(), chatType);
        else p->game->sendPlayerChatMessage(message, chatType);
    }
    (void)sender; // (text filtering would report fully filtered messages back to it)
}

void PlayerList::broadcastChatSession(ServerPlayer& player) {
    Packet p(pk::PLAYER_INFO_UPDATE);
    p.writeByte(1 << 1); // INITIALIZE_CHAT
    p.writeVarInt(1);
    p.writeUUID(player.profile().id);
    p.writeBool(player.chatSession.has_value());
    if (player.chatSession) player.chatSession->write(p);
    broadcastAll(p);
}

bool PlayerList::disconnectAllPlayersWithProfile(const UUID& id) {
    std::vector<ServerPlayer*> dupes;
    for (const auto& p : players_)
        if (p->profile().id == id) dupes.push_back(p.get());
    for (auto* p : dupes) p->connection().disconnect(Component::translatable("multiplayer.disconnect.duplicate_login"));
    return !dupes.empty();
}

void PlayerList::sendPlayerInfo(Connection& to, const std::vector<ServerPlayer*>& entries) {
    // All eight ClientboundPlayerInfoUpdatePacket actions, written in enum order.
    Packet p(pk::PLAYER_INFO_UPDATE);
    p.writeByte(0xff);
    p.writeVarInt(static_cast<int32_t>(entries.size()));
    for (auto* e : entries) {
        p.writeUUID(e->profile().id);
        p.writeUtf(e->profile().name); // ADD_PLAYER
        writeProfileProperties(p, e->profile());
        p.writeBool(e->chatSession.has_value()); // INITIALIZE_CHAT
        if (e->chatSession) e->chatSession->write(p);
        p.writeVarInt(static_cast<int>(e->gameMode)); // UPDATE_GAME_MODE
        p.writeBool(true);             // UPDATE_LISTED
        p.writeVarInt(e->latencyMs);   // UPDATE_LATENCY
        p.writeBool(false);            // UPDATE_DISPLAY_NAME
        p.writeVarInt(0);              // UPDATE_LIST_ORDER
        p.writeBool(true);             // UPDATE_HAT
    }
    to.send(p);
}

void PlayerList::sendAddEntity(Connection& to, const ServerPlayer& e) {
    static const int playerType = registries::id("minecraft:entity_type", "minecraft:player").value_or(0);
    Packet p(pk::ADD_ENTITY);
    p.writeVarInt(e.id());
    p.writeUUID(e.profile().id);
    p.writeVarInt(playerType);
    p.writeDouble(e.x);
    p.writeDouble(e.y);
    p.writeDouble(e.z);
    p.writeByte(0); // LpVec3 zero movement
    p.writeByte(static_cast<uint8_t>(packDegrees(e.xRot)));
    p.writeByte(static_cast<uint8_t>(packDegrees(e.yRot)));
    p.writeByte(static_cast<uint8_t>(packDegrees(e.yRot)));
    p.writeVarInt(0);
    to.send(p);
}

void PlayerList::loadOps() {
    try {
        ops_.load();
    } catch (const std::exception& e) {
        log::warn("Failed to load operators list: {}", e.what());
    }
}

void PlayerList::op(const NameAndId& user) {
    const ServerOpList::Entry* old = ops_.get(user.id);
    ops_.add({user, server_.properties().opPermissionLevel, old && old->bypassesPlayerLimit});
    if (ServerPlayer* p = getPlayer(user.id)) sendPlayerPermissionLevel(*p);
}

void PlayerList::deop(const NameAndId& user) {
    if (!ops_.remove(user.id)) return;
    if (ServerPlayer* p = getPlayer(user.id)) sendPlayerPermissionLevel(*p);
}

void PlayerList::sendPlayerPermissionLevel(ServerPlayer& player) {
    // ClientboundEntityEventPacket: 24 + the level (PERMISSION_LEVEL_ALL ... OWNERS)
    Packet ev(pk::ENTITY_EVENT);
    ev.writeInt(player.id());
    ev.writeByte(static_cast<uint8_t>(24 + server_.getProfilePermissions(player.profile().id)));
    player.connection().send(ev);
    server_.commands().sendCommands(player);
}

void PlayerList::placeNewPlayer(Connection& conn, GameProfile profile, ClientInformation info) {
    server_.nameToIdCache().add({profile.id, profile.name});
    ServerLevel& level = server_.overworld();
    const auto& props = server_.properties();
    auto owned = std::make_unique<ServerPlayer>(server_.nextEntityId(), std::move(profile), level, conn, std::move(info));
    ServerPlayer& player = *owned;
    const auto& spawn = level.spawn();
    player.x = spawn.x + 0.5;
    player.y = spawn.y;
    player.z = spawn.z + 0.5;
    player.gameMode = server_.defaultGameType();
    if (auto* storage = server_.storage()) {
        if (auto tag = storage->loadPlayer(player.profile().id)) player_data::load(player, *tag);
    }

    auto listener = std::make_unique<ServerGamePacketListener>(server_, conn, player);
    auto* game = listener.get();
    conn.setListener(std::move(listener));

    // ClientboundLoginPacket
    Packet login(pk::LOGIN);
    login.writeInt(player.id());
    login.writeBool(props.hardcore);
    login.writeVarInt(1);
    login.writeUtf(level.dimension());
    login.writeVarInt(props.maxPlayers);
    login.writeVarInt(props.viewDistance);
    login.writeVarInt(props.simulationDistance);
    const GameRules& rules = server_.gameRules();
    login.writeBool(rules.getBool(GameRules::ReducedDebugInfo));
    login.writeBool(!rules.getBool(GameRules::ImmediateRespawn)); // show death screen
    login.writeBool(rules.getBool(GameRules::LimitedCrafting));
    // CommonPlayerSpawnInfo
    login.writeVarInt(registries::id("minecraft:dimension_type", level.dimensionType()).value_or(0));
    login.writeUtf(level.dimension());
    login.writeLong(level.obfuscatedSeed());
    login.writeVarInt(static_cast<int>(player.gameMode));
    login.writeVarInt(0);    // previous game type: none
    login.writeBool(false);  // debug world
    login.writeBool(level.isFlat());
    login.writeBool(false);  // last death location
    login.writeVarInt(0);    // portal cooldown
    login.writeVarInt(level.seaLevel());
    login.writeBool(server_.usesAuthentication()); // online mode
    login.writeBool(server_.enforceSecureProfile()); // enforces secure chat
    conn.send(login);

    Packet difficulty(pk::CHANGE_DIFFICULTY);
    difficulty.writeVarInt(difficultyId(props.difficulty));
    difficulty.writeBool(false);
    conn.send(difficulty);

    game->sendAbilities();

    Packet held(pk::SET_HELD_SLOT);
    held.writeVarInt(player.inventory.selected());
    conn.send(held);

    Packet recipes(pk::UPDATE_RECIPES);
    recipes::writeUpdateRecipes(recipes);
    conn.send(recipes);
    sendPlayerPermissionLevel(player);

    game->teleport(player.x, player.y, player.z, player.yRot, player.xRot);

    players_.push_back(std::move(owned));
    std::vector<ServerPlayer*> all;
    for (auto& p : players_) all.push_back(p.get());
    sendPlayerInfo(conn, all);
    for (auto& other : players_) {
        if (other.get() == &player) continue;
        sendPlayerInfo(other->connection(), {&player});
        sendAddEntity(other->connection(), player);
        sendAddEntity(conn, *other);
        sendEquipment(conn, *other);
    }
    player.sentEquipment = {};

    Packet spawnPos(pk::SET_DEFAULT_SPAWN_POSITION);
    spawnPos.writeUtf(level.dimension());
    spawnPos.writeBlockPos(spawn.x, spawn.y, spawn.z);
    spawnPos.writeFloat(spawn.yaw);
    spawnPos.writeFloat(spawn.pitch);
    conn.send(spawnPos);

    sendTimeSync(conn, true);
    player.inventoryMenu->setSynchronizer(conn); // ServerPlayer.initInventoryMenu
    // ServerPlayer.initInventoryMenu / onEnterCombat...: attributes and active effects.
    player.syncAttributes(true);
    for (auto [effect, e] : std::map(player.effects)) player.addEffect(effect, e);

    Packet waitForChunks(pk::GAME_EVENT);
    waitForChunks.writeByte(13); // LEVEL_CHUNKS_LOAD_START
    waitForChunks.writeFloat(0);
    conn.send(waitForChunks);

    player.updateChunkTracking(props.viewDistance);

    log::info("{}[/{}] logged in with entity id {} at ({:.1f}, {:.1f}, {:.1f})", player.profile().name, conn.remoteAddress(),
              player.id(), player.x, player.y, player.z);
    broadcastSystemMessage(Component::translatable("multiplayer.player.joined", {Component::literal(player.profile().name)})
                               .withColor("yellow"));
}

void PlayerList::remove(ServerPlayer& player) {
    UUID id = player.profile().id;
    if (player.sleepingPos && player.game) player.game->stopSleepInBed(true, false); // ServerPlayer.disconnect
    save(player);
    // Player.remove: the crafting grid and an open menu are emptied into the world.
    player.disconnected = true;
    player.inventoryMenu->removed(player);
    if (player.containerMenu != player.inventoryMenu.get()) player.doCloseContainer();
    int entityId = player.id();
    std::string name = player.profile().name;
    std::erase_if(players_, [&](const auto& p) { return p.get() == &player; });
    updateSleepingPlayerList(); // (onTrackingEnd)

    Packet removeInfo(pk::PLAYER_INFO_REMOVE);
    removeInfo.writeVarInt(1);
    removeInfo.writeUUID(id);
    Packet removeEntity(pk::REMOVE_ENTITIES);
    removeEntity.writeVarInt(1);
    removeEntity.writeVarInt(entityId);
    broadcastAll(removeEntity);
    broadcastAll(removeInfo);
    broadcastSystemMessage(Component::translatable("multiplayer.player.left", {Component::literal(name)}).withColor("yellow"));
}

void PlayerList::broadcastAll(const Packet& packet, const ServerPlayer* except) {
    for (auto& p : players_)
        if (p.get() != except) p->connection().send(packet);
}

void PlayerList::broadcastSystemMessage(const Component& message) {
    log::info("{}", message.getString());
    Packet p(pk::SYSTEM_CHAT);
    nbt::writeNetwork(p, message.toNbt());
    p.writeBool(false);
    broadcastAll(p);
}

void PlayerList::save(ServerPlayer& player) {
    if (auto* storage = server_.storage()) {
        if (!storage->savePlayer(player.profile().id, player_data::save(player)))
            log::error("Failed to save player data for {}", player.profile().name);
    }
}

void PlayerList::saveAll() {
    for (auto& p : players_) save(*p);
}

void PlayerList::sendTimeSync(Connection& to, bool fullClocks) {
    // ClientboundSetTimePacket: game time plus clock states (ServerClockManager.createFullSyncPacket).
    auto& clocks = server_.clockManager();
    bool advance = server_.advanceTime();
    Packet p(pk::SET_TIME);
    p.writeLong(clocks.gameTime());
    if (!fullClocks) {
        p.writeVarInt(0);
    } else {
        p.writeVarInt(clocks.count());
        for (int i = 0; i < clocks.count(); i++) {
            const auto& c = clocks.clock(i);
            p.writeVarInt(i); // Holder<WorldClock> registry id
            p.writeVarLong(c.totalTicks);
            p.writeFloat(c.partialTick);
            p.writeFloat(c.paused || !advance ? 0.0f : c.rate);
        }
    }
    to.send(p);
}

void PlayerList::broadcastTime(bool fullClocks) {
    for (auto& p : players_) sendTimeSync(p->connection(), fullClocks);
}

void PlayerList::broadcastPlayerData(ServerPlayer& player) {
    Packet p(pk::SET_ENTITY_DATA);
    p.writeVarInt(player.id());
    p.writeByte(6);     // Entity.DATA_POSE
    p.writeVarInt(20);  // EntityDataSerializers.POSE
    p.writeVarInt(player.sleepingPos ? 2 : 0); // SLEEPING / STANDING
    p.writeByte(14);    // LivingEntity.SLEEPING_POS_ID
    p.writeVarInt(11);  // EntityDataSerializers.OPTIONAL_BLOCK_POS
    p.writeBool(player.sleepingPos.has_value());
    if (player.sleepingPos) p.writeBlockPos(player.sleepingPos->x, player.sleepingPos->y, player.sleepingPos->z);
    p.writeByte(0xff);
    for (auto& other : players_) other->connection().send(p);
}

namespace {
// SleepStatus.sleepersNeeded with players_sleeping_percentage 100.
int sleepersNeeded(int active) { return std::max(1, static_cast<int>(std::ceil(active * 100 / 100.0f))); }
} // namespace

void PlayerList::updateSleepingPlayerList() {
    // SleepStatus.update
    int oldActive = activePlayers_, oldSleeping = sleepingPlayers_;
    activePlayers_ = sleepingPlayers_ = 0;
    for (auto& p : players_) {
        if (p->gameMode == GameType::Spectator) continue;
        activePlayers_++;
        if (p->sleepingPos) sleepingPlayers_++;
    }
    bool changed = (oldSleeping > 0 || sleepingPlayers_ > 0) && (oldActive != activePlayers_ || oldSleeping != sleepingPlayers_);
    if (players_.empty() || !changed) return;
    // ServerLevel.announceSleepStatus
    Component message = sleepingPlayers_ >= sleepersNeeded(activePlayers_)
                            ? Component::translatable("sleep.skipping_night")
                            : Component::translatable("sleep.players_sleeping", {Component::literal(std::to_string(sleepingPlayers_)),
                                                                                 Component::literal(std::to_string(sleepersNeeded(activePlayers_)))});
    for (auto& p : players_)
        if (p->game) p->game->sendOverlay(message);
}

void PlayerList::broadcastLightChanges() {
    // ChunkHolder.broadcastChanges: changed light sections go to the players that have the chunk
    // on the border of what they see (inside, clients work light out themselves).
    ServerLevel& level = server_.overworld();
    for (const auto& change : level.takeLightChanges()) {
        std::optional<Packet> packet;
        for (auto& p : players_) {
            if (!p->hasChunk(change.pos)) continue;
            bool border = false; // ChunkMap.isChunkOnTrackedBorder
            for (int dx = -1; dx <= 1 && !border; dx++)
                for (int dz = -1; dz <= 1 && !border; dz++)
                    if ((dx != 0 || dz != 0) && !p->hasChunk({change.pos.x + dx, change.pos.z + dz})) border = true;
            if (!border) continue;
            if (!packet) {
                packet.emplace(pk::LIGHT_UPDATE);
                packet->writeVarInt(change.pos.x);
                packet->writeVarInt(change.pos.z);
                level.writeLightData(*packet, change.pos, &change.sky, &change.block);
            }
            p->connection().send(*packet);
        }
    }
}

void PlayerList::broadcastBlockChanges() {
    ServerLevel& level = server_.overworld();
    broadcastLightChanges();
    for (BlockPos pos : std::exchange(level.transientBlocks, {})) {
        Packet p(pk::ADD_TRANSIENT_BLOCK);
        p.writeBlockPos(pos.x, pos.y, pos.z);
        p.writeVarInt(static_cast<int32_t>(level.getBlockState(pos)));
        ChunkPos chunk{pos.x >> 4, pos.z >> 4};
        for (auto& pl : players_)
            if (pl->hasChunk(chunk)) pl->connection().send(p);
    }
    for (const auto& change : level.takeChanges()) {
        std::optional<Packet> packet;
        if (change.positions.size() == 1) {
            BlockPos pos = change.positions[0];
            packet.emplace(pk::BLOCK_UPDATE);
            packet->writeBlockPos(pos.x, pos.y, pos.z);
            packet->writeVarInt(static_cast<int32_t>(level.getBlockState(pos)));
        } else {
            packet.emplace(pk::SECTION_BLOCKS_UPDATE);
            packet->writeLong(change.sectionPos);
            packet->writeVarInt(static_cast<int32_t>(change.positions.size()));
            for (BlockPos pos : change.positions) {
                int64_t local = (pos.x & 15) << 8 | (pos.z & 15) << 4 | (pos.y & 15);
                packet->writeVarLong(static_cast<int64_t>(level.getBlockState(pos)) << 12 | local);
            }
        }
        for (auto& p : players_)
            if (p->hasChunk(change.chunk)) p->connection().send(*packet);
        // ChunkHolder.broadcastBlockEntityIfNeeded: block entities that override getUpdatePacket.
        LevelChunk& chunk = level.getChunk(change.chunk);
        for (BlockPos pos : change.positions) {
            auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z);
            if (!be || blocks::blockOf(level.getBlockState(pos)) != be->blockAtLoad) continue;
            std::string type = be->tag.getString("id");
            if (!block_entity::sendsUpdates(type)) continue;
            Packet data(pk::BLOCK_ENTITY_DATA);
            data.writeBlockPos(pos.x, pos.y, pos.z);
            data.writeVarInt(registries::id("minecraft:block_entity_type", type).value_or(0));
            nbt::writeNetwork(data, nbt::Tag(block_entity::getUpdateTag(be->tag)));
            for (auto& p : players_)
                if (p->hasChunk(change.chunk)) p->connection().send(data);
        }
    }
}

namespace {
constexpr EquipmentSlot kEquipmentSlots[] = {EquipmentSlot::MainHand, EquipmentSlot::OffHand, EquipmentSlot::Feet,
                                             EquipmentSlot::Legs, EquipmentSlot::Chest, EquipmentSlot::Head};

void writeEquipment(Packet& p, int entityId, const std::vector<std::pair<EquipmentSlot, ItemStack>>& slots) {
    p.writeVarInt(entityId);
    for (size_t i = 0; i < slots.size(); i++) {
        int id = static_cast<int>(slots[i].first);
        p.writeByte(static_cast<uint8_t>(i + 1 < slots.size() ? (id | 0x80) : id));
        slots[i].second.write(p);
    }
}
} // namespace

void PlayerList::sendEquipment(Connection& to, const ServerPlayer& p) {
    std::vector<std::pair<EquipmentSlot, ItemStack>> slots;
    for (EquipmentSlot s : kEquipmentSlots) {
        const ItemStack& item = const_cast<ServerPlayer&>(p).inventory.equipment(s);
        if (!item.isEmpty()) slots.emplace_back(s, item);
    }
    if (slots.empty()) return;
    Packet packet(pk::SET_EQUIPMENT);
    writeEquipment(packet, p.id(), slots);
    to.send(packet);
}

void PlayerList::broadcastEquipment(ServerPlayer& p) {
    // LivingEntity.detectEquipmentUpdates -> ClientboundSetEquipmentPacket for changed slots.
    std::vector<std::pair<EquipmentSlot, ItemStack>> changed;
    for (EquipmentSlot s : kEquipmentSlots) {
        ItemStack& now = p.inventory.equipment(s);
        ItemStack& sent = p.sentEquipment[static_cast<int>(s)];
        if (ItemStack::matches(now, sent)) continue;
        sent = now;
        changed.emplace_back(s, now);
    }
    if (changed.empty()) return;
    Packet packet(pk::SET_EQUIPMENT);
    writeEquipment(packet, p.id(), changed);
    broadcastAll(packet, &p);
}

void PlayerList::sendEntitySpawn(Connection& to, const Entity& e) {
    Packet add(pk::ADD_ENTITY);
    add.writeVarInt(e.id());
    add.writeUUID(e.uuid());
    add.writeVarInt(registries::id("minecraft:entity_type", e.type()).value_or(0));
    add.writeDouble(e.x);
    add.writeDouble(e.y);
    add.writeDouble(e.z);
    add.writeLpVec3(e.vx, e.vy, e.vz);
    add.writeByte(static_cast<uint8_t>(static_cast<int>(std::floor(e.xRot * 256.0f / 360.0f)))); // Mth.packDegrees
    add.writeByte(static_cast<uint8_t>(static_cast<int>(std::floor(e.yRot * 256.0f / 360.0f))));
    add.writeByte(0); // head
    add.writeVarInt(e.spawnData());
    to.send(add);
    sendEntityData(to, e);
}

void PlayerList::sendEntityData(Connection& to, const Entity& e) {
    Packet data(pk::SET_ENTITY_DATA);
    data.writeVarInt(e.id());
    if (!e.writeEntityData(data)) return;
    data.writeByte(0xff);
    to.send(data);
}

void PlayerList::touchItems(ServerPlayer& player) {
    // Player.aiStep -> touch(entity) -> ItemEntity / ExperienceOrb.playerTouch
    if (player.gameMode == GameType::Spectator) return;
    ServerLevel& level = player.level();
    for (ExperienceOrb* orb : level.entitiesIn<ExperienceOrb>(player.boundingBox().inflate(1.0, 0.5, 1.0))) {
        if (orb->removed || player.takeXpDelay != 0) continue;
        player.takeXpDelay = 2;
        Packet take(pk::TAKE_ITEM_ENTITY); // Player.take
        take.writeVarInt(orb->id());
        take.writeVarInt(player.id());
        take.writeVarInt(1);
        for (auto& p : players_)
            if (p.get() == &player || p->seenEntities.contains(orb->id())) p->connection().send(take);
        orb->playerTouch(level, player);
    }
    for (ItemEntity* item : level.itemsIn(player.boundingBox().inflate(1.0, 0.5, 1.0))) {
        if (item->removed || item->pickupDelay != 0 || (item->target && !(*item->target == player.profile().id))) continue;
        int before = item->item.count;
        ItemStack stack = item->item;
        player.inventory.add(stack);
        int taken = before - (stack.isEmpty() ? 0 : stack.count);
        if (taken <= 0) continue;
        // ClientboundTakeItemEntityPacket to everyone tracking the item, and the collector.
        Packet take(pk::TAKE_ITEM_ENTITY);
        take.writeVarInt(item->id());
        take.writeVarInt(player.id());
        take.writeVarInt(taken);
        for (auto& p : players_)
            if (p.get() == &player || p->seenEntities.contains(item->id())) p->connection().send(take);
        if (stack.isEmpty()) {
            item->removed = true;
        } else {
            item->item = stack;
            item->dataChanged = true;
        }
    }
}

bool PlayerList::inTrackingRange(const ServerPlayer& p, const Entity& e) const {
    int range = std::min(e.trackingRange(), server_.properties().viewDistance) * 16; // EntityType.clientTrackingRange
    return p.hasChunk(e.chunkPos()) && std::abs(e.x - p.x) <= range && std::abs(e.z - p.z) <= range;
}

void PlayerList::trackNewEntity(Entity& e) {
    for (auto& p : players_) {
        if (e.removed || p->seenEntities.contains(e.id()) || !inTrackingRange(*p, e)) continue;
        sendEntitySpawn(p->connection(), e);
        p->seenEntities.insert(e.id());
    }
}

void PlayerList::trackEntities() {
    ServerLevel& level = server_.overworld();
    std::vector<int> removed = level.takeRemovedEntities();
    for (auto& p : players_) {
        std::vector<int> gone;
        for (int id : removed)
            if (p->seenEntities.erase(id)) gone.push_back(id);
        for (const auto& e : level.entities()) {
            if (e->removed) continue;
            bool inRange = inTrackingRange(*p, *e);
            bool seen = p->seenEntities.contains(e->id());
            if (inRange && !seen) {
                sendEntitySpawn(p->connection(), *e);
                p->seenEntities.insert(e->id());
            } else if (!inRange && seen) {
                p->seenEntities.erase(e->id());
                gone.push_back(e->id());
            }
        }
        if (!gone.empty()) {
            Packet rm(pk::REMOVE_ENTITIES);
            rm.writeVarInt(static_cast<int32_t>(gone.size()));
            for (int id : gone) rm.writeVarInt(id);
            p->connection().send(rm);
        }
    }
    // ServerEntity.sendChanges (updateInterval 20; velocity when it changed a lot).
    for (const auto& ent : level.entities()) {
        Entity* item = ent.get();
        if (item->removed) continue;
        std::vector<ServerPlayer*> trackers;
        for (auto& p : players_)
            if (p->seenEntities.contains(item->id())) trackers.push_back(p.get());
        if (item->dataChanged) {
            for (auto* t : trackers) sendEntityData(t->connection(), *item);
            item->dataChanged = false;
        }
        bool update = item->tickCount % 20 == 0 || item->needsSync;
        if (!update) continue;
        // ServerEntity: ignore movement below 1/4096 of a block (7.6e-6 squared).
        double mx = item->x - item->sentX, my = item->y - item->sentY, mz = item->z - item->sentZ;
        bool moved = mx * mx + my * my + mz * mz >= 7.62939453125e-6;
        if (moved) {
            Packet sync(pk::ENTITY_POSITION_SYNC);
            sync.writeVarInt(item->id());
            sync.writeVarInt(0); // PositionPath.Linear
            sync.writeDouble(item->x);
            sync.writeDouble(item->y);
            sync.writeDouble(item->z);
            sync.writeFloat(0);
            sync.writeFloat(0);
            sync.writeBool(item->onGround);
            for (auto* t : trackers) t->connection().send(sync);
            item->sentX = item->x, item->sentY = item->y, item->sentZ = item->z;
        }
        if (item->needsSync || moved) {
            Packet motion(pk::SET_ENTITY_MOTION);
            motion.writeVarInt(item->id());
            motion.writeLpVec3(item->vx, item->vy, item->vz);
            for (auto* t : trackers) t->connection().send(motion);
        }
        item->needsSync = false;
    }
}

void PlayerList::tick() {
    int viewDistance = server_.properties().viewDistance;
    broadcastBlockChanges();
    for (const auto& ev : server_.overworld().takeLevelEvents()) {
        Packet p(pk::LEVEL_EVENT);
        p.writeInt(ev.type);
        p.writeBlockPos(ev.pos.x, ev.pos.y, ev.pos.z);
        p.writeInt(ev.data);
        p.writeBool(false);
        for (auto& pl : players_) {
            if (pl->id() == ev.excludeEntity) continue;
            double dx = ev.pos.x - pl->x, dy = ev.pos.y - pl->y, dz = ev.pos.z - pl->z;
            if (dx * dx + dy * dy + dz * dz < 64.0 * 64.0) pl->connection().send(p);
        }
    }
    for (const auto& ev : server_.overworld().takeBlockEvents()) {
        Packet p(pk::BLOCK_EVENT);
        p.writeBlockPos(ev.pos.x, ev.pos.y, ev.pos.z);
        p.writeByte(static_cast<uint8_t>(ev.b0));
        p.writeByte(static_cast<uint8_t>(ev.b1));
        p.writeVarInt(static_cast<int32_t>(ev.block));
        for (auto& pl : players_) {
            double dx = ev.pos.x - pl->x, dy = ev.pos.y - pl->y, dz = ev.pos.z - pl->z;
            if (dx * dx + dy * dy + dz * dz < 64.0 * 64.0) pl->connection().send(p);
        }
    }
    for (const auto& snd : server_.overworld().takeSounds()) {
        Packet p(pk::SOUND);
        p.writeVarInt(snd.sound + 1); // Holder: registry id + 1, or 0 + the inline SoundEvent
        if (snd.sound < 0) {
            p.writeUtf(snd.directSound);
            p.writeBool(false); // no fixed range
        }
        p.writeVarInt(snd.source);
        p.writeInt(static_cast<int32_t>(snd.x * 8.0));
        p.writeInt(static_cast<int32_t>(snd.y * 8.0));
        p.writeInt(static_cast<int32_t>(snd.z * 8.0));
        p.writeFloat(snd.volume);
        p.writeFloat(snd.pitch);
        p.writeLong(snd.seed);
        double range = snd.volume > 1.0f ? 16.0 * snd.volume : 16.0; // SoundEvent.getRange
        for (auto& pl : players_) {
            if (pl->id() == snd.excludeEntity) continue;
            double dx = snd.x - pl->x, dy = snd.y - pl->y, dz = snd.z - pl->z;
            if (dx * dx + dy * dy + dz * dz < range * range) pl->connection().send(p);
        }
    }
    for (const auto& pt : server_.overworld().takeParticles()) {
        Packet p(pk::LEVEL_PARTICLES);
        p.writeVarInt(pt.type);
        p.writeBool(false); // overrideLimiter
        p.writeBool(false); // alwaysShow
        p.writeDouble(pt.x);
        p.writeDouble(pt.y);
        p.writeDouble(pt.z);
        for (float f : {pt.xDist, pt.yDist, pt.zDist, pt.speed, pt.speed, pt.speed}) p.writeFloat(f);
        p.writeVarInt(pt.count);
        p.writeVarInt(0); // RandomizationType.DEFAULT
        for (auto& pl : players_) {
            double dx = pt.x - pl->x, dy = pt.y - pl->y, dz = pt.z - pl->z;
            if (dx * dx + dy * dy + dz * dz < 32.0 * 32.0) pl->connection().send(p);
        }
    }
    // SignBlockEntity.tick: an editor who left or walked away may no longer send text.
    server_.overworld().tickSignEditors([&](BlockPos pos, int id) {
        for (auto& p : players_)
            if (p->id() == id) return p->isWithinBlockInteractionRange(pos, 4.0);
        return false;
    });
    // ServerLevel.tick: when enough players sleep long enough, the night is skipped.
    if (sleepingPlayers_ > 0 && sleepingPlayers_ >= sleepersNeeded(activePlayers_)) {
        int deep = 0;
        for (auto& p : players_) deep += p->sleepingPos && p->sleepCounter >= 100; // Player.isSleepingLongEnough
        if (deep >= sleepersNeeded(activePlayers_)) {
            // ServerClockManager.moveToTimeMarker(overworld, wake_up_from_sleep: tick 0 of the 24000 day)
            if (auto* clock = server_.clockManager().byName("minecraft:overworld"); clock && server_.advanceTime()) {
                int64_t into = clock->totalTicks % 24000;
                if (into != 0) {
                    clock->totalTicks += 24000 - into;
                    clock->partialTick = 0;
                    broadcastTime(true);
                }
            }
            sleepingPlayers_ = 0; // wakeUpAllPlayers
            for (auto& p : players_)
                if (p->sleepingPos) p->game->stopSleepInBed(false, false);
        }
    }
    for (auto& p : players_) {
        p->tickExperience();
        p->tickEffects();
        p->syncAttributes(false);
    }
    for (auto& p : players_) {
        // LivingEntity.aiStep -> applyEffectsFromBlocks: blocks the player is in react (pressure
        // plates). (Simplified to the blocks around where the player is now.)
        if (p->gameMode == GameType::Spectator || p->disconnected) continue;
        AABB box = p->boundingBox().inflate(-1.0e-5, -1.0e-5, -1.0e-5);
        ServerLevel& level = p->level();
        for (int bx = static_cast<int>(std::floor(box.minX)); bx <= static_cast<int>(std::floor(box.maxX)); bx++)
            for (int by = std::max(level.minY(), static_cast<int>(std::floor(box.minY))); by <= std::min(level.maxY(), static_cast<int>(std::floor(box.maxY))); by++)
                for (int bz = static_cast<int>(std::floor(box.minZ)); bz <= static_cast<int>(std::floor(box.maxZ)); bz++) {
                    BlockPos bp{bx, by, bz};
                    if (!level.getChunkIfLoaded({bx >> 4, bz >> 4})) continue;
                    BlockStateId st = level.getBlockState(bp);
                    if (!blocks::isAir(st)) block_behaviour::redstone_blocks::entityInside(level, bp, st, box);
                }
    }
    for (auto& p : players_) touchItems(*p);
    trackEntities();
    for (auto& p : players_) {
        // ServerPlayer.tick: the open menu's changes, and closing it when it's no longer valid.
        p->containerMenu->broadcastChanges();
        if (!p->containerMenu->stillValid(*p)) p->closeContainer();
        broadcastEquipment(*p);
    }
    if (server_.tickCount() % 20 == 0) broadcastTime(false);
    for (auto& p : players_) {
        p->updateChunkTracking(viewDistance);
        p->chunkSender().sendNextChunks(*p);
    }
    // ServerEntity.sendChanges, simplified: absolute position sync whenever a player moved.
    for (auto& p : players_) {
        if (!p->consumeMoved()) continue;
        Packet sync(pk::ENTITY_POSITION_SYNC);
        sync.writeVarInt(p->id());
        sync.writeVarInt(0); // PositionPath.Linear
        sync.writeDouble(p->x);
        sync.writeDouble(p->y);
        sync.writeDouble(p->z);
        sync.writeFloat(p->yRot);
        sync.writeFloat(p->xRot);
        sync.writeBool(p->onGround);
        Packet head(pk::ROTATE_HEAD);
        head.writeVarInt(p->id());
        head.writeByte(static_cast<uint8_t>(packDegrees(p->yRot)));
        broadcastAll(sync, p.get());
        broadcastAll(head, p.get());
    }
    // Refresh tab-list latency every 30 seconds, like PlayerList.tick.
    if (server_.tickCount() % 600 == 0 && !players_.empty()) {
        Packet p(pk::PLAYER_INFO_UPDATE);
        p.writeByte(1 << 4); // UPDATE_LATENCY
        p.writeVarInt(playerCount());
        for (auto& pl : players_) {
            p.writeUUID(pl->profile().id);
            p.writeVarInt(pl->latencyMs);
        }
        broadcastAll(p);
    }
}

} // namespace mc
