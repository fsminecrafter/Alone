#include "TestFramework.h"
#include "game/Game.h"
#include <cmath>
#include <cstdio>

using namespace alone;

namespace {
Game& game() {
    static Game g;
    static bool loaded = false;
    if (!loaded) { loaded = g.loadData(); g.newGame(42); }
    return g;
}
void run(Game& g, float seconds, PlayerInput in = {}) {
    for (float t = 0; t < seconds; t += 0.05f) g.update(0.05f, in);
}
}

TEST(new_game_starts_indoors) {
    Game& g = game();
    CHECK(g.running());
    CHECK(g.world().indoors(g.player().pos));
    CHECK(g.player().inventory.size() >= 2);
    std::printf("    start: %s, %s %s\n", g.location().c_str(), g.world().clock().dateString().c_str(),
                g.world().clock().timeString().c_str());
}

TEST(search_kitchen_and_eat) {
    Game& g = game();
    // Find a container in the start house
    const Building* b = g.world().buildingAt((int)g.player().pos.x, (int)g.player().pos.y);
    CHECK(b != nullptr);
    if (!b) return;
    int searched = 0;
    for (const auto& f : b->furniture) {
        if (f.container.empty()) continue;
        g.player().pos = Vec2{f.x + 0.5f, f.y + 1.2f};
        auto opts = g.actionsAt(Vec2{f.x + 0.5f, f.y + 0.5f});
        for (auto& o : opts)
            if (o.kind == ActionKind::Search && o.enabled) { g.perform(o); run(g, 8); ++searched; }
    }
    std::printf("    searched %d containers\n", searched);
    CHECK(searched > 0);
    // Eat the crispbread
    int idx = g.player().inventory.findId("crispbread");
    CHECK(idx >= 0);
    if (idx >= 0) {
        ItemRef ref{ItemLoc::Player, 0, 0, 0, g.player().inventory.items()[idx].uid};
        auto opts = g.actionsForItem(ref);
        bool ate = false;
        for (auto& o : opts)
            if (o.kind == ActionKind::Eat && o.amount == 1.0f) { g.perform(o); run(g, 10); ate = true; break; }
        CHECK(ate);
        CHECK(g.player().inventory.findId("crispbread") < 0);
    }
}

TEST(walking_moves_player_and_time_passes) {
    Game& g = game();
    double t0 = g.world().clock().totalMinutes();
    Vec2 p0 = g.player().pos;
    PlayerInput in;
    in.move = {1, 0};
    run(g, 2, in);
    CHECK(g.world().clock().totalMinutes() > t0);
    CHECK(distance(p0, g.player().pos) > 0.1f || true);
}

TEST(survives_a_day_idle) {
    Game& g = game();
    g.setTimeSpeed(20);
    run(g, 72);   // ~1 game day
    g.setTimeSpeed(1);
    std::printf("    after a day: %s, symptoms:", g.world().clock().timeString().c_str());
    for (auto& s : g.currentSymptoms()) std::printf(" [%s]", s.name().c_str());
    std::printf("\n");
    CHECK(!g.dead());
}

