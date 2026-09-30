// Ports of ServerHandshakePacketListenerImpl, ServerStatusPacketListenerImpl and
// ServerLoginPacketListenerImpl.
#include "generated/packet_ids.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"

#include <mutex>
#include <random>
#include <thread>

namespace mc {

namespace pk = packets;

// ---------------------------------------------------------------- handshake

void ServerHandshakePacketListener::handlePacket(int id, ByteReader& in) {
    if (id != pk::handshake_sb::INTENTION) throw DecoderException("unexpected handshake packet");
    int protocol = in.readVarInt();
    std::string host = in.readUtf(255);
    in.readUnsignedShort();
    int intent = in.readVarInt();
    log::debug("{}: handshake protocol={} host={} intent={}", conn_.remoteAddress(), protocol, host, intent);

    switch (intent) {
    case 1: // STATUS
        conn_.setListener(std::make_unique<ServerStatusPacketListener>(server_, conn_));
        break;
    case 2:   // LOGIN
    case 3: { // TRANSFER
        bool transfer = intent == 3;
        conn_.setListener(std::make_unique<ServerLoginPacketListener>(server_, conn_, transfer));
        if (protocol != kProtocolVersion) {
            // Listener swap is deferred, so send the login-format disconnect directly.
            Packet p(pk::login_cb::LOGIN_DISCONNECT);
            Component reason = protocol < kProtocolVersion
                                   ? Component::translatable("multiplayer.disconnect.outdated_client", {Component::literal(kGameVersion)})
                                   : Component::translatable("multiplayer.disconnect.incompatible", {Component::literal(kGameVersion)});
            p.writeUtf(reason.toJson());
            conn_.send(p);
            conn_.close(reason);
        } else if (transfer && !server_.properties().acceptsTransfers) {
            Packet p(pk::login_cb::LOGIN_DISCONNECT);
            p.writeUtf(Component::translatable("multiplayer.disconnect.transfers_disabled").toJson());
            conn_.send(p);
            conn_.close(Component::literal("transfers disabled"));
        }
        break;
    }
    default: throw DecoderException("Unknown connection intent: " + std::to_string(intent));
    }
}

// ---------------------------------------------------------------- status

void ServerStatusPacketListener::handlePacket(int id, ByteReader& in) {
    if (id == pk::status_sb::STATUS_REQUEST) {
        if (hasRequestedStatus_) {
            conn_.close(Component::translatable("multiplayer.status.request_handled"));
            return;
        }
        hasRequestedStatus_ = true;
        const auto& props = server_.properties();
        auto& list = server_.playerList();
        std::string sample;
        int n = 0;
        for (const auto& p : list.players()) {
            if (!p->clientInfo.allowsListing || n >= 12) continue;
            sample += std::format("{}{{\"name\":{},\"id\":\"{}\"}}", n++ ? "," : "", jsonEscape(p->profile().name),
                                  p->profile().id.toString());
        }
        std::string json = std::format(
            "{{\"version\":{{\"name\":\"{}\",\"protocol\":{}}},\"players\":{{\"max\":{},\"online\":{}{}}},"
            "\"description\":{},\"enforcesSecureChat\":{}}}",
            kGameVersion, kProtocolVersion, props.maxPlayers, list.playerCount(),
            sample.empty() ? "" : ",\"sample\":[" + sample + "]", Component::literal(props.motd).toJson(),
            server_.enforceSecureProfile() ? "true" : "false");
        Packet p(pk::status_cb::STATUS_RESPONSE);
        p.writeUtf(json);
        conn_.send(p);
    } else if (id == pk::status_sb::PING_REQUEST) {
        Packet p(pk::status_cb::PONG_RESPONSE);
        p.writeLong(in.readLong());
        conn_.send(p);
        conn_.disconnect(Component::translatable("multiplayer.status.request_handled"));
    } else {
        throw DecoderException("unexpected status packet");
    }
}

// ---------------------------------------------------------------- login

namespace {
bool isValidPlayerName(const std::string& name) {
    if (name.empty() || name.size() > 16) return false;
    for (unsigned char c : name)
        if (c <= 32 || c >= 127) return false;
    return true;
}
} // namespace

struct ServerLoginPacketListener::Authentication {
    std::mutex mutex;
    bool done = false;
    SessionService::Result result;
};

void ServerLoginPacketListener::disconnect(const Component& reason) {
    log::info("Disconnecting {}: {}", requestedUsername_.empty() ? conn_.remoteAddress() : requestedUsername_ + " (" + conn_.remoteAddress() + ")",
              reason.getString());
    conn_.disconnect(reason);
}

void ServerLoginPacketListener::handlePacket(int id, ByteReader& in) {
    if (id == pk::login_sb::HELLO) {
        if (state_ != State::Hello) throw DecoderException("Unexpected hello packet");
        std::string name = in.readUtf(16);
        in.readUUID(); // (the id the client claims is never used)
        if (!isValidPlayerName(name)) throw DecoderException("Invalid characters in username");
        requestedUsername_ = name;
        if (server_.usesAuthentication()) {
            state_ = State::Key;
            uint32_t challenge = static_cast<uint32_t>(std::random_device{}()); // Ints.toByteArray(random int)
            for (int i = 0; i < 4; i++) challenge_[i] = static_cast<uint8_t>(challenge >> (24 - 8 * i));
            Packet p(pk::login_cb::HELLO); // ClientboundHelloPacket
            p.writeUtf("");                // server id
            p.writeByteArray(server_.keyPair().publicKeyDer());
            p.writeByteArray(challenge_);
            p.writeBool(true); // shouldAuthenticate
            conn_.send(p);
        } else {
            startClientVerification({UUID::offlinePlayer(name), name, {}}); // UUIDUtil.createOfflineProfile
        }
    } else if (id == pk::login_sb::KEY) {
        if (state_ != State::Key) throw DecoderException("Unexpected key packet");
        handleKey(in);
    } else if (id == pk::login_sb::LOGIN_ACKNOWLEDGED) {
        if (state_ != State::ProtocolSwitching) throw DecoderException("Unexpected login acknowledgement packet");
        state_ = State::Accepted;
        auto config = std::make_unique<ServerConfigurationPacketListener>(server_, conn_, profile_, ClientInformation{});
        auto* raw = config.get();
        conn_.setListener(std::move(config));
        raw->startConfiguration();
    } else if (id == pk::login_sb::CUSTOM_QUERY_ANSWER || id == pk::login_sb::COOKIE_RESPONSE) {
        disconnect(Component::translatable("multiplayer.disconnect.unexpected_query_response")); // (we never ask)
    } else {
        throw DecoderException("unexpected login packet " + std::to_string(id));
    }
}

void ServerLoginPacketListener::handleKey(ByteReader& in) {
    // ServerboundKeyPacket: the shared secret and our challenge, both encrypted with our public key.
    auto readArray = [&] {
        int32_t n = in.readVarInt();
        if (n < 0 || static_cast<size_t>(n) > in.remaining()) throw DecoderException("bad byte array");
        auto bytes = in.readBytes(static_cast<size_t>(n));
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };
    std::vector<uint8_t> keyBytes = readArray(), encryptedChallenge = readArray();
    const crypt::KeyPair& keys = server_.keyPair();
    auto challenge = keys.decrypt(encryptedChallenge);
    if (!challenge || *challenge != std::vector<uint8_t>(challenge_.begin(), challenge_.end())) throw DecoderException("Protocol error");
    auto secret = keys.decrypt(keyBytes);
    if (!secret || secret->size() != 16) throw DecoderException("Protocol error");
    std::string digest = crypt::serverIdHash("", *secret, keys.publicKeyDer());
    state_ = State::Authenticating;
    conn_.setEncryptionKey(*secret);

    // "User Authenticator #n": the session server is asked off the main thread.
    authentication_ = std::make_shared<Authentication>();
    std::optional<std::string> ip;
    if (server_.properties().preventProxyConnections) ip = conn_.remoteAddress().substr(0, conn_.remoteAddress().rfind(':'));
    std::thread([auth = authentication_, sessions = server_.sessionService(), name = requestedUsername_, digest, ip] {
        SessionService::Result result = sessions->hasJoinedServer(name, digest, ip);
        std::lock_guard lock(auth->mutex);
        auth->result = std::move(result);
        auth->done = true;
    }).detach();
}

void ServerLoginPacketListener::startClientVerification(GameProfile profile) {
    profile_ = std::move(profile);
    state_ = State::Verifying;
}

void ServerLoginPacketListener::verifyLoginAndFinishConnectionSetup() {
    auto& players = server_.playerList();
    if (auto reason = players.canPlayerLogin(profile_)) {
        disconnect(*reason);
        return;
    }
    int threshold = server_.properties().networkCompressionThreshold;
    if (threshold >= 0) {
        Packet p(pk::login_cb::LOGIN_COMPRESSION);
        p.writeVarInt(threshold);
        conn_.send(p);
        conn_.setCompressionThreshold(threshold);
    }
    if (players.disconnectAllPlayersWithProfile(profile_.id)) state_ = State::WaitingForDupeDisconnect;
    else finishLoginAndWaitForClient();
}

void ServerLoginPacketListener::finishLoginAndWaitForClient() {
    state_ = State::ProtocolSwitching;
    Packet p(pk::login_cb::LOGIN_FINISHED);
    p.writeUUID(profile_.id);
    p.writeUtf(profile_.name);
    writeProfileProperties(p, profile_);
    p.writeUUID(server_.sessionId());
    conn_.send(p);
    log::info("{} ({}) logged in from {}", profile_.name, profile_.id.toString(), conn_.remoteAddress());
}

void ServerLoginPacketListener::tick() {
    if (state_ == State::Authenticating) {
        std::lock_guard lock(authentication_->mutex);
        if (authentication_->done) {
            const auto& result = authentication_->result;
            if (result.outcome == SessionService::Outcome::Verified) {
                log::info("UUID of player {} is {}", result.profile.name, result.profile.id.toString());
                startClientVerification(result.profile);
            } else if (result.outcome == SessionService::Outcome::Unverified) {
                disconnect(Component::translatable("multiplayer.disconnect.unverified_username"));
                log::error("Username '{}' tried to join with an invalid session", requestedUsername_);
            } else {
                disconnect(Component::translatable("multiplayer.disconnect.authservers_down"));
                log::error("Couldn't verify username because servers are unavailable");
            }
        }
    }
    if (state_ == State::Verifying) verifyLoginAndFinishConnectionSetup();
    if (state_ == State::WaitingForDupeDisconnect && !server_.playerList().getPlayer(profile_.id)) finishLoginAndWaitForClient();
    if (ticks_++ == 600) disconnect(Component::translatable("multiplayer.disconnect.slow_login"));
}

} // namespace mc
