#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "network/chat/component.hpp"
#include "server/level/server_player.hpp"

namespace mc {

class MinecraftServer;
class Packet;

// Port of PlayerList: joining, leaving and broadcasting.
class PlayerList {
public:
    // A new entity: spawn it for the players in range now (ChunkMap.addEntity -> updatePlayers).
    void trackNewEntity(Entity& e);
    // Light sections that changed, as ClientboundLightUpdatePacket (also before a chunk is sent).
    void broadcastLightChanges();
    explicit PlayerList(MinecraftServer& server) : server_(server) {}

    ServerPlayer* getPlayer(const UUID& id) const;
    ServerPlayer* getPlayerByName(std::string_view name) const;
    const std::vector<std::unique_ptr<ServerPlayer>>& players() const { return players_; }
    int playerCount() const { return static_cast<int>(players_.size()); }

    // Returns a reason if the player may not join.
    std::optional<Component> canPlayerLogin(const GameProfile& profile) const;
    void placeNewPlayer(Connection& connection, GameProfile profile, ClientInformation info);
    // PlayerList.disconnectAllPlayersWithProfile: kicks players with this id (logging in twice).
    bool disconnectAllPlayersWithProfile(const UUID& id);
    void remove(ServerPlayer& player);

    void broadcastAll(const Packet& packet, const ServerPlayer* except = nullptr);
    // PlayerList.broadcastChatMessage: to everyone, as player chat (or disguised, from the
    // server); logged, "[Not Secure]" unless signed and recent.
    void broadcastChatMessage(const chat::PlayerChatMessage& message, ServerPlayer* sender, const chat::ChatTypeBound& chatType);
    // ClientboundPlayerInfoUpdatePacket(INITIALIZE_CHAT): the player's chat session to everyone.
    void broadcastChatSession(ServerPlayer& player);
    void broadcastSystemMessage(const Component& message);

    void tick();

    // PlayerList.saveAll: players/data/<uuid>.dat
    void saveAll();
    void save(ServerPlayer& player);

    void sendTimeSync(Connection& to, bool fullClocks);
    void broadcastTime(bool fullClocks);
    // The player's pose and bed (SynchedEntityData) to everyone, the player included.
    void broadcastPlayerData(ServerPlayer& player);
    // ServerLevel.updateSleepingPlayerList: counts sleepers and tells everyone.
    void updateSleepingPlayerList();

private:
    bool inTrackingRange(const ServerPlayer& p, const Entity& e) const;
    void sendPlayerInfo(Connection& to, const std::vector<ServerPlayer*>& entries);
    void sendAddEntity(Connection& to, const ServerPlayer& p);
    void broadcastBlockChanges();

    void trackEntities();
    void touchItems(ServerPlayer& player);
    void sendEntitySpawn(Connection& to, const Entity& entity);
    void sendEntityData(Connection& to, const Entity& entity);
    void broadcastEquipment(ServerPlayer& p);
    void sendEquipment(Connection& to, const ServerPlayer& p);

    MinecraftServer& server_;
    std::vector<std::unique_ptr<ServerPlayer>> players_;
    int activePlayers_ = 0, sleepingPlayers_ = 0; // SleepStatus
};

} // namespace mc
