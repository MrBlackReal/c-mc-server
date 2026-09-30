#include "world/level/chunk/storage/region_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include "nbt/lz4_block.hpp"
#include "util/log.hpp"

namespace mc {
namespace {
constexpr int kSectorBytes = 4096;
constexpr int kHeaderSectors = 2;
constexpr uint8_t kExternalFlag = 128;

uint32_t readBE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

bool preadAll(int fd, void* buf, size_t n, off_t off) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r, n -= static_cast<size_t>(r), off += r;
    }
    return true;
}

bool pwriteAll(int fd, const void* buf, size_t n, off_t off) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::pwrite(fd, p, n, off);
        if (r <= 0) return false;
        p += r, n -= static_cast<size_t>(r), off += r;
    }
    return true;
}

nbt::Compound parseChunk(std::span<const uint8_t> data) {
    ByteReader r(data);
    nbt::Tag root = nbt::readNamed(r);
    auto* c = root.asCompound();
    if (!c) throw std::runtime_error("chunk root is not a compound");
    return std::move(*c);
}
} // namespace

RegionFile::RegionFile(std::filesystem::path path, bool sync, Compression compression) : path_(std::move(path)), sync_(sync), compression_(compression) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        log::error("Failed to open region file {}: {}", path_.string(), std::strerror(errno));
        return;
    }
    struct stat st{};
    fstat(fd_, &st);
    std::array<uint8_t, kSectorBytes * kHeaderSectors> header{};
    if (st.st_size >= static_cast<off_t>(header.size())) {
        preadAll(fd_, header.data(), header.size(), 0);
    } else {
        pwriteAll(fd_, header.data(), header.size(), 0); // new file
    }
    for (int i = 0; i < 1024; i++) {
        offsets_[i] = readBE32(&header[i * 4]);
        timestamps_[i] = readBE32(&header[kSectorBytes + i * 4]);
    }
    int fileSectors = static_cast<int>((std::max<off_t>(st.st_size, header.size()) + kSectorBytes - 1) / kSectorBytes);
    used_.assign(static_cast<size_t>(fileSectors), false);
    used_[0] = used_[1] = true;
    for (int i = 0; i < 1024; i++) {
        uint32_t off = offsets_[i];
        if (!off) continue;
        int start = static_cast<int>(off >> 8), count = static_cast<int>(off & 0xFF);
        if (start < kHeaderSectors || start + count > fileSectors) {
            // RegionFile marks such entries invalid; keep the data untouched but unreferenced.
            log::warn("Region file {} has an invalid sector entry for chunk {}; ignoring it", path_.string(), i);
            offsets_[i] = 0;
            continue;
        }
        for (int s = start; s < start + count; s++) used_[s] = true;
    }
}

RegionFile::~RegionFile() {
    if (fd_ < 0) return;
    // padToFullSector
    struct stat st{};
    fstat(fd_, &st);
    off_t padded = (st.st_size + kSectorBytes - 1) / kSectorBytes * kSectorBytes;
    if (padded != st.st_size) {
        uint8_t zero = 0;
        pwriteAll(fd_, &zero, 1, padded - 1);
    }
    if (sync_) fsync(fd_);
    ::close(fd_);
}

std::filesystem::path RegionFile::externalPath(int chunkX, int chunkZ) const {
    return path_.parent_path() / ("c." + std::to_string(chunkX) + "." + std::to_string(chunkZ) + ".mcc");
}

std::optional<nbt::Compound> RegionFile::read(int chunkX, int chunkZ) {
    uint32_t off = offsets_[index(chunkX, chunkZ)];
    if (!off) return std::nullopt;
    int start = static_cast<int>(off >> 8), count = static_cast<int>(off & 0xFF);
    std::vector<uint8_t> buf(static_cast<size_t>(count) * kSectorBytes);
    if (!preadAll(fd_, buf.data(), buf.size(), static_cast<off_t>(start) * kSectorBytes))
        throw std::runtime_error("truncated region file");
    uint32_t length = readBE32(buf.data());
    if (length == 0 || length + 4 > buf.size()) throw std::runtime_error("invalid chunk length");
    uint8_t version = buf[4];
    std::vector<uint8_t> payload;
    if (version & kExternalFlag) {
        std::ifstream ext(externalPath(chunkX, chunkZ), std::ios::binary);
        if (!ext) throw std::runtime_error("missing external chunk file");
        payload.assign(std::istreambuf_iterator<char>(ext), std::istreambuf_iterator<char>());
        version &= ~kExternalFlag;
    } else {
        payload.assign(buf.begin() + 5, buf.begin() + 4 + length);
    }
    switch (version) {
    case Gzip: {
        auto d = nbt::gzipDecompress(payload);
        if (!d) throw std::runtime_error("bad gzip chunk data");
        return parseChunk(*d);
    }
    case Deflate: {
        auto d = nbt::zlibDecompress(payload);
        if (!d) throw std::runtime_error("bad zlib chunk data");
        return parseChunk(*d);
    }
    case None: return parseChunk(payload);
    case Lz4: {
        auto d = nbt::lz4BlockDecompress(payload);
        if (!d) throw std::runtime_error("bad lz4 chunk data");
        return parseChunk(*d);
    }
    default: // (RegionFileVersion.VERSION_CUSTOM: a named compression from a mod)
        throw std::runtime_error("unsupported chunk compression " + std::to_string(version));
    }
}

