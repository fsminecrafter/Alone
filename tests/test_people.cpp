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
    drive.throttle = 1;
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

TEST(driving_steers_like_a_car) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    // A car on an open road: put one down in the middle of a big car park
    Vec2 at{0, 0};
    for (const auto& b : g.world().gen().allBuildings())
        if (b.name == "Vimpeln") at = b.doorPos;
    Vehicle v;
    v.id = 77; v.kind = VehicleKind::Car; v.fuelL = 20; v.keyInIgnition = true; v.running = true;
    // somewhere with room to drive about
    bool found = false;
    for (int r = 0; r < 120 && !found; r += 2)
        for (int k = 0; k < 16 && !found; ++k) {
            Vec2 p = at + Vec2{std::cos(k * 0.4f) * r, std::sin(k * 0.4f) * r};
            bool clear = true;
            for (int dy = -8; dy <= 8 && clear; ++dy)
                for (int dx = -8; dx <= 8 && clear; ++dx) clear = !g.world().tileBlocked((int)p.x + dx, (int)p.y + dy);
            if (clear) { v.pos = p; found = true; }
        }
    CHECK(found);
    if (!found) return;
    g.vehicles().push_back(v);
    g.player().vehicle = 77;
    Vehicle* car = g.playerVehicle();
    // Steering on the spot does nothing: the wheels only turn you when rolling
    PlayerInput turn;
    turn.steer = 1;
    run(g, 1, turn);
    CHECK(std::fabs(car->heading) < 0.01f);
    // W: forward along the heading, whichever way the screen points
    PlayerInput gas;
    gas.throttle = 1;
    run(g, 1.5f, gas);
    CHECK(car->speed > 2);
    float h0 = car->heading;
    // D while rolling: turns right (heading up)
    gas.steer = 1;
    run(g, 0.6f, gas);
    CHECK(car->heading > h0 + 0.1f);
    // S: brakes to a stop, then backs up
    PlayerInput brake;
    brake.throttle = -1;
    run(g, 1.2f, brake);
    CHECK(car->speed <= 0.3f);
    run(g, 1.5f, brake);
    CHECK(car->speed < -0.5f);
    std::printf("    heading %.2f, reversing at %.1f m/s\n", car->heading, car->speed);
}

TEST(auto_drink_only_clean_open_water) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Player& p = g.player();
    p.inventory.items().clear();
    p.body.mutableState().waterDeficitL = 2.5f;   // properly thirsty
    Rng rng(3);
    ItemInstance lake = g.items().create("empty_bottle", rng);
    lake.liquidMl = 1000;
    lake.liquidSafe = false;
    p.inventory.add(lake);
    ItemInstance sealed = g.items().create("sparkling_water", rng);
    if (sealed.def) p.inventory.add(sealed);
    run(g, 3);
    CHECK(std::fabs(p.inventory.items()[0].liquidMl - 1000) < 0.1f);   // not the lake water
    if (sealed.def) CHECK(p.inventory.findId("sparkling_water") >= 0 && p.inventory.items()[1].portion > 0.99f);
    ItemInstance clean = g.items().create("water_bottle", rng);
    clean.liquidMl = 800;
    p.inventory.add(clean);
    float before = p.body.state().waterDeficitL;
    run(g, 3);
    int i = p.inventory.findId("water_bottle");
    CHECK(i >= 0 && p.inventory.items()[i].liquidMl < 800);
    CHECK(p.body.state().waterDeficitL < before);
    // and it can be switched off
    g.setAutoDrink(false);
    float left = p.inventory.items()[i].liquidMl;
    run(g, 40);
    CHECK(std::fabs(p.inventory.items()[i].liquidMl - left) < 0.1f);
}