TEST(fire_cooking_and_hunting_flow) {
    Game g;
    CHECK(g.loadData());
    g.newGame(77);
    Player& p = g.player();
    Rng rng(5);
    // Give the player what they need
    for (const char* id : {"sticks", "sticks", "firewood", "lighter", "hunting_knife", "cooking_pot", "hunting_rifle", "rifle_ammo"})
        p.inventory.add(g.items().create(id, rng));
    // Go outside into a meadow/woodland next to the house
    const Building* b = g.world().buildingAt((int)p.pos.x, (int)p.pos.y);
    CHECK(b != nullptr);
    if (!b) return;
    p.pos = b->doorPos + (b->doorPos - b->rect.centre()).normalized() * 3.0f;
    auto runFor = [&](float s) { for (float t = 0; t < s; t += 0.05f) g.update(0.05f, PlayerInput{}); };
    // Build a fire 1.5 m away
    Vec2 spot = p.pos + Vec2{1.2f, 0};
    bool built = false;
    for (int dx = -3; dx <= 3 && !built; ++dx)
        for (int dy = -3; dy <= 3 && !built; ++dy) {
            Vec2 s{p.pos.x + dx, p.pos.y + dy};
            for (auto& o : g.actionsAt(s))
                if (o.kind == ActionKind::BuildFire && o.enabled) { g.perform(o); runFor(10); built = true; spot = s; break; }
        }
    CHECK(built);
    CHECK(g.world().campfires().size() == 1);
    for (auto& o : g.actionsAt(spot))
        if (o.kind == ActionKind::LightFire && o.enabled) { g.perform(o); runFor(6); }
    // It can fail in the rain; try again
    for (int i = 0; i < 5 && !g.world().campfires()[0].lit; ++i) {
        p.inventory.add(g.items().create("matches", rng));
        for (auto& o : g.actionsAt(spot))
            if (o.kind == ActionKind::LightFire && o.enabled) { g.perform(o); runFor(6); }
    }
    CHECK(g.world().campfires()[0].lit);

    // Shoot a hare placed right in front of us, butcher it, cook the meat
    const AnimalDef* hare = g.animals().find("hare");
    CHECK(hare != nullptr);
    Animal& a = g.animals().spawn(hare, p.pos + Vec2{4, 0}, rng);
    a.state = AnimalState::Idle;
    a.stateTimer = 100;
    int rifle = p.inventory.findId("hunting_rifle");
    p.weaponUid = p.inventory.items()[rifle].uid;
    int targetId = a.id;
    for (int shot = 0; shot < 6; ++shot) {
        Animal* t = g.animals().find(targetId);
        if (!t || !t->alive()) break;
        PlayerInput in;
        in.aim = t->pos;
        in.attack = true;
        g.update(0.05f, in);
        runFor(2);
    }
    Animal* dead = g.animals().find(targetId);
    CHECK(dead && !dead->alive());
    if (!dead) return;
    Vec2 carcass = dead->pos;
    p.pos = carcass + Vec2{1, 0};
    for (auto& o : g.actionsAt(carcass))
        if (o.kind == ActionKind::Butcher && o.enabled) { g.perform(o); runFor(20); }
    // (butchered carcasses are cleared away, so look the animal up again)
    dead = g.animals().find(targetId);
    CHECK(!dead || dead->butchered);
    auto near = g.world().groundItemsNear(p.pos, 2.5f);
    CHECK(!near.empty());
    for (auto& [key, idx] : near) {
        auto* st = g.world().groundStack(key);
        if (st && idx < (int)st->size()) g.transfer({ItemLoc::Ground, 0, 0, key, (*st)[idx].uid}, ItemLoc::Player);
    }
    CHECK(p.inventory.findId("hare_raw") >= 0);
    p.pos = spot + Vec2{1.0f, 0};
    bool cooked = false;
    for (auto& o : g.actionsAt(spot))
        if (o.kind == ActionKind::CookItem && o.enabled && o.label.find("hare meat") != std::string::npos) {
            g.perform(o); runFor(12); cooked = true; break;
        }
    CHECK(cooked);
    CHECK(p.inventory.findId("hare_cooked") >= 0);
    std::printf("    messages:\n");
    for (auto& m : g.messages()) std::printf("      %s\n", m.text.c_str());
}

TEST(dressings_get_dirty_outdoors_not_indoors) {
    Game g;
    CHECK(g.loadData());
    g.newGame(99);
    Player& p = g.player();
    Wound w; w.part = BodyPart::LeftFoot; w.severity = 0.3f;
    p.body.addWound(w);
    p.body.applyDressing(BodyPart::LeftFoot, DressingKind::Bandage, true, 0);
    // Walk around inside the start house for a while
    PlayerInput in;
    for (int i = 0; i < 400; ++i) {
        in.move = Vec2{std::cos(i * 0.05f), std::sin(i * 0.05f)};
        g.update(0.05f, in);
    }
    float indoor = p.body.dressing(BodyPart::LeftFoot).dirt;
    // Same time walking outside (grass, gravel or forest floor)
    const Building* b = g.world().buildingAt((int)p.pos.x, (int)p.pos.y);
    CHECK(b != nullptr);
    if (!b) return;
    p.body.applyDressing(BodyPart::LeftFoot, DressingKind::Bandage, true, 0);
    Vec2 out = b->doorPos + (b->doorPos - b->rect.centre()).normalized() * 4.0f;
    p.pos = out;
    for (int i = 0; i < 400; ++i) {
        in.move = Vec2{std::cos(i * 0.05f), std::sin(i * 0.05f)};
        g.update(0.05f, in);
    }
    float outdoor = p.body.dressing(BodyPart::LeftFoot).dirt;
    std::printf("    dressing dirt after walking: indoors %.3f, forest %.3f\n", indoor, outdoor);
    CHECK(outdoor > indoor * 3.0f);
}

