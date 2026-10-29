// Upper floors and basements, carpentry skill, trapping, preserving food,
// the military container, farms and summer cottages.
#include "TestFramework.h"
#include "game/Game.h"
#include <cmath>
#include <cstdio>

using namespace alone;

namespace {
void run(Game& g, float seconds, PlayerInput in = {}) {
    for (float t = 0; t < seconds; t += 0.05f) g.update(0.05f, in);
}
void finish(Game& g) {
    for (int i = 0; i < 4000 && g.currentAction().active; ++i) g.update(0.05f, PlayerInput{});
}
void give(Game& g, const char* id, int n = 1) {
    Rng rng(5);
    for (int i = 0; i < n; ++i) g.player().inventory.add(g.items().create(id, rng));
}
Survivor* find(Game& g, const char* id) {
    for (auto& s : g.survivors())
        if (s.def->id == id) return &s;
    return nullptr;
}
// A building near the start with this kind of stairs (dir +1 up, -1 down)
const Building* withStairs(Game& g, int dir, bool big = false) {
    Vec2 c = g.player().pos;
    const Building* best = nullptr;
    float bestD = 1e9f;
    for (int id : g.world().gen().buildingsIn(IRect{(int)c.x - 400, (int)c.y - 400, 800, 800})) {
        const Building* b = g.world().building(id);
        if (!b || b->levelOf >= 0) continue;
        int n = 0, mains = 0;
        for (const auto& s : b->stairs) {
            if (s.dir != dir) continue;
            if (g.world().tile(s.x, s.y).prop == Prop::BrokenStairs) continue;
            ++n;
            mains += s.main;
        }
        if (n == 0 || (big && (n < 2 || mains != 1))) continue;
        float d = distance(b->rect.centre(), c);
        if (d < bestD) { bestD = d; best = b; }
    }
    return best;
}
// Take the stairs at (x, y)
void takeStairs(Game& g, int x, int y) {
    g.player().pos = Vec2{x + 0.5f, y + 1.4f};
    for (int k = 0; k < 4; ++k) {   // stand on a free side of them
        static const int dx[4] = {0, 1, 0, -1}, dy[4] = {1, 0, -1, 0};
        if (!g.world().tileBlocked(x + dx[k], y + dy[k])) { g.player().pos = Vec2{x + dx[k] + 0.5f, y + dy[k] + 0.5f}; break; }
    }
    for (auto& o : g.actionsAt(Vec2{x + 0.5f, y + 0.5f}))
        if (o.kind == ActionKind::UseStairs && o.enabled) { g.perform(o); finish(g); return; }
}
} // namespace

TEST(houses_have_stairs_up_and_down) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    const Building* house = withStairs(g, 1);
    CHECK(house != nullptr);
    if (!house) return;
    int baseId = house->id;
    Stair up{};
    for (const auto& s : house->stairs) if (s.dir > 0 && g.world().tile(s.x, s.y).prop == Prop::StairsUp) up = s;
    std::printf("    %s with %d staircase(s), stairs up at %d,%d\n", buildingTypeName(house->type), (int)house->stairs.size(), up.x, up.y);
    takeStairs(g, up.x, up.y);
    CHECK(g.floorLevel() == 1);
    const Building* floor = g.world().buildingAt((int)g.player().pos.x, (int)g.player().pos.y);
    CHECK(floor && floor->levelOf == baseId && floor->level == 1);
    CHECK(g.world().tile(up.x + kUpperFloorOffset, up.y).prop == Prop::StairsDown);
    CHECK(g.location().rfind("Upstairs", 0) == 0);
    CHECK(!g.world().tileBlocked((int)g.player().pos.x, (int)g.player().pos.y));
    std::printf("    upstairs: %s\n", g.location().c_str());
    // no way out but the stairs
    bool door = false;
    if (floor)
        for (const auto& e : floor->edges) {
            bool outer = e.x == floor->rect.x || e.x == floor->rect.right() || e.y == floor->rect.y || e.y == floor->rect.bottom();
            door |= outer && (e.bits & (DoorN | DoorW));
        }
    CHECK(!door);
    // saved and loaded up here, it's still here
    std::string path = "test_floors_save.txt";
    CHECK(g.save(path));
    Game h;
    CHECK(h.loadData());
    CHECK(h.load(path));
    CHECK(h.floorLevel() == 1);
    const Building* f2 = h.world().buildingAt((int)h.player().pos.x, (int)h.player().pos.y);
    CHECK(f2 && f2->level == 1);
    std::remove(path.c_str());
    // and back down
    takeStairs(g, up.x + kUpperFloorOffset, up.y);
    CHECK(g.floorLevel() == 0);
    CHECK(distance(g.player().pos, Vec2{up.x + 0.5f, up.y + 0.5f}) < 2.0f);

    // a basement somewhere: shelves, no windows
    const Building* cellar = withStairs(g, -1);
    CHECK(cellar != nullptr);
    if (!cellar) return;
    Stair down{};
    for (const auto& s : cellar->stairs) if (s.dir < 0) down = s;
    takeStairs(g, down.x, down.y);
    CHECK(g.floorLevel() == -1);
    const Building* bf = g.world().buildingAt((int)g.player().pos.x, (int)g.player().pos.y);
    bool windows = false, shelves = false;
    if (bf) {
        for (const auto& e : bf->edges) windows |= (e.bits & (WindowN | WindowW)) != 0;
        for (const auto& f : bf->furniture) shelves |= f.container == "basement_shelf";
    }
    CHECK(bf && !windows && shelves);
    std::printf("    basement: %s\n", g.location().c_str());
}