TEST(people_sleep_at_night_and_potter_by_day) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    CHECK(elin && elin->homeBuilding >= 0 && elin->hasBed);
    if (!elin) return;
    // Two in the morning, you a few rooms away
    double day = std::floor(g.world().clock().totalMinutes() / 1440.0) * 1440.0;
    g.world().clock().setTotalMinutes(day + 1440 + 2 * 60);
    g.player().pos = elin->home + Vec2{0, 40};
    run(g, 30);
    CHECK(elin->asleep());
    CHECK(distance(elin->pos, elin->bed) < 0.1f);
    // Waking her up to talk
    g.player().pos = elin->bedSide;
    std::string woke = g.greeting(elin->id);
    std::printf("    woken: \"%s\"\n", woke.c_str());
    CHECK(!elin->asleep() && !woke.empty());
    bool back = false;
    for (auto& [t, label] : g.talkTopics(elin->id)) back |= t == Topic::BackToSleep;
    CHECK(back);
    g.talk(elin->id, Topic::BackToSleep);
    g.player().pos = elin->home + Vec2{0, 40};
    run(g, 40);
    CHECK(elin->asleep());
    // Morning: up and about the house
    g.world().clock().setTotalMinutes(day + 1440 + 10 * 60);
    run(g, 60);
    CHECK(!elin->asleep());
    const Building* b = g.world().building(elin->homeBuilding);
    CHECK(b && b->rect.expanded(3).contains((int)elin->pos.x, (int)elin->pos.y));
    // Far away, people still keep their hours
    g.player().pos = elin->home + Vec2{400, 0};
    g.world().clock().setTotalMinutes(day + 2 * 1440 + 3 * 60);
    run(g, 1);
    CHECK(elin->asleep());
}

TEST(friendship_steps_from_stranger_to_lover) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    CHECK(elin != nullptr);
    if (!elin) return;
    CHECK(elin->bond() == Bond::Stranger);
    // stay awake for this
    double day = std::floor(g.world().clock().totalMinutes() / 1440.0) * 1440.0;
    g.world().clock().setTotalMinutes(day + 1440 + 13 * 60);
    g.player().pos = elin->pos + Vec2{1, 0};
    g.greeting(elin->id);
    CHECK(elin->met);
    elin->trust = 12;
    CHECK(elin->bond() == Bond::Acquaintance);
    elin->trust = 40;
    CHECK(elin->bond() == Bond::Friend);
    // A daily chat helps, but only once a day
    float t0 = elin->trust;
    g.talk(elin->id, Topic::Chat);
    float t1 = elin->trust;
    g.talk(elin->id, Topic::Chat);
    CHECK(t1 > t0 + 2 && elin->trust - t1 < 1);
    // Her story comes out piece by piece
    std::string story = g.talk(elin->id, Topic::About);
    CHECK(elin->storiesTold == 1 && !story.empty());
    elin->trust = 70;
    CHECK(elin->bond() == Bond::BestFriend);
    // Flirting, until she feels the same
    for (int d = 0; d < 30 && elin->romance < 50; ++d) {
        g.world().clock().setTotalMinutes(day + (2 + d) * 1440 + 13 * 60);
        g.talk(elin->id, Topic::Flirt);
        elin->trust = std::max(elin->trust, 70.0f);
    }
    CHECK(elin->romance >= 50);
    // Romance and family pull against each other: with this much romance she won't be "family"
    elin->kinship = 60;
    std::string notFamily = g.talk(elin->id, Topic::Family);
    CHECK(!elin->family);
    // and if she sees you as family, she won't be your lover
    std::string notLover = g.talk(elin->id, Topic::Confess);
    CHECK(!elin->lover);
    std::printf("    \"%s\" / \"%s\"\n", notFamily.c_str(), notLover.c_str());
    elin->kinship = 0;
    elin->romance = 60;
    bool confess = false;
    for (auto& [t, label] : g.talkTopics(elin->id)) confess |= t == Topic::Confess;
    CHECK(confess);
    std::string yes = g.talk(elin->id, Topic::Confess);
    std::printf("    \"%s\"\n", yes.c_str());
    CHECK(elin->bond() == Bond::Lover);
    // Lovers come along even if they'd usually stay home
    Survivor* gunnar = find(g, "gunnar");
    CHECK(gunnar && !gunnar->def->romanceable);
    gunnar->met = true;
    gunnar->trust = 90;
    g.player().pos = gunnar->pos + Vec2{1, 0};
    // Trust alone isn't enough to be family: you have to look out for each other
    auto offers = [&](Survivor* sv, Topic t) {
        for (auto& [tp, label] : g.talkTopics(sv->id)) if (tp == t) return true;
        return false;
    };
    CHECK(!offers(gunnar, Topic::Family) && offers(gunnar, Topic::Kin));
    CHECK(!offers(gunnar, Topic::Flirt));   // not romanceable
    for (int d = 0; d < 40 && gunnar->kinship < kBondNeeded; ++d) {
        g.world().clock().setTotalMinutes(day + (40 + d) * 1440 + 13 * 60);
        if (offers(gunnar, Topic::Kin)) g.talk(gunnar->id, Topic::Kin);
    }
    CHECK(gunnar->kinship >= kBondNeeded && offers(gunnar, Topic::Family));
    g.talk(gunnar->id, Topic::Family);
    CHECK(gunnar->bond() == Bond::Family);
    CHECK(!offers(gunnar, Topic::Kin));
    g.talk(gunnar->id, Topic::Join);
    CHECK(gunnar->joined);
    // Hurting someone ends it
    g.player().pos = elin->pos + Vec2{1, 0};
    g.talk(elin->id, Topic::Breakup);
    CHECK(elin->bond() != Bond::Lover);
    elin->trust = -50;
    CHECK(elin->bond() == Bond::Enemy);
    bool sorry = false;
    for (auto& [t, label] : g.talkTopics(elin->id)) sorry |= t == Topic::Sorry;
    CHECK(sorry);
    // {name} is filled in
    CHECK(g.personal("Hi {name}!") == "Hi " + g.player().look.name + "!");
}

