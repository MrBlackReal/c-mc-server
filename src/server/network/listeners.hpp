#pragma once

#include <array>

#include <algorithm>
#include <cstdint>
#include <map>

#include "network/chat/signed_chat.hpp"
#include "network/connection.hpp"
#include "server/level/server_player.hpp"

namespace mc {

class MinecraftServer;

class ServerHandshakePacketListener final : public PacketListener {
public:
    ServerHandshakePacketListener(MinecraftServer& server, Connection& conn) : server_(server), conn_(conn) {}
    ConnectionProtocol protocol() const override { return ConnectionProtocol::Handshake; }
    void handlePacket(int id, ByteReader& in) override;

private:
    MinecraftServer& server_;
    Connection& conn_;
};

class ServerStatusPacketListener final : public PacketListener {
public:
    ServerStatusPacketListener(MinecraftServer& server, Connection& conn) : server_(server), conn_(conn) {}
    ConnectionProtocol protocol() const override { return ConnectionProtocol::Status; }
    void handlePacket(int id, ByteReader& in) override;

private:
    MinecraftServer& server_;
    Connection& conn_;
    bool hasRequestedStatus_ = false;
};

class ServerLoginPacketListener final : public PacketListener {
public:
    ServerLoginPacketListener(MinecraftServer& server, Connection& conn, bool transferred)
        : server_(server), conn_(conn), transferred_(transferred) {}
    ConnectionProtocol protocol() const override { return ConnectionProtocol::Login; }
    void handlePacket(int id, ByteReader& in) override;
    void tick() override;

private:
    enum class State { Hello, Key, Authenticating, Verifying, WaitingForDupeDisconnect, ProtocolSwitching, Accepted };
    // The outcome of the session server check, filled in by the authenticator thread.
    struct Authentication;
    void disconnect(const Component& reason);
    void handleKey(ByteReader& in);
    void startClientVerification(GameProfile profile);
    void verifyLoginAndFinishConnectionSetup();
    void finishLoginAndWaitForClient();

    MinecraftServer& server_;
    Connection& conn_;
    bool transferred_;
    State state_ = State::Hello;
    std::string requestedUsername_;
    GameProfile profile_;
    std::array<uint8_t, 4> challenge_{};
    std::shared_ptr<Authentication> authentication_;
    int ticks_ = 0;
};

// ServerCommonPacketListenerImpl: keep-alive, client information, custom payloads.
class ServerCommonPacketListener : public PacketListener {
public:
    ServerCommonPacketListener(MinecraftServer& server, Connection& conn, GameProfile profile, ClientInformation info);
    void tick() override;

protected:
    // Returns true if the packet was one of the common ones.
    bool handleCommon(int id, ByteReader& in, int keepAliveId, int pongId, int clientInfoId, int customPayloadId);
    virtual void sendKeepAlive(int64_t challenge) = 0;
    virtual void onClientInformation(const ClientInformation&) {}

    MinecraftServer& server_;
    Connection& conn_;
    GameProfile profile_;
    ClientInformation clientInfo_;
    int latencyMs_ = 0;

private:
    int64_t keepAliveTime_;
    bool keepAlivePending_ = false;
    int64_t keepAliveChallenge_ = 0;
};

class ServerConfigurationPacketListener final : public ServerCommonPacketListener {
public:
    ServerConfigurationPacketListener(MinecraftServer& server, Connection& conn, GameProfile profile, ClientInformation info);
    ConnectionProtocol protocol() const override { return ConnectionProtocol::Configuration; }
    void startConfiguration();
    void handlePacket(int id, ByteReader& in) override;
    void onDisconnect(const Component& reason) override;

private:
    enum class Task { None, SynchronizeRegistries, JoinWorld };
    void sendKeepAlive(int64_t challenge) override;
    void sendRegistries(bool clientKnowsPacks);
    Task task_ = Task::None;
};

// Abilities.mayBuild: survival and creative players may change the world.
inline bool canModifyWorld(GameType mode) { return mode == GameType::Survival || mode == GameType::Creative; }

// InteractionResult, as far as the server cares: SUCCESS is predicted by the client (others see
// the swing), SUCCESS_SERVER swings for everyone, CONSUME doesn't swing.
enum class UseResult { Pass, TryEmptyHand, Consume, Success, SuccessServer };

class ServerGamePacketListener final : public ServerCommonPacketListener {
public:
    ServerGamePacketListener(MinecraftServer& server, Connection& conn, ServerPlayer& player);
    ConnectionProtocol protocol() const override { return ConnectionProtocol::Play; }
    void handlePacket(int id, ByteReader& in) override;
    void tick() override;
    void onDisconnect(const Component& reason) override;