TEST(big_buildings_have_main_and_side_stairs) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    const Building* big = withStairs(g, 1, true);
    CHECK(big != nullptr);
    if (!big) return;
    int ups = 0, mains = 0;
    for (const auto& s : big->stairs) if (s.dir > 0) { ++ups; mains += s.main; }
    std::printf("    %s %dx%d: %d stairs up, %d main\n", buildingTypeName(big->type), big->rect.w, big->rect.h, ups, mains);
    CHECK(ups >= 2 && mains == 1);
    // every staircase arrives on the same floor, right above where it starts
    Stair side{};
    for (const auto& s : big->stairs) if (s.dir > 0 && !s.main && g.world().tile(s.x, s.y).prop == Prop::StairsUp) side = s;
    if (side.dir == 0) return;
    takeStairs(g, side.x, side.y);
    CHECK(g.floorLevel() == 1);
    int downs = 0;
    for (const auto& s : big->stairs)
        if (s.dir > 0 && g.world().tile(s.x + kUpperFloorOffset, s.y).prop == Prop::StairsDown) ++downs;
    CHECK(downs == ups);
}

TEST(carpentry_levels_and_new_builds) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    CHECK(g.carpentryLevel() == 0);
    std::string why;
    CHECK(!g.canAffordBuild(BuildKind::DryingRack, &why));
    give(g, "hammer");
    give(g, "saw");
    give(g, "plank", 12);
    give(g, "nails", 3);
    give(g, "rope");
    CHECK(!g.canAffordBuild(BuildKind::DryingRack, &why) && why.find("carpentry") != std::string::npos);
    g.carpentryPractice(Game::carpentryXpFor(1));
    CHECK(g.carpentryLevel() == 1);
    CHECK(g.canAffordBuild(BuildKind::DryingRack, &why));
    CHECK(!g.canAffordBuild(BuildKind::BoxTrap, &why));   // needs Competent
    g.carpentryPractice(Game::carpentryXpFor(2) - g.player().carpentryXp);
    CHECK(g.canAffordBuild(BuildKind::BoxTrap, &why));
    std::printf("    level %d: %s\n", g.carpentryLevel(), Game::carpentryLevelName(g.carpentryLevel()));
}

TEST(drying_smoking_and_salting) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    // salt: raw venison into salted meat, right away
    give(g, "venison_raw");
    give(g, "salt");
    Player& p = g.player();
    ItemRef ref{ItemLoc::Player, 0, 0, 0, p.inventory.items()[p.inventory.findId("venison_raw")].uid};
    ActionOption salt;
    for (auto& o : g.actionsForItem(ref)) if (o.kind == ActionKind::SaltFood) salt = o;
    CHECK(salt.kind == ActionKind::SaltFood && salt.enabled);
    g.perform(salt);
    finish(g);
    int salted = 0;
    for (const auto& it : p.inventory.items()) salted += it.def->id == "salted_meat";
    CHECK(p.inventory.findId("venison_raw") < 0 && salted >= 2);
    CHECK(p.inventory.items()[p.inventory.findTag("salt")].usesLeft == 5);
    // a rack outdoors: fish dries in a day and a half, beside a fire it smokes in hours
    Vec2 at = p.pos + Vec2{40, 0};
    int rx = (int)at.x, ry = (int)at.y;
    g.world().setProp(rx, ry, Prop::None);
    g.world().addBuild(Built{BuildKind::DryingRack, rx, ry, 0});
    Rng rng(3);
    g.world().container(rx, ry).items.push_back(g.items().create("fish_raw", rng));
    Weather dry;
    dry.kind = WeatherKind::Clear;
    g.world().clock().forceWeather(dry, g.world().clock().totalMinutes() + 3000);
    g.updateDryingRacks(30 * 60.0f);
    CHECK(g.world().container(rx, ry).items[0].def->id == "fish_raw");
    g.updateDryingRacks(8 * 60.0f);
    CHECK(g.world().container(rx, ry).items[0].def->id == "dried_fish");
    // smoke
    auto& items = g.world().container(rx, ry).items;
    items.clear();
    items.push_back(g.items().create("fish_raw", rng));
    Campfire f;
    f.pos = Vec2{rx + 1.5f, ry + 0.5f};
    f.lit = true;
    f.fuelMinutes = 600;
    f.id = g.world().newId();
    g.world().campfires().push_back(f);
    g.updateDryingRacks(7 * 60.0f);
    CHECK(!items.empty() && items[0].def->id == "smoked_fish");
    std::printf("    salted %d pieces, rack: %s\n", salted, items.empty() ? "-" : items[0].def->name.c_str());
}

