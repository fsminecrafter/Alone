#include "TestFramework.h"
#include "core/Paths.h"
#include "items/ItemRegistry.h"
#include "world/World.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

using namespace alone;

namespace {
struct Fixture {
    MapData map;
    ItemRegistry items;
    std::unique_ptr<World> world;
    Fixture() {
        map.load(Paths::data("maps/alingsas"));
        items.loadDirectory(Paths::data("items"));
        items.loadContainers(Paths::data("loot/containers.ini"));
        world = std::make_unique<World>(map, items, 1234);
    }
};
Fixture& fx() { static Fixture f; return f; }
}

TEST(map_loads) {
    auto& m = fx().map;
    CHECK(m.pixelWidth() >= 1365);
    CHECK(m.widthM() > 9000 && m.widthM() < 9500);
    CHECK(!m.roads().empty());
    CHECK(!m.rails().empty());
    CHECK(m.landmarks().size() >= 10 || m.osmBuildings().size() > 3000);
    // Lake Mjörn is west of town, Gerdsken south of centre
    CHECK(m.zoneAt(100 * 6.85f, 300 * 6.85f) == Zone::Water);
    CHECK(m.zoneAt(735 * 6.85f, 560 * 6.85f) == Zone::Water);
    CHECK(m.districtAt({650 * 6.85f, 375 * 6.85f}) == "Centrum");
}

TEST(vimpeln_and_storken) {
    // Vimpeln by Gerdsken is the big shopping centre with the food court;
    // Gallerian Storken on Kungsgatan is the small gallery with a café.
    auto& w = *fx().world;
    const MapData& m = fx().map;
    for (const auto& b : m.osmBuildings())
        if (b.name == "Vimpeln" || b.name == "Gallerian Storken") w.tile((int)b.centre.x, (int)b.centre.y);   // generates the area
    const Building* vimpeln = nullptr;
    const Building* storken = nullptr;
    for (const auto& b : w.gen().allBuildings()) {
        if (b.name == "Vimpeln") vimpeln = &b;
        if (b.name == "Gallerian Storken") storken = &b;
    }
    CHECK(vimpeln != nullptr);
    CHECK(storken != nullptr);
    if (!vimpeln || !storken) return;
    CHECK(vimpeln->type == BuildingType::Mall);
    CHECK(vimpeln->rect.area() > storken->rect.area() * 2);
    bool court = false, tools = false, pharmacy = false;
    for (const auto& r : vimpeln->rooms) {
        if (r.loot == "food_court_counter") court = true;
        if (r.loot == "tool_store_shelf") tools = true;
        if (r.loot == "pharmacy_shelf") pharmacy = true;
    }
    CHECK(court);
    CHECK(tools);
    CHECK(pharmacy);
    bool cafe = false, storkenCourt = false;
    for (const auto& r : storken->rooms) {
        if (r.loot == "cafe_counter") cafe = true;
        if (r.loot == "food_court_counter") storkenCourt = true;
    }
    CHECK(cafe);
    CHECK(!storkenCourt);
    CHECK(vimpeln->sign == "Vimpeln");
}

TEST(items_load) {
    auto& it = fx().items;
    CHECK(it.all().size() > 60);
    const ItemDef* hare = it.find("hare_raw");
    CHECK(hare && hare->raw && hare->nutrients.protein > 90);
    CHECK(it.find("hare_cooked") != nullptr);
    CHECK(it.container("fridge") != nullptr);
    auto loot = it.rollLoot("kitchen_cupboard", 99);
    auto loot2 = it.rollLoot("kitchen_cupboard", 99);
    CHECK(loot.size() == loot2.size());   // deterministic
}

