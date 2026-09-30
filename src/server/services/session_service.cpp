#include "server/services/session_service.hpp"

#include <cstdlib>

#include <thread>

#include "util/base64.hpp"
#include "util/crypt.hpp"
#include "util/http_client.hpp"
#include "util/json.hpp"
#include "util/log.hpp"

namespace mc {

namespace {
// MinecraftServicesEnvironment
constexpr const char* kProdDiscovery = "https://discovery.minecraftservices.com/minecraft/client";
constexpr const char* kStagingDiscovery = "https://discovery-staging.minecraftservices.com/minecraft/client";
// RetryableFetch(.., 24, 1): a good document is kept a day; after a failure, retry in a minute.
constexpr auto kKeepFor = std::chrono::hours(24);
constexpr auto kRetryAfter = std::chrono::minutes(1);
} // namespace

SessionService::SessionService() {
    // EnvironmentParser.getEnvironmentFromProperties
    const char* env = std::getenv("MINECRAFT_API_ENV");
    const char* host = std::getenv("MINECRAFT_API_DISCOVERY_HOST");
    if (env && std::string_view(env) == "staging") discoveryUrl_ = kStagingDiscovery;
    else if (env && std::string_view(env) == "prod") discoveryUrl_ = kProdDiscovery;
    else if (host && *host) discoveryUrl_ = host;
    else discoveryUrl_ = kProdDiscovery;
}

std::optional<std::string> SessionService::endpoint(std::string_view service, std::string_view key) {
    std::lock_guard lock(discoveryMutex_);
    auto now = std::chrono::steady_clock::now();
    bool fresh = haveDiscovery_ && now - discoveryFetchedAt_ < (discovery_ ? kKeepFor : kRetryAfter);
    if (!fresh) {
        std::string error;
        auto response = http::get(discoveryUrl_, error);
        haveDiscovery_ = true;
        discoveryFetchedAt_ = now;
        discovery_.reset();
        if (!response || response->status != 200) {
            log::warn("Services are unavailable (discovery {}: {})", discoveryUrl_,
                      response ? "HTTP " + std::to_string(response->status) : error);
        } else {
            try {
                json::Value doc = json::parse(response->body);
                if (!(doc["environment"].isString() && doc["environment"].string() == "offline")) discovery_ = std::move(doc);
            } catch (const std::exception& e) {
                log::warn("Bad discovery document from {}: {}", discoveryUrl_, e.what());
            }
        }
    }
    if (!discovery_) return std::nullopt;
    const json::Value& e = (*discovery_)["discovery"][service]["endpoints"][key];
    if (!e["uri"].isString()) {
        log::warn("Services are available, but unable to find uri for {}/{}", service, key);
        return std::nullopt;
    }
    return e["uri"].string();
}

std::optional<std::pair<UUID, std::string>> SessionService::findProfileByName(const std::string& name) {
    auto url = endpoint("profiles", "getByName");
    if (!url) return std::nullopt;
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string target = *url;
    if (size_t at = target.find("{name}"); at != std::string::npos) target.replace(at, 6, http::urlEncode(lower));
    std::string error;
    auto response = http::get(target, error);
    if (!response || response->status != 200) {
        log::warn("Couldn't find profile with name: {}", name);
        return std::nullopt;
    }
    try {
        json::Value doc = json::parse(response->body);
        if (!doc["id"].isString() || !doc["name"].isString()) return std::nullopt;
        auto id = UUID::parse(doc["id"].string());
        if (!id) return std::nullopt;
        return std::pair{*id, doc["name"].string()};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void SessionService::refreshKeysInBackground() {
    {
        std::lock_guard lock(mutex_);
        if (fetchingKeys_) return;
        auto age = std::chrono::steady_clock::now() - keysFetchedAt_;
        if (haveKeys_ && age < (profileKeys_.empty() ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::minutes(5)) : kKeepFor))
            return;
        fetchingKeys_ = true;
    }
    std::thread([self = shared_from_this()] { self->fetchKeys(); }).detach();
}

void SessionService::fetchKeys() {
    std::vector<std::vector<uint8_t>> keys;
    if (auto url = endpoint("authentication", "getPublicKeys")) {
        std::string error;
        auto response = http::get(*url, error);
        if (!response || response->status != 200) {
            log::error("Failed to request Minecraft Services public key: {}", response ? "HTTP " + std::to_string(response->status) : error);
        } else {
            try {
                json::Value doc = json::parse(response->body);
                if (doc["playerCertificateKeys"].isArray())
                    for (const auto& k : doc["playerCertificateKeys"].array())
                        if (k["publicKey"].isString()) {
                            auto der = base64::decode(k["publicKey"].string());
                            if (der && crypt::isRsaPublicKey(*der)) keys.push_back(std::move(*der));
                        }
            } catch (const std::exception& e) {
                log::error("Received malformed Minecraft Services public key data: {}", e.what());
            }
        }
    }
    std::lock_guard lock(mutex_);
    profileKeys_ = std::move(keys);
    haveKeys_ = true;
    fetchingKeys_ = false;
    keysFetchedAt_ = std::chrono::steady_clock::now();
}

bool SessionService::canValidateProfileKeys() {
    refreshKeysInBackground();
    std::lock_guard lock(mutex_);
    return !profileKeys_.empty();
}

bool SessionService::validateProfileKeySignature(std::span<const uint8_t> payload, std::span<const uint8_t> signature) {
    std::lock_guard lock(mutex_);
    for (const auto& key : profileKeys_)
        if (crypt::verifySignature(key, crypt::Digest::Sha1, payload, signature)) return true;
    return false;
}

SessionService::Result SessionService::hasJoinedServer(const std::string& name, const std::string& serverIdHash,
                                                       const std::optional<std::string>& ip) {
    Result result;
    auto base = endpoint("session", "verify");
    if (!base) return result; // Unavailable
    // HttpDiscoveryService.buildQuery / concatenateURL
    std::string url = *base + (base->find('?') == std::string::npos ? "?" : "&") + "username=" + http::urlEncode(name) +
                      "&serverId=" + http::urlEncode(serverIdHash);
    if (ip) url += "&ip=" + http::urlEncode(*ip);
    std::string error;
    auto response = http::get(url, error);
    if (!response || response->status >= 500) { // (MinecraftClientException -> AuthenticationUnavailableException)
        log::warn("Couldn't reach the session server: {}", response ? "HTTP " + std::to_string(response->status) : error);
        return result;
    }
    result.outcome = Outcome::Unverified;
    if (response->status != 200 || response->body.empty()) return result; // (204: not joined)
    try {
        json::Value doc = json::parse(response->body);
        if (!doc["id"].isString()) return result;
        auto id = UUID::parse(doc["id"].string());
        if (!id) return result;
        result.profile.id = *id;
        result.profile.name = name; // (the profile keeps the requested name)
        if (doc["properties"].isArray()) {
            for (const auto& p : doc["properties"].array()) {
                ProfileProperty prop;
                prop.name = p["name"].isString() ? p["name"].string() : "";
                prop.value = p["value"].isString() ? p["value"].string() : "";
                if (p["signature"].isString()) prop.signature = p["signature"].string();
                result.profile.properties.push_back(std::move(prop));
            }
        }
        result.outcome = Outcome::Verified;
    } catch (const std::exception& e) {
        log::warn("Bad response from the session server: {}", e.what());
    }
    return result;
}

} // namespace mc
