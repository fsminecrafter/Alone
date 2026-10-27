#pragma once
#include "items/ItemRegistry.h"
#include "world/Clock.h"
#include "world/WorldGen.h"
#include <memory>
#include <unordered_map>
#include <vector>

namespace alone {

struct ContainerState {
    std::string kind;
    bool searched = false;
    std::vector<ItemInstance> items;
};

struct Campfire {
    Vec2 pos;
    float fuelMinutes = 0;   // burns while > 0
    bool lit = false;
    int id = 0;
};

struct Snare {
    Vec2 pos;
    float minutesSet = 0;
    bool caught = false;
    int id = 0;
};

// Things changed by the player, kept even when a chunk is unloaded.
struct TileOverride {
    bool hasProp = false;
    Prop prop = Prop::None;
    float harvestedAtDay = -1;   // berries / forage: day picked
};

class SaveGame;

class World {
    friend class SaveGame;
public:
    World(const MapData& map, const ItemRegistry& items, uint32_t seed);

    const MapData& map() const { return map_; }
    WorldGen& gen() { return gen_; }
    const ItemRegistry& items() const { return items_; }
    Clock& clock() { return clock_; }
    const Clock& clock() const { return clock_; }
    uint32_t seed() const { return seed_; }

    // --- tiles ---------------------------------------------------------
    Chunk& chunk(int cx, int cy);
    Chunk* chunkIfLoaded(int cx, int cy);
    const Tile& tile(int x, int y);
    Tile& mutableTile(int x, int y);
    void touch(int x, int y);           // mark the chunk changed
    void setProp(int x, int y, Prop p); // permanent change
    const Building* buildingAt(int x, int y);
    const Building* building(int id) const { return gen_.building(id); }
    bool indoors(Vec2 p);

    // --- movement --------------------------------------------------------
    bool tileBlocked(int x, int y);                           // water, solid props
    bool edgeBlocked(int x, int y, int dx, int dy);           // wall/closed door between neighbours
    // Slide a circle of `radius` from `from` by `delta`, stopping at walls.
    Vec2 moveCircle(Vec2 from, Vec2 delta, float radius);
    bool lineOfSight(Vec2 a, Vec2 b);

    // --- doors -----------------------------------------------------------
    // dir: 0 north edge of (x,y), 1 west edge of (x,y)
    bool doorAt(int x, int y, int dir);
    bool doorOpen(int x, int y, int dir) const;
    void toggleDoor(int x, int y, int dir);
    // Find a door edge within reach of a point. Returns false if none.
    bool findDoorNear(Vec2 p, float reach, int& x, int& y, int& dir);

    // --- containers & loot -------------------------------------------------
    std::string containerKindAt(int x, int y);
    ContainerState& container(int x, int y);
    std::vector<ItemInstance>& groundItems(int x, int y);
    void dropItem(Vec2 p, const ItemInstance& it);
    // Items on the ground in a radius (for the inventory "Floor" pane)
    std::vector<std::pair<int64_t, int>> groundItemsNear(Vec2 p, float radius);
    std::vector<ItemInstance>* groundStack(int64_t key);
    const std::unordered_map<int64_t, std::vector<ItemInstance>>& allGroundItems() const { return ground_; }

    // Water you can drink from: lakes, rivers, toilet tanks, rain barrels
    enum class WaterSource { None, Lake, ToiletTank, RainBarrel };
    WaterSource waterSourceNear(Vec2 p, float reach, int* tx = nullptr, int* ty = nullptr);
    float takeTankWater(int x, int y, float ml);   // returns ml taken

    // Foraging state of a tile
    bool harvested(int x, int y, float regrowDays);
    void markHarvested(int x, int y);

    // --- fires & snares ---------------------------------------------------
    std::vector<Campfire>& campfires() { return fires_; }
    std::vector<Snare>& snares() { return snares_; }
    Campfire* fireNear(Vec2 p, float reach, bool litOnly);
    float fireWarmthAt(Vec2 p);   // degrees C added by nearby fires

    // --- environment ---------------------------------------------------------
    float temperatureAt(Vec2 p);
    float outdoorTemp() const { return clock_.outdoorTempC(); }

    // The grid keeps running for a while after everyone vanished.
    bool powerOn() const { return clock_.totalMinutes() < powerOffMinutes_; }
    double powerOffMinutes() const { return powerOffMinutes_; }
    // Some people left their lights on (only matters while there is power).
    bool buildingLightsOn(int buildingId) const;
    float snowCover() const { return snowCover_; }
    float groundWetness() const { return wetness_; }

    void update(float gameMinutes, Vec2 focus);
    void unloadFarChunks(Vec2 focus, float radius);
    size_t loadedChunks() const { return chunks_.size(); }

private:
    const MapData& map_;
    const ItemRegistry& items_;
    uint32_t seed_;
    WorldGen gen_;
    Clock clock_;
    std::unordered_map<int64_t, std::unique_ptr<Chunk>> chunks_;
    std::unordered_map<int64_t, TileOverride> overrides_;
    std::unordered_map<int64_t, bool> doors_;                 // key: tile*2+dir -> open
    std::unordered_map<int64_t, ContainerState> containers_;
    std::unordered_map<int64_t, std::vector<ItemInstance>> ground_;
    std::unordered_map<int64_t, float> tankWater_;
    std::vector<Campfire> fires_;
    std::vector<Snare> snares_;
    int nextId_ = 1;
    float clockTime_ = 0;
    uint32_t versionCounter_ = 1;
    double powerOffMinutes_ = 0;
    float snowCover_ = 0, wetness_ = 0;   // chunk versions are unique, so stale meshes are always rebuilt
public:
    int newId() { return nextId_++; }
};

} // namespace alone
