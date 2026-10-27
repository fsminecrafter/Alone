#pragma once
// Helpers shared by the renderer's source files.
#include "client/MeshBuilder.h"
#include "world/Terrain.h"
#include "raylib.h"
#include <algorithm>
#include <cmath>

namespace alone::rc {

constexpr float kStorey = 3.0f;
constexpr float kInnerWall = 2.7f;
constexpr float kWallT = 0.14f;
constexpr float kDeepWater = -0.35f;
constexpr float kShallowWater = -0.12f;

inline Vector3 V(float x, float y, float z) { return Vector3{x, y, z}; }

inline bool isWater(Ground g) { return g == Ground::WaterShallow || g == Ground::WaterDeep; }
inline float groundHeight(Ground g) { return g == Ground::WaterDeep ? kDeepWater : g == Ground::WaterShallow ? kShallowWater : 0.0f; }

Color groundColour(Ground g);
int groundMaterial(Ground g);

// Fraction of leaves left on a deciduous tree on this day of the year
float leafDensity(Prop p, int doy);
Color leafColour(Prop p, int doy, uint8_t variant);

} // namespace alone::rc
