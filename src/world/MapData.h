#pragma once
#include "core/Math.h"
#include "world/Terrain.h"
#include <string>
#include <vector>

namespace alone {

enum class RoadClass : uint8_t { None = 0, Minor = 1, Major = 2, Motorway = 3, Railway = 4 };

struct RoadSegment {
    Vec2 a, b;           // metres
    RoadClass cls = RoadClass::Minor;
    float halfWidth = 3;
};

struct RoadHit {
    RoadClass cls = RoadClass::None;
    float distance = 1e9f;   // to the centre line
    float halfWidth = 0;
    float along = 0;         // metres along the segment (for dashed lines)
    Vec2 dir;                // segment direction
};

struct District { std::string name; Vec2 centre; float radius; };
struct WaterName { std::string name; Vec2 pos; };
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
    float widthM() const { return width_ * mpp_; }
    float heightM() const { return height_ * mpp_; }
    float metresPerPixel() const { return mpp_; }
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
    std::vector<MapLabel> labels_;
    std::vector<DenseArea> dense_;
};

} // namespace alone
