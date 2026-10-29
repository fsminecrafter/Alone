// Other survivors, bikes and cars.
#include "TestFramework.h"
#include "game/Game.h"
#include <cmath>
#include <cstdio>

using namespace alone;

namespace {
void run(Game& g, float seconds, PlayerInput in = {}) {
    for (float t = 0; t < seconds; t += 0.05f) g.update(0.05f, in);
}
Survivor* find(Game& g, const char* id) {
    for (auto& s : g.survivors())
        if (s.def->id == id) return &s;
    return nullptr;
}
void give(Game& g, const char* id, int n = 1) {
    Rng rng(9);
    for (int i = 0; i < n; ++i) g.player().inventory.add(g.items().create(id, rng));
}
// A parked car somewhere in town (the Vimpeln car park has plenty)
bool findCar(Game& g, Vec2 around, int& cx, int& cy, int skip = 0) {
    for (int r = 0; r < 300; r += 2)
        for (int dy = -r; dy <= r; dy += 1)
            for (int dx = -r; dx <= r; dx += (std::abs(dy) == r ? 1 : 2 * r)) {
                int x = (int)around.x + dx, y = (int)around.y + dy;
                if (g.world().tile(x, y).prop == Prop::Car && skip-- <= 0) { cx = x; cy = y; return true; }
            }
    return false;
}
} // namespace

TEST(survivors_live_in_alingsas) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    CHECK(g.survivors().size() >= 8);
    int known = 0;
    for (const auto& s : g.survivors()) {
        std::printf("    %-16s %-8s %s (%.0f m away)\n", s.def->name.c_str(), dispositionName(s.def->disposition), s.def->homeName.c_str(),
                    distance(s.pos, g.player().pos));
        known += s.known;
        CHECK(s.alive());
    }
    CHECK(known == 1);   // the light in the window
    Survivor* elin = find(g, "elin");
    CHECK(elin && elin->known && distance(elin->pos, g.player().pos) < 600);
    CHECK(!g.objective().empty());
    std::printf("    objective: %s\n", g.objective().c_str());
}

TEST(help_elin_and_she_joins) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    CHECK(elin != nullptr);
    if (!elin) return;
    g.player().pos = elin->pos + Vec2{1.0f, 0};
    std::string hi = g.greeting(elin->id);
    CHECK(elin->met && !hi.empty());
    std::string need = g.talk(elin->id, Topic::Request);
    CHECK(elin->requestKnown);
    // Without the can opener there's nothing to hand over
    bool canGive = false;
    for (auto& [t, label] : g.talkTopics(elin->id)) canGive |= t == Topic::Give;
    CHECK(!canGive);
    give(g, "can_opener");
    for (auto& [t, label] : g.talkTopics(elin->id)) canGive |= t == Topic::Give;
    CHECK(canGive);
    int bandages = 0;
    g.talk(elin->id, Topic::Give);
    for (const auto& it : g.player().inventory.items()) bandages += it.def->id == "bandage";
    CHECK(elin->requestDone && bandages == 2);
    CHECK(g.player().inventory.findId("can_opener") < 0);
    // She tells you about the others
    g.talk(elin->id, Topic::Others);
    CHECK(find(g, "gunnar")->known && find(g, "sofia")->known);
    // and comes along
    g.talk(elin->id, Topic::Join);
    CHECK(elin->joined);
    g.player().pos = g.player().pos + Vec2{0, 0};
    PlayerInput walk;
    walk.move = Vec2{0, 0};
    run(g, 2, walk);
    CHECK(distance(elin->pos, g.player().pos) < 6);
}

TEST(trading_needs_a_fair_offer) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* ahmed = find(g, "ahmed");
    CHECK(ahmed != nullptr);
    if (!ahmed) return;
    std::vector<uint32_t> theirs;
    for (const auto& it : ahmed->inventory.items())
        if (it.def->id == "jerky") { theirs.push_back(it.uid); break; }
    std::string why;
    CHECK(!g.tradeFair(ahmed->id, {}, theirs, &why));
    give(g, "canned_meatballs", 3);
    std::vector<uint32_t> mine;
    for (const auto& it : g.player().inventory.items())
        if (it.def->id == "canned_meatballs") mine.push_back(it.uid);
    CHECK(g.tradeFair(ahmed->id, mine, theirs, &why));
    CHECK(g.trade(ahmed->id, mine, theirs));
    CHECK(g.player().inventory.findId("jerky") >= 0);
    CHECK(ahmed->inventory.findId("canned_meatballs") >= 0);
    // His crowbar isn't for sale
    std::vector<uint32_t> weapon{ahmed->weaponUid};
    CHECK(!g.tradeFair(ahmed->id, mine, weapon, &why));
}

