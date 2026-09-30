#pragma once

#include <cstdint>

namespace mc {

// Port of LegacyRandomSource (java.util.Random's LCG, with BitRandomSource's derived values), for
// places whose results depend on a known seed (the enchanting table's seed, ...).
class LegacyRandom {
public:
    explicit LegacyRandom(int64_t seed = 0) { setSeed(seed); }

    void setSeed(int64_t seed) { seed_ = (static_cast<uint64_t>(seed) ^ kMultiplier) & kMask; }

    int32_t next(int bits) {
        seed_ = (seed_ * kMultiplier + kIncrement) & kMask;
        return static_cast<int32_t>(static_cast<int64_t>(seed_) >> (48 - bits));
    }
    int32_t nextInt() { return next(32); }
    int32_t nextInt(int32_t bound) {
        if (bound <= 0) return 0;
        if ((bound & (bound - 1)) == 0) return static_cast<int32_t>(static_cast<int64_t>(bound) * next(31) >> 31);
        int32_t sample, modulo;
        do {
            sample = next(31);
            modulo = sample % bound;
        } while (static_cast<int32_t>(static_cast<uint32_t>(sample - modulo) + static_cast<uint32_t>(bound - 1)) < 0);
        return modulo;
    }
    int64_t nextLong() { return static_cast<int64_t>(static_cast<uint64_t>(static_cast<int64_t>(next(32))) << 32) + next(32); }
    bool nextBoolean() { return next(1) != 0; }
    float nextFloat() { return static_cast<float>(next(24)) * 5.9604645E-8F; }
    double nextDouble() { return static_cast<double>((static_cast<int64_t>(next(26)) << 27) + next(27)) * static_cast<double>(1.110223E-16F); }

private:
    static constexpr uint64_t kMultiplier = 0x5DEECE66DULL, kIncrement = 0xBULL, kMask = (1ULL << 48) - 1;
    uint64_t seed_ = 0;
};

} // namespace mc
