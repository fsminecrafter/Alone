#pragma once
#include "core/Math.h"
#include "world/Terrain.h"
#include <array>
#include <string>
#include <vector>

namespace alone {

enum class RoadClass : uint8_t { None = 0, Minor = 1, Major = 2, Motorway = 3, Railway = 4, Path = 5, Service = 6, Track = 7 };

inline bool isFootpath(RoadClass c) { return c == RoadClass::Path || c == RoadClass::Track; }

struct RoadSegment {
    Vec2 a, b;           // metres
    RoadClass cls = RoadClass::Minor;
    float halfWidth = 3;
    int name = -1;       // index into MapData::roadNames()
};

struct RoadHit {
    RoadClass cls = RoadClass::None;
    float distance = 1e9f;   // to the centre line
    float halfWidth = 0;
    float along = 0;         // metres along the segment (for dashed lines)
    Vec2 dir;                // segment direction
    int name = -1;
};

struct District { std::string name; Vec2 centre; float radius; };
struct WaterName { std::string name; Vec2 pos; };
// A real building from OpenStreetMap, already snapped to the grid.
struct OsmBuilding {
    std::string kind;
    int levels = 0;
    Vec2 centre;
    int w = 0, h = 0;
    std::string name;
    uint32_t colour = 0, roofColour = 0;   // 0 = not mapped
    int roof = -1;                          // -1 not mapped, 0 gable, 1 flat, 2 hip
};
struct Pier { std::vector<Vec2> pts; bool area = false; };

struct MapLabel { std::string text; Vec2 pos; float angle = 0; };
struct Plaza { std::string name; Vec2 centre; Vec2 size; };
struct DenseArea { std::string name; Vec2 centre; float radius; };

struct Landmark {
    std::string kind;
    std::string name;
    Vec2 centre;     // metres
    Vec2 size;       // metres
};

// The static description of the real place: land cover, roads, railway,
// named places and landmark buildings. Everything is in metres; one game
// tile is one metre.
class MapData {
public:
    bool load(const std::string& mapDir);

    const std::string& name() const { return name_; }
    float widthM() const { return width_ * tmpp_; }
    float heightM() const { return height_ * tmpp_; }
    float metresPerPixel() const { return mpp_; }
    // terrain grid (see terrainMetresPerPixel)
    int pixelWidth() const { return width_; }
    int pixelHeight() const { return height_; }
    Zone zoneAtPixel(int px, int py) const;
    Vec2 spawn() const { return spawn_; }
    float latitude() const { return latitude_; }

    // Land cover at a point, with organic edges.
    Zone zoneAt(float x, float y) const;
    // 0..1 how much of this spot is water (smooth, for shorelines)
    float waterCoverage(float x, float y) const;
    // Nearest road (or railway when rail = true) centre line.
    RoadHit nearestRoad(float x, float y, bool rail = false) const;
    // Ignores footpaths and tracks
    RoadHit nearestStreet(float x, float y) const;

    const std::vector<RoadSegment>& roads() const { return roads_; }
    const std::vector<RoadSegment>& rails() const { return rails_; }
    const std::vector<District>& districts() const { return districts_; }
    const std::vector<WaterName>& waterNames() const { return waterNames_; }
    const std::vector<Landmark>& landmarks() const { return landmarks_; }
    const std::vector<Plaza>& plazas() const { return plazas_; }
    const std::vector<MapLabel>& labels() const { return labels_; }
    const std::vector<DenseArea>& denseAreas() const { return dense_; }
    const DenseArea* denseAreaAt(Vec2 p) const;
    const Plaza* plazaAt(Vec2 p) const;
    std::string districtAt(Vec2 p) const;
    std::string streetAt(Vec2 p) const;             // street name within a few metres, or ""
    const std::vector<std::string>& roadNames() const { return roadNames_; }

    // OpenStreetMap detail
    bool osmCovered(float x, float y) const;
    const std::vector<OsmBuilding>& osmBuildings() const { return osmBuildings_; }
    const std::vector<Vec2>& osmTrees() const { return osmTrees_; }
    const std::vector<Vec2>& osmLamps() const { return osmLamps_; }
    const std::vector<Pier>& piers() const { return piers_; }
    bool pierAt(float x, float y) const;
    float terrainMetresPerPixel() const { return tmpp_; }

    // Height of the ground above sea level (metres), from elevation.pgm
    // (tools/mapgen/dem_import.py). Lakes and rivers: their surface. 0 when
    // the map has no elevation.
    bool hasElevation() const { return !elev_.empty(); }
    float elevation(float x, float y) const;            // smooth (Catmull-Rom)
    float elevationLinear(float x, float y) const;      // cheaper, for drawing the map
    float waterLevel(float x, float y) const;           // flat over water, no rise at the shore
    float lowestElevation() const { return elevMin_; }
    float highestElevation() const { return elevMax_; }

private:
    void index(const std::vector<RoadSegment>& segs, std::vector<std::vector<int>>& grid);
    RoadHit query(const std::vector<RoadSegment>& segs, const std::vector<std::vector<int>>& grid, float x, float y) const;
    float waterAtPixel(int px, int py) const;

    std::string name_ = "Unknown";
    int width_ = 0, height_ = 0;
    float mpp_ = 1;
    float latitude_ = 58;
    Vec2 spawn_;
    std::vector<uint8_t> zones_;
    std::vector<RoadSegment> roads_, rails_;
    std::vector<std::vector<int>> roadGrid_, railGrid_;
    int gridW_ = 0, gridH_ = 0;
    static constexpr float kGridCell = 48.0f;
    std::vector<District> districts_;
    std::vector<WaterName> waterNames_;
    std::vector<Landmark> landmarks_;
    std::vector<Plaza> plazas_;
    std::vector<std::string> roadNames_;
    std::vector<OsmBuilding> osmBuildings_;
    std::vector<Vec2> osmTrees_, osmLamps_;
    std::vector<Pier> piers_;
    std::vector<std::array<float, 4>> coverage_;
    float tmpp_ = 1;        // terrain metres per pixel (may differ from feature units)
    std::vector<MapLabel> labels_;
    std::vector<DenseArea> dense_;
    std::vector<float> elev_;
    int ew_ = 0, eh_ = 0;
    float empp_ = 8, elevMin_ = 0, elevMax_ = 0;
    float elevAt(int ix, int iy) const;
};

} // namespace alone
