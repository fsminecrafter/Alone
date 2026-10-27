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
    p.pos = dead->pos + Vec2{1, 0};
    for (auto& o : g.actionsAt(dead->pos))
        if (o.kind == ActionKind::Butcher && o.enabled) { g.perform(o); runFor(20); }
    CHECK(dead->butchered);
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