TEST(raiders_hunt_you) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* robin = find(g, "robin");
    CHECK(robin != nullptr);
    if (!robin) return;
    // Out in the open, in the daytime
    g.world().clock().setTotalMinutes(g.world().clock().totalMinutes() + 3 * 60);
    robin->pos = g.player().pos + Vec2{6, 0};
    // stand outside the house so they can see each other
    g.player().pos = g.world().gen().building(g.world().tile((int)g.player().pos.x, (int)g.player().pos.y).building)->doorPos + Vec2{0, 2.5f};
    robin->pos = g.player().pos + Vec2{5, 0};
    run(g, 12);
    std::printf("    wounds after 12 s with Robin: %d, threat %.2f\n", (int)g.player().body.state().wounds.size(), g.threat());
    CHECK(robin->state == SurvivorState::Hunt);
    CHECK(g.player().body.state().wounds.size() >= 1);
}

TEST(hitting_a_friend_turns_them) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    if (!elin) return;
    g.player().pos = elin->pos + Vec2{0.8f, 0};
    PlayerInput atk;
    atk.attack = true;
    atk.aim = elin->pos;
    for (int i = 0; i < 40 && !elin->angry; ++i) g.update(0.05f, atk), g.update(0.5f, PlayerInput{});
    CHECK(elin->angry);
    CHECK(g.talkTopics(elin->id).empty());
}

TEST(car_doors_and_keys) {
    Game g;
    CHECK(g.loadData());
    g.newGame(7);
    // Try the doors of 60 cars: about a fifth open
    Vec2 vimpeln{0, 0};
    for (const auto& b : g.world().gen().allBuildings())
        if (b.name == "Vimpeln") vimpeln = b.rect.centre();
    int opened = 0, keys = 0, tried = 0;
    for (int i = 0; i < 60; ++i) {
        int cx, cy;
        if (!findCar(g, vimpeln, cx, cy)) break;
        g.player().vehicle = -1;
        g.player().pos = Vec2{cx + 0.5f, cy + 2.2f};
        ActionOption try_;
        for (auto& o : g.actionsAt(Vec2{cx + 0.5f, cy + 0.5f}))
            if (o.kind == ActionKind::TryCarDoor && o.enabled) try_ = o;
        if (try_.kind != ActionKind::TryCarDoor) {
            // locked already: look further away
            g.world().setProp(cx, cy, Prop::None);
            continue;
        }
        ++tried;
        g.perform(try_);
        if (g.player().vehicle >= 0) {
            ++opened;
            keys += g.playerVehicle()->keyInIgnition;
        } else {
            g.world().setProp(cx, cy, Prop::None);   // move on to the next car
        }
    }
    std::printf("    %d of %d car doors opened, %d had the key\n", opened, tried, keys);
    CHECK(tried >= 40);
    CHECK(opened >= tried / 10 && opened <= tried * 4 / 10);
}

TEST(drive_a_car_and_ride_a_bike) {
    Game g;
    CHECK(g.loadData());
    g.newGame(7);
    Vec2 vimpeln{0, 0};
    for (const auto& b : g.world().gen().allBuildings())
        if (b.name == "Vimpeln") vimpeln = b.rect.centre();
    int cx, cy;
    CHECK(findCar(g, vimpeln, cx, cy));
    // Pretend it opened, with the key in and a little petrol
    g.player().pos = Vec2{cx + 0.5f, cy + 2.2f};
    ActionOption o;
    o.kind = ActionKind::TryCarDoor;
    o.x = cx; o.y = cy;
    for (int seed = 0; seed < 1 && g.player().vehicle < 0; ++seed) g.perform(o);
    if (g.player().vehicle < 0) {
        // locked: smash the window instead
        give(g, "crowbar");
        o.kind = ActionKind::SmashCarWindow;
        g.perform(o);
    }
    Vehicle* v = g.playerVehicle();
    CHECK(v != nullptr);
    if (!v) return;
    v->keyInIgnition = true;
    v->fuelL = 10;
    ActionOption start;
    start.kind = ActionKind::StartEngine;
    g.perform(start);
    CHECK(v->running);
    Vec2 before = v->pos;
    PlayerInput drive;
    drive.move = v->forward();
    run(g, 4, drive);
    float moved = distance(before, v->pos);
    std::printf("    drove %.1f m in 4 s (speed %.1f m/s), fuel %.2f l\n", moved, v->speed, v->fuelL);
    CHECK(distance(g.player().pos, v->pos) < 0.01f);
    // it either drove off or ran into something on the way
    CHECK(moved > 3.0f || v->damage > 0);
    // Stop and get out
    run(g, 4);
    ActionOption out;
    out.kind = ActionKind::ExitVehicle;
    g.perform(out);
    CHECK(g.player().vehicle < 0);
    CHECK(g.environment().wind >= 0);

    // A bike by a front door somewhere
    int bx = 0, by = 0;
    bool bike = false;
    Vec2 c = g.player().pos;
    for (int r = 0; r < 600 && !bike; r += 3)
        for (int dy = -r; dy <= r && !bike; dy += 3)
            for (int dx = -r; dx <= r && !bike; dx += (std::abs(dy) == r ? 3 : 2 * r))
                if (g.world().tile((int)c.x + dx, (int)c.y + dy).prop == Prop::Bike) { bx = (int)c.x + dx; by = (int)c.y + dy; bike = true; }
    CHECK(bike);
    if (!bike) return;
    g.player().pos = Vec2{bx + 0.5f, by + 1.3f};
    ActionOption mount;
    mount.kind = ActionKind::MountBike;
    mount.x = bx; mount.y = by;
    g.perform(mount);
    CHECK(g.playerVehicle() && g.playerVehicle()->kind == VehicleKind::Bike);
    CHECK(g.world().tile(bx, by).prop == Prop::None);
}