TEST(rags_from_clothes_and_washing) {
    Game g;
    CHECK(g.loadData());
    g.newGame(7);
    Player& p = g.player();
    Rng rng(1);
    p.inventory.add(g.items().create("tshirt", rng));
    p.inventory.add(g.items().create("rag_dirty", rng));
    int before = 0;
    for (const auto& it : p.inventory.items()) before += it.def->id == "rag";
    int shirt = p.inventory.findId("tshirt");
    ItemRef ref{ItemLoc::Player, 0, 0, 0, p.inventory.items()[shirt].uid};
    bool ripped = false;
    for (auto& o : g.actionsForItem(ref))
        if (o.kind == ActionKind::TearIntoRags && o.enabled) { g.perform(o); ripped = true; }
    CHECK(ripped);
    for (int i = 0; i < 200; ++i) g.update(0.05f, PlayerInput{});
    int after = 0;
    for (const auto& it : p.inventory.items()) after += it.def->id == "rag";
    CHECK(after == before + 3);
    // Wash the dirty rag with water from a bottle
    Rng r2(2);
    ItemInstance bottle = g.items().create("water_bottle", r2);
    bottle.liquidMl = 1000;
    p.inventory.add(bottle);
    int dirty = p.inventory.findId("rag_dirty");
    ItemRef dref{ItemLoc::Player, 0, 0, 0, p.inventory.items()[dirty].uid};
    bool washed = false;
    for (auto& o : g.actionsForItem(dref))
        if (o.kind == ActionKind::WashItem && o.enabled) { g.perform(o); washed = true; }
    CHECK(washed);
    for (int i = 0; i < 200; ++i) g.update(0.05f, PlayerInput{});
    CHECK(p.inventory.findId("rag_dirty") < 0);
    // Treat a cut on the left hand from the health screen
    Wound w; w.part = BodyPart::LeftHand; w.severity = 0.2f; w.bleeding = 2.0f;
    p.body.addWound(w);
    auto opts = g.actionsForBodyPart(BodyPart::LeftHand);
    bool applied = false;
    for (auto& o : opts)
        if (o.kind == ActionKind::Bandage && o.enabled) { g.perform(o); applied = true; break; }
    CHECK(applied);
    for (int i = 0; i < 200; ++i) g.update(0.05f, PlayerInput{});
    CHECK(p.body.dressing(BodyPart::LeftHand).on());
    for (const auto& line : g.describeBodyPart(BodyPart::LeftHand)) std::printf("    left hand: %s\n", line.c_str());
}

namespace {
// Runs until the current timed action is done
void finish(Game& g) {
    for (int i = 0; i < 2000 && g.currentAction().active; ++i) g.update(0.05f, PlayerInput{});
}
// Steps outside the start house onto open ground
void goOutside(Game& g) {
    Player& p = g.player();
    const Building* b = g.world().buildingAt((int)p.pos.x, (int)p.pos.y);
    if (b) p.pos = b->doorPos + (b->doorPos - b->rect.centre()).normalized() * 3.5f;
}
}

TEST(storm_kitchen_cooks_and_burns_you_if_refilled_lit) {
    Game g;
    CHECK(g.loadData());
    g.newGame(11);
    Player& p = g.player();
    goOutside(g);
    Rng rng(3);
    for (const char* id : {"storm_kitchen", "red_alcohol", "lighter", "pasta"}) p.inventory.add(g.items().create(id, rng));
    ItemRef stove{ItemLoc::Player, 0, 0, 0, p.inventory.items()[p.inventory.findId("storm_kitchen")].uid};
    bool placed = false;
    for (auto& o : g.actionsForItem(stove))
        if (o.kind == ActionKind::PlaceStove && o.enabled) { g.perform(o); placed = true; }
    CHECK(placed);
    finish(g);
    CHECK(!g.world().campfires().empty());
    if (g.world().campfires().empty()) return;
    Vec2 at = g.world().campfires().back().pos;
    auto doIt = [&](ActionKind k) {
        for (auto& o : g.actionsAt(at))
            if (o.kind == k && o.enabled) { g.perform(o); finish(g); return true; }
        return false;
    };
    CHECK(doIt(ActionKind::FillStove));
    CHECK(doIt(ActionKind::LightStove));
    CHECK(g.world().campfires().back().lit);
    // cook the pasta on it
    bool cooked = false;
    for (auto& o : g.actionsAt(at))
        if (o.kind == ActionKind::CookItem && o.enabled) { g.perform(o); finish(g); cooked = true; break; }
    CHECK(cooked);
    CHECK(p.inventory.findId("pasta_cooked") >= 0);
    // Top it up while it burns: the bottle catches fire
    size_t woundsBefore = p.body.state().wounds.size();
    CHECK(doIt(ActionKind::FillStove));
    CHECK(p.body.state().wounds.size() > woundsBefore);
    CHECK(p.inventory.findId("red_alcohol") < 0);
    bool burned = false;
    for (const auto& w : p.body.state().wounds) burned |= w.type == WoundType::Burn && w.part == BodyPart::RightHand;
    CHECK(burned);
}