TEST(world_generates_town) {
    World& w = *fx().world;
    auto t0 = std::chrono::steady_clock::now();
    Vec2 centre{650 * 6.85f, 375 * 6.85f};
    std::map<Ground, int> grounds;
    int buildingsTiles = 0, trees = 0, walls = 0;
    for (int cy = -4; cy < 4; ++cy)
        for (int cx = -4; cx < 4; ++cx) {
            Chunk& c = w.chunk((int)centre.x / kChunkSize + cx, (int)centre.y / kChunkSize + cy);
            for (auto& t : c.tiles) {
                grounds[t.ground]++;
                if (t.building >= 0) ++buildingsTiles;
                if (isTree(t.prop)) ++trees;
                if (t.walls) ++walls;
            }
        }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    64 chunks in %.0f ms, %zu buildings known, building tiles %d, walls %d, asphalt %d\n", ms,
                w.gen().allBuildings().size(), buildingsTiles, walls, grounds[Ground::Asphalt]);
    CHECK(buildingsTiles > 2000);
    CHECK(grounds[Ground::Asphalt] > 1000);
    CHECK(walls > 500);
}

TEST(start_house_exists) {
    World& w = *fx().world;
    const Building* b = w.gen().findStartHouse(fx().map.spawn());
    CHECK(b != nullptr);
    if (!b) return;
    std::printf("    start house at %d,%d %dx%d rooms %zu furniture %zu\n", b->rect.x, b->rect.y, b->rect.w, b->rect.h,
                b->rooms.size(), b->furniture.size());
    CHECK(b->rooms.size() >= 2);
    CHECK(!b->furniture.empty());
    // The door leads outside and isn't blocked
    CHECK(!w.tileBlocked((int)b->doorPos.x, (int)b->doorPos.y));
}

// Every room of every home can be reached from the front door, going
// through doors only, and every home has a kitchen (or kitchen-living room)
TEST(home_rooms_reachable) {
    World& w = *fx().world;
    int homes = 0, bad = 0, noKitchen = 0;
    for (const Building& b : w.gen().allBuildings()) {
        bool home = b.type == BuildingType::House || b.type == BuildingType::Cottage || b.type == BuildingType::Farmhouse ||
                    b.type == BuildingType::Manor;
        if (!home || b.rooms.empty()) continue;
        ++homes;
        const IRect& r = b.rect;
        // walls (and which of them are doors) by edge
        std::map<int64_t, uint8_t> edge;
        for (const auto& e : b.edges) edge[tileKey(e.x, e.y)] |= e.bits;
        auto open = [&](int x, int y, int nx, int ny) {   // can you step from (x,y) to the neighbour?
            int ex = std::max(x, nx), ey = std::max(y, ny);
            uint8_t bits = edge.count(tileKey(ex, ey)) ? edge[tileKey(ex, ey)] : 0;
            if (nx != x) return !(bits & WallW) || (bits & DoorW);
            return !(bits & WallN) || (bits & DoorN);
        };
        // start on the inside of the front door
        int sx = std::clamp((int)std::floor(b.doorPos.x), r.x, r.right() - 1);
        int sy = std::clamp((int)std::floor(b.doorPos.y), r.y, r.bottom() - 1);
        std::set<int64_t> seen{tileKey(sx, sy)};
        std::vector<std::pair<int, int>> todo{{sx, sy}};
        while (!todo.empty()) {
            auto [x, y] = todo.back();
            todo.pop_back();
            const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& dd : d) {
                int nx = x + dd[0], ny = y + dd[1];
                if (!r.contains(nx, ny) || seen.count(tileKey(nx, ny)) || !open(x, y, nx, ny)) continue;
                seen.insert(tileKey(nx, ny));
                todo.push_back({nx, ny});
            }
        }
        bool kitchen = false;
        for (const Room& room : b.rooms) {
            kitchen |= room.type == RoomType::Kitchen || room.type == RoomType::Living;
            bool reached = false;
            for (int y = room.rect.y; y < room.rect.bottom() && !reached; ++y)
                for (int x = room.rect.x; x < room.rect.right() && !reached; ++x) reached = seen.count(tileKey(x, y)) > 0;
            if (!reached) {
                if (bad < 3)
                    std::printf("    unreachable %s in %dx%d home at %d,%d\n", roomTypeName(room.type), r.w, r.h, r.x, r.y);
                ++bad;
            }
        }
        if (!kitchen) ++noKitchen;
    }
    std::printf("    %d homes, %d unreachable rooms, %d without a kitchen\n", homes, bad, noKitchen);
    CHECK(homes > 100);
    CHECK(bad == 0);
    CHECK(noKitchen == 0);
}

