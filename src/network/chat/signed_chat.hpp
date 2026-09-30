#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "network/chat/component.hpp"
#include "network/friendly_byte_buf.hpp"
#include "util/uuid.hpp"

// Ports of the secure chat pieces in net.minecraft.network.chat: signed message bodies, links and
// chains, last-seen acknowledgements and the signature cache, and players' chat sessions.
namespace mc::chat {

// MessageSignature: 256 bytes (SHA256withRSA by the sender's profile key).
struct MessageSignature {
    std::array<uint8_t, 256> bytes{};
    bool operator==(const MessageSignature&) const = default;
    int32_t checksum() const; // Arrays.hashCode(bytes)
    static MessageSignature read(ByteReader& in);
};

// LastSeenMessages: the signatures a sender had seen, signed along with the message.
struct LastSeenMessages {
    std::vector<MessageSignature> entries;
    static constexpr int kMaxLength = 20;
    uint8_t computeChecksum() const;
};

struct SignedMessageLink {
    int32_t index = 0;
    UUID sender, sessionId;
};

struct SignedMessageBody {
    std::string content;
    int64_t timeStampMillis = 0; // (Instant, as ByteBufCodecs.INSTANT sends it)
    int64_t salt = 0;
    LastSeenMessages lastSeen;
};

// PlayerChatMessage (filter masks are always PASS_THROUGH: no text filtering server).
struct PlayerChatMessage {
    SignedMessageLink link;
    std::optional<MessageSignature> signature;
    SignedMessageBody body;
    std::optional<Component> unsignedContent;

    // PlayerChatMessage.unsigned / system (the nil sender)
    static PlayerChatMessage unsignedFrom(const UUID& sender, std::string content);
    static PlayerChatMessage system(std::string content) { return unsignedFrom(UUID{}, std::move(content)); }
    bool isSystem() const { return link.sender == UUID{}; }
    // updateSignature: what the signature covers.
    std::vector<uint8_t> signedPayload() const;
    bool verify(const std::vector<uint8_t>& profileKeyDer) const;
    // hasExpiredServer: older than 5 minutes.
    bool hasExpiredServer(int64_t nowMillis) const { return nowMillis > body.timeStampMillis + 5 * 60 * 1000; }
    // decoratedContent: the unsigned (decorated) content, or the signed text.
    Component decoratedContent() const { return unsignedContent ? *unsignedContent : Component::literal(body.content); }
};

// LastSeenMessages.Update from the client: which of the window it acknowledges.
struct LastSeenUpdate {
    int32_t offset = 0;
    uint32_t acknowledged = 0; // fixedBitSet(20)
    uint8_t checksum = 0;      // 0: ignore
    static LastSeenUpdate read(ByteReader& in);
};

// LastSeenMessagesValidator: the server's view of a client's last-seen window.
class LastSeenMessagesValidator {
public:
    LastSeenMessagesValidator();
    void addPending(const MessageSignature& signature);
    int trackedMessagesCount() const { return static_cast<int>(tracked_.size()); }
    // Empty with `error` set when the update doesn't fit (ValidationException).
    bool applyOffset(int32_t offset, std::string& error);
    std::optional<LastSeenMessages> applyUpdate(const LastSeenUpdate& update, std::string& error);

private:
    struct Entry {
        MessageSignature signature;
        bool pending;
    };
    std::vector<std::optional<Entry>> tracked_;
    std::optional<MessageSignature> lastPending_;
};

// MessageSignatureCache: the signatures a client has, so outgoing messages can refer to them by index.
class MessageSignatureCache {
public:
    // MessageSignature.Packed / LastSeenMessages.Packed writers.
    void writePackedSignature(ByteWriter& out, const MessageSignature& signature) const;
    void writePackedBody(ByteWriter& out, const SignedMessageBody& body) const; // SignedMessageBody.Packed
    void push(const SignedMessageBody& body, const std::optional<MessageSignature>& signature);

private:
    int pack(const MessageSignature& signature) const;
    std::array<std::optional<MessageSignature>, 128> entries_;
};

// ProfilePublicKey.Data / RemoteChatSession.Data
struct ProfilePublicKeyData {
    int64_t expiresAtMillis = 0;
    std::vector<uint8_t> key;       // X.509 DER
    std::vector<uint8_t> signature; // by Mojang's services key
    bool operator==(const ProfilePublicKeyData&) const = default;
    static ProfilePublicKeyData read(ByteReader& in);
    void write(ByteWriter& out) const;
    // signedPayload: profile id, expiry and key, as the services key signed them.
    std::vector<uint8_t> signedPayload(const UUID& profileId) const;
    bool hasExpired(int64_t nowMillis, int64_t graceMillis = 0) const { return expiresAtMillis + graceMillis < nowMillis; }
};
struct RemoteChatSession {
    UUID sessionId;
    ProfilePublicKeyData profilePublicKey;
    void write(ByteWriter& out) const; // RemoteChatSession.Data.STREAM_CODEC
};

// ChatType.Bound: a minecraft:chat_type with the sender's name (and a target, for /msg).
struct ChatTypeBound {
    std::string type; // e.g. "minecraft:chat"
    Component name;
    std::optional<Component> targetName;
    void write(ByteWriter& out) const; // ChatType.Bound.STREAM_CODEC
    // ChatType.Bound.decorate: the chat decoration applied to `content` (for the server log).
    Component decorate(const Component& content) const;
};

// SignedMessageChain.Decoder: turns incoming chat into PlayerChatMessages, checking signatures and
// the chain (index, ordering) when the player has a chat session.
class SignedMessageDecoder {
public:
    // Decoder.unsigned: without a session (refuses everything while secure chat is enforced).
    SignedMessageDecoder(const UUID& profileId, bool enforcesSecureChat) : profileId_(profileId), enforce_(enforcesSecureChat) {}
    // RemoteChatSession.createMessageDecoder
    SignedMessageDecoder(const UUID& profileId, const RemoteChatSession& session);

    // unpack: the message, or empty with `failure` set (a chat.disabled.* translation key).
    std::optional<PlayerChatMessage> unpack(const std::optional<MessageSignature>& signature, SignedMessageBody body, int64_t nowMillis,
                                            std::string& failure);
    void setChainBroken() { nextLink_.reset(); }

private:
    UUID profileId_;
    bool enforce_ = false;
    std::optional<RemoteChatSession> session_;
    std::optional<SignedMessageLink> nextLink_;
    int64_t lastTimeStampMillis_ = 0;
};

} // namespace mc::chat