TEST(character_and_bonds_survive_saving) {
    Game g;
    CHECK(g.loadData());
    CharacterChoice c = CharacterChoice::defaults();
    c.look.name = "Åsa Ström";
    c.look.gender = Gender::Woman;
    c.look.hairStyle = 2;
    c.look.skin = 0x8d5a3b;
    c.look.age = 52;
    c.wear[(size_t)ClothingSlot::Head] = "wool_hat";
    c.tint[(size_t)ClothingSlot::Head] = 0x3e5a36;
    g.newGame(42, &c);
    CHECK(g.player().look.name == "Åsa Ström");
    const ItemInstance& hat = g.player().worn[(size_t)ClothingSlot::Head];
    CHECK(hat.def && hat.def->id == "wool_hat" && hat.tint == 0x3e5a36);
    CHECK(g.player().worn[(size_t)ClothingSlot::Sweater].def && g.player().worn[(size_t)ClothingSlot::Sweater].def->id == "fleece");
    Survivor* elin = find(g, "elin");
    if (!elin) return;
    elin->met = true;
    elin->lover = true;
    elin->romance = 72;
    elin->kinship = 12;
    elin->storiesTold = 2;
    std::string path = "test_people_save2.txt";
    CHECK(g.save(path));
    Game h;
    CHECK(h.loadData());
    CHECK(h.load(path));
    CHECK(h.player().look.name == "Åsa Ström" && h.player().look.gender == Gender::Woman && h.player().look.hairStyle == 2);
    CHECK(h.player().look.skin == 0x8d5a3b);
    CHECK(h.player().look.age == 52);
    const ItemInstance& hat2 = h.player().worn[(size_t)ClothingSlot::Head];
    CHECK(hat2.def && hat2.tint == 0x3e5a36);
    Survivor* e2 = find(h, "elin");
    CHECK(e2 && e2->lover && e2->bond() == Bond::Lover && std::fabs(e2->romance - 72) < 0.01f && e2->storiesTold == 2);
    CHECK(e2 && std::fabs(e2->kinship - 12) < 0.01f);
    std::remove(path.c_str());
}