    void teleport(double x, double y, double z, float yRot, float xRot) { teleport(x, y, z, yRot, xRot, 0); }
    // ServerGamePacketListenerImpl.teleport(PositionMoveRotation, Set<Relative>): `relatives` are
    // ClientboundPlayerPositionPacket's flags; relative components are offsets from where the player is.
    void teleport(double x, double y, double z, float yRot, float xRot, int relatives);
    // ServerPlayer.setGameMode: false if the player already is in that mode.
    bool setGameMode(GameType mode);
    // ServerGamePacketListenerImpl.disconnect
    void disconnect(const Component& reason) { conn_.disconnect(reason); }
    // ServerPlayer.sendChatMessage
    void sendChatMessage(const chat::PlayerChatMessage& message, const chat::ChatTypeBound& chatType);
    void sendAbilities();
    void sendSystemMessage(const Component& message);
    ServerPlayer& player() { return player_; }
    // ServerGamePacketListenerImpl.sendPlayerChatMessage / sendDisguisedChatMessage
    void sendPlayerChatMessage(const chat::PlayerChatMessage& message, const chat::ChatTypeBound& chatType);
    void sendDisguisedChatMessage(const Component& content, const chat::ChatTypeBound& chatType);

private:
    void sendKeepAlive(int64_t challenge) override;
    void onClientInformation(const ClientInformation& info) override;
    void handleMove(ByteReader& in, bool hasPos, bool hasRot);
    // Secure chat (game_chat.cpp)
    void handleChat(ByteReader& in);
    void handleCommand(const std::string& command, const std::map<std::string, chat::PlayerChatMessage>* signedArguments = nullptr);
    void handleSignedCommand(ByteReader& in);
    void handleCommandSuggestion(ByteReader& in);
    void handleChatSessionUpdate(ByteReader& in);
    void handleChatAck(ByteReader& in);
    bool tryHandleChat(const std::string& message, bool isCommand);
    std::optional<chat::LastSeenMessages> unpackAndApplyLastSeen(const chat::LastSeenUpdate& update);
    void handleMessageDecodeFailure(const std::string& translationKey);
    void resetPlayerChatState(chat::RemoteChatSession session);
    chat::SignedMessageDecoder signedMessageDecoder_;
    chat::LastSeenMessagesValidator lastSeenMessages_;
    chat::MessageSignatureCache messageSignatureCache_;
    int nextChatIndex_ = 0;
    void handlePlayerAction(ByteReader& in);
    void handleUseItemOn(ByteReader& in);
    void handleSetCreativeModeSlot(ByteReader& in);
    void handleSetCarriedItem(ByteReader& in);
    void handlePickItemFromBlock(ByteReader& in);
    void handleContainerClick(ByteReader& in);

