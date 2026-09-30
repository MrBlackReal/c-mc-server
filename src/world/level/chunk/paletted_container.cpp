#include "world/level/chunk/paletted_container.hpp"

#include <algorithm>
#include <bit>

#include "generated/blocks.hpp"

namespace mc {
namespace {
int ceillog2(uint32_t n) { return n <= 1 ? 0 : std::bit_width(n - 1); }

size_t longsFor(int bits, int entries) {
    if (bits == 0) return 0;
    int perLong = 64 / bits;
    return static_cast<size_t>((entries + perLong - 1) / perLong);
}
} // namespace

PalettedContainer::Strategy PalettedContainer::blockStrategy() {
    return {4, ceillog2(gen::kBlockStateCount), 4, 8};
}

PalettedContainer::Strategy PalettedContainer::biomeStrategy(int biomeCount) {
    return {2, ceillog2(static_cast<uint32_t>(biomeCount)), 1, 3};
}

PalettedContainer::PalettedContainer(Strategy strategy, uint32_t initial) : strategy_(strategy) {
    palette_ = {initial};
}

int PalettedContainer::bitsForPaletteSize(size_t size) const {
    int bits = ceillog2(static_cast<uint32_t>(size));
    if (bits == 0) return 0;
    if (bits <= strategy_.maxIndirectBits) return std::max(bits, strategy_.minIndirectBits);
    return strategy_.globalBits;
}

uint32_t PalettedContainer::getIndex(int i) const {
    if (bits_ == 0) return palette_[0];
    int perLong = 64 / bits_;
    uint64_t word = storage_[i / perLong];
    uint32_t v = static_cast<uint32_t>((word >> ((i % perLong) * bits_)) & ((1ull << bits_) - 1));
    return isGlobal() ? v : palette_[v];
}

void PalettedContainer::setIndex(int i, uint32_t v) {
    int perLong = 64 / bits_;
    uint64_t& word = storage_[i / perLong];
    int shift = (i % perLong) * bits_;
    uint64_t mask = ((1ull << bits_) - 1) << shift;
    word = (word & ~mask) | ((static_cast<uint64_t>(v) << shift) & mask);
}

int PalettedContainer::paletteIdFor(uint32_t value) {
    if (isGlobal()) return static_cast<int>(value);
    auto it = std::find(palette_.begin(), palette_.end(), value);
    if (it != palette_.end()) return static_cast<int>(it - palette_.begin());
    size_t capacity = bits_ == 0 ? 1 : (1u << bits_);
    if (palette_.size() >= capacity) {
        resize(bitsForPaletteSize(palette_.size() + 1));
        return paletteIdFor(value);
    }
    palette_.push_back(value);
    return static_cast<int>(palette_.size() - 1);
}

void PalettedContainer::resize(int newBits) {
    int n = strategy_.entryCount();
    std::vector<uint32_t> values(n);
    for (int i = 0; i < n; i++) values[i] = getIndex(i);
    bits_ = newBits;
    storage_.assign(longsFor(bits_, n), 0);
    if (isGlobal()) {
        palette_.clear();
        for (int i = 0; i < n; i++) setIndex(i, values[i]);
    } else {
        // Keep existing palette order so ids stay stable.
        for (int i = 0; i < n; i++) setIndex(i, static_cast<uint32_t>(paletteIdFor(values[i])));
    }
}

uint32_t PalettedContainer::set(int x, int y, int z, uint32_t value) {
    int i = index(x, y, z);
    uint32_t old = getIndex(i);
    if (old == value) return old;
    int id = paletteIdFor(value);
    setIndex(i, static_cast<uint32_t>(id));
    return old;
}

void PalettedContainer::fill(uint32_t value) {
    bits_ = 0;
    palette_ = {value};
    storage_.clear();
}

int PalettedContainer::diskBits(size_t size) const {
    // Strategy.getConfigurationForPaletteSize(...).bitsInStorage(): the in-memory configuration's
    // bits, except that the global palette is stored with exactly ceillog2(size) bits.
    int bits = ceillog2(static_cast<uint32_t>(size));
    if (bits == 0) return 0;
    return bits <= strategy_.maxIndirectBits ? std::max(bits, strategy_.minIndirectBits) : bits;
}

PalettedContainer::Packed PalettedContainer::pack() const {
    Packed p;
    int n = strategy_.entryCount();
    std::vector<uint32_t> indices(n);
    uint32_t lastValue = UINT32_MAX, lastIndex = 0;
    for (int i = 0; i < n; i++) {
        uint32_t v = getIndex(i);
        if (v != lastValue) {
            lastValue = v;
            auto it = std::find(p.palette.begin(), p.palette.end(), v);
            lastIndex = static_cast<uint32_t>(it - p.palette.begin());
            if (it == p.palette.end()) p.palette.push_back(v);
        }
        indices[i] = lastIndex;
    }
    int bits = diskBits(p.palette.size());
    if (bits == 0) return p;
    int perLong = 64 / bits;
    p.data.assign(longsFor(bits, n), 0);
    for (int i = 0; i < n; i++) p.data[i / perLong] |= static_cast<uint64_t>(indices[i]) << ((i % perLong) * bits);
    return p;
}

bool PalettedContainer::unpack(const std::vector<uint32_t>& palette, const std::vector<uint64_t>& data) {
    if (palette.empty()) return false;
    int n = strategy_.entryCount();
    int bits = diskBits(palette.size());
    if (bits == 0) {
        fill(palette[0]);
        return true;
    }
    if (data.size() != longsFor(bits, n)) return false;
    fill(palette[0]);
    int perLong = 64 / bits;
    uint64_t mask = (1ull << bits) - 1;
    for (int i = 0; i < n; i++) {
        uint64_t idx = (data[i / perLong] >> ((i % perLong) * bits)) & mask;
        if (idx >= palette.size()) return false;
        int x = i & ((1 << strategy_.bitsPerAxis) - 1);
        int z = (i >> strategy_.bitsPerAxis) & ((1 << strategy_.bitsPerAxis) - 1);
        int y = i >> (2 * strategy_.bitsPerAxis);
        set(x, y, z, palette[idx]);
    }
    return true;
}

void PalettedContainer::write(ByteWriter& out) const {
    out.writeByte(static_cast<uint8_t>(bits_));
    if (bits_ == 0) {
        out.writeVarInt(static_cast<int32_t>(palette_[0]));
    } else if (!isGlobal()) {
        out.writeVarInt(static_cast<int32_t>(palette_.size()));
        for (uint32_t v : palette_) out.writeVarInt(static_cast<int32_t>(v));
    }
    out.writeFixedSizeLongArray(storage_);
}

} // namespace mc
