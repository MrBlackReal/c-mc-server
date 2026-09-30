// Ports of ServerCommonPacketListenerImpl and ServerConfigurationPacketListenerImpl.
#include <chrono>

#include "core/registries.hpp"
#include "generated/packet_ids.hpp"
#include "nbt/json_nbt.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/json.hpp"
#include "util/log.hpp"

namespace mc {

namespace pk = packets;

namespace {
int64_t millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
constexpr int64_t kKeepAliveInterval = 15000;
} // namespace

ClientInformation readClientInformation(ByteReader& in) {
    ClientInformation info;
    info.language = in.readUtf(16);
    info.viewDistance = in.readByte();
    info.chatVisibility = in.readVarInt();
    info.chatColors = in.readBool();
    info.modelCustomisation = in.readUnsignedByte();
    info.mainHand = in.readVarInt();
    info.textFiltering = in.readBool();
    info.allowsListing = in.readBool();
    info.particleStatus = in.readVarInt();
    return info;
}

// ---------------------------------------------------------------- common

ServerCommonPacketListener::ServerCommonPacketListener(MinecraftServer& server, Connection& conn, GameProfile profile,
                                                       ClientInformation info)
    : server_(server), conn_(conn), profile_(std::move(profile)), clientInfo_(std::move(info)), keepAliveTime_(millis()) {}

void ServerCommonPacketListener::tick() {
    int64_t now = millis();
    if (now - keepAliveTime_ < kKeepAliveInterval) return;
    if (keepAlivePending_) {
        conn_.disconnect(Component::translatable("disconnect.timeout"));
        return;
    }
    keepAlivePending_ = true;
    keepAliveTime_ = now;
    keepAliveChallenge_ = now;
    sendKeepAlive(now);
}

bool ServerCommonPacketListener::handleCommon(int id, ByteReader& in, int keepAliveId, int pongId, int clientInfoId,
                                              int customPayloadId) {
    if (id == keepAliveId) {
        int64_t challenge = in.readLong();
        if (keepAlivePending_ && challenge == keepAliveChallenge_) {
            int elapsed = static_cast<int>(millis() - keepAliveTime_);
            latencyMs_ = (latencyMs_ * 3 + elapsed) / 4;
            keepAlivePending_ = false;
        } else {
            conn_.disconnect(Component::translatable("disconnect.timeout"));
        }
        return true;
    }
    if (id == pongId) {
        in.readInt();
        return true;
    }
    if (id == clientInfoId) {
        clientInfo_ = readClientInformation(in);
        onClientInformation(clientInfo_);
        return true;
    }
    if (id == customPayloadId) {
        std::string channel = in.readUtf();
        log::debug("{}: custom payload {} ({} bytes)", profile_.name, channel, in.remaining());
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- configuration

ServerConfigurationPacketListener::ServerConfigurationPacketListener(MinecraftServer& server, Connection& conn,
                                                                     GameProfile profile, ClientInformation info)
    : ServerCommonPacketListener(server, conn, std::move(profile), std::move(info)) {}

void ServerConfigurationPacketListener::sendKeepAlive(int64_t challenge) {
    Packet p(pk::configuration_cb::KEEP_ALIVE);
    p.writeLong(challenge);
    conn_.send(p);
}

void ServerConfigurationPacketListener::startConfiguration() {
    Packet brand(pk::configuration_cb::CUSTOM_PAYLOAD);
    brand.writeUtf("minecraft:brand");
    brand.writeUtf(kServerBrand);
    conn_.send(brand);

    Packet features(pk::configuration_cb::UPDATE_ENABLED_FEATURES);
    features.writeVarInt(static_cast<int32_t>(gen::kEnabledFeatures.size()));
    for (auto f : gen::kEnabledFeatures) features.writeUtf(f);
    conn_.send(features);

    // SynchronizeRegistriesTask.start
    task_ = Task::SynchronizeRegistries;
    Packet packs(pk::configuration_cb::SELECT_KNOWN_PACKS);
    packs.writeVarInt(static_cast<int32_t>(gen::kKnownPacks.size()));
    for (const auto& kp : gen::kKnownPacks) {
        packs.writeUtf(kp.ns);
        packs.writeUtf(kp.id);
        packs.writeUtf(kp.version);
    }
    conn_.send(packs);
}

void ServerConfigurationPacketListener::sendRegistries(bool clientKnowsPacks) {
    // A client with the vanilla core pack gets only the entry ids; one without (a modified
    // client) gets every element's data too (RegistrySynchronization.packRegistries).
    static const json::Value elements = [] {
        try {
            return json::parse(gen::kRegistryElementsJson);
        } catch (const std::exception& e) {
            log::error("Failed to parse registry elements: {}", e.what());
            return json::Value{};
        }
    }();
    for (const auto& reg : gen::kSyncedRegistries) {
        Packet p(pk::configuration_cb::REGISTRY_DATA);
        p.writeUtf(reg.name);
        p.writeVarInt(static_cast<int32_t>(reg.entries.size()));
        for (auto entry : reg.entries) {
            p.writeUtf(entry);
            const json::Value& data = elements[reg.name][entry];
            bool send = !clientKnowsPacks && !data.isNull();
            p.writeBool(send);
            if (send) nbt::writeNetwork(p, nbt::fromJson(data));
        }
        conn_.send(p);
    }
    Packet tags(pk::configuration_cb::UPDATE_TAGS);
    tags.writeVarInt(static_cast<int32_t>(gen::kTags.size()));
    for (const auto& reg : gen::kTags) {
        tags.writeUtf(reg.registry);
        tags.writeVarInt(static_cast<int32_t>(reg.tags.size()));
        for (const auto& tag : reg.tags) {
            tags.writeUtf(tag.name);
            tags.writeVarInt(static_cast<int32_t>(tag.ids.size()));
            for (int32_t v : tag.ids) tags.writeVarInt(v);
        }
    }
    conn_.send(tags);
}

void ServerConfigurationPacketListener::handlePacket(int id, ByteReader& in) {
    namespace sb = pk::configuration_sb;
    if (handleCommon(id, in, sb::KEEP_ALIVE, sb::PONG, sb::CLIENT_INFORMATION, sb::CUSTOM_PAYLOAD)) return;

    switch (id) {
    case sb::SELECT_KNOWN_PACKS: {
        if (task_ != Task::SynchronizeRegistries) throw DecoderException("unexpected known packs response");
        int n = in.readVarInt();
        bool matches = n == static_cast<int>(gen::kKnownPacks.size());
        for (int i = 0; i < n; i++) {
            std::string ns = in.readUtf(), pid = in.readUtf(), version = in.readUtf();
            if (matches && (ns != gen::kKnownPacks[i].ns || pid != gen::kKnownPacks[i].id || version != gen::kKnownPacks[i].version))
                matches = false;
        }
        sendRegistries(matches);
        if (!conn_.isConnected()) return;
        // Next tasks: code of conduct / resource pack (not configured), PrepareSpawn, JoinWorld.
        task_ = Task::JoinWorld;
        conn_.send(Packet(pk::configuration_cb::FINISH_CONFIGURATION));
        break;
    }
    case sb::FINISH_CONFIGURATION: {
        if (task_ != Task::JoinWorld) throw DecoderException("unexpected finish configuration");
        task_ = Task::None;
        if (server_.playerList().getPlayer(profile_.id)) {
            conn_.disconnect(Component::translatable("multiplayer.disconnect.duplicate_login"));
            return;
        }
        server_.playerList().placeNewPlayer(conn_, profile_, clientInfo_);
        break;
    }
    case sb::RESOURCE_PACK:
    case sb::COOKIE_RESPONSE:
    case sb::ACCEPT_CODE_OF_CONDUCT:
    case sb::CUSTOM_CLICK_ACTION: break;
    default: throw DecoderException("unexpected configuration packet " + std::to_string(id));
    }
}

void ServerConfigurationPacketListener::onDisconnect(const Component& reason) {
    log::info("{} ({}) lost connection: {}", profile_.name, profile_.id.toString(), reason.getString());
}

} // namespace mc