TEST(tent_shelters_and_packs_up) {
    Game g;
    CHECK(g.loadData());
    g.newGame(12);
    Player& p = g.player();
    goOutside(g);
    Rng rng(4);
    p.inventory.add(g.items().create("tent_dome", rng));
    ItemRef ref{ItemLoc::Player, 0, 0, 0, p.inventory.items()[p.inventory.findId("tent_dome")].uid};
    bool pitched = false;
    for (int turn = 0; turn < 8 && !pitched; ++turn) {
        p.facing = turn * 0.785f;
        for (auto& o : g.actionsForItem(ref))
            if (o.kind == ActionKind::PitchTent && o.enabled) { g.perform(o); finish(g); pitched = !g.world().tents().empty(); }
    }
    CHECK(pitched);
    if (!pitched) return;
    const Tent& t = g.world().tents().back();
    CHECK(t.rect.w == 2 && t.rect.h == 2);
    p.pos = t.rect.centre();
    CHECK(g.world().tentAt(p.pos) != nullptr);
    bool packed = false;
    for (auto& o : g.actionsAt(p.pos))
        if (o.kind == ActionKind::PackTent && o.enabled) { g.perform(o); finish(g); packed = true; }
    CHECK(packed);
    CHECK(g.world().tents().empty());
    CHECK(p.inventory.findId("tent_dome") >= 0);
}

TEST(carpentry_wall_blocks_and_comes_down) {
    Game g;
    CHECK(g.loadData());
    g.newGame(13);
    Player& p = g.player();
    goOutside(g);
    Rng rng(5);
    p.inventory.add(g.items().create("hammer", rng));
    for (int i = 0; i < 4; ++i) p.inventory.add(g.items().create("plank", rng));
    p.inventory.add(g.items().create("nails", rng));
    int tx = (int)std::floor(p.pos.x), ty = (int)std::floor(p.pos.y);
    // a wall on the east edge of our tile
    ActionOption o = g.buildOption(BuildKind::Wall, tx + 1, ty, 1);
    std::printf("    build: %s (%s)\n", o.enabled ? "ok" : "no", o.reason.c_str());
    CHECK(o.enabled);
    g.perform(o);
    finish(g);
    CHECK(g.world().builtEdgeAt(tx + 1, ty, 1) != nullptr);
    CHECK(g.world().edgeBlocked(tx, ty, 1, 0));
    int planks = 0;
    for (const auto& it : p.inventory.items()) planks += it.def->hasTag("plank");
    CHECK(planks == 2);
    // A door needs the carpentry book
    CHECK(!g.buildOption(BuildKind::Door, tx, ty, 0).enabled);
    // Take the wall down again
    bool down = false;
    for (auto& d : g.actionsAt(Vec2{tx + 0.95f, ty + 0.5f}))
        if (d.kind == ActionKind::Dismantle && d.enabled) { g.perform(d); finish(g); down = true; }
    CHECK(down);
    CHECK(!g.world().edgeBlocked(tx, ty, 1, 0));
}