TEST(people_and_vehicles_are_saved) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    if (!elin) return;
    g.greeting(elin->id);
    elin->trust = 33;
    g.player().knowsHotwire = true;
    Vehicle v;
    v.id = 5; v.kind = VehicleKind::Car; v.pos = Vec2{100, 200}; v.fuelL = 12; v.hotwired = true;
    g.vehicles().push_back(v);
    std::string path = "test_people_save.txt";
    CHECK(g.save(path));
    Game h;
    CHECK(h.loadData());
    CHECK(h.load(path));
    Survivor* e2 = find(h, "elin");
    CHECK(e2 && e2->met && std::fabs(e2->trust - 33) < 0.01f);
    CHECK(h.player().knowsHotwire);
    CHECK(h.vehicles().size() == 1 && h.vehicles()[0].hotwired && std::fabs(h.vehicles()[0].fuelL - 12) < 0.01f);
    std::remove(path.c_str());
}

TEST(petrol_gas_cans_and_hoses) {
    Game g;
    CHECK(g.loadData());
    g.newGame(11);
    Vec2 vimpeln{0, 0};
    for (const auto& b : g.world().gen().allBuildings())
        if (b.name == "Vimpeln") vimpeln = b.rect.centre();
    // Every parked car has 0-40% of a tank
    int cars = 0;
    float most = 0, total = 0;
    for (int i = 0; i < 40; ++i) {
        int cx, cy;
        if (!findCar(g, vimpeln, cx, cy, i)) break;
        float f = g.carFuelAt(cx, cy);
        CHECK(f >= 0 && f <= 0.4f * 70 + 0.01f);
        most = std::max(most, f);
        total += f;
        ++cars;
    }
    std::printf("    %d cars: up to %.1f l, %.1f l on average\n", cars, most, cars ? total / cars : 0);
    CHECK(cars >= 20 && most > 5);
    // Siphon one into an empty gas can, then pour it into another car
    Rng rng(1);
    ItemInstance can = g.items().create("gas_can", rng);
    can.liquidMl = 0;
    g.player().inventory.add(can);
    give(g, "hose");
    int cx = 0, cy = 0;
    for (int i = 0; i < 40; ++i)
        if (findCar(g, vimpeln, cx, cy, i) && g.carFuelAt(cx, cy) > 3) break;
    float before = g.carFuelAt(cx, cy);
    g.player().pos = Vec2{cx + 0.5f, cy + 2.0f};
    ActionOption siphon;
    for (auto& o : g.actionsAt(Vec2{cx + 0.5f, cy + 0.5f}))
        if (o.kind == ActionKind::Siphon) siphon = o;
    CHECK(siphon.kind == ActionKind::Siphon && siphon.enabled);
    g.perform(siphon);
    int ci = g.player().inventory.findId("gas_can");
    CHECK(ci >= 0);
    if (ci < 0) return;
    const ItemInstance& c = g.player().inventory.items()[ci];
    std::printf("    siphoned %.1f l of %.1f\n", c.liquidMl / 1000.0f, before);
    CHECK(c.liquid == 1 && c.liquidMl > 2000);
    CHECK(g.carFuelAt(cx, cy) < before - 1.9f);
    // You can't drink petrol, or top it up with lake water
    ItemRef ref{ItemLoc::Player, 0, 0, 0, c.uid};
    for (auto& o : g.actionsForItem(ref))
        if (o.kind == ActionKind::Drink) CHECK(!o.enabled);
    // Pour it into a car you got going
    Vehicle v;
    v.id = 77; v.kind = VehicleKind::Car; v.pos = g.player().pos + Vec2{3, 0}; v.fuelL = 0; v.tankL = 50;
    g.vehicles().push_back(v);
    ActionOption pour;
    pour.kind = ActionKind::Refuel;
    pour.targetId = 77;
    g.perform(pour);
    CHECK(g.vehicle(77)->fuelL > 1.9f);
    CHECK(g.player().inventory.items()[g.player().inventory.findId("gas_can")].liquidMl < 1);
}

