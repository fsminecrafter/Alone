#pragma once
#include "items/ItemRegistry.h"
#include "world/Clock.h"
#include "world/WorldGen.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
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
    bool stove = false;      // a storm kitchen: burns red alcohol from its burner
    float fuelMl = 0;        // stove: alcohol in the burner (100 ml lasts ~25 min)
};

// A pitched tent: shelter from wind and rain, somewhere warmer to sleep.
struct Tent {
    int id = 0;
    IRect rect;              // the tiles it covers
    int dir = 0;             // which way the door faces: 0 N, 1 E, 2 S, 3 W
    std::string item;        // the item it packs back into
};

// Carpentry: things the player has built. Edge pieces sit on the north (dir 0)
// or west (dir 1) edge of a tile, like the walls of the houses.
enum class BuildKind : uint8_t { Wall, Door, Window, Floor, Roof, Crate, RainCollector, Bed, Fence, Shelf, DryingRack, BoxTrap, Count };
struct Built {
    BuildKind kind = BuildKind::Wall;
    int x = 0, y = 0, dir = 0;
};
const char* buildKindName(BuildKind k);
inline bool isEdgeBuild(BuildKind k) { return k == BuildKind::Wall || k == BuildKind::Door || k == BuildKind::Window; }

// Traps: a wire snare (hare, squirrel, grouse), a fish trap in shallow
// water (fills up with fish and crayfish), a built box trap (fox, badger,
// beaver by the water)
enum class TrapKind : uint8_t { Snare, FishTrap, BoxTrap };
struct Snare {
    Vec2 pos;
    float minutesSet = 0;
    bool caught = false;
    int id = 0;
    TrapKind kind = TrapKind::Snare;
    int fish = 0, crayfish = 0;   // fish trap: what's in it
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
    // An upper floor (+1) or basement (-1) of a building: made the first time,
    // and the chunks it stands in rebuilt to show it. -1 if there is none.
    int ensureFloor(int baseId, int level);
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

    // --- height (metres above sea level) ----------------------------------
    float terrainHeight(Vec2 p) const { return map_.elevation(p.x, p.y); }   // the natural ground
    // What you stand on there: a building's floor, a jetty, the bottom of
    // shallow water, or the ground
    float standHeight(Vec2 p);
    // How steep the ground is going in `dir` (unit): rise per metre, + uphill.
    // Level inside buildings.
    float slope(Vec2 p, Vec2 dir);

    // --- movement --------------------------------------------------------
    bool tileBlocked(int x, int y);                           // water, solid props
    bool edgeBlocked(int x, int y, int dx, int dy);           // wall/closed door between neighbours
    // Slide a circle of `radius` from `from` by `delta`, stopping at walls.
    Vec2 moveCircle(Vec2 from, Vec2 delta, float radius);
    // Nothing solid between a and b: walls and closed doors block, windows don't
    bool lineOfSight(Vec2 a, Vec2 b);
    // A wall (not a window, not an open door) between the tile and its neighbour
    bool sightBlocked(int x, int y, int dx, int dy);

    // --- doors -----------------------------------------------------------
    // dir: 0 north edge of (x,y), 1 west edge of (x,y)
    bool doorAt(int x, int y, int dir);
    bool doorOpen(int x, int y, int dir) const;
    void toggleDoor(int x, int y, int dir);
    // Find a door edge within reach of a point. Returns false if none.
    bool findDoorNear(Vec2 p, float reach, int& x, int& y, int& dir);
    // --- windows (dir as for doors) ----------------------------------------
    bool windowAt(int x, int y, int dir);
    bool windowBroken(int x, int y, int dir) const;
    void breakWindow(int x, int y, int dir);
    bool findWindowNear(Vec2 p, float reach, int& x, int& y, int& dir);

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
    float tankWater(int x, int y);                 // ml left (without taking any)

    // Foraging state of a tile
    bool harvested(int x, int y, float regrowDays);
    void markHarvested(int x, int y);

    // --- shelters & carpentry ---------------------------------------------
    std::vector<Tent>& tents() { return tents_; }
    Tent* tentAt(Vec2 p);
    bool canPitch(const IRect& r, std::string* why);
    std::vector<Built>& builds() { return builds_; }
    bool canBuild(const Built& b, std::string* why);
    void addBuild(const Built& b);
    bool removeBuild(const Built& b);
    const Built* builtAt(int x, int y, BuildKind kind) const;
    const Built* builtEdgeAt(int x, int y, int dir) const;
    bool roofAt(int x, int y) const;

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
    std::unordered_set<int64_t> brokenWindows_;              // key as for doors
    std::unordered_map<int64_t, ContainerState> containers_;
    std::unordered_map<int64_t, std::vector<ItemInstance>> ground_;
    std::unordered_map<int64_t, float> tankWater_;
    std::vector<Campfire> fires_;
    std::vector<Snare> snares_;
    std::vector<Tent> tents_;
    std::vector<Built> builds_;
    void applyBuild(const Built& b, bool on);
    int nextId_ = 1;
    float clockTime_ = 0;
    uint32_t versionCounter_ = 1;
    double powerOffMinutes_ = 0;
    float snowCover_ = 0, wetness_ = 0;   // chunk versions are unique, so stale meshes are always rebuilt
public:
    int newId() { return nextId_++; }
};

} // namespace alone
