#pragma once
#include <cstdint>

namespace alone {

// Smooth 2D value noise in [0,1] and fractal sum of octaves.
float valueNoise(float x, float y, uint32_t seed);
float fbm(float x, float y, uint32_t seed, int octaves = 4);

} // namespace alone