TEST(people_walk_round_what_is_in_their_way) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    CHECK(elin != nullptr);
    if (!elin) return;
    World& w = g.world();
    // An open patch near the start, with nothing in it and no walls
    auto open = [&](int x0, int y0) {
        for (int y = y0; y < y0 + 26; ++y)
            for (int x = x0; x < x0 + 26; ++x)
                if (w.tileBlocked(x, y) || w.tile(x, y).walls || w.tile(x, y).building >= 0) return false;
        return true;
    };
    Vec2 start = g.player().pos;
    int ox = 0, oy = 0;
    bool found = false;
    for (int r = 0; r < 400 && !found; r += 6)
        for (int k = 0; k < 16 && !found; ++k) {
            int x = (int)start.x + (int)(r * std::cos(k * 0.39f)), y = (int)start.y + (int)(r * std::sin(k * 0.39f));
            if (open(x, y)) { ox = x; oy = y; found = true; }
        }
    CHECK(found);
    if (!found) return;
    // A fence like a cup round her, open at the back, between her and you
    int cx = ox + 13, cy = oy + 13;
    for (int y = cy - 5; y <= cy + 5; ++y) w.setProp(cx, y, Prop::Fence);
    for (int x = cx - 4; x < cx; ++x) { w.setProp(x, cy - 5, Prop::Fence); w.setProp(x, cy + 5, Prop::Fence); }
    elin->joined = true;
    elin->state = SurvivorState::Follow;
    elin->trust = 80;
    elin->pos = Vec2{cx - 1.5f, cy + 0.5f};
    g.player().pos = Vec2{cx + 6.5f, cy + 0.5f};
    PlayerInput stand;
    float closest = 1e9f;
    for (float t = 0; t < 20; t += 0.05f) {
        g.update(0.05f, stand);
        closest = std::min(closest, distance(elin->pos, g.player().pos));
    }
    std::printf("    got within %.1f m\n", closest);
    CHECK(closest < 3.5f);
}

TEST(romance_hint_says_why_flirting_is_missing) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    if (!elin) return;
    g.player().pos = elin->pos + Vec2{1.0f, 0};
    g.greeting(elin->id);
    auto hasFlirt = [&] {
        for (auto& [t, label] : g.talkTopics(elin->id)) if (t == Topic::Flirt) return true;
        return false;
    };
    elin->trust = 5;
    CHECK(!hasFlirt());
    CHECK(g.romanceHint(elin->id).find("trust") != std::string::npos);
    elin->trust = 40;
    CHECK(hasFlirt());
    CHECK(g.romanceHint(elin->id).empty());
    g.talk(elin->id, Topic::Flirt);
    CHECK(!hasFlirt());
    CHECK(g.romanceHint(elin->id).find("tomorrow") != std::string::npos || g.romanceHint(elin->id).find("best friends") != std::string::npos);
    std::printf("    %s\n", g.romanceHint(elin->id).c_str());
}

TEST(sleep_in_a_car_and_comfort) {
    Game g;
    CHECK(g.loadData());
    g.newGame(7);
    Vec2 vimpeln{0, 0};
    for (const auto& b : g.world().gen().allBuildings())
        if (b.name == "Vimpeln") vimpeln = b.rect.centre();
    // Get into a car whose door opens
    bool in = false;
    for (int i = 0; i < 80 && !in; ++i) {
        int cx, cy;
        if (!findCar(g, vimpeln, cx, cy)) break;
        g.player().pos = Vec2{cx + 0.5f, cy + 2.2f};
        for (auto& o : g.actionsAt(Vec2{cx + 0.5f, cy + 0.5f}))
            if ((o.kind == ActionKind::TryCarDoor || o.kind == ActionKind::EnterVehicle) && o.enabled) { g.perform(o); break; }
        in = g.player().vehicle >= 0;
        if (!in) g.world().setProp(cx, cy, Prop::None);
    }
    CHECK(in);
    if (!in) return;
    ActionOption sleep;
    for (auto& o : g.actionsAt(g.player().pos))
        if (o.kind == ActionKind::Sleep) sleep = o;
    CHECK(sleep.kind == ActionKind::Sleep && sleep.enabled);
    float dryCar = g.sleepComfort().score;
    give(g, "pillow");
    float pillowCar = g.sleepComfort().score;
    CHECK(pillowCar > dryCar);
    g.player().body.mutableState().sleepPressure = 1.0f;   // worn out
    g.perform(sleep);
    CHECK(g.player().asleep && g.player().vehicle >= 0);
    float early = g.sleepComfort().score;
    // hours in the seat get uncomfortable
    double t0 = g.world().clock().totalMinutes();
    float late = early;
    for (int i = 0; i < 200000 && g.player().asleep && g.world().clock().totalMinutes() - t0 < 6 * 60; ++i) {
        g.update(0.05f, PlayerInput{});
        if (g.player().asleep) late = g.sleepComfort().score;
    }
    std::printf("    car: %.2f, with pillow %.2f, asleep %.2f, after %.1f h %.2f\n", dryCar, pillowCar, early,
                (g.world().clock().totalMinutes() - t0) / 60.0, late);
    CHECK(g.world().clock().totalMinutes() - t0 > 3 * 60);
    CHECK(late < early - 0.1f);
    g.cancelAction();
    // a bed beats a car; wet clothes make anything worse
    g.player().vehicle = -1;
    int bx = 0, by = 0;
    bool bed = false;
    for (int r = 0; r < 200 && !bed; ++r)
        for (int dy = -r; dy <= r && !bed; ++dy)
            for (int dx = -r; dx <= r && !bed; dx += (std::abs(dy) == r ? 1 : 2 * r))
                if (g.world().tile((int)vimpeln.x + dx, (int)vimpeln.y + dy).prop == Prop::Bed) { bx = (int)vimpeln.x + dx; by = (int)vimpeln.y + dy; bed = true; }
    if (bed) {
        g.player().pos = Vec2{bx + 0.5f, by + 0.5f};
        float inBed = g.sleepComfort().score;
        g.player().body.mutableState().wetness = 1.0f;
        float wetBed = g.sleepComfort().score;
        std::printf("    bed with pillow %.2f, soaked %.2f\n", inBed, wetBed);
        CHECK(inBed > pillowCar);
        CHECK(wetBed < inBed - 0.2f);
    }
}

