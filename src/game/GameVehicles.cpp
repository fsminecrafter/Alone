// Bikes and cars: getting into them, starting them, riding and driving.
//
// Parked cars are props on the map. The first time you try a car's door it
// is decided, once and for all, whether it opens: 20% of cars are unlocked or
// have the key left in the door. Inside, 20% have the key in the ignition.
// Without one you can still sit in it out of the rain, or hotwire it if
// someone has shown you how.
#include "game/Game.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace alone {

namespace {

constexpr float kCarOpenChance = 0.2f;
constexpr float kKeyChance = 0.2f;

std::string fmtL(const char* f, float l) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, l);
    return buf;
}

int64_t carKey(int x, int y) { return ((int64_t)x << 32) | (uint32_t)y; }

float wrapAngle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

bool heavyTool(const ItemDef* d) {
    return d->hasTag("axe") || d->hasTag("hammer") || d->id == "hammer" || d->id == "crowbar" || d->id == "sledgehammer" ||
           d->id == "pipe_wrench" || d->id == "baseball_bat" || d->id == "golf_club";
}

// Vans have bigger tanks (body style as in RenderVehicles.cpp)
float carTank(int look) { return ((look >> 4) % 4) == 3 ? 70.0f : 50.0f; }

} // namespace

// Every parked car has 0-40% of a tank, unless someone has siphoned it since
float Game::carFuelAt(int x, int y) const {
    auto it = propFuel_.find(carKey(x, y));
    if (it != propFuel_.end()) return it->second;
    uint32_t look = hash2(x, y, 0xC0102u) & 0xFFFF;
    return std::round(hashFloat(hash2(x, y, seed_ ^ 0xF0E1u)) * 0.4f * carTank((int)look) * 10.0f) / 10.0f;
}

int Game::gasCan(bool wantPetrol, bool wantSpace) const {
    const auto& v = player_.inventory.items();
    int best = -1;
    for (size_t i = 0; i < v.size(); ++i) {
        const ItemInstance& it = v[i];
        if (!it.def->hasTag("gascan")) continue;
        bool petrol = it.liquid == 1 && it.liquidMl > 0;
        bool empty = it.liquidMl <= 0;
        if (wantPetrol && !petrol) continue;
        if (wantSpace && !(petrol || empty)) continue;          // water in it: not for petrol
        if (wantSpace && it.liquidMl >= it.def->capacityMl - 1) continue;
        if (best < 0 || (wantPetrol ? it.liquidMl > v[best].liquidMl : it.liquidMl < v[best].liquidMl)) best = (int)i;
    }
    return best;
}

Vec2 Vehicle::forward() const { return {std::cos(heading), std::sin(heading)}; }

Vehicle* Game::vehicle(int id) {
    for (auto& v : vehicles_)
        if (v.id == id) return &v;
    return nullptr;
}

Vehicle& Game::vehicleFromProp(int x, int y, VehicleKind kind) {
    const Tile& t = world_->tile(x, y);
    Vehicle v;
    v.id = nextVehicleId_++;
    v.kind = kind;
    v.pos = Vec2{x + 0.5f, y + 0.5f};
    uint32_t h = hash2(x, y, seed_ ^ 0xCA7u);
    if (kind == VehicleKind::Car) {
        // looking and pointing the way the parked car was drawn (RenderProps.cpp)
        v.variant = (int)(hash2(x, y, 0xC0102u) & 0xFFFF);
        v.heading = ((t.variant & 1) ? kPi * 0.5f : 0.0f) + (((t.variant >> 4) & 15) / 15.0f - 0.5f) * 0.25f;
        v.keyInIgnition = hashFloat(hash32(h + 1)) < kKeyChance;
        v.tankL = carTank(v.variant);
        v.fuelL = carFuelAt(x, y);
        propFuel_.erase(carKey(x, y));
    } else {
        v.variant = t.variant;
        v.heading = t.variant / 256.0f * 2 * kPi;
    }
    world_->setProp(x, y, Prop::None);
    vehicles_.push_back(v);
    return vehicles_.back();
}

