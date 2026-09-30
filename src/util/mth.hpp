#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

// The parts of net.minecraft.util.Mth whose exact results matter (tables, fast approximations).
namespace mc::mth {

// Mth.sin / Mth.cos: a 65536-entry table.
inline const std::vector<float>& sinTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (int i = 0; i < 65536; i++) t[i] = static_cast<float>(std::sin(i / 10430.378350470453));
        return t;
    }();
    return table;
}
inline float sin(double v) { return sinTable()[static_cast<size_t>(static_cast<int64_t>(v * 10430.378350470453) & 65535)]; }
inline float cos(double v) { return sinTable()[static_cast<size_t>(static_cast<int64_t>(v * 10430.378350470453 + 16384.0) & 65535)]; }

// Mth.wrapDegrees (Java's % keeps the dividend's sign, like fmod)
inline float wrapDegrees(float angle) {
    float a = std::fmod(angle, 360.0f);
    if (a >= 180.0f) a -= 360.0f;
    if (a < -180.0f) a += 360.0f;
    return a;
}
inline double wrapDegrees(double angle) {
    double a = std::fmod(angle, 360.0);
    if (a >= 180.0) a -= 360.0;
    if (a < -180.0) a += 360.0;
    return a;
}

// Mth.fastInvSqrt
inline double fastInvSqrt(double x) {
    double xhalf = 0.5 * x;
    int64_t i = std::bit_cast<int64_t>(x);
    i = 6910469410427058090LL - (i >> 1);
    x = std::bit_cast<double>(i);
    return x * (1.5 - xhalf * x * x);
}

// Mth.atan2: table-driven approximation.
inline double atan2(double y, double x) {
    static const auto tables = [] {
        std::array<std::array<double, 257>, 2> t{};
        for (int i = 0; i < 257; i++) {
            double asinv = std::asin(i / 256.0);
            t[0][i] = asinv;
            t[1][i] = std::cos(asinv);
        }
        return t;
    }();
    const double fracBias = std::bit_cast<double>(int64_t{4805340802404319232LL});
    double d2 = x * x + y * y;
    if (std::isnan(d2)) return NAN;
    bool negY = y < 0.0;
    if (negY) y = -y;
    bool negX = x < 0.0;
    if (negX) x = -x;
    bool steep = y > x;
    if (steep) std::swap(x, y);
    double rinv = fastInvSqrt(d2);
    x *= rinv;
    y *= rinv;
    double yp = fracBias + y;
    int index = static_cast<int32_t>(std::bit_cast<int64_t>(yp));
    double phi = tables[0][index];
    double cPhi = tables[1][index];
    double sPhi = yp - fracBias;
    double sd = y * cPhi - x * sPhi;
    double d = (6.0 + sd * sd) * sd * 0.16666666666666666;
    double theta = phi + d;
    if (steep) theta = 1.5707963267948966 - theta;
    if (negX) theta = 3.141592653589793 - theta;
    if (negY) theta = -theta;
    return theta;
}

} // namespace mc::mth
