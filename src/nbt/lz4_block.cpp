#include "nbt/lz4_block.hpp"

#include <lz4.h>

#include <algorithm>
#include <cstring>

namespace mc::nbt {

namespace {
constexpr uint8_t kMagic[8] = {'L', 'Z', '4', 'B', 'l', 'o', 'c', 'k'};
constexpr size_t kHeaderLength = 8 + 1 + 4 + 4 + 4;
constexpr int kBlockSize = 1 << 16;          // LZ4BlockOutputStream's default
constexpr uint8_t kMethodRaw = 0x10, kMethodLz4 = 0x20;
constexpr uint32_t kSeed = 0x9747b28c;       // LZ4BlockOutputStream.DEFAULT_SEED
constexpr uint8_t kLevel = 6;                // compressionLevel(64 KiB): 32 - clz(65535) - 10

uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
uint32_t readLE32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
void writeLE32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t checksum(std::span<const uint8_t> data) { return xxh32(data, kSeed) & 0xFFFFFFF; }
} // namespace

uint32_t xxh32(std::span<const uint8_t> data, uint32_t seed) {
    constexpr uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    const uint8_t* p = data.data();
    const uint8_t* end = p + data.size();
    uint32_t h;
    if (data.size() >= 16) {
        uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* limit = end - 16;
        do {
            v1 = rotl(v1 + readLE32(p) * P2, 13) * P1;
            v2 = rotl(v2 + readLE32(p + 4) * P2, 13) * P1;
            v3 = rotl(v3 + readLE32(p + 8) * P2, 13) * P1;
            v4 = rotl(v4 + readLE32(p + 12) * P2, 13) * P1;
            p += 16;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    } else {
        h = seed + P5;
    }
    h += static_cast<uint32_t>(data.size());
    for (; p + 4 <= end; p += 4) h = rotl(h + readLE32(p) * P3, 17) * P4;
    for (; p < end; p++) h = rotl(h + *p * P5, 11) * P1;
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

std::vector<uint8_t> lz4BlockCompress(std::span<const uint8_t> data) {
    std::vector<uint8_t> out;
    std::vector<char> buffer(static_cast<size_t>(LZ4_compressBound(kBlockSize)));
    auto block = [&](std::span<const uint8_t> chunk) {
        int compressed = chunk.empty() ? 0
                                       : LZ4_compress_default(reinterpret_cast<const char*>(chunk.data()), buffer.data(),
                                                              static_cast<int>(chunk.size()), static_cast<int>(buffer.size()));
        bool raw = chunk.empty() || compressed <= 0 || compressed >= static_cast<int>(chunk.size());
        out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
        out.push_back(static_cast<uint8_t>((raw ? kMethodRaw : kMethodLz4) | kLevel));
        writeLE32(out, static_cast<uint32_t>(raw ? chunk.size() : compressed));
        writeLE32(out, static_cast<uint32_t>(chunk.size()));
        writeLE32(out, chunk.empty() ? 0 : checksum(chunk));
        if (raw) out.insert(out.end(), chunk.begin(), chunk.end());
        else out.insert(out.end(), buffer.begin(), buffer.begin() + compressed);
    };
    for (size_t off = 0; off < data.size(); off += kBlockSize) block(data.subspan(off, std::min<size_t>(kBlockSize, data.size() - off)));
    block({}); // LZ4BlockOutputStream.finish
    return out;
}

std::optional<std::vector<uint8_t>> lz4BlockDecompress(std::span<const uint8_t> data) {
    std::vector<uint8_t> out;
    size_t pos = 0;
    while (pos + kHeaderLength <= data.size()) {
        if (std::memcmp(data.data() + pos, kMagic, 8) != 0) return std::nullopt;
        uint8_t token = data[pos + 8];
        uint32_t compressed = readLE32(data.data() + pos + 9), original = readLE32(data.data() + pos + 13), check = readLE32(data.data() + pos + 17);
        pos += kHeaderLength;
        uint8_t method = token & 0xF0;
        if (original > (1u << 25) || compressed > (1u << 25) || pos + compressed > data.size()) return std::nullopt;
        if (original == 0) { // the end of the stream
            if (compressed != 0) return std::nullopt;
            return out;
        }
        size_t at = out.size();
        out.resize(at + original);
        if (method == kMethodRaw) {
            if (compressed != original) return std::nullopt;
            std::memcpy(out.data() + at, data.data() + pos, original);
        } else if (method == kMethodLz4) {
            int n = LZ4_decompress_safe(reinterpret_cast<const char*>(data.data() + pos), reinterpret_cast<char*>(out.data() + at),
                                        static_cast<int>(compressed), static_cast<int>(original));
            if (n != static_cast<int>(original)) return std::nullopt;
        } else {
            return std::nullopt;
        }
        if (checksum({out.data() + at, original}) != check) return std::nullopt;
        pos += compressed;
    }
    return out; // (a stream without the end block: what we have)
}

} // namespace mc::nbt
