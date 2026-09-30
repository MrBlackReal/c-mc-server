// Port of ServerGamePacketListenerImpl (movement, chunks, chat, commands so far).
#include <cmath>

#include "core/registries.hpp"
#include "generated/packet_ids.hpp"
#include "server/commands/commands.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"
#include "world/inventory/workstation_menus.hpp"
#include "world/phys/aabb.hpp"

namespace mc {

namespace cb = packets::play_cb;
namespace sb = packets::play_sb;

ServerGamePacketListener::ServerGamePacketListener(MinecraftServer& server, Connection& conn, ServerPlayer& player)
    : ServerCommonPacketListener(server, conn, player.profile(), player.clientInfo),
      signedMessageDecoder_(player.profile().id, server.enforceSecureProfile()), player_(player) {
    player.game = this;
}

void ServerGamePacketListener::sendKeepAlive(int64_t challenge) {
    Packet p(cb::KEEP_ALIVE);
    p.writeLong(challenge);
    conn_.send(p);
}

void ServerGamePacketListener::onClientInformation(const ClientInformation& info) { player_.clientInfo = info; }

void ServerGamePacketListener::tick() {
    ServerCommonPacketListener::tick();
    // ServerGamePacketListenerImpl.tickPlayer: movement bookkeeping and the floating kick.
    resetPosition();
    listenerTicks_++;
    knownMovePacketCount_ = receivedMovePacketCount_;
    if (clientLoadedTimeout_ > 0 && !clientLoaded_) clientLoadedTimeout_--;
    if (clientIsFloating_) {
        if (++aboveGroundTickCount_ > 80) {
            log::warn("{} was kicked for floating too long!", player_.profile().name);
            conn_.disconnect(Component::translatable("multiplayer.disconnect.flying"));
            return;
        }
    } else {
        aboveGroundTickCount_ = 0;
    }
    player_.latencyMs = latencyMs_;
    if (player_.dropSpam > 0) player_.dropSpam--;
    tickDestroy();
    tickSleeping();
    // Runs after PlayerList::tick broadcast this tick's block changes, like vanilla, so the client
    // sees the authoritative states before it drops its predictions.
    if (player_.ackBlockChangesUpTo > -1) {
        Packet p(cb::BLOCK_CHANGED_ACK);
        p.writeVarInt(player_.ackBlockChangesUpTo);
        conn_.send(p);
        player_.ackBlockChangesUpTo = -1;
    }
}

void ServerGamePacketListener::teleport(double x, double y, double z, float yRot, float xRot) {
    player_.x = x, player_.y = y, player_.z = z, player_.yRot = yRot, player_.xRot = xRot;
    if (++teleportId_ == INT32_MAX) teleportId_ = 0;
    awaitingTeleport_ = teleportId_;
    awaitingTeleportTime_ = listenerTicks_;
    Packet p(cb::PLAYER_POSITION);
    p.writeVarInt(teleportId_);
    p.writeDouble(x);
    p.writeDouble(y);
    p.writeDouble(z);
    p.writeDouble(0); // delta movement
    p.writeDouble(0);
    p.writeDouble(0);
    p.writeFloat(yRot);
    p.writeFloat(xRot);
    p.writeInt(0); // no relative components
    conn_.send(p);
}

void ServerGamePacketListener::sendAbilities() {
    // Abilities flags: invulnerable 1, flying 2, mayfly 4, instabuild 8.
    uint8_t flags = 0;
    switch (player_.gameMode) {
    case GameType::Creative: flags = 1 | 4 | 8; break;
    case GameType::Spectator: flags = 1 | 2 | 4; break;
    default: break;
    }
    Packet p(cb::PLAYER_ABILITIES);
    p.writeByte(flags);
    p.writeFloat(0.05f);
    p.writeFloat(0.1f);
    conn_.send(p);
}

void ServerGamePacketListener::setGameMode(GameType mode) {
    player_.gameMode = mode;
    Packet event(cb::GAME_EVENT);
    event.writeByte(3); // CHANGE_GAME_MODE
    event.writeFloat(static_cast<float>(mode));
    conn_.send(event);
    sendAbilities();
    Packet info(cb::PLAYER_INFO_UPDATE);
    info.writeByte(1 << 2); // UPDATE_GAME_MODE
    info.writeVarInt(1);
    info.writeUUID(player_.profile().id);
    info.writeVarInt(static_cast<int>(mode));
    server_.playerList().broadcastAll(info);
}

void ServerGamePacketListener::sendSystemMessage(const Component& message) {
    Packet p(cb::SYSTEM_CHAT);
    nbt::writeNetwork(p, message.toNbt());
    p.writeBool(false);
    conn_.send(p);
}

void ServerGamePacketListener::resetPosition() {
    firstGoodX_ = lastGoodX_ = player_.x;
    firstGoodY_ = lastGoodY_ = player_.y;
    firstGoodZ_ = lastGoodZ_ = player_.z;
}

void ServerGamePacketListener::handleMove(ByteReader& in, bool hasPos, bool hasRot) {
    // ServerGamePacketListenerImpl.handleMovePlayer
    double x = player_.x, y = player_.y, z = player_.z;
    float yRot = player_.yRot, xRot = player_.xRot;
    if (hasPos) {
        x = in.readDouble();
        y = in.readDouble();
        z = in.readDouble();
    }
    if (hasRot) {
        yRot = in.readFloat();
        xRot = in.readFloat();
    }
    uint8_t flags = in.readUnsignedByte();
    if (std::isnan(x) || std::isnan(y) || std::isnan(z) || !std::isfinite(yRot) || !std::isfinite(xRot)) {
        conn_.disconnect(Component::translatable("multiplayer.disconnect.invalid_player_movement"));
        return;
    }
    if (hasPos) {
        // One position per client tick (ServerboundClientTickEndPacket resets this).
        if (receivedPositionThisTick_) {
            conn_.disconnect(Component::translatable("multiplayer.disconnect.invalid_player_movement"));
            return;
        }
        receivedPositionThisTick_ = true;
    }
    if (listenerTicks_ == 0) resetPosition();
    if (!hasClientLoaded()) return;
    auto wrap = [](float d) { // Mth.wrapDegrees
        float w = std::fmod(d, 360.0f);
        if (w >= 180.0f) w -= 360.0f;
        if (w < -180.0f) w += 360.0f;
        return w;
    };
    if (awaitingTeleport_ != 0) { // updateAwaitingTeleport: resend if the client doesn't confirm
        if (listenerTicks_ - awaitingTeleportTime_ > 20) {
            awaitingTeleportTime_ = listenerTicks_;
            teleport(player_.x, player_.y, player_.z, player_.yRot, player_.xRot);
        }
        player_.yRot = wrap(yRot);
        player_.xRot = std::clamp(wrap(xRot), -90.0f, 90.0f);
        return;
    }
    awaitingTeleportTime_ = listenerTicks_;
    handlePlayerPositionChange(x, y, z, yRot, xRot, flags & 1, flags & 2);
}

void ServerGamePacketListener::handlePlayerPositionChange(double tx, double ty, double tz, float yRot, float xRot, bool onGround,
                                                          bool horizontalCollision) {
    tx = std::clamp(tx, -3.0e7, 3.0e7);
    ty = std::clamp(ty, -2.0e7, 2.0e7);
    tz = std::clamp(tz, -3.0e7, 3.0e7);
    auto wrap = [](float d) {
        float w = std::fmod(d, 360.0f);
        if (w >= 180.0f) w -= 360.0f;
        if (w < -180.0f) w += 360.0f;
        return w;
    };
    float targetYRot = wrap(yRot), targetXRot = std::clamp(wrap(xRot), -90.0f, 90.0f);
    ServerLevel& level = player_.level();
    double startX = player_.x, startY = player_.y, startZ = player_.z;
    bool creative = player_.gameMode == GameType::Creative, spectator = player_.gameMode == GameType::Spectator;

    // "moved too quickly": more than 100 blocks (squared) per move packet since the last tick.
    double xd = tx - firstGoodX_, yd = ty - firstGoodY_, zd = tz - firstGoodZ_;
    double movedDist = xd * xd + yd * yd + zd * zd;
    double expectedDist = 0.0; // server-side delta movement (no knockback yet)
    receivedMovePacketCount_++;
    int deltaPackets = receivedMovePacketCount_ - knownMovePacketCount_;
    if (deltaPackets > 5) deltaPackets = 1;
    if (movedDist - expectedDist > 100.0 * deltaPackets) {
        log::warn("{} moved too quickly! {},{},{}", player_.profile().name, xd, yd, zd);
        teleport(player_.x, player_.y, player_.z, player_.yRot, player_.xRot);
        return;
    }

    // Replay the move on the server with collisions (Entity.move, stepping up to 0.6 blocks).
    AABB oldBox = player_.boundingBox();
    double dx = tx - lastGoodX_, dy = ty - lastGoodY_, dz = tz - lastGoodZ_;
    bool standsOnSomething = verticalCollisionBelow_;
    if (!spectator) {
        double cx = dx, cy = dy, cz = dz;
        collision::collideWithStepUp(oldBox, player_.onGround, 0.6, cx, cy, cz, [&](const AABB& b) { return level.collisionBoxes(b); });
        verticalCollisionBelow_ = cy != dy && dy < 0;
        player_.x += cx, player_.y += cy, player_.z += cz;
    } else {
        player_.x += dx, player_.y += dy, player_.z += dz;
    }
    xd = tx - player_.x;
    yd = 0.0; // vanilla: "if (yDist > -0.5 || yDist < 0.5) yDist = 0" - vertical error never counts
    zd = tz - player_.z;
    movedDist = xd * xd + yd * yd + zd * zd;
    bool fail = movedDist > 0.0625 && !creative && !spectator;
    if (fail) log::warn("{} moved wrongly!", player_.profile().name);

    // Level.noCollision / isEntityCollidingWithAnythingNew
    auto noCollision = [&](const AABB& box) {
        for (const auto& b : level.collisionBoxes(box))
            if (b.intersects(box)) return false;
        return true;
    };
    auto collidingWithAnythingNew = [&] {
        AABB newBox = player_.boundingBox().move(tx - player_.x, ty - player_.y, tz - player_.z);
        AABB oldDeflated = collision::deflate(oldBox, 9.999999747378752E-6);
        AABB newDeflated = collision::deflate(newBox, 9.999999747378752E-6);
        for (const auto& b : level.collisionBoxes(newDeflated))
            if (b.intersects(newDeflated) && !b.intersects(oldDeflated)) return true;
        return false;
    };
    if (spectator || ((!fail || !noCollision(oldBox)) && !collidingWithAnythingNew())) {
        player_.x = tx, player_.y = ty, player_.z = tz;
        player_.yRot = targetYRot, player_.xRot = targetXRot;
        // Floating in survival without flight: kicked after 80 ticks (see tick()).
        auto noBlocksAround = [&] {
            AABB box = player_.boundingBox().inflate(0.0625, 0.0625, 0.0625).expandTowards(0, -0.55, 0);
            for (int bx = static_cast<int>(std::floor(box.minX)); bx <= static_cast<int>(std::floor(box.maxX)); bx++)
                for (int by = static_cast<int>(std::floor(box.minY)); by <= static_cast<int>(std::floor(box.maxY)); by++)
                    for (int bz = static_cast<int>(std::floor(box.minZ)); bz <= static_cast<int>(std::floor(box.maxZ)); bz++)
                        if (!blocks::isAir(level.getBlockState(bx, by, bz))) return false;
            return true;
        };
        static const int kLevitation = registries::id("minecraft:mob_effect", "minecraft:levitation").value_or(-1);
        bool mayfly = creative || spectator;
        clientIsFloating_ = yd >= -0.03125 && !standsOnSomething && !spectator && !server_.properties().allowFlight && !mayfly &&
                            !player_.hasEffect(kLevitation) && noBlocksAround();
        player_.onGround = onGround;
        (void)horizontalCollision;
        lastGoodX_ = player_.x, lastGoodY_ = player_.y, lastGoodZ_ = player_.z;
    } else {
        teleport(startX, startY, startZ, targetYRot, targetXRot);
    }
}

void ServerGamePacketListener::handlePacket(int id, ByteReader& in) {
    if (handleCommon(id, in, sb::KEEP_ALIVE, sb::PONG, sb::CLIENT_INFORMATION, sb::CUSTOM_PAYLOAD)) return;

    switch (id) {
    case sb::ACCEPT_TELEPORTATION: {
        int tid = in.readVarInt();
        if (tid == awaitingTeleport_) { // handleAcceptTeleportPacket
            awaitingTeleport_ = 0;
            lastGoodX_ = player_.x, lastGoodY_ = player_.y, lastGoodZ_ = player_.z;
        }
        break;
    }
    case sb::MOVE_PLAYER_POS: handleMove(in, true, false); break;
    case sb::MOVE_PLAYER_POS_ROT: handleMove(in, true, true); break;
    case sb::MOVE_PLAYER_ROT: handleMove(in, false, true); break;
    case sb::MOVE_PLAYER_STATUS_ONLY: handleMove(in, false, false); break;
    case sb::CHUNK_BATCH_RECEIVED: player_.chunkSender().onBatchReceived(in.readFloat()); break;
    case sb::CHAT: handleChat(in); break;
    case sb::CHAT_COMMAND: {
        std::string command = in.readUtf();
        if (tryHandleChat(command, true)) handleCommand(command);
        break;
    }
    case sb::CHAT_COMMAND_SIGNED: handleSignedCommand(in); break;
    case sb::CHAT_SESSION_UPDATE: handleChatSessionUpdate(in); break;
    case sb::CHAT_ACK: handleChatAck(in); break;
    case sb::PLAYER_LOADED:
        log::debug("{} finished loading terrain", player_.profile().name);
        clientLoaded_ = true;
        break;
    case sb::CLIENT_TICK_END: receivedPositionThisTick_ = false; break;
    case sb::PLAYER_ACTION: handlePlayerAction(in); break;
    case sb::USE_ITEM_ON: handleUseItemOn(in); break;
    case sb::SIGN_UPDATE: handleSignUpdate(in); break;
    case sb::USE_ITEM: {
        in.readVarInt(); // hand
        ackBlockChangesUpTo(in.readVarInt());
        // TODO: using items in the air (food, bows, buckets, ...)
        break;
    }
    case sb::SET_CREATIVE_MODE_SLOT: handleSetCreativeModeSlot(in); break;
    case sb::SET_CARRIED_ITEM: handleSetCarriedItem(in); break;
    case sb::PICK_ITEM_FROM_BLOCK: handlePickItemFromBlock(in); break;
    case sb::CONTAINER_CLICK: handleContainerClick(in); break;
    case sb::CONTAINER_BUTTON_CLICK: { // handleContainerButtonClick
        int containerId = in.readVarInt();
        int buttonId = in.readVarInt();
        AbstractContainerMenu* menu = player_.containerMenu;
        if (menu && menu->containerId == containerId && player_.gameMode != GameType::Spectator && menu->stillValid(player_) &&
            menu->clickMenuButton(player_, buttonId))
            menu->broadcastChanges();
        break;
    }
    case sb::RENAME_ITEM: { // handleRenameItem
        std::string name = in.readUtf();
        if (auto* anvil = dynamic_cast<AnvilMenu*>(player_.containerMenu); anvil && anvil->stillValid(player_)) anvil->setItemName(name);
        break;
    }
    case sb::SET_BEACON: { // handleSetBeaconPacket
        auto effect = [&]() -> std::optional<int> {
            if (!in.readBool()) return std::nullopt;
            return in.readVarInt();
        };
        std::optional<int> primary = effect(), secondary = effect();
        if (auto* beacon = dynamic_cast<BeaconMenu*>(player_.containerMenu); beacon && beacon->stillValid(player_)) {
            if (!beacon->updateEffects(primary, secondary)) {
                log::warn("Player {} tried to set invalid beacon effects", player_.profile().name);
                conn_.disconnect(Component::translatable("multiplayer.disconnect.generic"));
            }
        }
        break;
    }
    case sb::CONTAINER_SLOT_STATE_CHANGED: { // handleContainerSlotStateChanged
        int slot = in.readVarInt();
        int containerId = in.readVarInt();
        bool enabled = in.readBool();
        if (player_.gameMode != GameType::Spectator && player_.containerMenu && player_.containerMenu->containerId == containerId)
            if (auto* crafter = dynamic_cast<CrafterMenu*>(player_.containerMenu)) crafter->setSlotState(slot, enabled);
        break;
    }
    case sb::BUNDLE_ITEM_SELECTED: { // handleBundleItemSelectedPacket
        int slot = in.readVarInt();
        int selected = in.readVarInt();
        if (player_.containerMenu) player_.containerMenu->setSelectedBundleItemIndex(slot, selected);
        break;
    }
    case sb::CONTAINER_CLOSE:
        in.readVarInt();             // (handleContainerClose closes whatever is open)
        player_.doCloseContainer();
        break;
    case sb::PLAYER_INPUT: player_.sneaking = in.readUnsignedByte() & 0x20; break; // Input.FLAG_SHIFT
    case sb::PLAYER_COMMAND: {
        in.readVarInt(); // entity id
        int action = in.readVarInt();
        in.readVarInt(); // data (a horse jump's strength)
        if (action == 0) { // STOP_SLEEPING: the "Leave Bed" button
            if (player_.sleepingPos) stopSleepInBed(false, true);
        } else if (action == 1) {
            player_.sprinting = true; // START_SPRINTING
        } else if (action == 2) {
            player_.sprinting = false;
        }
        break;
    }
    case sb::PLAYER_ABILITIES: {
        bool flying = in.readByte() & 2;
        player_.flying = flying && (player_.gameMode == GameType::Creative || player_.gameMode == GameType::Spectator); // mayfly
        break;
    }
    case sb::PUNCH: swing(0, false); break;
    default:
        // Unhandled gameplay packets are ignored for now, the way a lenient server would.
        log::debug("{}: unhandled play packet 0x{:02x}", player_.profile().name, id);
        break;
    }
}

void ServerGamePacketListener::onDisconnect(const Component& reason) {
    if (removed_) return;
    removed_ = true;
    log::info("{} lost connection: {}", player_.profile().name, reason.getString());
    server_.playerList().remove(player_); // destroys player_
}

} // namespace mc