// The parked car (prop) whose body covers this point, if any
static bool carPropNear(World& w, Vec2 pt, int& ox, int& oy) {
    int tx = (int)std::floor(pt.x), ty = (int)std::floor(pt.y);
    float best = 1e9f;
    bool found = false;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const Tile& t = w.tile(tx + dx, ty + dy);
            if (t.prop != Prop::Car) continue;
            Vec2 c{tx + dx + 0.5f, ty + dy + 0.5f};
            Vec2 fwd = (t.variant & 1) ? Vec2{0, 1} : Vec2{1, 0};
            Vec2 rel = pt - c;
            float along = std::fabs(dot(rel, fwd)), across = std::fabs(rel.x * fwd.y - rel.y * fwd.x);
            if (along > 2.3f || across > 1.0f) continue;
            float d = rel.length();
            if (d < best) { best = d; ox = tx + dx; oy = ty + dy; found = true; }
        }
    return found;
}

std::vector<ActionOption> Game::vehicleActionsAt(Vec2 pt) {
    std::vector<ActionOption> out;
    auto add = [&](ActionKind k, const std::string& label, int id, bool ok = true, const std::string& why = "") {
        ActionOption o;
        o.kind = k; o.label = label; o.targetId = id; o.enabled = ok; o.reason = why;
        out.push_back(o);
        return &out.back();
    };
    Player& p = player_;
    // Petrol in and out of a tank (a moved car: id; a parked one: its tile)
    auto fuelActions = [&](float fuel, float tank, int id, int x, int y, bool reach, const std::string& far) {
        if (gasCan(true, false) >= 0) {
            auto* o = add(ActionKind::Refuel, "Pour petrol into the tank", id, reach && fuel < tank - 0.5f, !reach ? far : "The tank is full");
            o->x = x; o->y = y;
        }
        if (p.inventory.findTag("hose") >= 0) {
            bool can = gasCan(false, true) >= 0;
            auto* o = add(ActionKind::Siphon, "Siphon petrol with the hose", id, reach && can && fuel > 0.2f,
                          !reach ? far : !can ? "You need a gas can (empty, or with petrol in it)" : "The tank is dry");
            o->x = x; o->y = y;
        }
    };

    // Sitting in one
    if (Vehicle* v = playerVehicle()) {
        bool car = v->kind == VehicleKind::Car;
        if (car) {
            if (!v->running) {
                if (v->canStart()) add(ActionKind::StartEngine, v->keyInIgnition ? "Turn the key" : "Start the engine", v->id, v->damage < 1, "It's wrecked");
                else if (p.knowsHotwire) {
                    bool sd = p.inventory.findTag("screwdriver") >= 0;
                    add(ActionKind::Hotwire, "Hotwire it", v->id, sd, "You need a screwdriver");
                }
            }
        }
        bool slow = std::fabs(v->speed) < 1.0f;
        add(ActionKind::ExitVehicle, car ? "Get out" : "Get off the bike", v->id, slow, "Stop first");
        if (car && v->running) add(ActionKind::StopEngine, "Switch off the engine", v->id);
        return out;
    }

    // A vehicle that's already been moved
    for (auto& v : vehicles_) {
        Vec2 rel = pt - v.pos;
        Vec2 f = v.forward();
        float along = std::fabs(dot(rel, f)), across = std::fabs(rel.x * f.y - rel.y * f.x);
        if (along > v.length() * 0.5f + 0.3f || across > v.halfWidth() + 0.5f) continue;
        float d = distance(v.pos, p.pos);
        bool reach = d < (v.kind == VehicleKind::Car ? 3.2f : 2.0f);
        std::string far = reach ? "" : "Too far away";
        if (v.kind == VehicleKind::Bike) {
            add(ActionKind::EnterVehicle, "Ride the bike", v.id, reach, far);
            continue;
        }
        add(ActionKind::EnterVehicle, "Get in the car", v.id, reach, far);
        if (!v.searched) add(ActionKind::SearchGlovebox, "Search the glovebox", v.id, reach, far);
        fuelActions(v.fuelL, v.tankL, v.id, 0, 0, reach, far);
        return out;
    }

    // Parked cars and bikes on the map
    int cx, cy;
    if (carPropNear(*world_, pt, cx, cy)) {
        float d = distance(Vec2{cx + 0.5f, cy + 0.5f}, p.pos);
        bool reach = d < 3.2f;
        std::string far = reach ? "" : "Too far away";
        if (lockedCars_.count(carKey(cx, cy))) {
            add(ActionKind::TryCarDoor, "Locked", 0, false, "You tried it: it's locked");
            bool tool = false;
            for (const auto& it : p.inventory.items()) tool |= heavyTool(it.def);
            auto* o = add(ActionKind::SmashCarWindow, "Smash the side window", 0, reach && tool, !reach ? far : "You need something heavy");
            o->x = cx; o->y = cy;
        } else {
            auto* o = add(ActionKind::TryCarDoor, "Try the driver's door", 0, reach, far);
            o->x = cx; o->y = cy;
        }
        fuelActions(carFuelAt(cx, cy), carTank((int)(hash2(cx, cy, 0xC0102u) & 0xFFFF)), 0, cx, cy, reach, far);
        return out;
    }
    int tx = (int)std::floor(pt.x), ty = (int)std::floor(pt.y);
    if (world_->tile(tx, ty).prop == Prop::FuelPump) {
        bool reach = distance(Vec2{tx + 0.5f, ty + 0.5f}, p.pos) < 2.2f;
        bool can = gasCan(false, true) >= 0;
        bool power = world_->powerOn();
        bool hose = p.inventory.findTag("hose") >= 0;
        float left = 1500.0f - pumpFuel_[carKey(tx, ty)];
        std::string label = power ? "Fill a gas can at the pump" : "Siphon from the station's tank";
        auto* o = add(ActionKind::FillFromPump, label, 0, reach && can && (power || hose) && left > 1,
                      !reach ? "Too far away" : !can ? "You need a gas can (empty, or with petrol in it)"
                      : left <= 1 ? "The tank under the station is dry" : "No power for the pump. A hose down the filler pipe would do");
        o->x = tx; o->y = ty;
    }
    if (world_->tile(tx, ty).prop == Prop::Bike) {
        bool reach = distance(Vec2{tx + 0.5f, ty + 0.5f}, p.pos) < 2.0f;
        auto* o = add(ActionKind::MountBike, "Get on the bike", 0, reach, reach ? "" : "Too far away");
        o->x = tx; o->y = ty;
    }
    return out;
}