namespace {
// An empty patch of open ground near the start: no props, walls or buildings
bool openPatch(Game& g, int size, int& ox, int& oy) {
    World& w = g.world();
    auto open = [&](int x0, int y0) {
        for (int y = y0; y < y0 + size; ++y)
            for (int x = x0; x < x0 + size; ++x)
                if (w.tileBlocked(x, y) || w.tile(x, y).walls || w.tile(x, y).building >= 0) return false;
        return true;
    };
    Vec2 start = g.player().pos;
    for (int r = 0; r < 500; r += 6)
        for (int k = 0; k < 16; ++k) {
            int x = (int)start.x + (int)(r * std::cos(k * 0.39f)), y = (int)start.y + (int)(r * std::sin(k * 0.39f));
            if (open(x, y)) { ox = x; oy = y; return true; }
        }
    return false;
}
// A closed 3x3 room of walls with its corner at (x, y)
void room(World& w, int x, int y) {
    for (int i = 0; i < 3; ++i) {
        w.addBuild({BuildKind::Wall, x + i, y, 0});
        w.addBuild({BuildKind::Wall, x + i, y + 3, 0});
        w.addBuild({BuildKind::Wall, x, y + i, 1});
        w.addBuild({BuildKind::Wall, x + 3, y + i, 1});
    }
}
void finishTimed(Game& g) {
    for (int i = 0; i < 400 && g.currentAction().active; ++i) g.update(0.05f, PlayerInput{});
}
} // namespace

TEST(walls_block_sight_and_windows_break) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    int ox, oy;
    CHECK(openPatch(g, 30, ox, oy));
    World& w = g.world();
    int cx = ox + 13, cy = oy + 13;
    room(w, cx, cy);
    Vec2 in{cx + 1.5f, cy + 1.5f};
    // nothing gets out of a closed room, not even exactly through a corner
    int seen = 0;
    for (int k = 0; k < 64; ++k) {
        float a = k * 6.2832f / 64;
        seen += w.lineOfSight(in, in + Vec2{std::cos(a), std::sin(a)} * 9.0f);
    }
    seen += w.lineOfSight(in, in + Vec2{5, 5}) + w.lineOfSight(in, in + Vec2{-5, 5}) + w.lineOfSight(in, in + Vec2{5, -5});
    CHECK(seen == 0);
    // a window lets you see out, and can be smashed and climbed through
    w.removeBuild({BuildKind::Wall, cx + 1, cy, 0});
    w.addBuild({BuildKind::Window, cx + 1, cy, 0});
    CHECK(w.lineOfSight(in, Vec2{cx + 1.5f, cy - 6.0f}));
    g.player().pos = Vec2{cx + 1.5f, cy - 0.6f};
    ActionOption brk, climb;
    for (auto& o : g.actionsAt(Vec2{cx + 1.5f, cy - 0.1f})) {
        if (o.kind == ActionKind::BreakWindow) brk = o;
        if (o.kind == ActionKind::ClimbWindow) climb = o;
    }
    CHECK(brk.kind == ActionKind::BreakWindow && brk.enabled && climb.kind == ActionKind::None);
    g.perform(brk);
    finishTimed(g);
    CHECK(w.windowBroken(cx + 1, cy, 0));
    for (auto& o : g.actionsAt(Vec2{cx + 1.5f, cy - 0.1f}))
        if (o.kind == ActionKind::ClimbWindow) climb = o;
    CHECK(climb.kind == ActionKind::ClimbWindow && climb.enabled);
    g.perform(climb);
    finishTimed(g);
    Vec2 p = g.player().pos;
    std::printf("    climbed to %.2f, %.2f (room %d..%d)\n", p.x, p.y, cy, cy + 3);
    CHECK(p.y > cy && p.y < cy + 3 && p.x > cx && p.x < cx + 3);
    // and it stays broken through a save
    std::string path = "test_windows_save.txt";
    CHECK(g.save(path));
    Game h;
    CHECK(h.loadData());
    CHECK(h.load(path));
    CHECK(h.world().windowBroken(cx + 1, cy, 0));
    std::remove(path.c_str());
}

