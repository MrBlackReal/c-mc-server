#include "network/chat/signed_chat.hpp"

#include <algorithm>
#include <climits>
#include <chrono>
#include <deque>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "nbt/tag.hpp"
#include "util/crypt.hpp"
#include "util/json.hpp"
#include "util/log.hpp"

namespace mc::chat {

namespace {
void putInt(std::vector<uint8_t>& out, int32_t v) { // Ints.toByteArray
    for (int i = 3; i >= 0; i--) out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
}
void putLong(std::vector<uint8_t>& out, int64_t v) { // Longs.toByteArray
    for (int i = 7; i >= 0; i--) out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i)));
}
void putUuid(std::vector<uint8_t>& out, const UUID& u) { // UUIDUtil.uuidToByteArray
    putLong(out, static_cast<int64_t>(u.most));
    putLong(out, static_cast<int64_t>(u.least));
}
int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
} // namespace

int32_t MessageSignature::checksum() const {
    int32_t h = 1;
    for (uint8_t b : bytes) h = static_cast<int32_t>(31u * static_cast<uint32_t>(h) + static_cast<uint32_t>(static_cast<int8_t>(b)));
    return h;
}

MessageSignature MessageSignature::read(ByteReader& in) {
    MessageSignature s;
    auto b = in.readBytes(256);
    std::copy(b.begin(), b.end(), s.bytes.begin());
    return s;
}

uint8_t LastSeenMessages::computeChecksum() const {
    int32_t checksum = 1;
    for (const auto& e : entries) checksum = static_cast<int32_t>(31u * static_cast<uint32_t>(checksum) + static_cast<uint32_t>(e.checksum()));
    auto b = static_cast<uint8_t>(checksum);
    return b == 0 ? 1 : b;
}

PlayerChatMessage PlayerChatMessage::unsignedFrom(const UUID& sender, std::string content) {
    // SignedMessageBody.unsigned (now, no salt, nothing seen) and SignedMessageLink.unsigned
    PlayerChatMessage m;
    m.link = {0, sender, UUID{}};
    m.body.content = std::move(content);
    m.body.timeStampMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return m;
}

std::vector<uint8_t> PlayerChatMessage::signedPayload() const {
    std::vector<uint8_t> out;
    putInt(out, 1);
    putUuid(out, link.sender); // SignedMessageLink.updateSignature
    putUuid(out, link.sessionId);
    putInt(out, link.index);
    putLong(out, body.salt); // SignedMessageBody.updateSignature
    putLong(out, floorDiv(body.timeStampMillis, 1000));
    putInt(out, static_cast<int32_t>(body.content.size()));
    out.insert(out.end(), body.content.begin(), body.content.end());
    putInt(out, static_cast<int32_t>(body.lastSeen.entries.size())); // LastSeenMessages.updateSignature
    for (const auto& e : body.lastSeen.entries) out.insert(out.end(), e.bytes.begin(), e.bytes.end());
    return out;
}

bool PlayerChatMessage::verify(const std::vector<uint8_t>& profileKeyDer) const {
    if (!signature) return false;
    return crypt::verifySignature(profileKeyDer, crypt::Digest::Sha256, signedPayload(), signature->bytes);
}

LastSeenUpdate LastSeenUpdate::read(ByteReader& in) {
    LastSeenUpdate u;
    u.offset = in.readVarInt();
    auto bits = in.readBytes(3); // ByteBufCodecs.fixedBitSet(20): BitSet.toByteArray order
    u.acknowledged = bits[0] | (static_cast<uint32_t>(bits[1]) << 8) | (static_cast<uint32_t>(bits[2]) << 16);
    u.checksum = in.readUnsignedByte();
    return u;
}

LastSeenMessagesValidator::LastSeenMessagesValidator() : tracked_(LastSeenMessages::kMaxLength) {}