void Game::performVehicle(const ActionOption& o) {
    Player& p = player_;
    auto enter = [&](Vehicle& v) {
        p.vehicle = v.id;
        p.pos = v.pos;
        p.facing = v.heading;
        p.sneaking = p.running = false;
        if (v.kind == VehicleKind::Car)
            log(v.keyInIgnition ? "You get in. The key is in the ignition!" : v.hotwired ? "You get in." : "You get in. There's no key in the ignition.",
                v.keyInIgnition ? MessageKind::Good : MessageKind::Info);
        else
            log("You get on the bike.");
    };
    switch (o.kind) {
        case ActionKind::TryCarDoor: {
            uint32_t h = hash2(o.x, o.y, seed_ ^ 0xCA7u);
            if (hashFloat(h) < kCarOpenChance) {
                // unlocked, or the key left in the lock (it's a door key on its own ring)
                bool keyInDoor = hashFloat(hash32(h + 7)) < 0.4f;
                log(keyInDoor ? "There's a key left in the lock. The door opens." : "It isn't locked.", MessageKind::Good);
                sound(Sfx::Door, p.pos);
                Vehicle& v = vehicleFromProp(o.x, o.y, VehicleKind::Car);
                enter(v);
            } else {
                lockedCars_.insert(carKey(o.x, o.y));
                sound(Sfx::Click, p.pos);
                log("Locked.");
            }
            return;
        }
        case ActionKind::SmashCarWindow: {
            sound(Sfx::Glass, p.pos);
            animals_.scare(p.pos, 80);
            // raiders hear it
            for (auto& s : survivors_)
                if (s.alive() && s.def->disposition == Disposition::Raider && distance(s.pos, p.pos) < 90) {
                    s.state = SurvivorState::Hunt;
                    s.stateTimer = 30;
                }
            log("The window bursts. Glass everywhere.");
            if (rng_.chance(0.3f)) {
                Wound w;
                w.part = rng_.chance(0.5f) ? BodyPart::RightHand : BodyPart::RightArm;
                w.type = WoundType::Laceration;
                w.severity = rng_.range(0.08f, 0.25f);
                w.bleeding = 6;
                w.contamination = 0.25f;
                p.body.addWound(w);
                log("You cut yourself on the glass.", MessageKind::Bad);
            }
            lockedCars_.erase(carKey(o.x, o.y));
            Vehicle& v = vehicleFromProp(o.x, o.y, VehicleKind::Car);
            v.windowSmashed = true;
            enter(v);
            return;
        }
        case ActionKind::MountBike: {
            Vehicle& v = vehicleFromProp(o.x, o.y, VehicleKind::Bike);
            enter(v);
            return;
        }
        case ActionKind::EnterVehicle:
            if (Vehicle* v = vehicle(o.targetId)) enter(*v);
            return;
        case ActionKind::ExitVehicle: {
            Vehicle* v = playerVehicle();
            p.vehicle = -1;
            if (!v) return;
            v->speed = 0;
            if (v->running) { v->running = false; }
            // step out beside it
            Vec2 f = v->forward(), side{-f.y, f.x};
            float off = v->halfWidth() + 0.55f;
            for (Vec2 c : {v->pos - side * off, v->pos + side * off, v->pos - f * (v->length() * 0.5f + 0.5f), v->pos + f * (v->length() * 0.5f + 0.5f)})
                if (!world_->tileBlocked((int)std::floor(c.x), (int)std::floor(c.y))) { p.pos = c; break; }
            log(v->kind == VehicleKind::Car ? "You get out." : "You get off the bike.");
            return;
        }
        case ActionKind::StartEngine: {
            Vehicle* v = playerVehicle();
            if (!v) return;
            sound(Sfx::Engine, p.pos);
            if (!v->canStart()) { log("No key."); return; }
            if (v->fuelL <= 0.05f) { log("The engine turns over and over but won't catch. The tank is empty.", MessageKind::Bad); return; }
            if (v->damage >= 1) { log("Nothing. It's wrecked.", MessageKind::Bad); return; }
            v->running = true;
            log(v->fuelL < 3 ? "The engine starts. The fuel light is on." : "The engine coughs and starts.", MessageKind::Good);
            return;
        }
        case ActionKind::StopEngine:
            if (Vehicle* v = playerVehicle()) v->running = false;
            return;
        case ActionKind::Hotwire:
            startTimed(o, 7.0f);
            return;
        case ActionKind::SearchGlovebox: {
            Vehicle* v = vehicle(o.targetId);
            if (!v) return;
            v->searched = true;
            auto loot = items_.rollLoot("glovebox", hash32(seed_ * 13 + (uint32_t)v->id * 7919u));
            if (loot.empty()) { log("The glovebox has the car's papers in it, nothing else."); return; }
            std::string got;
            for (auto& it : loot) {
                got += (got.empty() ? "" : ", ") + it.def->name;
                giveItem(it);
            }
            log("In the glovebox: " + got + ".", MessageKind::Good);
            return;
        }
        case ActionKind::Refuel: {
            int ci = gasCan(true, false);
            if (ci < 0) return;
            ItemInstance& can = p.inventory.items()[ci];
            Vehicle* v = o.targetId ? vehicle(o.targetId) : nullptr;
            float fuel = v ? v->fuelL : carFuelAt(o.x, o.y);
            float tank = v ? v->tankL : carTank((int)(hash2(o.x, o.y, 0xC0102u) & 0xFFFF));
            float l = std::min(can.liquidMl / 1000.0f, tank - fuel);
            can.liquidMl -= l * 1000.0f;
            if (v) v->fuelL += l;
            else propFuel_[carKey(o.x, o.y)] = fuel + l;
            sound(Sfx::Drink, p.pos);
            log(fmtL("You pour %.1f litres into the tank. It glugs.", l));
            return;
        }
        case ActionKind::Siphon: {
            int ci = gasCan(false, true);
            if (ci < 0) return;
            ItemInstance& can = p.inventory.items()[ci];
            Vehicle* v = o.targetId ? vehicle(o.targetId) : nullptr;
            float fuel = v ? v->fuelL : carFuelAt(o.x, o.y);
            float l = std::min(fuel, (can.def->capacityMl - can.liquidMl) / 1000.0f);
            if (l <= 0.05f) { log("Nothing comes."); return; }
            can.liquid = 1;
            can.liquidSafe = true;
            can.liquidMl += l * 1000.0f;
            if (v) v->fuelL -= l;
            else propFuel_[carKey(o.x, o.y)] = fuel - l;
            log(fmtL("A mouthful of petrol, then it flows: %.1f litres.", l));
            if (rng_.chance(0.35f)) {
                log("You retch at the taste.", MessageKind::Body);
                player_.body.mutableState().nauseaExtra += 0.15f;
            }
            return;
        }
        case ActionKind::FillFromPump: {
            int ci = gasCan(false, true);
            if (ci < 0) return;
            ItemInstance& can = p.inventory.items()[ci];
            float& taken = pumpFuel_[carKey(o.x, o.y)];
            float l = std::min(1500.0f - taken, (can.def->capacityMl - can.liquidMl) / 1000.0f);
            if (l <= 0) return;
            taken += l;
            can.liquid = 1;
            can.liquidSafe = true;
            can.liquidMl += l * 1000.0f;
            log(world_->powerOn() ? fmtL("The pump whirrs. %.1f litres.", l) : fmtL("You fish the hose down the filler pipe and suck. %.1f litres.", l),
                MessageKind::Good);
            return;
        }
        default: return;
    }
}