TEST(jonas_freezes_you_walks_up_and_talks) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* jonas = find(g, "jonas");
    if (!jonas) return;
    int ox, oy;
    CHECK(openPatch(g, 30, ox, oy));
    // daytime, out in the open
    double add = 12.0 - g.world().clock().hourOfDay();
    if (add < 0) add += 24;
    g.world().clock().advance(add * 60);
    jonas->pos = jonas->home = Vec2{ox + 5.5f, oy + 15.5f};
    jonas->homeBuilding = -1;
    jonas->state = SurvivorState::Home;
    g.player().pos = Vec2{ox + 18.5f, oy + 15.5f};
    PlayerInput stand;
    bool froze = false;
    int asked = 0;
    for (float t = 0; t < 25 && !asked; t += 0.05f) {
        g.update(0.05f, stand);
        froze |= jonas->state == SurvivorState::Alert;
        asked = g.takeTalkRequest();
    }
    std::printf("    froze %d, asked to talk %d, %.1f m away, says \"%s\"\n", froze, asked == jonas->id, distance(jonas->pos, g.player().pos),
                jonas->speech.c_str());
    CHECK(froze);
    CHECK(asked == jonas->id);
    CHECK(distance(jonas->pos, g.player().pos) < 4.0f);
    CHECK(g.player().body.state().wounds.empty());
    bool parley = false;
    for (auto& [t, l] : g.talkTopics(jonas->id)) parley |= t == Topic::Parley;
    CHECK(parley);
    // walk away: he goes back and leaves you be for a while
    g.talk(jonas->id, Topic::Bye);
    CHECK(jonas->state != SurvivorState::Alert && jonas->freezeCooldown > 0);
    // a wall between you: he can't see you
    jonas->freezeCooldown = 0;
    jonas->state = SurvivorState::Home;
    jonas->pos = jonas->home;
    room(g.world(), ox + 4, oy + 14);
    run(g, 4, stand);
    CHECK(jonas->state != SurvivorState::Alert);
    // hurt him once, and he shoots on sight
    jonas->angry = true;
    CHECK(jonas->hostileTo());
}

