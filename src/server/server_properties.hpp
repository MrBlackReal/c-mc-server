#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace mc {

// Port of DedicatedServerProperties: reads server.properties, fills in defaults and writes
// the file back so new keys appear for the user to edit.
class ServerProperties {
public:
    static ServerProperties load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;

    std::string serverIp;
    int serverPort = 25565;
    std::string motd = "A Minecraft Server";
    int maxPlayers = 20;
    int viewDistance = 10;
    int simulationDistance = 10;
    int networkCompressionThreshold = 256;
    bool onlineMode = true;
    bool hardcore = false;
    std::string gamemode = "survival";
    std::string difficulty = "easy";
    bool acceptsTransfers = false;
    bool allowFlight = false; // allow-flight: don't kick survival players for floating
    bool enforceSecureProfile = true;
    bool preventProxyConnections = false; // online mode: the session server checks the player's IP
    std::string levelName = "world";
    std::string levelSeed;
    // Only superflat generation exists so far, so that's our default instead of minecraft:normal.
    std::string levelType = "minecraft:flat";
    bool syncChunkWrites = true;
    std::string regionFileCompression = "deflate"; // RegionFileVersion: deflate, lz4 or none
    int opPermissionLevel = 4;        // op-permission-level: the level /op gives (operatorUserPermissions)
    bool broadcastConsoleToOps = true; // broadcast-console-to-ops

private:
    std::map<std::string, std::string> raw_;
};

} // namespace mc