    // ServerPlayerGameMode
    void handleBlockBreakAction(int action, BlockPos pos, int sequence);
    bool destroyBlock(BlockPos pos);
    void abortDestroy(BlockPos pos);
    float destroyProgress(BlockStateId state);
    float incrementDestroyProgress(BlockStateId state, BlockPos pos, int ticksSpentDestroying);
    void sendDestroyProgress(BlockPos pos, int stage);
    void hurtTool(ItemStack& tool, BlockStateId state);
    void hurtAndBreak(ItemStack& stack, int amount, int hand);
    void createBlockEntityFromItem(BlockPos pos, BlockStateId placed, const ItemStack& stack);
    bool blockActionRestricted(BlockPos pos);
public:
    void tickDestroy();
private:
    UseResult useItemOn(int hand, BlockPos hitPos, Direction face, double hx, double hy, double hz);
    // BlockState.useItemOn / useWithoutItem (game_block_use.cpp)
    UseResult useItemOnBlock(ItemStack& stack, int hand, BlockPos pos, BlockStateId state, Direction face, double hx, double hy, double hz);
    UseResult useBlockWithoutItem(BlockPos pos, BlockStateId state, Direction face, double hx, double hy, double hz);
    // ItemStack.useOn (game_item_use.cpp)
    UseResult useItemStackOn(ItemStack& stack, int hand, BlockPos pos, Direction face, double hx, double hy, double hz);
    UseResult transformBlock(const std::string& id, ItemStack& stack, int hand, BlockPos pos, Direction face);
    UseResult useItemOnCauldron(ItemStack& stack, int hand, BlockPos pos, BlockStateId state);
    // ServerGamePacketListenerImpl.handleUseItem -> ServerPlayerGameMode.useItem -> Item.use
    // (game_item_use.cpp): so far buckets.
    void handleUseItem(ByteReader& in);
    UseResult useBucket(ItemStack& stack, int hand, ItemStack& transformedTo);
    bool emptyBucketContents(uint8_t content, BlockPos pos, const std::optional<ServerLevel::BlockHit>& hit);
    nbt::Compound* blockEntityAt(BlockPos pos, std::string_view type);
    bool openBlockMenu(BlockPos pos, BlockStateId state);
    void giveOrDrop(ItemStack stack);
    // Beds (game_bed.cpp)
    struct Vec3 {
        double x, y, z;
    };
public:
    bool isDarkOutside();
    UseResult useBed(BlockPos pos, BlockStateId state);
    void stopSleepInBed(bool forcefulWakeUp, bool updateLevelList);
    void tickSleeping();
    void sendOverlay(const Component& message);
private:
    std::optional<Vec3> findStandUpPosition(BlockPos bed, Direction forward, float yaw);
    // Signs (game_sign.cpp)
    void handleSignUpdate(ByteReader& in);
    nbt::Compound* signAt(BlockPos pos);
    bool isFacingFrontText(BlockPos pos, BlockStateId state);
    void openTextEdit(BlockPos pos, bool front);
    void signPlaced(BlockPos pos);
    bool executeSignClickCommands(BlockPos pos, bool front);
    UseResult useSignWithoutItem(BlockPos pos, BlockStateId state);
    UseResult useItemOnSign(ItemStack& stack, BlockPos pos, BlockStateId state);
    bool placeBlock(ItemStack& stack, BlockPos hitPos, Direction face, double hx, double hy, double hz);

    void ackBlockChangesUpTo(int sequence) { player_.ackBlockChangesUpTo = std::max(player_.ackBlockChangesUpTo, sequence); }
    void sendBlockUpdate(BlockPos pos);
    void swing(int hand, bool includeSelf);
    ItemStack& itemInHand(int hand) { return hand == 0 ? player_.inventory.selectedItem() : player_.inventory.equipment(EquipmentSlot::OffHand); }

    ServerPlayer& player_;
    int awaitingTeleport_ = 0;
    int teleportId_ = 0;
    // ServerGamePacketListenerImpl movement checks.
    void resetPosition();
    void handlePlayerPositionChange(double x, double y, double z, float yRot, float xRot, bool onGround, bool horizontalCollision);
    bool hasClientLoaded() const { return clientLoaded_ || clientLoadedTimeout_ <= 0; }
    int listenerTicks_ = 0;
    int awaitingTeleportTime_ = 0;
    double firstGoodX_ = 0, firstGoodY_ = 0, firstGoodZ_ = 0, lastGoodX_ = 0, lastGoodY_ = 0, lastGoodZ_ = 0;
    int receivedMovePacketCount_ = 0, knownMovePacketCount_ = 0;
    bool receivedPositionThisTick_ = false;
    bool clientIsFloating_ = false;
    int aboveGroundTickCount_ = 0;
    bool verticalCollisionBelow_ = false;
    bool clientLoaded_ = false;
    int clientLoadedTimeout_ = 60;
    bool removed_ = false;
};

ClientInformation readClientInformation(ByteReader& in);

} // namespace mc