TEST(share_a_tent_with_your_lover) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    Survivor* elin = find(g, "elin");
    if (!elin) return;
    int ox, oy;
    CHECK(openPatch(g, 20, ox, oy));
    Tent t;
    t.id = g.world().newId();
    t.rect = IRect{ox + 8, oy + 8, 2, 2};
    t.item = "tent_dome";
    g.world().tents().push_back(t);
    elin->met = elin->joined = elin->lover = true;
    elin->trust = 80;
    elin->romance = 70;
    elin->state = SurvivorState::Follow;
    g.player().pos = Vec2{ox + 9.0f, oy + 11.5f};
    elin->pos = Vec2{ox + 10.5f, oy + 12.0f};
    ActionOption crawl;
    for (auto& o : g.actionsAt(Vec2{ox + 9.0f, oy + 9.0f}))
        if (o.kind == ActionKind::Sleep && o.enabled) crawl = o;
    CHECK(crawl.kind == ActionKind::Sleep);
    g.perform(crawl);
    // she crawls in, the talk opens, and you're not asleep yet
    CHECK(!g.player().asleep);
    CHECK(g.takeTalkRequest() == elin->id);
    CHECK(g.inTentWith(elin->id));
    std::string hi = g.greeting(elin->id);
    bool sleep = false, hold = false, whisper = false;
    for (auto& [tp, l] : g.talkTopics(elin->id)) {
        sleep |= tp == Topic::TentSleep;
        hold |= tp == Topic::TentHold;
        whisper |= tp == Topic::TentWhisper;
    }
    CHECK(sleep && hold && whisper);
    std::string held = g.talk(elin->id, Topic::TentHold);
    std::string words = g.talk(elin->id, Topic::TentWhisper);
    float comfort = g.sleepComfort().score;
    g.player().body.mutableState().sleepPressure = 1.0f;   // worn out
    std::string night = g.talk(elin->id, Topic::TentSleep);
    std::printf("    \"%s\"\n    \"%s\"\n    \"%s\"\n    \"%s\"\n    comfort %.2f\n", hi.c_str(), held.c_str(), words.c_str(), night.c_str(), comfort);
    CHECK(g.player().asleep);
    run(g, 2);
    CHECK(elin->dozing && g.world().tentAt(elin->pos) != nullptr);
    // alone in a bivvy there's no room: you just sleep
    Game h;
    CHECK(h.loadData());
    h.newGame(42);
    Survivor* e2 = find(h, "elin");
    CHECK(openPatch(h, 20, ox, oy));
    Tent b;
    b.id = h.world().newId();
    b.rect = IRect{ox + 8, oy + 8, 1, 2};
    b.item = "tent_small";
    h.world().tents().push_back(b);
    e2->met = e2->joined = true;
    e2->state = SurvivorState::Follow;
    h.player().pos = Vec2{ox + 8.5f, oy + 10.8f};
    e2->pos = Vec2{ox + 9.5f, oy + 11.0f};
    for (auto& o : h.actionsAt(Vec2{ox + 8.5f, oy + 9.0f}))
        if (o.kind == ActionKind::Sleep && o.enabled) { h.perform(o); break; }
    CHECK(h.player().asleep && h.takeTalkRequest() == 0);
}

TEST(sling_a_weapon_and_count_rounds) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    give(g, "hunting_rifle");
    give(g, "kitchen_knife");
    give(g, "rifle_ammo");
    Player& p = g.player();
    uint32_t rifle = p.inventory.items()[p.inventory.findId("hunting_rifle")].uid;
    uint32_t knife = p.inventory.items()[p.inventory.findId("kitchen_knife")].uid;
    p.weaponUid = rifle;
    // R: five in the gun, five left in the box
    CHECK(g.canReload());
    g.reload();
    run(g, 4);
    CHECK(g.weapon()->usesLeft == 5 && g.ammoCarried(g.weapon()->def) == 5);
    CHECK(g.weapon()->displayName(false).find("[5/5]") != std::string::npos);
    // sling it, take the knife; 1 swaps them
    ActionOption sling;
    for (auto& o : g.actionsForItem(ItemRef{ItemLoc::Player, 0, 0, 0, rifle}))
        if (o.kind == ActionKind::Stow) sling = o;
    CHECK(sling.kind == ActionKind::Stow);
    g.perform(sling);
    CHECK(p.stowedUid == rifle && p.weaponUid == 0);
    p.weaponUid = knife;
    g.toggleStow();
    CHECK(p.weaponUid == rifle && p.stowedUid == knife);
    g.toggleStow();
    CHECK(p.weaponUid == knife && p.stowedUid == rifle);
    // kept through a save
    std::string path = "test_sling_save.txt";
    CHECK(g.save(path));
    Game h;
    CHECK(h.loadData());
    CHECK(h.load(path));
    CHECK(h.player().stowedUid == rifle && h.stowedWeapon() && h.stowedWeapon()->usesLeft == 5);
    std::remove(path.c_str());
    // empty the gun: the next pull of the trigger reloads from the box
    g.toggleStow();
    run(g, 1);   // swapping takes a moment
    for (int i = 0; i < 5; ++i) {
        PlayerInput in;
        in.aim = p.pos + Vec2{5, 0};
        in.attack = true;
        g.update(0.05f, in);
        run(g, 2.5f);
    }
    CHECK(g.weapon()->usesLeft == 0);
    PlayerInput in;
    in.aim = p.pos + Vec2{5, 0};
    in.attack = true;
    g.update(0.05f, in);
    CHECK(g.currentAction().active && g.currentAction().option.kind == ActionKind::Reload);
    run(g, 4);
    CHECK(g.weapon()->usesLeft == 5 && g.ammoCarried(g.weapon()->def) == 0);
}