void LastSeenMessagesValidator::addPending(const MessageSignature& signature) {
    if (lastPending_ == signature) return;
    tracked_.push_back(Entry{signature, true});
    lastPending_ = signature;
}

bool LastSeenMessagesValidator::applyOffset(int32_t offset, std::string& error) {
    int maxOffset = static_cast<int>(tracked_.size()) - LastSeenMessages::kMaxLength;
    if (offset < 0 || offset > maxOffset) {
        error = "Advanced last seen window by " + std::to_string(offset) + " messages, but expected at most " + std::to_string(maxOffset);
        return false;
    }
    tracked_.erase(tracked_.begin(), tracked_.begin() + offset);
    return true;
}

std::optional<LastSeenMessages> LastSeenMessagesValidator::applyUpdate(const LastSeenUpdate& update, std::string& error) {
    if (!applyOffset(update.offset, error)) return std::nullopt;
    if (update.acknowledged >> LastSeenMessages::kMaxLength) { // (can't be: 20 bits were read)
        error = "Last seen update contained too many messages";
        return std::nullopt;
    }
    LastSeenMessages lastSeen;
    for (int i = 0; i < LastSeenMessages::kMaxLength; i++) {
        bool acknowledged = update.acknowledged >> i & 1;
        auto& entry = tracked_[i];
        if (acknowledged) {
            if (!entry) {
                error = "Last seen update acknowledged unknown or previously ignored message at index " + std::to_string(i);
                return std::nullopt;
            }
            entry->pending = false;
            lastSeen.entries.push_back(entry->signature);
        } else {
            if (entry && !entry->pending) {
                error = "Last seen update ignored previously acknowledged message at index " + std::to_string(i);
                return std::nullopt;
            }
            entry.reset();
        }
    }
    if (update.checksum != 0 && update.checksum != lastSeen.computeChecksum()) {
        error = "Checksum mismatch on last seen update: the client and server must have desynced";
        return std::nullopt;
    }
    return lastSeen;
}

int MessageSignatureCache::pack(const MessageSignature& signature) const {
    for (size_t i = 0; i < entries_.size(); i++)
        if (entries_[i] == signature) return static_cast<int>(i);
    return -1;
}

void MessageSignatureCache::writePackedSignature(ByteWriter& out, const MessageSignature& signature) const {
    int id = pack(signature);
    out.writeVarInt(id + 1);
    if (id == -1) out.writeBytes(signature.bytes);
}

void MessageSignatureCache::writePackedBody(ByteWriter& out, const SignedMessageBody& body) const {
    out.writeUtf(body.content);
    out.writeLong(body.timeStampMillis);
    out.writeLong(body.salt);
    out.writeVarInt(static_cast<int32_t>(body.lastSeen.entries.size()));
    for (const auto& e : body.lastSeen.entries) writePackedSignature(out, e);
}

void MessageSignatureCache::push(const SignedMessageBody& body, const std::optional<MessageSignature>& signature) {
    std::deque<MessageSignature> queue(body.lastSeen.entries.begin(), body.lastSeen.entries.end());
    if (signature) queue.push_back(*signature);
    std::vector<MessageSignature> added(queue.begin(), queue.end());
    for (size_t i = 0; !queue.empty() && i < entries_.size(); i++) {
        std::optional<MessageSignature> entry = entries_[i];
        entries_[i] = queue.back();
        queue.pop_back();
        if (entry && std::find(added.begin(), added.end(), *entry) == added.end()) queue.push_front(*entry);
    }
}

ProfilePublicKeyData ProfilePublicKeyData::read(ByteReader& in) {
    ProfilePublicKeyData d;
    d.expiresAtMillis = in.readLong();
    auto readArray = [&](size_t max) {
        int32_t n = in.readVarInt();
        if (n < 0 || static_cast<size_t>(n) > max) throw DecoderException("ByteArray with size " + std::to_string(n) + " is bigger than allowed " + std::to_string(max));
        auto b = in.readBytes(static_cast<size_t>(n));
        return std::vector<uint8_t>(b.begin(), b.end());
    };
    d.key = readArray(512);
    if (!crypt::isRsaPublicKey(d.key)) throw DecoderException("Malformed public key");
    d.signature = readArray(4096);
    return d;
}

