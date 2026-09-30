#pragma once

#include <cstdint>
#include <vector>

#include "core/direction.hpp"

namespace mc {

// A set of BlockPos that iterates in the order java.util.HashSet<BlockPos> would. Tree features
// hand their trunk and leaf sets to decorators that walk them while drawing random numbers, so the
// same seed only grows the same tree if the walk goes in the same order.
//
// Emulates java.util.HashMap: Vec3i.hashCode, the spread (h ^ h >>> 16), power-of-two tables
// starting at 16 that double past a 0.75 load factor, bins as insertion-ordered lists (a resize
// keeps their relative order), and a bin growing past 8 entries in a table under 64 resizing it.
// Treeified bins (8+ entries in a table of 64+) keep list order here; vanilla may differ then.
class JavaBlockPosSet {
public:
    static int32_t hashCode(BlockPos p) { // Vec3i.hashCode
        return static_cast<int32_t>((static_cast<uint32_t>(p.y) + static_cast<uint32_t>(p.z) * 31u) * 31u + static_cast<uint32_t>(p.x));
    }

    bool contains(BlockPos p) const {
        if (table_.empty()) return false;
        for (const auto& e : table_[index(p, table_.size())])
            if (e == p) return true;
        return false;
    }
    // HashSet.add: false if it was already there.
    bool add(BlockPos p) {
        if (table_.empty()) table_.resize(16);
        auto& bin = table_[index(p, table_.size())];
        for (const auto& e : bin)
            if (e == p) return false;
        bin.push_back(p);
        if (bin.size() > 8 && table_.size() < 64) resize(); // putVal: treeifyBin on a small table (chain was 8 long)
        if (++size_ > table_.size() * 3 / 4) resize();
        return true;
    }
    // Iterator.next() then Iterator.remove() on a fresh iterator: takes the first position in
    // iteration order out (the table keeps its size).
    BlockPos popFirst() {
        for (auto& bin : table_)
            if (!bin.empty()) {
                BlockPos p = bin.front();
                bin.erase(bin.begin());
                size_--;
                return p;
            }
        return {};
    }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // The positions in iteration order.
    std::vector<BlockPos> ordered() const {
        std::vector<BlockPos> out;
        out.reserve(size_);
        for (const auto& bin : table_) out.insert(out.end(), bin.begin(), bin.end());
        return out;
    }

private:
    static size_t index(BlockPos p, size_t n) {
        uint32_t h = static_cast<uint32_t>(hashCode(p));
        h ^= h >> 16;
        return h & (n - 1);
    }
    void resize() {
        std::vector<std::vector<BlockPos>> bigger(table_.size() * 2);
        for (const auto& bin : table_)
            for (const auto& e : bin) bigger[index(e, bigger.size())].push_back(e);
        table_ = std::move(bigger);
    }

    std::vector<std::vector<BlockPos>> table_;
    size_t size_ = 0;
};

} // namespace mc
