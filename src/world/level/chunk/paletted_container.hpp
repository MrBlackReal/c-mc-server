#pragma once

#include <cstdint>
#include <vector>

#include "network/friendly_byte_buf.hpp"

namespace mc {

// Port of PalettedContainer + Strategy. Values are global registry ids (block states or biomes).
// Palette kinds: single value (0 bits), indirect (linear/hashmap, identical on the wire) and global.
class PalettedContainer {
public:
    struct Strategy {
        int bitsPerAxis;     // 4 for blocks (16^3), 2 for biomes (4^3)
        int globalBits;      // ceillog2(registry size)
        int minIndirectBits; // blocks round 1..4 bits up to 4, biomes use the exact count
        int maxIndirectBits; // above this the global palette is used
        int entryCount() const { return 1 << (bitsPerAxis * 3); }
    };

    static Strategy blockStrategy();
    static Strategy biomeStrategy(int biomeCount);

    PalettedContainer(Strategy strategy, uint32_t initial);

    uint32_t get(int x, int y, int z) const { return getIndex(index(x, y, z)); }
    // Returns the previous value.
    uint32_t set(int x, int y, int z, uint32_t value);
    void fill(uint32_t value);

    void write(ByteWriter& out) const;
    int bits() const { return bits_; }
    // PalettedContainer.maybeHas: whether some palette entry matches (always true for the global palette).
    template <class Pred>
    bool maybeHas(Pred pred) const {
        if (isGlobal()) return true;
        for (uint32_t v : palette_)
            if (pred(v)) return true;
        return false;
    }

    // Disk form (PalettedContainer.pack/unpack): palette of the values actually used, in first-seen
    // order, plus a SimpleBitStorage long array (absent when the palette has one entry).
    struct Packed {
        std::vector<uint32_t> palette;
        std::vector<uint64_t> data;
    };
    Packed pack() const;
    // Returns false if `data` doesn't have the length the palette size implies.
    bool unpack(const std::vector<uint32_t>& palette, const std::vector<uint64_t>& data);
    // Bits per entry vanilla uses on disk for a palette of `size` entries.
    int diskBits(size_t size) const;

private:
    int index(int x, int y, int z) const {
        return (((y << strategy_.bitsPerAxis) | z) << strategy_.bitsPerAxis) | x;
    }
    uint32_t getIndex(int i) const;
    void setIndex(int i, uint32_t paletteId);
    int paletteIdFor(uint32_t value);
    void resize(int newBits);
    int bitsForPaletteSize(size_t size) const;
    bool isGlobal() const { return bits_ > strategy_.maxIndirectBits; }

    Strategy strategy_;
    int bits_ = 0;
    std::vector<uint32_t> palette_; // indirect/single: palette index -> global id
    std::vector<uint64_t> storage_; // SimpleBitStorage: values never straddle longs
};

} // namespace mc
