#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "server/level/server_player.hpp"
#include "util/json.hpp"

namespace mc {

// Port of authlib's MinecraftServicesSessionService.hasJoinedServer and the discovery document it
// finds the endpoint in (MinecraftServicesDiscoveryService): the verification step of online-mode
// logins. Thread safe; the calls block, so logins run them on worker threads.
//
// Like authlib, the discovery URL can be overridden (for tests or other environments) with the
// environment variables MINECRAFT_API_DISCOVERY_HOST (a full URL) or MINECRAFT_API_ENV
// (prod, staging), matching the minecraft.api.* system properties vanilla reads.
class SessionService : public std::enable_shared_from_this<SessionService> {
public:
    SessionService();

    enum class Outcome { Verified, Unverified, Unavailable };
    struct Result {
        Outcome outcome = Outcome::Unavailable;
        GameProfile profile; // when verified: the id and properties (skin) from the session server
    };
    // hasJoinedServer(name, serverIdHash, ip): did this player tell the session server they're
    // joining (the client posts the same hash)? `ip` only with prevent-proxy-connections.
    Result hasJoinedServer(const std::string& name, const std::string& serverIdHash, const std::optional<std::string>& ip);

    const std::string& discoveryUrl() const { return discoveryUrl_; }

    // MinecraftServicesProfileRepository.findProfileByName: the account with this name (PROFILES/
    // getByName), or nothing if there's none or the services can't be reached. Blocks.
    std::optional<std::pair<UUID, std::string>> findProfileByName(const std::string& name);

    // The services key set (MinecraftServicesKeyInfo; AUTHENTICATION/getPublicKeys): Mojang's keys
    // that sign players' chat keys. Fetched in the background, so these never block the caller:
    // until the set arrives there are no keys.
    void refreshKeysInBackground();
    // Services.canValidateProfileKeys
    bool canValidateProfileKeys();
    // Services.profileKeySignatureValidator().validate: SHA1withRSA by any of the keys.
    bool validateProfileKeySignature(std::span<const uint8_t> payload, std::span<const uint8_t> signature);

private:
    // An endpoint's uri from the (cached) discovery document.
    std::optional<std::string> endpoint(std::string_view service, std::string_view key);
    void fetchKeys();

    std::string discoveryUrl_;
    std::mutex discoveryMutex_; // (held while fetching: never taken on the main thread)
    std::optional<json::Value> discovery_;
    std::chrono::steady_clock::time_point discoveryFetchedAt_{};
    bool haveDiscovery_ = false;
    std::mutex mutex_; // the keys
    std::vector<std::vector<uint8_t>> profileKeys_; // playerCertificateKeys, X.509 DER
    std::chrono::steady_clock::time_point keysFetchedAt_{};
    bool haveKeys_ = false, fetchingKeys_ = false;
};

} // namespace mc
