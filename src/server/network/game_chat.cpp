// Secure chat in ServerGamePacketListenerImpl: chat and commands (signed or not), chat sessions
// and message acknowledgements.
#include <chrono>

#include "generated/packet_ids.hpp"
#include "server/commands/commands.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"

namespace mc {

namespace cb = packets::play_cb;

namespace {
int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
// StringUtil.isAllowedChatCharacter (per UTF-16 unit; the text arrives as UTF-8)
bool isChatMessageIllegal(const std::string& message) {
    for (size_t i = 0; i < message.size(); i++) {
        auto c = static_cast<unsigned char>(message[i]);
        if (c < 32 || c == 127) return true;
        if (c == 0xC2 && i + 1 < message.size() && static_cast<unsigned char>(message[i + 1]) == 0xA7) return true; // U+00A7 §
    }
    return false;
}
std::optional<chat::MessageSignature> readOptionalSignature(ByteReader& in) {
    if (!in.readBool()) return std::nullopt;
    return chat::MessageSignature::read(in);
}
} // namespace

bool ServerGamePacketListener::tryHandleChat(const std::string& message, bool isCommand) {
    if (isChatMessageIllegal(message)) {
        conn_.disconnect(Component::translatable("multiplayer.disconnect.illegal_characters"));
        return false;
    }
    if (!isCommand && player_.clientInfo.chatVisibility == 2) { // ChatVisiblity.HIDDEN
        Packet p(cb::SYSTEM_CHAT);
        nbt::writeNetwork(p, Component::translatable("chat.disabled.options").withColor("red").toNbt());
        p.writeBool(false);
        conn_.send(p);
        return false;
    }
    return true;
}

std::optional<chat::LastSeenMessages> ServerGamePacketListener::unpackAndApplyLastSeen(const chat::LastSeenUpdate& update) {
    std::string error;
    auto result = lastSeenMessages_.applyUpdate(update, error);
    if (!result) {
        log::error("Failed to validate message acknowledgements from {}: {}", player_.profile().name, error);
        conn_.disconnect(Component::translatable("multiplayer.disconnect.chat_validation_failed"));
    }
    return result;
}

void ServerGamePacketListener::handleMessageDecodeFailure(const std::string& translationKey) {
    Component reason = Component::translatable(translationKey);
    log::warn("Failed to update secure chat state for {}: '{}'", player_.profile().name, reason.getString());
    Component red = reason;
    sendSystemMessage(red.withColor("red"));
}

void ServerGamePacketListener::handleChat(ByteReader& in) {
    // ServerboundChatPacket
    std::string message = in.readUtf(256);
    int64_t timeStamp = in.readLong();
    int64_t salt = in.readLong();
    auto signature = readOptionalSignature(in);
    auto update = chat::LastSeenUpdate::read(in);
    auto lastSeen = unpackAndApplyLastSeen(update);
    if (!lastSeen || !tryHandleChat(message, false)) return;
    std::string failure;
    auto signedMessage = signedMessageDecoder_.unpack(signature, {message, timeStamp, salt, std::move(*lastSeen)}, nowMillis(), failure);
    if (!signedMessage) {
        handleMessageDecodeFailure(failure);
        return;
    }
    // (ChatDecorator.PLAIN: the decorated content is the text itself, so no unsigned content)
    server_.playerList().broadcastChatMessage(*signedMessage, &player_, {"minecraft:chat", player_.displayName(), std::nullopt});
}

void ServerGamePacketListener::handleSignedCommand(ByteReader& in) {
    // ServerboundChatCommandSignedPacket
    std::string command = in.readUtf();
    int64_t timeStamp = in.readLong();
    int64_t salt = in.readLong();
    struct ArgumentSignature {
        std::string name;
        chat::MessageSignature signature;
    };
    std::vector<ArgumentSignature> argumentSignatures;
    int count = in.readVarInt();
    if (count < 0 || count > 8) throw DecoderException("too many argument signatures");
    for (int i = 0; i < count; i++) {
        std::string name = in.readUtf(16);
        argumentSignatures.push_back({std::move(name), chat::MessageSignature::read(in)});
    }
    auto update = chat::LastSeenUpdate::read(in);
    auto lastSeen = unpackAndApplyLastSeen(update);
    if (!lastSeen || !tryHandleChat(command, true)) return;

    // SignableCommand: the arguments of type minecraft:message. TODO: Brigadier; so far /say's.
    std::vector<std::pair<std::string, std::string>> signable;
    if (command.starts_with("say ")) signable.emplace_back("message", command.substr(4));

    std::map<std::string, chat::PlayerChatMessage> signedArguments; // collectSignedArguments
    std::string failure;
    if (argumentSignatures.empty()) {
        for (const auto& [name, value] : signable) { // collectUnsignedArguments
            auto m = signedMessageDecoder_.unpack(std::nullopt, {value, nowMillis(), 0, {}}, nowMillis(), failure);
            if (!m) return handleMessageDecodeFailure(failure);
            signedArguments.emplace(name, std::move(*m));
        }
    } else {
        auto mismatch = [&] {
            log::error("Signed command mismatch between server and client ('{}')", command);
            handleMessageDecodeFailure("chat.disabled.invalid_command_signature");
        };
        for (const auto& arg : argumentSignatures) {
            auto expected = std::find_if(signable.begin(), signable.end(), [&](const auto& s) { return s.first == arg.name; });
            if (expected == signable.end()) {
                signedMessageDecoder_.setChainBroken();
                return mismatch();
            }
            auto m = signedMessageDecoder_.unpack(arg.signature, {expected->second, timeStamp, salt, *lastSeen}, nowMillis(), failure);
            if (!m) return handleMessageDecodeFailure(failure);
            signedArguments.emplace(arg.name, std::move(*m));
        }
        for (const auto& [name, value] : signable)
            if (!signedArguments.contains(name)) return mismatch();
    }
    handleCommand(command, &signedArguments);
}

void ServerGamePacketListener::handleCommand(const std::string& command, const std::map<std::string, chat::PlayerChatMessage>* signedArguments) {
    log::info("{} issued server command: /{}", player_.profile().name, command);
    // performUnsignedChatCommand: with secure chat enforced, commands with message arguments must be signed.
    if (!signedArguments && server_.enforceSecureProfile() && command.starts_with("say ")) {
        log::error("Received unsigned command packet from {}, but the command requires signable arguments: {}", player_.profile().name, command);
        sendSystemMessage(Component::translatable("chat.disabled.invalid_command_signature").withColor("red"));
        return;
    }
    commands::execute({server_, this, signedArguments}, command);
}

void ServerGamePacketListener::handleChatSessionUpdate(ByteReader& in) {
    // ServerboundChatSessionUpdatePacket (RemoteChatSession.Data)
    chat::RemoteChatSession session;
    session.sessionId = in.readUUID();
    session.profilePublicKey = chat::ProfilePublicKeyData::read(in);
    const auto& oldSession = player_.chatSession;
    if (oldSession && oldSession->profilePublicKey == session.profilePublicKey) return;
    if (oldSession && session.profilePublicKey.expiresAtMillis < oldSession->profilePublicKey.expiresAtMillis) {
        conn_.disconnect(Component::translatable("multiplayer.disconnect.expired_public_key"));
        return;
    }
    auto sessions = server_.sessionService();
    if (!sessions->canValidateProfileKeys()) {
        log::warn("Ignoring chat session from {} due to missing Services public key", player_.profile().name);
        return;
    }
    // RemoteChatSession.Data.validate -> ProfilePublicKey.createValidated
    if (!sessions->validateProfileKeySignature(session.profilePublicKey.signedPayload(player_.profile().id), session.profilePublicKey.signature)) {
        log::error("Failed to validate profile key: invalid signature");
        conn_.disconnect(Component::translatable("multiplayer.disconnect.invalid_public_key_signature"));
        return;
    }
    resetPlayerChatState(std::move(session));
}

void ServerGamePacketListener::resetPlayerChatState(chat::RemoteChatSession session) {
    signedMessageDecoder_ = chat::SignedMessageDecoder(player_.profile().id, session);
    player_.chatSession = std::move(session);
    server_.playerList().broadcastChatSession(player_); // (INITIALIZE_CHAT to everyone)
}

void ServerGamePacketListener::handleChatAck(ByteReader& in) {
    std::string error;
    if (!lastSeenMessages_.applyOffset(in.readVarInt(), error)) {
        log::error("Failed to validate message acknowledgement offset from {}: {}", player_.profile().name, error);
        conn_.disconnect(Component::translatable("multiplayer.disconnect.chat_validation_failed"));
    }
}

void ServerGamePacketListener::sendPlayerChatMessage(const chat::PlayerChatMessage& message, const chat::ChatTypeBound& chatType) {
    Packet p(cb::PLAYER_CHAT); // ClientboundPlayerChatPacket
    p.writeVarInt(nextChatIndex_++);
    p.writeUUID(message.link.sender);
    p.writeVarInt(message.link.index);
    p.writeBool(message.signature.has_value());
    if (message.signature) p.writeBytes(message.signature->bytes);
    messageSignatureCache_.writePackedBody(p, message.body);
    p.writeBool(message.unsignedContent.has_value());
    if (message.unsignedContent) nbt::writeNetwork(p, message.unsignedContent->toNbt());
    p.writeVarInt(0); // FilterMask PASS_THROUGH
    chatType.write(p);
    conn_.send(p);
    if (message.signature) {
        messageSignatureCache_.push(message.body, message.signature);
        lastSeenMessages_.addPending(*message.signature);
        if (lastSeenMessages_.trackedMessagesCount() > 4096)
            conn_.disconnect(Component::translatable("multiplayer.disconnect.too_many_pending_chats"));
    }
}

void ServerGamePacketListener::sendDisguisedChatMessage(const Component& content, const chat::ChatTypeBound& chatType) {
    Packet p(cb::DISGUISED_CHAT);
    nbt::writeNetwork(p, content.toNbt());
    chatType.write(p);
    conn_.send(p);
}

} // namespace mc
