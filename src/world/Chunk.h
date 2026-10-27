#pragma once
#include "world/Terrain.h"
#include <cstdint>
#include <vector>

namespace alone {

constexpr int kChunkSize = 32;   // tiles (metres) per chunk side

struct Tile {
    Ground ground = Ground::Grass;
    Prop prop = Prop::None;
    uint8_t variant = 0;         // random per tile, for rendering variety / orientation
    uint8_t walls = 0;           // WallBits on the N and W edges
    Marking mark = Marking::None;
    uint8_t markDir = 0;         // 0: along x, 1: along y, 2+: diagonal hints
    int32_t building = -1;       // building id whose floor this is
};

struct Chunk {
    int cx = 0, cy = 0;
    Tile tiles[kChunkSize * kChunkSize];
    std::vector<int> buildings;  // buildings touching this chunk
    uint32_t version = 1;        // bumped when something changes (for mesh rebuilds)
    float lastUsed = 0;

    Tile& at(int lx, int ly) { return tiles[ly * kChunkSize + lx]; }
    const Tile& at(int lx, int ly) const { return tiles[ly * kChunkSize + lx]; }
};

inline int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int64_t tileKey(int x, int y) { return ((int64_t)(uint32_t)x << 32) | (uint32_t)y; }

} // namespace alone
