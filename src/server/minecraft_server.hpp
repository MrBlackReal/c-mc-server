#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "server/level/server_level.hpp"
#include "server/players/player_list.hpp"
#include "server/players/user_lists.hpp"
#include "server/services/session_service.hpp"
#include "util/crypt.hpp"
#include "server/server_properties.hpp"
#include "util/uuid.hpp"
#include "world/level/storage/level_storage.hpp"

namespace mc {

class Connection;
namespace commands {
class Commands;
}

inline constexpr int kProtocolVersion = 777;
inline constexpr const char* kGameVersion = "26.3";
inline constexpr const char* kServerBrand = "cmc";

// Port of MinecraftServer / DedicatedServer: owns the world, the players and a single-threaded
// epoll event loop that interleaves network I/O with 20 TPS ticks.
class MinecraftServer {
public:
    explicit MinecraftServer(ServerProperties properties);
    ~MinecraftServer();

    int run();
    void halt() { running_ = false; }

    const ServerProperties& properties() const { return properties_; }
    ServerLevel& overworld() { return *overworld_; }
    PlayerList& playerList() { return playerList_; }
    const UUID& sessionId() const { return sessionId_; }
    uint64_t tickCount() const { return tickCount_; }
    int nextEntityId() { return nextEntityId_++; }
    GameType defaultGameType() const { return defaultGameType_; }
    ServerClockManager& clockManager() { return clocks_; }
    bool advanceTime() const { return gameRules_.getBool(GameRules::AdvanceTime); }
    GameRules& gameRules() { return gameRules_; }
    // MinecraftServer.onGameRuleChanged: tell clients about rules they know of.
    void onGameRuleChanged(GameRules::Rule rule);

    // Online mode (MinecraftServer.usesAuthentication): the login key pair and the session server.
    bool usesAuthentication() const { return properties_.onlineMode; }
    // DedicatedServer.enforceSecureProfile: chat must be signed (needs online mode and the services keys).
    bool enforceSecureProfile() { return properties_.enforceSecureProfile && properties_.onlineMode && sessionService_->canValidateProfileKeys(); }
    const crypt::KeyPair& keyPair() const { return *keyPair_; }
    // (shared: login threads may outlive a stopping server)
    std::shared_ptr<SessionService> sessionService() { return sessionService_; }

    // MinecraftServer.getCommands: the dispatcher every command goes through.
    commands::Commands& commands() { return *commands_; }
    // Services.nameToIdCache (usercache.json)
    UserNameToIdResolver& nameToIdCache() { return nameToIdCache_; }
    // MinecraftServer.getProfilePermissions: an operator's level, else 0 (LevelBasedPermissionSet.ALL).
    int getProfilePermissions(const UUID& id) const;
    // MinecraftServer.sendSystemMessage: the console's output (the log).
    void sendSystemMessage(const Component& message);

    // DedicatedServer.handleConsoleInputs: a command typed on the console.
    void handleConsoleCommand(const std::string& line);
    LevelStorage* storage() { return storage_.get(); }
    void saveEverything(bool logIt);

private:
    bool loadLevel();
    bool bind();
    void acceptConnections();
    void readConsole();
    void tick();
    void updateInterest(Connection& c);

    ServerProperties properties_;
    std::unique_ptr<LevelStorage> storage_;
    LevelStorage::LevelData levelData_;
    std::unique_ptr<ServerLevel> overworld_;
    PlayerList playerList_;
    UserNameToIdResolver nameToIdCache_{"usercache.json"};
    std::unique_ptr<commands::Commands> commands_;
    UUID sessionId_ = UUID::random();
    ServerClockManager clocks_;
    GameRules gameRules_;
    std::optional<crypt::KeyPair> keyPair_;
    std::shared_ptr<SessionService> sessionService_ = std::make_shared<SessionService>();
    GameType defaultGameType_;
    uint64_t tickCount_ = 0;
    int nextEntityId_ = 1;
    bool running_ = true;

    int epollFd_ = -1;
    int listenFd_ = -1;
    std::string consoleBuffer_;
    std::vector<std::unique_ptr<Connection>> connections_;
};

} // namespace mc
