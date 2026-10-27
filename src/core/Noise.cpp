#include "core/Noise.h"
#include "core/Rng.h"
#include <cmath>

namespace alone {

float valueNoise(float x, float y, uint32_t seed) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = x - xi, fy = y - yi;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    float a = hashFloat(hash2(xi, yi, seed));
    float b = hashFloat(hash2(xi + 1, yi, seed));
    float c = hashFloat(hash2(xi, yi + 1, seed));
    float d = hashFloat(hash2(xi + 1, yi + 1, seed));
    float top = a + (b - a) * sx;
    float bot = c + (d - c) * sx;
    return top + (bot - top) * sy;
}

float fbm(float x, float y, uint32_t seed, int octaves) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise(x, y, seed + i * 1013) * amp;
        norm += amp;
        x *= 2.03f; y *= 2.03f; amp *= 0.5f;
    }
    return sum / norm;
}

} // namespace alone