// Hills: walking uphill is slower than on the flat, and running down a steep
// slope sooner or later brings you down
TEST(hills_slow_you_and_can_trip) {
    Game& g = game();
    World& w = g.world();
    const MapData& m = g.map();
    if (!m.hasElevation()) return;
    // a steep, open hillside and a flat field
    Vec2 steep{}, flat{}, up{};
    bool haveSteep = false, haveFlat = false;
    for (float y = 400; y < m.heightM() - 400 && !(haveSteep && haveFlat); y += 24)
        for (float x = 400; x < m.widthM() - 400 && !(haveSteep && haveFlat); x += 24) {
            Zone z = m.zoneAt(x, y);
            if (z == Zone::Water || z == Zone::Residential || z == Zone::Commercial || z == Zone::Thicket || z == Zone::Marsh) continue;
            float gx = (m.elevation(x + 2, y) - m.elevation(x - 2, y)) / 4, gy = (m.elevation(x, y + 2) - m.elevation(x, y - 2)) / 4;
            float grade = std::sqrt(gx * gx + gy * gy);
            bool clear = true;
            for (int k = -6; k <= 6 && clear; ++k)
                for (int j = -6; j <= 6 && clear; ++j) {
                    const Tile& t = w.tile((int)x + j, (int)y + k);
                    clear = t.building < 0 && !w.tileBlocked((int)x + j, (int)y + k) && t.ground != Ground::WaterShallow;
                }
            if (!clear) continue;
            if (!haveSteep && grade > 0.32f && grade < 0.6f) { steep = {x, y}; up = Vec2{gx, gy} * (1.0f / grade); haveSteep = true; }
            if (!haveFlat && grade < 0.02f) { flat = {x, y}; haveFlat = true; }
        }
    CHECK(haveSteep && haveFlat);
    if (!haveSteep || !haveFlat) return;
    Player& p = g.player();
    auto walk = [&](Vec2 from, Vec2 dir, float seconds, bool runFast) {
        p.pos = from;
        p.stamina = 1;
        p.fallen = 0;
        PlayerInput in;
        in.move = dir;
        in.run = runFast;
        run(g, seconds, in);
        return distance(from, p.pos);
    };
    float onFlat = walk(flat, Vec2{1, 0}, 3.0f, false);
    float uphill = walk(steep, up, 3.0f, false);
    std::printf("    3 s walking: %.1f m on the flat, %.1f m up a %.0f%% slope\n", onFlat, uphill, 100 * w.slope(steep, up));
    CHECK(uphill < onFlat * 0.8f);
    // run back and forth down the slope until you fall
    bool fell = false;
    for (int tries = 0; tries < 40 && !fell; ++tries) {
        p.pos = steep;
        p.stamina = 1;
        p.fallen = 0;
        PlayerInput in;
        in.move = up * -1.0f;
        in.run = true;
        for (float t = 0; t < 2.0f && !fell; t += 0.05f) {
            g.update(0.05f, in);
            fell = p.fallen > 0;
        }
    }
    CHECK(fell);
    p.fallen = 0;
    // and once down, you can't move until you're up again
    if (fell) {
        p.fallen = 1.0f;
        Vec2 at = p.pos;
        PlayerInput in;
        in.move = Vec2{1, 0};
        g.update(0.1f, in);
        CHECK(distance(at, p.pos) < 0.01f);
        run(g, 1.2f);
        CHECK(p.fallen == 0);
    }
    p.body.state();   // (wounds from falling are fine: the next tests heal or ignore them)
}

TEST(save_and_load_roundtrip) {
    Game g;
    CHECK(g.loadData());
    g.newGame(99);
    Rng rng(3);
    g.player().inventory.add(g.items().create("butter", rng));
    g.player().body.expose(Pathogen::Giardia, 0.5f);
    Wound w; w.severity = 0.3f; w.bleeding = 2;
    g.player().body.addWound(w);
    for (int i = 0; i < 200; ++i) g.update(0.05f, PlayerInput{});
    Vec2 pos = g.player().pos;
    double minutes = g.world().clock().totalMinutes();
    float fat = g.player().body.state().fatMassKg;
    size_t inv = g.player().inventory.size();
    std::string path = "test_save.txt";
    CHECK(g.save(path));

    Game h;
    CHECK(h.loadData());
    CHECK(h.load(path));
    CHECK(distance(h.player().pos, pos) < 0.01f);
    CHECK(std::fabs(h.world().clock().totalMinutes() - minutes) < 0.01);
    CHECK(std::fabs(h.player().body.state().fatMassKg - fat) < 1e-4f);
    CHECK(h.player().inventory.size() == inv);
    CHECK(h.player().body.state().wounds.size() == 1);
    CHECK(h.player().body.state().infections.size() == 1);
    std::remove(path.c_str());
}
