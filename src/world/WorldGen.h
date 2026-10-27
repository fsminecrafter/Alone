#pragma once
#include "world/Building.h"
#include "world/Chunk.h"
#include "world/MapData.h"
#include <unordered_map>
#include <vector>

namespace alone {

// Deterministic world generation from the map: the same seed and map always
// give the same world, so chunks can be thrown away and rebuilt at will.
class WorldGen {
public:
    WorldGen(const MapData& map, uint32_t seed);

    void generateChunk(Chunk& c);

    // Buildings overlapping a rectangle (generating their lots if needed).
    std::vector<int> buildingsIn(const IRect& r);
    const Building* building(int id) const;
    const std::vector<Building>& allBuildings() const { return buildings_; }
    // Find a house to start in near a point (map spawn).
    const Building* findStartHouse(Vec2 near);

    uint32_t seed() const { return seed_; }
    const MapData& map() const { return map_; }

private:
    static constexpr int kLot = 26;

    void ensureLot(int lx, int ly);
    void makeLandmarks();
    void makeDenseBlocks();
    bool siteIsClear(const IRect& r, bool allowRoads = false) const;
    bool tryPlace(BuildingType type, const IRect& area, uint32_t h, Building& out);
    void layout(Building& b);
    void splitRooms(Building& b, IRect r, int depth, uint32_t& h);
    void assignRooms(Building& b, uint32_t& h);
    void furnish(Building& b, uint32_t& h);
    void addExteriorDoor(Building& b, uint32_t& h);
    void styleBuilding(Building& b, uint32_t& h);
    int addBuilding(Building b);

    void groundFor(Tile& t, int x, int y, Zone z, float water);
    void applyRoads(Tile& t, int x, int y, Zone z);
    void applyBuilding(Chunk& c, const Building& b);

    const MapData& map_;
    uint32_t seed_;
    std::vector<Building> buildings_;
    std::unordered_map<int64_t, int> lotDone_;   // lot -> 1 when generated
    std::vector<IRect> reserved_;                // landmark footprints
    std::vector<IRect> denseRects_;              // old-town buildings placed along streets
    std::vector<std::vector<int>> buildingGrid_; // 64 m cells -> building ids
    int gridW_ = 0, gridH_ = 0;
};

} // namespace alone
