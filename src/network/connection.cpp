#include "network/connection.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <cstring>

#include "generated/packet_ids.hpp"
#include "util/log.hpp"

namespace mc {
namespace {
constexpr size_t kMaxFrame = 2097151;           // Varint21FrameDecoder: 3 byte length prefix
constexpr size_t kMaxUncompressed = 8388608;    // CompressionDecoder.MAXIMUM_UNCOMPRESSED_LENGTH
constexpr size_t kMaxBuffered = 64 * 1024 * 1024; // drop peers that stop reading

std::vector<uint8_t> zlibCompress(std::span<const uint8_t> in) {
    uLongf size = compressBound(in.size());
    std::vector<uint8_t> out(size);
    if (compress2(out.data(), &size, in.data(), in.size(), Z_DEFAULT_COMPRESSION) != Z_OK) throw std::runtime_error("deflate failed");
    out.resize(size);
    return out;
}

std::vector<uint8_t> zlibDecompress(std::span<const uint8_t> in, size_t expected) {
    std::vector<uint8_t> out(expected);
    uLongf size = expected;
    if (uncompress(out.data(), &size, in.data(), in.size()) != Z_OK || size != expected)
        throw DecoderException("Badly compressed packet");
    return out;
}
} // namespace

Connection::Connection(int fd, std::string remoteAddress) : fd_(fd), remote_(std::move(remoteAddress)) {}

Connection::~Connection() {
    if (fd_ >= 0) ::close(fd_);
}

void Connection::setListener(std::unique_ptr<PacketListener> listener) {
    // Listeners switch protocol from inside their own handlers, so defer destroying the
    // current one until the dispatch loop regains control.
    if (listener_) pendingListener_ = std::move(listener);
    else listener_ = std::move(listener);
}

void Connection::send(const Packet& packet) {
    if (closed_ || closing_) return;
    ByteWriter frame;
    const auto& body = packet.data();
    if (compression_ >= 0) {
        ByteWriter inner;
        if (body.size() >= static_cast<size_t>(compression_)) {
            inner.writeVarInt(static_cast<int32_t>(body.size()));
            inner.writeBytes(zlibCompress(body));
        } else {
            inner.writeVarInt(0);
            inner.writeBytes(body);
        }
        frame.writeVarInt(static_cast<int32_t>(inner.size()));
        frame.writeBytes(inner.data());
    } else {
        frame.writeVarInt(static_cast<int32_t>(body.size()));
        frame.writeBytes(body);
    }
    size_t start = out_.size();
    out_.insert(out_.end(), frame.data().begin(), frame.data().end());
    if (encrypt_) encrypt_->update(out_.data() + start, out_.size() - start);
    if (out_.size() - outOffset_ > kMaxBuffered) close(Component::literal("Send buffer overflow"));
}

void Connection::setEncryptionKey(std::span<const uint8_t> sharedSecret) {
    encrypt_ = std::make_unique<crypt::Cfb8Cipher>(sharedSecret, true);
    decrypt_ = std::make_unique<crypt::Cfb8Cipher>(sharedSecret, false);
    decryptBuffered_ = true;
}

void Connection::disconnect(const Component& reason) {
    if (closed_ || closing_) return;
    switch (protocol()) {
    case ConnectionProtocol::Login: {
        Packet p(packets::login_cb::LOGIN_DISCONNECT);
        p.writeUtf(reason.toJson());
        send(p);
        break;
    }
    case ConnectionProtocol::Configuration:
    case ConnectionProtocol::Play: {
        Packet p(protocol() == ConnectionProtocol::Play ? packets::play_cb::DISCONNECT : packets::configuration_cb::DISCONNECT);
        nbt::writeNetwork(p, reason.toNbt());
        send(p);
        break;
    }
    default: break;
    }
    closing_ = true;
    disconnectReason_ = reason;
    if (listener_) listener_->onDisconnect(reason);
}

void Connection::close(const Component& reason) {
    if (closed_) return;
    bool notify = !closing_;
    closing_ = true;
    closed_ = true;
    if (notify && listener_) listener_->onDisconnect(reason);
    ::shutdown(fd_, SHUT_RDWR);
}

bool Connection::onReadable() {
    uint8_t buf[65536];
    while (!closed_) {
        ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n > 0) {
            if (decrypt_) decrypt_->update(buf, static_cast<size_t>(n));
            in_.insert(in_.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            close(Component::translatable("disconnect.endOfStream"));
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        close(Component::literal(std::strerror(errno)));
        break;
    }
    if (!closing_) processInbound();
    return !closed_;
}

void Connection::processInbound() {
    size_t pos = 0;
    try {
        while (!closing_ && pos < in_.size()) {
            int32_t length = 0;
            size_t header = tryReadVarInt(std::span(in_).subspan(pos), length);
            if (header == 0) break;
            if (length < 0 || static_cast<size_t>(length) > kMaxFrame) throw DecoderException("Bad packet length");
            if (in_.size() - pos - header < static_cast<size_t>(length)) break;
            std::span<const uint8_t> frame(in_.data() + pos + header, static_cast<size_t>(length));
            pos += header + static_cast<size_t>(length);
            dispatch(frame);
            if (decryptBuffered_) {
                decryptBuffered_ = false;
                decrypt_->update(in_.data() + pos, in_.size() - pos);
            }
        }
    } catch (const std::exception& e) {
        log::debug("{}: decode error: {}", remote_, e.what());
        disconnect(Component::translatable("disconnect.genericReason",
                                           {Component::literal(std::string("Internal Exception: ") + e.what())}));
    }
    in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(std::min(pos, in_.size())));
}

void Connection::dispatch(std::span<const uint8_t> frame) {
    std::vector<uint8_t> decompressed;
    if (compression_ >= 0) {
        ByteReader r(frame);
        int32_t dataLength = r.readVarInt();
        if (dataLength != 0) {
            if (dataLength < compression_) throw DecoderException("Badly compressed packet - size below threshold");
            if (static_cast<size_t>(dataLength) > kMaxUncompressed) throw DecoderException("Badly compressed packet - too large");
            decompressed = zlibDecompress(r.rest(), static_cast<size_t>(dataLength));
            frame = decompressed;
        } else {
            frame = r.rest();
        }
    }
    ByteReader in(frame);
    int id = in.readVarInt();
    if (!listener_) return;
    listener_->handlePacket(id, in);
    if (pendingListener_) listener_ = std::move(pendingListener_);
}

bool Connection::flush() {
    while (outOffset_ < out_.size()) {
        ssize_t n = ::send(fd_, out_.data() + outOffset_, out_.size() - outOffset_, MSG_NOSIGNAL);
        if (n > 0) {
            outOffset_ += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        close(Component::literal("Connection reset"));
        return false;
    }
    if (outOffset_ == out_.size()) {
        out_.clear();
        outOffset_ = 0;
        if (closing_ && !closed_) {
            closed_ = true;
            ::shutdown(fd_, SHUT_RDWR);
        }
    } else if (outOffset_ > (1 << 20)) {
        out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(outOffset_));
        outOffset_ = 0;
    }
    return !closed_;
}

void Connection::tick() {
    if (listener_ && !closing_) {
        listener_->tick();
        if (pendingListener_) listener_ = std::move(pendingListener_);
    }
}

} // namespace mc
