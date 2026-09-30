#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "network/chat/component.hpp"
#include "network/friendly_byte_buf.hpp"
#include "util/crypt.hpp"

namespace mc {

enum class ConnectionProtocol { Handshake, Status, Login, Configuration, Play };

// A serialised packet: id followed by its body.
class Packet : public ByteWriter {
public:
    explicit Packet(int id) { writeVarInt(id); }
};

class Connection;

// Port of PacketListener: one implementation per protocol state.
class PacketListener {
public:
    virtual ~PacketListener() = default;
    virtual ConnectionProtocol protocol() const = 0;
    virtual void handlePacket(int id, ByteReader& in) = 0;
    virtual void tick() {}
    virtual void onDisconnect(const Component& reason) {}
};

// Port of net.minecraft.network.Connection: framing, compression and the active listener.
// Non-blocking; driven by the server's event loop.
class Connection {
public:
    Connection(int fd, std::string remoteAddress);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    int fd() const { return fd_; }
    const std::string& remoteAddress() const { return remote_; }
    bool isConnected() const { return !closed_ && !closing_; }
    bool isClosed() const { return closed_; }
    bool wantsWrite() const { return outOffset_ < out_.size(); }

    void setListener(std::unique_ptr<PacketListener> listener);
    PacketListener* listener() const { return listener_.get(); }
    ConnectionProtocol protocol() const { return listener_ ? listener_->protocol() : ConnectionProtocol::Handshake; }

    void send(const Packet& packet);
    void setCompressionThreshold(int threshold) { compression_ = threshold; }
    // Connection.setEncryptionKey: AES/CFB8 with the shared secret from here on, both ways (bytes
    // already received after the current packet are decrypted too).
    void setEncryptionKey(std::span<const uint8_t> sharedSecret);

    // Sends the protocol-appropriate disconnect packet, then closes once flushed.
    void disconnect(const Component& reason);
    // Closes immediately, without notifying the peer.
    void close(const Component& reason);

    // Event loop hooks. Return false once the connection is closed.
    bool onReadable();
    bool flush();
    void tick();

private:
    void processInbound();
    void dispatch(std::span<const uint8_t> frame);

    int fd_;
    std::string remote_;
    std::vector<uint8_t> in_;
    std::vector<uint8_t> out_;
    size_t outOffset_ = 0;
    int compression_ = -1;
    bool closing_ = false;
    bool closed_ = false;
    Component disconnectReason_;
    std::unique_ptr<PacketListener> listener_;
    std::unique_ptr<PacketListener> pendingListener_;
    std::unique_ptr<crypt::Cfb8Cipher> encrypt_, decrypt_;
    bool decryptBuffered_ = false; // encryption began while dispatching: decrypt what's left in in_
};

} // namespace mc
