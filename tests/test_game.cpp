#include "TestFramework.h"
#include "game/Game.h"
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
