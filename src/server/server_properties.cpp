#include "server/server_properties.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>

#include "util/log.hpp"

namespace mc {
namespace {
std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r");
    size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// java.util.Properties escaping, just enough for the values we write (e.g. "level-type=minecraft\:flat").
std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ':' || c == '=' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

struct Binder {
    std::map<std::string, std::string>& raw;

    void str(const char* key, std::string& field) {
        if (auto it = raw.find(key); it != raw.end()) field = it->second;
        else raw[key] = field;
    }
    void integer(const char* key, int& field) {
        if (auto it = raw.find(key); it != raw.end()) {
            int v;
            auto [p, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
            if (ec == std::errc()) field = v;
            else log::warn("Invalid integer for {}: {}", key, it->second);
        } else {
            raw[key] = std::to_string(field);
        }
    }
    void boolean(const char* key, bool& field) {
        if (auto it = raw.find(key); it != raw.end()) field = it->second == "true";
        else raw[key] = field ? "true" : "false";
    }
};
} // namespace

ServerProperties ServerProperties::load(const std::filesystem::path& path) {
    ServerProperties p;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '!') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        p.raw_[trim(line.substr(0, eq))] = unescape(trim(line.substr(eq + 1)));
    }
    Binder b{p.raw_};
    b.str("server-ip", p.serverIp);
    b.integer("server-port", p.serverPort);
    b.str("motd", p.motd);
    b.integer("max-players", p.maxPlayers);
    b.integer("view-distance", p.viewDistance);
    b.integer("simulation-distance", p.simulationDistance);
    b.integer("network-compression-threshold", p.networkCompressionThreshold);
    b.boolean("online-mode", p.onlineMode);
    b.boolean("hardcore", p.hardcore);
    b.str("gamemode", p.gamemode);
    b.str("difficulty", p.difficulty);
    b.boolean("accepts-transfers", p.acceptsTransfers);
    b.boolean("allow-flight", p.allowFlight);
    b.boolean("enforce-secure-profile", p.enforceSecureProfile);
    b.boolean("prevent-proxy-connections", p.preventProxyConnections);
    b.str("level-name", p.levelName);
    b.str("level-seed", p.levelSeed);
    b.str("level-type", p.levelType);
    b.boolean("sync-chunk-writes", p.syncChunkWrites);
    b.str("region-file-compression", p.regionFileCompression);
    b.integer("op-permission-level", p.opPermissionLevel);
    p.opPermissionLevel = std::clamp(p.opPermissionLevel, 0, 4);
    b.boolean("broadcast-console-to-ops", p.broadcastConsoleToOps);
    return p;
}

void ServerProperties::save(const std::filesystem::path& path) const {
    std::ofstream out(path);
    out << "#Minecraft server properties\n";
    for (const auto& [k, v] : raw_) out << k << '=' << escape(v) << '\n';
}

} // namespace mc