// Heights from the Copernicus elevation model: the lakes, the valley the
// town is in and the hills around it, and level floors in buildings
TEST(elevation_of_alingsas) {
    World& w = *fx().world;
    const MapData& m = fx().map;
    CHECK(m.hasElevation());
    if (!m.hasElevation()) return;
    float mjorn = m.elevation(1000, 3000);        // the lake Mjörn, west of town
    float town = m.elevation(4456, 2534);         // Gallerian Storken
    float hjortmarka = m.elevation(7443, 2207);   // the forest hills in the east
    std::printf("    Mjörn %.1f m, Storken %.1f m, Hjortmarka %.1f m, range %.0f..%.0f m\n", mjorn, town, hjortmarka,
                m.lowestElevation(), m.highestElevation());
    CHECK(std::fabs(mjorn - 57.0f) < 1.5f);
    CHECK(town > mjorn + 2 && town < 90);
    CHECK(hjortmarka > town + 15);
    // a lake is flat right up to its shore
    CHECK(std::fabs(m.waterLevel(1000, 3000) - m.waterLevel(1200, 3100)) < 0.5f);
    // you stand on a building's level floor; its floor is near the ground
    const Building* b = w.gen().findStartHouse(fx().map.spawn());
    CHECK(b != nullptr);
    if (!b) return;
    Vec2 a{b->rect.x + 1.5f, b->rect.y + 1.5f}, c{b->rect.right() - 1.5f, b->rect.bottom() - 1.5f};
    CHECK(std::fabs(w.standHeight(a) - b->floorY) < 0.01f);
    CHECK(std::fabs(w.standHeight(c) - b->floorY) < 0.01f);
    CHECK(std::fabs(b->floorY - m.elevation(b->rect.centre().x, b->rect.centre().y)) < 3.0f);
    CHECK(w.slope(a, Vec2{1, 0}) == 0.0f);   // indoors it's level
}

TEST(walls_block_movement) {
    World& w = *fx().world;
    const Building* b = w.gen().findStartHouse(fx().map.spawn());
    if (!b) return;
    // Walk from outside straight into the north wall away from doors: should stop
    Vec2 outside{b->rect.x + 0.5f, b->rect.y - 2.0f};
    Vec2 end = w.moveCircle(outside, {0, 4.0f}, 0.3f);
    CHECK(end.y < b->rect.y);
}

TEST(chunks_unload_and_keep_changes) {
    World& w = *fx().world;
    Vec2 a{3000, 2500};
    // Find a tree, chop it (permanent change), walk far away, come back
    int tx = -1, ty = -1;
    for (int y = 2500; y < 2600 && tx < 0; ++y)
        for (int x = 3000; x < 3100; ++x)
            if (isTree(w.tile(x, y).prop)) { tx = x; ty = y; break; }
    if (tx < 0) return;
    w.setProp(tx, ty, Prop::Stump);
    uint32_t v1 = w.chunk(floorDiv(tx, kChunkSize), floorDiv(ty, kChunkSize)).version;
    w.unloadFarChunks(Vec2{8000, 4000}, 300);
    CHECK(w.chunkIfLoaded(floorDiv(tx, kChunkSize), floorDiv(ty, kChunkSize)) == nullptr);
    CHECK(w.tile(tx, ty).prop == Prop::Stump);
    uint32_t v2 = w.chunk(floorDiv(tx, kChunkSize), floorDiv(ty, kChunkSize)).version;
    CHECK(v2 != v1);
    (void)a;
}
