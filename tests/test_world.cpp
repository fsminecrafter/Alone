#include "TestFramework.h"
#include "core/Paths.h"
#include "items/ItemRegistry.h"
#include "world/World.h"
#include <chrono>
#include <cstdio>
#include <map>

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
    CHECK(m.pixelWidth() == 1365);
    CHECK(m.widthM() > 9000 && m.widthM() < 9500);
    CHECK(!m.roads().empty());
    CHECK(!m.rails().empty());
    CHECK(m.landmarks().size() >= 10);
    // Lake Mjörn is west of town, Gerdsken south of centre
    CHECK(m.zoneAt(100 * 6.85f, 300 * 6.85f) == Zone::Water);
    CHECK(m.zoneAt(735 * 6.85f, 560 * 6.85f) == Zone::Water);
    CHECK(m.districtAt({650 * 6.85f, 375 * 6.85f}) == "Centrum");
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
