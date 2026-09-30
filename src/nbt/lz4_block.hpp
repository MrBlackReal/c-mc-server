#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mc::nbt {

// lz4-java's LZ4BlockOutputStream / LZ4BlockInputStream format, which region files use for
// region-file-compression=lz4: "LZ4Block" blocks of up to 64 KiB, each with a header (method and
// level, compressed and original length, masked XXH32 checksum), ending with an empty block.
std::vector<uint8_t> lz4BlockCompress(std::span<const uint8_t> data);
std::optional<std::vector<uint8_t>> lz4BlockDecompress(std::span<const uint8_t> data);

// XXH32 (as net.jpountz.xxhash's streaming hash).
uint32_t xxh32(std::span<const uint8_t> data, uint32_t seed);

} // namespace mc::nbt