TEST(companions_ride_along) {
    Game g;
    CHECK(g.loadData());
    g.newGame(42);
    int ox, oy;
    CHECK(openPatch(g, 30, ox, oy));
    World& w = g.world();
    Survivor* elin = find(g, "elin");
    Survivor* gunnar = find(g, "gunnar");
    if (!elin || !gunnar) return;
    for (Survivor* s : {elin, gunnar}) {
        s->met = s->joined = true;
        s->trust = 70;
        s->state = SurvivorState::Follow;
    }
    // a car in the open: both get in
    int cx = ox + 8, cy = oy + 8;
    w.setProp(cx, cy, Prop::Car);
    g.player().pos = Vec2{cx + 0.5f, cy + 2.0f};
    elin->pos = Vec2{cx + 2.5f, cy + 3.0f};
    gunnar->pos = Vec2{cx - 1.5f, cy + 3.5f};
    ActionOption smash;
    smash.kind = ActionKind::SmashCarWindow;
    smash.x = cx; smash.y = cy;
    g.perform(smash);
    Vehicle* car = g.playerVehicle();
    CHECK(car && car->kind == VehicleKind::Car);
    if (!car) return;
    CHECK(elin->ride == car->id && gunnar->ride == car->id && elin->seat != gunnar->seat);
    CHECK(Game::carSeats(*car) == 5 || Game::carSeats(*car) == 7);
    // the car moves: they move with it
    car->pos = car->pos + Vec2{4, 0};
    g.player().pos = car->pos;
    run(g, 0.5f);
    CHECK(distance(elin->pos, car->pos) < 0.01f && distance(gunnar->pos, car->pos) < 0.01f);
    // out you get, and so do they
    ActionOption out;
    out.kind = ActionKind::ExitVehicle;
    g.perform(out);
    run(g, 0.5f);
    CHECK(elin->ride < 0 && gunnar->ride < 0);
    CHECK(distance(elin->pos, g.player().pos) < 5 && distance(gunnar->pos, g.player().pos) < 5);
    std::printf("    out of the car: Elin %.1f m, Gunnar %.1f m from you\n", distance(elin->pos, g.player().pos), distance(gunnar->pos, g.player().pos));

    // bikes: you get on one, Elin fetches the one by the fence, Gunnar finds none
    g.vehicles().clear();
    int bx = ox + 18, by = oy + 20;
    w.setProp(bx, by, Prop::Bike);
    w.setProp(bx + 5, by, Prop::Bike);
    g.player().pos = Vec2{bx + 0.5f, by + 1.3f};
    elin->pos = Vec2{bx + 3.5f, by + 2.5f};
    gunnar->state = SurvivorState::Wait;   // he stays behind this time
    ActionOption mount;
    mount.kind = ActionKind::MountBike;
    mount.x = bx; mount.y = by;
    g.perform(mount);
    CHECK(g.playerVehicle() && g.playerVehicle()->kind == VehicleKind::Bike);
    CHECK(elin->seekingBike);
    run(g, 6);
    CHECK(elin->ride >= 0 && elin->ride != g.player().vehicle);
    // ride off: she follows on hers
    PlayerInput pedal;
    pedal.throttle = 1;
    run(g, 6, pedal);
    float gap = distance(elin->pos, g.player().pos);
    std::printf("    on bikes: %.1f m behind you\n", gap);
    CHECK(gap < 15);
    ActionOption off;
    off.kind = ActionKind::ExitVehicle;
    run(g, 3);
    g.perform(off);
    run(g, 0.5f);
    CHECK(elin->ride < 0);
}
