#include "network/friendly_byte_buf.hpp"

#include <algorithm>
#include <cmath>

namespace mc {

int varIntSize(int32_t value) {
    auto v = static_cast<uint32_t>(value);
    int n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

void ByteWriter::writeVarInt(int32_t value) {
    auto v = static_cast<uint32_t>(value);
    while (v >= 0x80) {
        buf_.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    buf_.push_back(static_cast<uint8_t>(v));
}

void ByteWriter::writeVarLong(int64_t value) {
    auto v = static_cast<uint64_t>(value);
    while (v >= 0x80) {
        buf_.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    buf_.push_back(static_cast<uint8_t>(v));
}

void ByteWriter::writeUtf(std::string_view s) {
    writeVarInt(static_cast<int32_t>(s.size()));
    writeBytes(s.data(), s.size());
}

void ByteWriter::writeBlockPos(int x, int y, int z) {
    uint64_t packed = (static_cast<uint64_t>(x) & 0x3FFFFFF) << 38 | (static_cast<uint64_t>(z) & 0x3FFFFFF) << 12 |
                      (static_cast<uint64_t>(y) & 0xFFF);
    writeLong(static_cast<int64_t>(packed));
}

void ByteWriter::writeBitSet(std::span<const uint64_t> words) {
    // ByteBufCodecs.BIT_SET: BitSet.toByteArray() (little-endian, trailing zero bytes dropped)
    // written as a varint-prefixed byte array.
    std::vector<uint8_t> bytes;
    for (uint64_t w : words)
        for (int i = 0; i < 8; i++) bytes.push_back(static_cast<uint8_t>(w >> (8 * i)));
    while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
    writeByteArray(bytes);
}

void ByteWriter::writeLpVec3(double x, double y, double z) {
    auto sanitize = [](double v) { return std::isnan(v) ? 0.0 : std::clamp(v, -1.7179869183e10, 1.7179869183e10); };
    x = sanitize(x), y = sanitize(y), z = sanitize(z);
    double chessboard = std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (chessboard < 3.051944088384301e-5) {
        writeByte(0);
        return;
    }
    auto scale = static_cast<int64_t>(std::ceil(chessboard));
    bool partial = (scale & 3) != scale;
    uint64_t markers = partial ? static_cast<uint64_t>((scale & 3) | 4) : static_cast<uint64_t>(scale);
    auto pack = [&](double v) { return static_cast<uint64_t>(std::llround((v / static_cast<double>(scale) * 0.5 + 0.5) * 32766.0)); };
    uint64_t buffer = markers | pack(x) << 3 | pack(y) << 18 | pack(z) << 33;
    writeByte(static_cast<uint8_t>(buffer));
    writeByte(static_cast<uint8_t>(buffer >> 8));
    writeInt(static_cast<int32_t>(static_cast<uint32_t>(buffer >> 16)));
    if (partial) writeVarInt(static_cast<int32_t>(scale >> 2));
}

int32_t ByteReader::readVarInt() {
    uint32_t result = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        uint8_t b = readUnsignedByte();
        result |= static_cast<uint32_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return static_cast<int32_t>(result);
    }
    throw DecoderException("VarInt too big");
}

int64_t ByteReader::readVarLong() {
    uint64_t result = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        uint8_t b = readUnsignedByte();
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return static_cast<int64_t>(result);
    }
    throw DecoderException("VarLong too big");
}

std::string ByteReader::readUtf(size_t maxChars) {
    int32_t len = readVarInt();
    if (len < 0 || static_cast<size_t>(len) > maxChars * 3) throw DecoderException("string too long");
    auto bytes = readBytes(static_cast<size_t>(len));
    std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // Java limits by UTF-16 code units; counting code points is close enough for validation.
    size_t chars = 0;
    for (unsigned char c : s) chars += (c & 0xC0) != 0x80;
    if (chars > maxChars) throw DecoderException("string too long");
    return s;
}

size_t tryReadVarInt(std::span<const uint8_t> data, int32_t& out) {
    uint32_t result = 0;
    for (size_t i = 0; i < 5; i++) {
        if (i >= data.size()) return 0;
        result |= static_cast<uint32_t>(data[i] & 0x7F) << (7 * i);
        if (!(data[i] & 0x80)) {
            out = static_cast<int32_t>(result);
            return i + 1;
        }
    }
    throw DecoderException("VarInt too big");
}

} // namespace mc
