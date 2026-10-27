#pragma once
#include <cstdint>

namespace alone {

// Stateless integer hashing, used for deterministic world generation:
// the same coordinates and seed always give the same result.
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash2(int32_t x, int32_t y, uint32_t seed) {
    return hash32((uint32_t)x * 0x9E3779B1U ^ hash32((uint32_t)y + seed * 0x85EBCA77U));
}
inline uint32_t hash3(int32_t x, int32_t y, uint32_t salt, uint32_t seed) {
    return hash32(hash2(x, y, seed) ^ (salt * 0xC2B2AE3DU));
}
inline float hashFloat(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); }

// Small fast PRNG (PCG32) for runtime randomness.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x853c49e6748fea9bULL) { reseed(seed); }
    void reseed(uint64_t seed) { state_ = 0; next(); state_ += seed; next(); }
    uint32_t next() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xs >> rot) | (xs << ((32 - rot) & 31));
    }
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }
    int rangeInt(int lo, int hiInclusive) { return lo + (int)(next() % (uint32_t)(hiInclusive - lo + 1)); }
    bool chance(float p) { return uniform() < p; }
private:
    uint64_t state_ = 0;
};

} // namespace alone
