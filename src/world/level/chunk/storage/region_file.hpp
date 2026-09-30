#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nbt/tag.hpp"

namespace mc {

// Port of RegionFile: 32x32 chunks per .mca file in 4 KiB sectors. The header holds one
// (sector << 8 | count) entry and one timestamp per chunk; chunk payloads are
// [int length][byte compression][data]. Payloads of 256+ sectors go to c.X.Z.mcc next to it.
class RegionFile {
public:
    enum Compression : uint8_t { Gzip = 1, Deflate = 2, None = 3, Lz4 = 4, Custom = 127 };

    // `compression`: how chunks get written (server.properties region-file-compression).
    RegionFile(std::filesystem::path path, bool sync, Compression compression = Deflate);
    ~RegionFile();
    RegionFile(const RegionFile&) = delete;
    RegionFile& operator=(const RegionFile&) = delete;

    bool ok() const { return fd_ >= 0; }
    bool hasChunk(int localX, int localZ) const { return offsets_[index(localX, localZ)] != 0; }

    // Absolute chunk coordinates. Empty if absent; throws std::runtime_error if the data is damaged
    // or uses an unsupported compression, so callers can refuse to overwrite it.
    std::optional<nbt::Compound> read(int chunkX, int chunkZ);
    bool write(int chunkX, int chunkZ, const nbt::Compound& tag);

private:
    static int index(int localX, int localZ) { return (localX & 31) + (localZ & 31) * 32; }
    std::filesystem::path externalPath(int chunkX, int chunkZ) const;
    int allocate(int sectors);
    void free(int start, int count);
    bool writeHeader();

    std::filesystem::path path_;
    int fd_ = -1;
    bool sync_;
    Compression compression_;
    std::array<uint32_t, 1024> offsets_{};
    std::array<uint32_t, 1024> timestamps_{};
    std::vector<bool> used_; // RegionBitmap
};

// Port of RegionFileStorage: caches open region files for one folder (region/ or entities/).
class RegionFileStorage {
public:
    RegionFileStorage(std::filesystem::path folder, bool sync, RegionFile::Compression compression = RegionFile::Deflate)
        : folder_(std::move(folder)), sync_(sync), compression_(compression) {}

    std::optional<nbt::Compound> read(int chunkX, int chunkZ);
    bool write(int chunkX, int chunkZ, const nbt::Compound& tag);
    void flush() { cache_.clear(); lru_.clear(); }

private:
    RegionFile* get(int chunkX, int chunkZ, bool create);

    std::filesystem::path folder_;
    bool sync_;
    RegionFile::Compression compression_;
    std::unordered_map<int64_t, std::unique_ptr<RegionFile>> cache_;
    std::list<int64_t> lru_;
    static constexpr size_t kMaxCacheSize = 256;
};

} // namespace mc