TEST(people_pass_on_rumours_and_warnings) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    if (!elin) return;
    g.greeting(elin->id);
    // Her own leads first, then whoever else she's heard of, raiders included
    int known0 = 0, known1 = 0;
    for (auto& s : g.survivors()) known0 += s.known;
    std::string a = g.talk(elin->id, Topic::Others);
    std::string b = g.talk(elin->id, Topic::Others);
    std::string c = g.talk(elin->id, Topic::Others);
    for (auto& s : g.survivors()) known1 += s.known;
    std::printf("    \"%s\"\n    \"%s\"\n    \"%s\"\n", a.c_str(), b.c_str(), c.c_str());
    CHECK(known1 >= known0 + 3);
    for (auto& s : g.survivors())
        if (s.known && !s.met) CHECK(s.rumoured);
    // A raider close by gets a warning when you say hello
    Survivor* robin = find(g, "robin");
    robin->known = false;
    robin->pos = elin->pos + Vec2{120, 0};
    std::string hi = g.greeting(elin->id);
    std::printf("    \"%s\"\n", hi.c_str());
    CHECK(hi.find("Robin") != std::string::npos && robin->known);
}

TEST(a_deal_with_jonas) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* jonas = find(g, "jonas");
    CHECK(jonas != nullptr);
    if (!jonas) return;
    // From across the yard
    g.player().pos = jonas->pos + Vec2{18, 0};
    CHECK(g.canTalk(jonas->id) && g.talkRange(jonas->id) > 18);
    g.greeting(jonas->id);
    bool letIn = false, canGive = false;
    for (auto& [t, l] : g.talkTopics(jonas->id)) letIn |= t == Topic::LetIn;
    CHECK(letIn);
    std::string no = g.talk(jonas->id, Topic::LetIn);
    CHECK(!jonas->admitted);
    g.talk(jonas->id, Topic::Parley);
    g.talk(jonas->id, Topic::Request);
    give(g, "painkillers", 2);
    for (auto& [t, l] : g.talkTopics(jonas->id)) canGive |= t == Topic::Give;
    CHECK(canGive);
    std::string thanks = g.talk(jonas->id, Topic::Give);
    std::printf("    \"%s\"\n    \"%s\"\n", no.c_str(), thanks.c_str());
    CHECK(jonas->admitted);
    // Now you can walk right up without being shot
    g.player().pos = jonas->pos + Vec2{1.5f, 0};
    run(g, 6);
    CHECK(jonas->state != SurvivorState::Hunt);
    CHECK(g.player().body.state().wounds.empty());
}

TEST(pay_off_or_face_down_raiders) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* kalle = find(g, "kalle");
    Survivor* robin = find(g, "robin");
    if (!kalle || !robin) return;
    kalle->pos = g.player().pos + Vec2{6, 0};
    robin->pos = g.player().pos + Vec2{7, 1};
    kalle->state = SurvivorState::Hunt;
    CHECK(g.canTalk(kalle->id));
    g.greeting(kalle->id);
    // Nothing worth taking: they attack
    for (auto& it : g.player().inventory.items()) (void)it;
    g.player().inventory.items().clear();
    g.talk(kalle->id, Topic::PayOff);
    CHECK(kalle->state == SurvivorState::Hunt && !kalle->truce);
    // With something worth their while, they let you go for a day - both of them
    give(g, "hunting_rifle");
    give(g, "canned_meatballs", 3);
    std::string r = g.talk(kalle->id, Topic::PayOff);
    std::printf("    \"%s\"\n", r.c_str());
    CHECK(kalle->truce && robin->truce);
    CHECK(g.player().inventory.findId("hunting_rifle") < 0);
    CHECK(!kalle->hostileTo());
    run(g, 5);
    CHECK(kalle->state != SurvivorState::Hunt);
}