TEST(traps_catch_more_than_hares) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    for (const char* a : {"red_squirrel", "black_grouse", "fox", "badger", "beaver"}) CHECK(g.animals().find(a) != nullptr);
    // a fish trap in the shallows fills up and can be emptied
    Snare t;
    t.kind = TrapKind::FishTrap;
    t.pos = g.player().pos + Vec2{1, 0};
    t.id = g.world().newId();
    g.world().snares().push_back(t);
    for (int i = 0; i < 200; ++i) g.update(0.05f, PlayerInput{});
    Snare& s = g.world().snares().back();
    s.fish = 2;
    s.crayfish = 1;
    ActionOption empty;
    for (auto& o : g.actionsAt(s.pos)) if (o.kind == ActionKind::EmptyTrap) empty = o;
    CHECK(empty.kind == ActionKind::EmptyTrap && empty.enabled);
    g.perform(empty);
    finish(g);
    int fish = 0, cray = 0;
    for (const auto& it : g.player().inventory.items()) { fish += it.def->id == "fish_raw"; cray += it.def->id == "crayfish_raw"; }
    CHECK(fish == 2 && cray == 1);
}

TEST(military_container_and_the_soldier) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    const Building* box = nullptr;
    for (const auto& b : g.world().gen().allBuildings())
        if (b.type == BuildingType::MilitaryContainer) box = &b;
    CHECK(box && box->name == "Militärcontainer");
    if (!box) return;
    int crates = 0;
    for (const auto& f : box->furniture) crates += f.container == "military_crate";
    std::printf("    container at %d,%d %dx%d, %d crates, floor %.1f\n", box->rect.x, box->rect.y, box->rect.w, box->rect.h, crates, box->floorY);
    CHECK(crates >= 3);
    CHECK(g.items().find("ak5") && g.items().find("ak5")->magazine == 30);
    // Oskar tells you where it is once he trusts you
    Survivor* oskar = find(g, "oskar");
    CHECK(oskar != nullptr);
    if (!oskar) return;
    g.player().pos = oskar->pos + Vec2{1, 0};
    g.greeting(oskar->id);
    oskar->trust = 10;
    g.talk(oskar->id, Topic::Others);
    CHECK(g.knownPlaces().empty());
    oskar->trust = 35;
    std::string told = g.talk(oskar->id, Topic::Others);
    CHECK(g.knownPlaces().size() == 1);
    if (!g.knownPlaces().empty()) CHECK(box->rect.contains((int)g.knownPlaces()[0].pos.x, (int)g.knownPlaces()[0].pos.y));
    std::printf("    \"%s...\"\n", told.substr(0, 80).c_str());
    // armour: a helmet takes half of a blow to the head
    give(g, "combat_helmet");
    g.player().worn[(size_t)ClothingSlot::Head] = g.player().inventory.items().back();
    CHECK(std::fabs(g.armourAt(BodyPart::Head) - 0.5f) < 0.01f && g.armourAt(BodyPart::LeftLeg) == 0);
}

TEST(farms_and_summer_cottages) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    int farms = 0, cottages = 0, barns = 0;
    // the countryside all round town
    for (int y = 0; y < 5300; y += 400)
        for (int x = 0; x < 9400; x += 400)
            for (int id : g.world().gen().buildingsIn(IRect{x, y, 400, 400})) {
                const Building* b = g.world().building(id);
                if (!b) continue;
                farms += b->type == BuildingType::Farmhouse;
                cottages += b->type == BuildingType::SummerCottage;
                barns += b->type == BuildingType::Barn;
            }
    std::printf("    %d farmhouses, %d barns, %d summer cottages\n", farms, barns, cottages);
    CHECK(farms >= 5 && cottages >= 5 && barns >= 3);
}
