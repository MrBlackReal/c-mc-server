#include "world/level/biome/biome_climate.hpp"

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/level/server_level.hpp"
#include "util/json.hpp"
#include "util/legacy_random.hpp"

namespace mc::biome {
namespace {

// SimplexNoise (GradientNoise with the noise offset discarded), 2D.
class SimplexNoise {
public:
    explicit SimplexNoise(LegacyRandom& random) {
        // GradientNoise(random, 0.0): the offsets still draw (times zero)
        offsetX_ = random.nextDouble() * 0.0;
        offsetY_ = random.nextDouble() * 0.0;
        random.nextDouble();
        for (int i = 0; i < 256; i++) perms_[i] = static_cast<uint8_t>(i);
        for (int i = 0; i < 256; i++) {
            int offset = random.nextInt(256 - i);
            std::swap(perms_[i], perms_[offset + i]);
        }
    }
    float get(double xin0, double yin0) const {
        static const double kSqrt3 = std::sqrt(3.0);
        static const double kF2 = 0.5 * (kSqrt3 - 1.0), kG2 = (3.0 - kSqrt3) / 6.0;
        static constexpr int kGrad[12][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 1}, {0, -1}};
        double xin = xin0 + offsetX_, yin = yin0 + offsetY_;
        double s = (xin + yin) * kF2;
        int i = floorInt(xin + s), j = floorInt(yin + s);
        double t = (i + j) * kG2;
        double x0 = xin - (i - t), y0 = yin - (j - t);
        int i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
        double x1 = x0 - i1 + kG2, y1 = y0 - j1 + kG2;
        double x2 = x0 - 1.0 + 2.0 * kG2, y2 = y0 - 1.0 + 2.0 * kG2;
        int ii = i & 0xFF, jj = j & 0xFF;
        int gi0 = permute(ii + permute(jj)) % 12;
        int gi1 = permute(ii + i1 + permute(jj + j1)) % 12;
        int gi2 = permute(ii + 1 + permute(jj + 1)) % 12;
        auto corner = [&](int g, double x, double y) { // getCornerNoise3D(g, x, y, 0, 0.5)
            double t0 = 0.5 - x * x - y * y;
            if (t0 < 0.0) return 0.0;
            t0 *= t0;
            return t0 * t0 * (kGrad[g][0] * x + kGrad[g][1] * y);
        };
        return static_cast<float>(70.0 * (corner(gi0, x0, y0) + corner(gi1, x1, y1) + corner(gi2, x2, y2)));
    }

private:
    static int floorInt(double v) { return static_cast<int>(std::floor(v)); }
    int permute(int x) const { return perms_[x & 0xFF]; }
    double offsetX_ = 0, offsetY_ = 0;
    std::array<uint8_t, 256> perms_{};
};

const SimplexNoise& temperatureNoise() { // Biome.TEMPERATURE_NOISE
    static const SimplexNoise n = [] {
        LegacyRandom r(1234);
        return SimplexNoise(r);
    }();
    return n;
}
const SimplexNoise& biomeInfoNoise() { // Biome.BIOME_INFO_NOISE
    static const SimplexNoise n = [] {
        LegacyRandom r(2345);
        return SimplexNoise(r);
    }();
    return n;
}
// Biome.FROZEN_TEMPERATURE_NOISE: a NoiseStack of three simplex layers.
float frozenNoise(double x, double z) {
    struct Layer {
        SimplexNoise noise;
        double frequency;
        float amplitude;
    };
    static const std::vector<Layer> layers = [] {
        LegacyRandom r(3456);
        std::vector<Layer> v;
        v.push_back({SimplexNoise(r), 1.0, 0.14285715f});
        v.push_back({SimplexNoise(r), 0.5, 0.2857143f});
        v.push_back({SimplexNoise(r), 0.25, 0.5714286f});
        return v;
    }();
    float value = 0.0f;
    for (const auto& l : layers) value += l.amplitude * l.noise.get(x * l.frequency, z * l.frequency);
    return value;
}

struct Climate {
    float temperature = 0.5f;
    bool frozen = false;
};
const Climate& climate(uint32_t biome) {
    static const std::vector<Climate> table = [] {
        json::Value all = json::parse(gen::kRegistryElementsJson)["minecraft:worldgen/biome"];
        std::vector<Climate> t(registries::size("minecraft:worldgen/biome"));
        for (size_t i = 0; i < t.size(); i++) {
            const json::Value& b = all[registries::name("minecraft:worldgen/biome", static_cast<int>(i))];
            t[i].temperature = static_cast<float>(b["temperature"].number(0.5));
            t[i].frozen = b["temperature_modifier"].isString() && b["temperature_modifier"].string() == "frozen";
        }
        return t;
    }();
    static const Climate kDefault;
    return biome < table.size() ? table[biome] : kDefault;
}

} // namespace

float temperature(uint32_t biome, BlockPos pos, int seaLevel) {
    const Climate& c = climate(biome);
    float adjusted = c.temperature; // TemperatureModifier.modifyTemperature
    if (c.frozen) {
        double large = frozenNoise(pos.x * 0.05, pos.z * 0.05) * 7.0f;
        double edge = biomeInfoNoise().get(pos.x * 0.2, pos.z * 0.2);
        if (large + edge < 0.3 && biomeInfoNoise().get(pos.x * 0.09, pos.z * 0.09) < 0.8) adjusted = 0.2f;
    }
    int snowLevel = seaLevel + 17;
    if (pos.y <= snowLevel) return adjusted;
    float v = temperatureNoise().get(pos.x / 8.0f, pos.z / 8.0f) * 8.0f;
    return adjusted - (v + pos.y - snowLevel) * 0.05f / 40.0f;
}

bool shouldFreeze(ServerLevel& level, uint32_t biome, BlockPos pos, bool checkNeighbors) {
    if (warmEnoughToRain(biome, pos, level.seaLevel())) return false;
    if (pos.y < level.minY() || pos.y > level.maxY() || level.brightness(light::LightLayer::Block, pos) >= 10) return false;
    BlockStateId s = level.getBlockState(pos);
    if (!blocks::is(s, gen::block::WATER) || !(blocks::flags(s) & blocks::kFluidSource)) return false; // a still water source (LiquidBlock)
    if (!checkNeighbors) return true;
    auto water = [&](BlockPos p) { return (blocks::flags(level.getBlockState(p)) & blocks::kWater) != 0; };
    return !(water(pos.west()) && water(pos.east()) && water(pos.north()) && water(pos.south()));
}

} // namespace mc::biome