// Riding and driving: called instead of walking while you're on a bike or in a car.
void Game::updateVehicle(float dt, const PlayerInput& in) {
    Player& p = player_;
    Vehicle* v = playerVehicle();
    if (!v) { p.vehicle = -1; return; }
    bool car = v->kind == VehicleKind::Car;
    p.fallen = std::max(0.0f, p.fallen - dt);
    Capacity cap = p.body.capacity();

    // What the ground lets you do
    int tx = (int)std::floor(v->pos.x), ty = (int)std::floor(v->pos.y);
    const Tile& t = world_->tile(tx, ty);
    Ground g = t.ground;
    bool road = g == Ground::Asphalt || g == Ground::Concrete || g == Ground::Sidewalk || g == Ground::Cobble;
    bool track = g == Ground::Gravel || g == Ground::Dirt || g == Ground::Sand;
    float top = car ? (road ? 17.0f : track ? 9.0f : 5.0f) : (road ? 6.5f : track ? 4.8f : 2.6f);
    if (t.building >= 0) top = car ? 3.0f : 2.0f;
    Zone zone = map_.zoneAt(v->pos.x, v->pos.y);
    if (!road && !track && (zone == Zone::ForestDense || zone == Zone::Thicket || zone == Zone::Marsh)) top *= 0.5f;
    if (g == Ground::WaterShallow) top *= 0.4f;
    Vec2 fwd = v->forward();
    float grade = world_->slope(v->pos, v->speed >= 0 ? fwd : fwd * -1.0f);
    if (!car) {
        top *= cap.moveSpeed * std::clamp(1.0f - grade * 5.0f, 0.3f, 1.35f);
        if (p.stamina < 0.05f) top *= 0.4f;
    }
    if (car && !v->running) top = 0;

    // Steer towards where you want to go; back up if that's behind you
    float len = in.move.length();
    float throttle = 0;
    if (len > 0.1f && p.fallen <= 0) {
        float want = std::atan2(in.move.y, in.move.x);
        float diff = wrapAngle(want - v->heading);
        bool reverse = car && std::fabs(diff) > 2.2f && v->speed < 1.5f;
        if (reverse) diff = wrapAngle(want + kPi - v->heading);
        float turnRate = car ? 1.7f * std::min(1.0f, std::fabs(v->speed) / 3.0f) : 0.6f + 2.2f * std::min(1.0f, std::fabs(v->speed) / 1.5f);
        float turn = std::clamp(diff, -turnRate * dt, turnRate * dt);
        float newHeading = v->heading + turn;
        // only turn if the body fits there
        Vec2 nf{std::cos(newHeading), std::sin(newHeading)};
        bool fits = true;
        if (car)
            for (float off : {-1.6f, 1.6f}) {
                Vec2 q = v->pos + nf * off;
                fits &= !world_->tileBlocked((int)std::floor(q.x), (int)std::floor(q.y));
            }
        if (fits) v->heading = wrapAngle(newHeading);
        throttle = (reverse ? -0.45f : 1.0f) * std::clamp(std::cos(diff) * 1.2f, 0.25f, 1.0f) * std::min(1.0f, len);
        if (in.sneak) throttle *= 0.4f;    // Ctrl: creep
    }
    float target = throttle * top;
    float accel = car ? (target * v->speed < 0 || std::fabs(target) < std::fabs(v->speed) ? 9.0f : 4.5f)
                      : (std::fabs(target) < std::fabs(v->speed) ? 5.0f : 2.2f);
    if (len <= 0.1f) accel = car ? 3.0f : 2.0f;   // rolling to a stop
    if (v->speed < target) v->speed = std::min(target, v->speed + accel * dt);
    else v->speed = std::max(target, v->speed - accel * dt);

    // Move, front and back
    fwd = v->forward();
    Vec2 delta = fwd * (v->speed * dt);
    bool blocked = false;
    if (delta.lengthSq() > 1e-8f) {
        std::vector<float> offs = car ? std::vector<float>{-1.55f, 0.0f, 1.55f} : std::vector<float>{0.0f};
        float r = car ? 0.82f : 0.32f;
        for (float off : offs) {
            Vec2 q = v->pos + fwd * off;
            Vec2 moved = world_->moveCircle(q, delta, r);
            if (distance(moved, q + delta) > 0.03f) { blocked = true; break; }
        }
        if (!blocked) v->pos = v->pos + delta;
    }
    float spd = std::fabs(v->speed);
    if (blocked) {
        if (spd > 6.0f) {
            float sev = std::clamp((spd - 5.0f) / 14.0f, 0.05f, 0.8f);
            sound(Sfx::Hit, v->pos);
            if (car) {
                v->damage = std::min(1.0f, v->damage + spd / 45.0f);
                log(v->damage >= 1 ? "CRASH. Steam pours out of the bonnet. It's finished." : "You slam into something!", MessageKind::Bad);
                if (v->damage >= 1) v->running = false;
                if (spd > 9.0f) playerHurtBy(sev * 0.7f, "The crash", false);
            } else {
                log("You go over the handlebars!", MessageKind::Bad);
                playerHurtBy(sev, "The fall", false);
                v->speed = 0;
                p.vehicle = -1;      // thrown off: you land beside it
                p.fallen = 2.0f;
                Vec2 side{-fwd.y, fwd.x};
                p.pos = world_->moveCircle(v->pos, side * 0.8f, 0.28f);
                return;
            }
        }
        v->speed = -v->speed * 0.15f;
    }

    // Running into animals and people
    if (spd > 3.0f) {
        Vec2 front = v->pos + fwd * (v->length() * 0.5f * (v->speed > 0 ? 1.0f : -1.0f));
        float hitR = car ? 1.3f : 0.6f;
        for (auto& a : animals_.animals())
            if (a.alive() && distance(a.pos, front) < hitR + a.def->size * 0.4f) {
                a.hp -= spd * (car ? 9.0f : 2.0f);
                sound(Sfx::Hit, a.pos);
                if (a.hp <= 0) { a.state = AnimalState::Dead; log("You run down the " + std::string(a.def->name) + ".", MessageKind::Bad); }
                else { a.state = AnimalState::Flee; a.stateTimer = 8; }
                v->speed *= car ? 0.6f : 0.2f;
                if (car) v->damage = std::min(1.0f, v->damage + 0.02f * a.def->size);
            }
        for (auto& s : survivors_)
            if (s.alive() && !s.joined && distance(s.pos, front) < hitR + 0.3f) {
                hurtSurvivor(s, spd * (car ? 8.0f : 1.5f), true);
                v->speed *= car ? 0.7f : 0.2f;
            }
    }

    // Fuel and effort
    if (car && v->running) {
        v->fuelL -= dt * (0.0004f + 0.00011f * spd);
        if (v->fuelL <= 0) {
            v->fuelL = 0;
            v->running = false;
            log("The engine sputters and dies. Out of petrol.", MessageKind::Bad);
        }
    }
    if (!car && spd > 0.5f) {
        float uphill = std::max(0.0f, grade);
        p.stamina -= dt * (0.006f + uphill * 0.5f) * (spd / 5.0f);
    } else {
        p.stamina += dt / 22.0f * cap.endurance;
    }
    p.stamina = clamp01(p.stamina);

    p.pos = v->pos;
    p.facing = v->heading;
    p.grade = grade;
    p.moving = false;
    p.running = false;
    p.noise = car ? (v->running ? 40.0f + spd * 3.0f : 4.0f) : 5.0f + spd;
    stepDistance_ = 0;
}

} // namespace alone