void ProfilePublicKeyData::write(ByteWriter& out) const {
    out.writeLong(expiresAtMillis);
    out.writeByteArray(key);
    out.writeByteArray(signature);
}

std::vector<uint8_t> ProfilePublicKeyData::signedPayload(const UUID& profileId) const {
    std::vector<uint8_t> out;
    putUuid(out, profileId);
    putLong(out, expiresAtMillis);
    out.insert(out.end(), key.begin(), key.end());
    return out;
}

void RemoteChatSession::write(ByteWriter& out) const {
    out.writeUUID(sessionId);
    profilePublicKey.write(out);
}

void ChatTypeBound::write(ByteWriter& out) const {
    out.writeVarInt(registries::id("minecraft:chat_type", type).value_or(0) + 1); // a registry holder
    nbt::writeNetwork(out, name.toNbt());
    out.writeBool(targetName.has_value());
    if (targetName) nbt::writeNetwork(out, targetName->toNbt());
}

Component ChatTypeBound::decorate(const Component& content) const {
    static const json::Value types = json::parse(gen::kRegistryElementsJson)["minecraft:chat_type"];
    const json::Value& decoration = types[type]["chat"];
    if (!decoration["translation_key"].isString()) return content;
    std::vector<Component> args; // ChatTypeDecoration.resolveParameters
    for (const auto& p : decoration["parameters"].array()) {
        const std::string& which = p.string();
        if (which == "sender") args.push_back(name);
        else if (which == "target") args.push_back(targetName.value_or(Component::literal("")));
        else args.push_back(content);
    }
    return Component::translatable(decoration["translation_key"].string(), std::move(args));
}

SignedMessageDecoder::SignedMessageDecoder(const UUID& profileId, const RemoteChatSession& session)
    : profileId_(profileId), session_(session), nextLink_(SignedMessageLink{0, profileId, session.sessionId}) {}

std::optional<PlayerChatMessage> SignedMessageDecoder::unpack(const std::optional<MessageSignature>& signature, SignedMessageBody body,
                                                              int64_t nowMillis, std::string& failure) {
    if (!session_) { // Decoder.unsigned
        if (enforce_) {
            failure = "chat.disabled.missingProfileKey";
            return std::nullopt;
        }
        return PlayerChatMessage::unsignedFrom(profileId_, std::move(body.content));
    }
    if (!signature) {
        failure = "chat.disabled.missingProfileKey";
        return std::nullopt;
    }
    if (session_->profilePublicKey.hasExpired(nowMillis)) {
        failure = "chat.disabled.expiredProfileKey";
        return std::nullopt;
    }
    if (!nextLink_) {
        failure = "chat.disabled.chain_broken";
        return std::nullopt;
    }
    if (body.timeStampMillis < lastTimeStampMillis_) {
        setChainBroken();
        failure = "chat.disabled.out_of_order_chat";
        return std::nullopt;
    }
    lastTimeStampMillis_ = body.timeStampMillis;
    PlayerChatMessage message{*nextLink_, signature, std::move(body), std::nullopt};
    if (!message.verify(session_->profilePublicKey.key)) {
        setChainBroken();
        failure = "chat.disabled.invalid_signature";
        return std::nullopt;
    }
    if (message.hasExpiredServer(nowMillis))
        log::warn("Received expired chat: '{}'. Is the client/server system time unsynchronized?", message.body.content);
    if (nextLink_->index == INT_MAX) nextLink_.reset(); // SignedMessageLink.advance
    else nextLink_->index++;
    return message;
}

} // namespace mc::chat