int RegionFile::allocate(int sectors) {
    // RegionBitmap.allocate: first fit.
    int run = 0;
    for (int i = 0; i < static_cast<int>(used_.size()); i++) {
        run = used_[i] ? 0 : run + 1;
        if (run == sectors) {
            int start = i - sectors + 1;
            for (int s = start; s <= i; s++) used_[s] = true;
            return start;
        }
    }
    int start = static_cast<int>(used_.size()) - run;
    used_.resize(static_cast<size_t>(start + sectors), false);
    for (int s = start; s < start + sectors; s++) used_[s] = true;
    return start;
}

void RegionFile::free(int start, int count) {
    for (int s = start; s < start + count && s < static_cast<int>(used_.size()); s++) used_[s] = false;
}

bool RegionFile::writeHeader() {
    std::array<uint8_t, kSectorBytes * kHeaderSectors> header{};
    for (int i = 0; i < 1024; i++) {
        writeBE32(&header[i * 4], offsets_[i]);
        writeBE32(&header[kSectorBytes + i * 4], timestamps_[i]);
    }
    return pwriteAll(fd_, header.data(), header.size(), 0);
}

bool RegionFile::write(int chunkX, int chunkZ, const nbt::Compound& tag) {
    if (fd_ < 0) return false;
    ByteWriter raw;
    nbt::writeNamed(raw, "", nbt::Tag(tag));
    std::vector<uint8_t> compressed = compression_ == Lz4    ? nbt::lz4BlockCompress(raw.data())
                                      : compression_ == None ? raw.data()
                                                             : nbt::zlibCompress(raw.data());

    int idx = index(chunkX, chunkZ);
    uint32_t old = offsets_[idx];
    size_t total = 5 + compressed.size();
    int sectors = static_cast<int>((total + kSectorBytes - 1) / kSectorBytes);
    std::filesystem::path ext = externalPath(chunkX, chunkZ);
    std::error_code ec;
    int start;
    if (sectors >= 256) {
        log::warn("Saving oversized chunk [{}, {}] ({} bytes) to external file {}", chunkX, chunkZ, total, ext.string());
        std::filesystem::path tmp = ext;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
            if (!out.flush()) return false;
        }
        start = allocate(1);
        uint8_t stub[5];
        writeBE32(stub, 1);
        stub[4] = compression_ | kExternalFlag;
        if (!pwriteAll(fd_, stub, 5, static_cast<off_t>(start) * kSectorBytes)) return false;
        std::filesystem::rename(tmp, ext, ec);
        sectors = 1;
    } else {
        std::vector<uint8_t> buf(static_cast<size_t>(sectors) * kSectorBytes, 0);
        writeBE32(buf.data(), static_cast<uint32_t>(compressed.size() + 1));
        buf[4] = compression_;
        std::memcpy(buf.data() + 5, compressed.data(), compressed.size());
        start = allocate(sectors);
        if (!pwriteAll(fd_, buf.data(), buf.size(), static_cast<off_t>(start) * kSectorBytes)) {
            free(start, sectors);
            return false;
        }
        std::filesystem::remove(ext, ec);
    }
    offsets_[idx] = static_cast<uint32_t>(start) << 8 | static_cast<uint32_t>(sectors);
    timestamps_[idx] = static_cast<uint32_t>(std::time(nullptr));
    if (!writeHeader()) return false;
    if (sync_) fdatasync(fd_);
    if (old) free(static_cast<int>(old >> 8), static_cast<int>(old & 0xFF));
    return true;
}

// ---------------------------------------------------------------- RegionFileStorage

RegionFile* RegionFileStorage::get(int chunkX, int chunkZ, bool create) {
    int rx = chunkX >> 5, rz = chunkZ >> 5;
    int64_t key = static_cast<int64_t>(static_cast<uint32_t>(rx)) | static_cast<int64_t>(rz) << 32;
    if (auto it = cache_.find(key); it != cache_.end()) {
        lru_.remove(key);
        lru_.push_front(key);
        return it->second.get();
    }
    std::filesystem::path file = folder_ / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
    std::error_code ec;
    if (!create && !std::filesystem::exists(file, ec)) return nullptr;
    std::filesystem::create_directories(folder_, ec);
    if (cache_.size() >= kMaxCacheSize) {
        cache_.erase(lru_.back());
        lru_.pop_back();
    }
    auto region = std::make_unique<RegionFile>(file, sync_, compression_);
    if (!region->ok()) return nullptr;
    RegionFile* raw = region.get();
    cache_.emplace(key, std::move(region));
    lru_.push_front(key);
    return raw;
}

std::optional<nbt::Compound> RegionFileStorage::read(int chunkX, int chunkZ) {
    RegionFile* r = get(chunkX, chunkZ, false);
    return r ? r->read(chunkX, chunkZ) : std::nullopt;
}

bool RegionFileStorage::write(int chunkX, int chunkZ, const nbt::Compound& tag) {
    RegionFile* r = get(chunkX, chunkZ, true);
    return r && r->write(chunkX, chunkZ, tag);
}

} // namespace mc
