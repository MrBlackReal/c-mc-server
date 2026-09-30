#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "util/uuid.hpp"

namespace mc {

struct DecoderException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

int varIntSize(int32_t value);

// Growable big-endian output buffer, mirrors the write side of FriendlyByteBuf.
class ByteWriter {
public:
    std::vector<uint8_t>& data() { return buf_; }
    const std::vector<uint8_t>& data() const { return buf_; }
    size_t size() const { return buf_.size(); }

    void writeBytes(const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    void writeBytes(std::span<const uint8_t> s) { writeBytes(s.data(), s.size()); }
    void writeByte(uint8_t v) { buf_.push_back(v); }
    void writeBool(bool v) { writeByte(v ? 1 : 0); }
    void writeShort(int16_t v) { writeBE(static_cast<uint16_t>(v)); }
    void writeInt(int32_t v) { writeBE(static_cast<uint32_t>(v)); }
    void writeLong(int64_t v) { writeBE(static_cast<uint64_t>(v)); }
    void writeFloat(float v) { writeBE(std::bit_cast<uint32_t>(v)); }
    void writeDouble(double v) { writeBE(std::bit_cast<uint64_t>(v)); }
    void writeVarInt(int32_t v);
    void writeVarLong(int64_t v);
    void writeUtf(std::string_view s);
    void writeUUID(const UUID& u) {
        writeLong(static_cast<int64_t>(u.most));
        writeLong(static_cast<int64_t>(u.least));
    }
    // BlockPos.asLong
    void writeBlockPos(int x, int y, int z);
    // java.util.BitSet as sent by ByteBufCodecs.BIT_SET
    void writeBitSet(std::span<const uint64_t> words);
    void writeFixedSizeLongArray(std::span<const uint64_t> words) {
        for (uint64_t w : words) writeLong(static_cast<int64_t>(w));
    }
    void writeLongArray(std::span<const uint64_t> words) {
        writeVarInt(static_cast<int32_t>(words.size()));
        writeFixedSizeLongArray(words);
    }
    // LpVec3: the compact velocity encoding (Vec3.LP_STREAM_CODEC).
    void writeLpVec3(double x, double y, double z);
    void writeByteArray(std::span<const uint8_t> bytes) {
        writeVarInt(static_cast<int32_t>(bytes.size()));
        writeBytes(bytes);
    }

private:
    template <class T>
    void writeBE(T v) {
        if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
        writeBytes(&v, sizeof v);
    }
    std::vector<uint8_t> buf_;
};

// Bounds-checked big-endian reader over a packet body. Throws DecoderException on underflow.
class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

    size_t remaining() const { return data_.size() - pos_; }
    size_t position() const { return pos_; }
    std::span<const uint8_t> rest() const { return data_.subspan(pos_); }

    std::span<const uint8_t> readBytes(size_t n) {
        need(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    uint8_t readUnsignedByte() {
        need(1);
        return data_[pos_++];
    }
    int8_t readByte() { return static_cast<int8_t>(readUnsignedByte()); }
    bool readBool() { return readUnsignedByte() != 0; }
    int16_t readShort() { return static_cast<int16_t>(readBE<uint16_t>()); }
    uint16_t readUnsignedShort() { return readBE<uint16_t>(); }
    int32_t readInt() { return static_cast<int32_t>(readBE<uint32_t>()); }
    int64_t readLong() { return static_cast<int64_t>(readBE<uint64_t>()); }
    float readFloat() { return std::bit_cast<float>(readBE<uint32_t>()); }
    double readDouble() { return std::bit_cast<double>(readBE<uint64_t>()); }
    int32_t readVarInt();
    int64_t readVarLong();
    std::string readUtf(size_t maxChars = 32767);
    UUID readUUID() {
        UUID u;
        u.most = static_cast<uint64_t>(readLong());
        u.least = static_cast<uint64_t>(readLong());
        return u;
    }

private:
    void need(size_t n) const {
        if (remaining() < n) throw DecoderException("packet too short");
    }
    template <class T>
    T readBE() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, data_.data() + pos_, sizeof v);
        pos_ += sizeof v;
        if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
        return v;
    }
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

// Try to read a varint from the start of `data`. Returns bytes consumed, 0 if incomplete.
size_t tryReadVarInt(std::span<const uint8_t> data, int32_t& out);

} // namespace mc
