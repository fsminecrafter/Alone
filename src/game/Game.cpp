#include "game/Game.h"
#include "core/Log.h"
#include "core/Paths.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace alone {

namespace {

constexpr float kReach = 1.7f;

bool inSeason(int doy, int from, int to) { return doy >= from && doy <= to; }

std::string lower(std::string s) {
    if (!s.empty()) s[0] = (char)std::tolower((unsigned char)s[0]);
    return s;
}

const char* sourceName(World::WaterSource s) {
    switch (s) {
        case World::WaterSource::Lake: return "the lake";
        case World::WaterSource::ToiletTank: return "the toilet tank";
        case World::WaterSource::RainBarrel: return "the rain barrel";
        default: return "";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool Game::loadData() {
    bool ok = items_.loadDirectory(Paths::data("items"));
    ok &= items_.loadContainers(Paths::data("loot/containers.ini"));
    ok &= map_.load(Paths::data("maps/alingsas"));
    ok &= symptoms_.load(Paths::data("text/symptoms.ini"));
    ok &= animals_.load(Paths::data("animals/animals.ini"));
    return ok;
}

void Game::newGame(uint32_t seed) {
    seed_ = seed;
    world_ = std::make_unique<World>(map_, items_, seed);
    rng_.reseed(seed * 7919ULL + 17);
    player_ = Player{};
    player_.body = Body(seed + 99);
    animals_.animals().clear();
    messages_.clear();
    action_ = {};
    openContainer_ = false;
    death_ = {};
    speed_ = 1;
    lastSymptomLevels_.clear();

    // Start inside a house near the spawn point
    const Building* home = world_->gen().findStartHouse(map_.spawn());
    if (home) {
        const Room* living = &home->rooms[0];
        for (const auto& r : home->rooms)
            if (r.type == RoomType::Living) living = &r;
        Vec2 c = living->rect.centre();
        player_.pos = c;
        for (int i = 0; i < 20 && world_->tileBlocked((int)player_.pos.x, (int)player_.pos.y); ++i)
            player_.pos = c + Vec2{rng_.range(-2, 2), rng_.range(-2, 2)};
    } else {
        player_.pos = map_.spawn();
    }

    auto wear = [&](const char* id) {
        ItemInstance it = items_.create(id, rng_);
        if (it.def) player_.worn[(size_t)it.def->slot] = it;
    };
    wear("tshirt");
    wear("fleece");
    wear("jeans");
    wear("sneakers");
    ItemInstance bottle = items_.create("water_bottle", rng_);
    bottle.liquidMl = 600;
    player_.inventory.add(bottle);
    ItemInstance bread = items_.create("crispbread", rng_);
    bread.portion = 0.4f;
    player_.inventory.add(bread);

    log("It has been eleven days since you last saw another person.", MessageKind::Info);
    log("The town is silent. You need food, water and warmth to get through the autumn.", MessageKind::Info);
    log("Right-click things to interact. Tab opens your inventory.", MessageKind::Info);
}

void Game::log(const std::string& text, MessageKind kind) {
    messages_.push_back({text, kind, 0});
    while (messages_.size() > 40) messages_.pop_front();
    Log::info("> %s", text.c_str());
}

std::vector<ActiveSymptom> Game::currentSymptoms() const {
    return symptoms_.evaluate(player_.body.signals());
}

std::string Game::location() const {
    std::string district = map_.districtAt(player_.pos);
    int tx = (int)std::floor(player_.pos.x), ty = (int)std::floor(player_.pos.y);
    const Tile& t = world_->tile(tx, ty);
    if (t.building >= 0) {
        const Building* b = world_->building(t.building);
        if (b) {
            std::string what = b->name.empty() ? buildingTypeName(b->type) : b->name;
            if (const Room* r = b->roomAt(tx, ty)) {
                if (b->name.empty() && (b->type == BuildingType::House || b->type == BuildingType::Apartment ||
                                        b->type == BuildingType::Cottage || b->type == BuildingType::Farmhouse))
                    what = roomTypeName(r->type);
            }
            return what + ", " + district;
        }
    }
    return district;
}

bool Game::knowsMushrooms() const {
    return player_.inventory.findTag("field_guide") >= 0;
}

float Game::carryCapacityKg() const {
    float cap = 16.0f;
    for (const auto& w : player_.worn)
        if (w.def) cap += w.def->carryBonusKg;
    return cap * (0.5f + 0.5f * player_.body.capacity().strength);
}

float Game::carriedKg() const {
    float w = player_.inventory.totalWeight();
    for (const auto& c : player_.worn)
        if (c.def) w += c.weightKg() * 0.3f;   // worn clothes barely count
    return w;
}

const ItemInstance* Game::weapon() const {
    int i = player_.inventory.indexOfUid(player_.weaponUid);
    return i >= 0 ? &player_.inventory.items()[i] : nullptr;
}

Environment Game::environment() {
    Environment env;
    bool inside = world_->indoors(player_.pos);
    env.airTempC = world_->temperatureAt(player_.pos);
    const Weather& w = world_->clock().weather();
    env.wind = inside ? 0.0f : w.wind;
    env.wetness = inside ? 0.0f : w.rain;
    int tx = (int)std::floor(player_.pos.x), ty = (int)std::floor(player_.pos.y);
    if (world_->tile(tx, ty).ground == Ground::WaterShallow) env.wetness = 1.0f;
    float clo = 0;
    float waterproof = 0;
    for (const auto& c : player_.worn) {
        if (!c.def) continue;
        clo += c.def->clo;
        if (c.def->slot == ClothingSlot::Jacket) waterproof = std::max(waterproof, c.def->waterproof);
    }
    env.clothingClo = clo;
    env.waterproof = waterproof;
    env.carriedKg = carriedKg();
    // Sleeping in a bed with covers is much warmer
    if (player_.asleep) {
        const Tile& t = world_->tile(tx, ty);
        bool bed = false;
        for (int dy = -1; dy <= 1 && !bed; ++dy)
            for (int dx = -1; dx <= 1 && !bed; ++dx) {
                Prop p = world_->tile(tx + dx, ty + dy).prop;
                bed = p == Prop::Bed || p == Prop::HospitalBed;
            }
        if (bed) env.clothingClo += 2.0f;
        (void)t;
    }
    return env;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void Game::update(float dt, const PlayerInput& input) {
    if (!world_) return;
    for (auto& m : messages_) m.age += dt;
    if (dead()) return;

    float timeScale = action_.active ? action_.timeScale : 1.0f;
    float gameMinutes = dt * minutesPerSecond_ * speed_ * timeScale;

    world_->update(gameMinutes, player_.pos);
    unloadTimer_ += dt;
    if (unloadTimer_ > 5.0f) {   // keep memory bounded while exploring
        unloadTimer_ = 0;
        world_->unloadFarChunks(player_.pos, 420.0f);
    }
    updatePlayer(dt, gameMinutes, input);
    updateCombat(dt, input);

    // Wildlife
    PlayerPresence presence{player_.pos, player_.noise, !dead()};
    // Animals move in real time; when the clock is sped up they keep up
    float animalDt = std::min(dt * std::max(1.0f, speed_ * timeScale * 0.25f), 0.25f);
    for (const auto& atk : animals_.update(animalDt, gameMinutes, *world_, presence, rng_))
        playerHurt(atk.severity, atk.by);

    // Snares: chance of catching a hare each game hour in good habitat
    for (auto& s : world_->snares()) {
        if (s.caught) continue;
        s.minutesSet += gameMinutes;
        Zone z = map_.zoneAt(s.pos.x, s.pos.y);
        float habitat = (z == Zone::Woodland || z == Zone::Meadow || z == Zone::Field) ? 1.0f : z == Zone::Forest ? 0.6f : 0.15f;
        if (rng_.chance(0.035f * habitat * gameMinutes / 60.0f)) {
            s.caught = true;
            if (const AnimalDef* hare = animals_.find("hare")) {
                Animal& a = animals_.spawn(hare, s.pos, rng_);
                a.state = AnimalState::Dead;
                a.hp = 0;
            }
        }
    }

    // Timed action progress
    if (action_.active) {
        action_.elapsed += dt;
        bool done = action_.elapsed >= action_.duration;
        if (action_.option.kind == ActionKind::Sleep) {
            const BodyState& b = player_.body.state();
            done = b.sleepPressure < 0.03f && world_->clock().daylight() > 0.2f;
            if (b.sleepPressure < 0.01f) done = true;
            BodySignals g = player_.body.signals();
            if (g.cold > 0.6f || g.pain > 0.7f || g.thirst > 0.75f || g.hunger > 0.97f || g.nausea > 0.75f || g.bleeding > 0.3f) {
                log("You wake up. You can't sleep like this.", MessageKind::Bad);
                done = true;
            }
        }
        if (done) finishAction();
    }

    if (dead()) {
        death_.cause = player_.body.deathCause();
        death_.daysSurvived = (float)((world_->clock().totalMinutes() - 9 * 60) / 1440.0);
        death_.place = location();
        action_.active = false;
        log("You died.", MessageKind::Bad);
    }
}

void Game::updatePlayer(float dt, float gameMinutes, const PlayerInput& in) {
    Player& p = player_;
    Capacity cap = p.body.capacity();

    // Movement
    float len = in.move.length();
    bool wantsMove = len > 0.1f;
    if (wantsMove && action_.active) {
        if (action_.option.kind == ActionKind::Sleep) log("You get up.");
        cancelAction();
    }
    p.moving = wantsMove;
    p.sneaking = in.sneak;
    bool canRun = p.stamina > 0.05f && !in.sneak;
    p.running = wantsMove && in.run && canRun;

    float speed = p.sneaking ? 1.0f : p.running ? 4.6f : 2.1f;
    speed *= cap.moveSpeed;
    float over = carriedKg() / carryCapacityKg();
    if (over > 1.0f) speed *= std::max(0.35f, 1.0f - (over - 1.0f));
    int tx = (int)std::floor(p.pos.x), ty = (int)std::floor(p.pos.y);
    Ground g = world_->tile(tx, ty).ground;
    if (g == Ground::WaterShallow) speed *= 0.45f;
    else if (g == Ground::TallGrass || g == Ground::FieldSoil) speed *= 0.85f;

    if (wantsMove) {
        Vec2 dir = in.move * (1.0f / std::max(1.0f, len));
        Vec2 before = p.pos;
        p.pos = world_->moveCircle(p.pos, dir * (speed * dt), 0.28f);
        stepDistance_ += distance(before, p.pos);
        float stride = p.running ? 1.5f : p.sneaking ? 0.6f : 0.85f;
        if (stepDistance_ > stride) { stepDistance_ = 0; sound(Sfx::Footstep, p.pos); }
        p.facing = std::atan2(dir.y, dir.x);
    }

    // Stamina (real-time)
    if (p.running) p.stamina -= dt / 14.0f;
    else p.stamina += dt / 22.0f * cap.endurance * (p.moving ? 0.6f : 1.0f);
    p.stamina = clamp01(p.stamina);

    p.noise = p.sneaking ? 3.0f : p.running ? 26.0f : p.moving ? 11.0f : 4.0f;
    if (action_.active && (action_.option.kind == ActionKind::ChopTree)) p.noise = 45.0f;
    if (p.asleep) p.noise = 2.0f;
    p.lastAttack += dt;
    p.swing = std::max(0.0f, p.swing - dt * 3.0f);

    // Flashlight battery
    if (p.flashlightOn) {
        int fl = p.inventory.findTag("light");
        if (fl < 0) p.flashlightOn = false;
        else {
            ItemInstance& f = p.inventory.items()[fl];
            flashlightAcc_ += gameMinutes;
            while (flashlightAcc_ >= 1.0f && f.usesLeft > 0) { flashlightAcc_ -= 1.0f; --f.usesLeft; }
            if (f.usesLeft <= 0) { p.flashlightOn = false; log("The flashlight flickers and dies."); }
        }
    }

    // Body
    Activity act = Activity::Standing;
    if (p.asleep) act = Activity::Sleeping;
    else if (p.lastAttack < 2.0f) act = Activity::Fighting;
    else if (p.running) act = Activity::Running;
    else if (p.moving) act = Activity::Walking;
    else if (action_.active) act = action_.option.kind == ActionKind::Rest || action_.option.kind == ActionKind::Wait ? Activity::Resting : Activity::Working;
    Environment env = environment();
    p.body.update(gameMinutes, env, act);
    float temp = env.airTempC;
    p.inventory.age(gameMinutes / 60.0f, temp);
    handleBodyEvents();

    // Mention symptoms as they appear or worsen
    symptomMsgTimer_ -= dt;
    auto syms = currentSymptoms();
    if (symptomMsgTimer_ <= 0) {
        symptomMsgTimer_ = 1.0f;
        std::map<std::string, int> now;
        for (const auto& s : syms) now[s.def->id] = s.level;
        // Announce a symptom when it gets worse than it has been lately;
        // it has to stay better for a while before it can be announced again.
        for (const auto& s : syms) {
            Announced& a = lastSymptomLevels_[s.def->id];
            if (s.level > a.level) {
                if (s.def->id != "full") log(s.name() + ".", MessageKind::Body);
                a.level = s.level;
                a.calm = 0;
            }
        }
        for (auto& [id, a] : lastSymptomLevels_) {
            int cur = now.count(id) ? now[id] : 0;
            if (cur < a.level) {
                a.calm += 1.0f;
                if (a.calm > 90.0f) { a.level = cur; a.calm = 0; }
            } else {
                a.calm = 0;
            }
        }
    }
}

void Game::handleBodyEvents() {
    for (BodyEvent e : player_.body.takeEvents()) {
        switch (e) {
            case BodyEvent::Vomited: log("You double over and throw up.", MessageKind::Bad); break;
            case BodyEvent::Diarrhea: log("Your bowels cramp. You have to go, now.", MessageKind::Bad); break;
            case BodyEvent::Shivering: log("You start shivering.", MessageKind::Body); break;
            case BodyEvent::StoppedShivering: log("You've stopped shivering. You feel oddly warm.", MessageKind::Bad); break;
            case BodyEvent::WoundInfected: log("A wound feels hot and throbs.", MessageKind::Bad); break;
            case BodyEvent::FellAsleep:
                if (!player_.asleep) {
                    log("Your eyes close on their own...", MessageKind::Bad);
                    ActionOption o;
                    o.kind = ActionKind::Sleep;
                    o.label = "Sleeping";
                    perform(o);
                }
                break;
            default: break;
        }
    }
}

void Game::playerHurt(float severity, const AnimalDef* by) {
    sound(Sfx::Hurt, player_.pos);
    if (player_.asleep) { cancelAction(); log("You wake with a start!", MessageKind::Bad); }
    static const BodyPart parts[] = {BodyPart::LeftLeg, BodyPart::RightLeg, BodyPart::LeftFoot, BodyPart::RightFoot,
                                     BodyPart::LeftArm, BodyPart::RightArm, BodyPart::Torso, BodyPart::LeftHand, BodyPart::RightHand};
    Wound w;
    w.part = parts[rng_.rangeInt(0, 8)];
    w.type = by && by->id == "moose" ? WoundType::Laceration : (rng_.chance(0.5f) ? WoundType::Bite : WoundType::Laceration);
    w.severity = clamp01(severity);
    w.bleeding = 6.0f + 40.0f * severity * severity;
    w.contamination = 0.45f;
    player_.body.addWound(w);
    char buf[160];
    std::snprintf(buf, sizeof buf, "The %s %s your %s!", by ? lower(by->name).c_str() : "animal",
                  w.type == WoundType::Bite ? "gores" : "tramples", bodyPartName(w.part));
    log(buf, MessageKind::Bad);
}

void Game::updateCombat(float dt, const PlayerInput& in) {
    Player& p = player_;
    p.attackTimer -= dt;
    if (!in.attack || p.attackTimer > 0 || dead() || p.asleep) return;
    if (action_.active) cancelAction();
    Capacity cap = p.body.capacity();
    const ItemInstance* w = weapon();
    Vec2 aimDir = (in.aim - p.pos).normalized();
    if (aimDir.lengthSq() < 0.1f) aimDir = {std::cos(p.facing), std::sin(p.facing)};
    p.facing = std::atan2(aimDir.y, aimDir.x);
    p.lastAttack = 0;
    p.swing = 1.0f;

    if (w && w->def->firearm) {
        int ammo = p.inventory.find([&](const ItemInstance& it) { return it.def->id == w->def->ammo && it.usesLeft > 0; });
        p.attackTimer = w->def->cooldown;
        if (ammo < 0) { log("Click. No cartridges."); return; }
        ItemInstance& a = p.inventory.items()[ammo];
        if (--a.usesLeft <= 0) p.inventory.take(ammo);
        p.noise = w->def->noise;
        // Find the first animal along the line of fire
        Animal* hit = nullptr;
        float bestT = w->def->reach;
        // aim sway from dizziness, cold hands, exhaustion
        float sway = (1.0f - cap.accuracy) * 0.12f + (p.running ? 0.08f : 0.0f);
        float ang = p.facing + rng_.range(-sway, sway);
        Vec2 dir{std::cos(ang), std::sin(ang)};
        for (auto& an : animals_.animals()) {
            if (!an.alive()) continue;
            Vec2 rel = an.pos - p.pos;
            float t = dot(rel, dir);
            if (t < 0 || t > bestT) continue;
            float off = (rel - dir * t).length();
            if (off < an.def->size * 0.6f && world_->lineOfSight(p.pos, an.pos)) { bestT = t; hit = &an; }
        }
        animals_.scare(p.pos, 350.0f);
        sound(Sfx::Gunshot, p.pos);
        log("BANG!");
        if (hit) {
            hit->hp -= w->def->damage * rng_.range(0.6f, 1.2f);
            if (hit->hp <= 0) { hit->state = AnimalState::Dead; log("The " + lower(hit->def->name) + " drops.", MessageKind::Good); }
            else log("You hit the " + lower(hit->def->name) + ". It runs.");
        }
        return;
    }

    float reach = w ? w->def->reach : 0.9f;
    float damage = (w ? w->def->damage : 2.0f) * cap.strength;
    p.attackTimer = w ? w->def->cooldown : 0.7f;
    p.stamina = std::max(0.0f, p.stamina - 0.08f);
    p.noise = w ? w->def->noise : 5.0f;
    sound(Sfx::Swing, p.pos);
    Animal* target = nullptr;
    float best = 1e9f;
    for (auto& an : animals_.animals()) {
        if (!an.alive()) continue;
        Vec2 rel = an.pos - p.pos;
        float d = rel.length();
        if (d > reach + an.def->size * 0.5f + 0.3f) continue;
        if (d > 0.3f && dot(rel * (1.0f / d), aimDir) < 0.35f) continue;
        if (d < best) { best = d; target = &an; }
    }
    if (!target) return;
    if (!rng_.chance(0.8f * cap.accuracy)) { log("You miss."); return; }
    target->hp -= damage * rng_.range(0.7f, 1.3f);
    sound(Sfx::Hit, target->pos);
    if (target->hp <= 0) {
        target->state = AnimalState::Dead;
        log("The " + lower(target->def->name) + " collapses.", MessageKind::Good);
    } else if (target->def->aggression > 0 && rng_.chance(std::min(1.0f, target->def->aggression * 2))) {
        target->state = AnimalState::Charge;
        target->stateTimer = 8;
    } else {
        target->state = AnimalState::Flee;
        target->stateTimer = 8;
    }
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

ItemInstance* Game::resolve(const ItemRef& ref) {
    std::vector<ItemInstance>* list = nullptr;
    switch (ref.loc) {
        case ItemLoc::Player: list = &player_.inventory.items(); break;
        case ItemLoc::Container: list = &world_->container(ref.x, ref.y).items; break;
        case ItemLoc::Ground: list = world_->groundStack(ref.groundKey); break;
        case ItemLoc::Worn:
            for (auto& w : player_.worn)
                if (w.def && w.uid == ref.uid) return &w;
            return nullptr;
    }
    if (!list) return nullptr;
    for (auto& it : *list)
        if (it.uid == ref.uid) return &it;
    return nullptr;
}

ItemInstance Game::takeItem(const ItemRef& ref) {
    std::vector<ItemInstance>* list = nullptr;
    switch (ref.loc) {
        case ItemLoc::Player: list = &player_.inventory.items(); break;
        case ItemLoc::Container: list = &world_->container(ref.x, ref.y).items; break;
        case ItemLoc::Ground: list = world_->groundStack(ref.groundKey); break;
        case ItemLoc::Worn:
            for (auto& w : player_.worn)
                if (w.def && w.uid == ref.uid) { ItemInstance it = w; w = ItemInstance{}; return it; }
            return {};
    }
    if (!list) return {};
    for (size_t i = 0; i < list->size(); ++i)
        if ((*list)[i].uid == ref.uid) {
            ItemInstance it = (*list)[i];
            list->erase(list->begin() + i);
            if (ref.loc == ItemLoc::Player && it.uid == player_.weaponUid) player_.weaponUid = 0;
            return it;
        }
    return {};
}

void Game::giveItem(const ItemInstance& it) {
    player_.inventory.add(it);
}

bool Game::transfer(const ItemRef& ref, ItemLoc to) {
    if (ref.loc == to) return false;
    ItemInstance it = takeItem(ref);
    if (!it.def) return false;
    switch (to) {
        case ItemLoc::Player: player_.inventory.add(it); break;
        case ItemLoc::Container:
            if (openContainer_) world_->container(openX_, openY_).items.push_back(it);
            else world_->dropItem(player_.pos, it);
            break;
        case ItemLoc::Ground: world_->dropItem(player_.pos, it); break;
        case ItemLoc::Worn: player_.inventory.add(it); break;
    }
    return true;
}

int Game::findFuel() const {
    return player_.inventory.findTag("fuel");
}

// ---------------------------------------------------------------------------
// Context actions
// ---------------------------------------------------------------------------

std::vector<ActionOption> Game::actionsAt(Vec2 pt) {
    std::vector<ActionOption> out;
    if (!world_ || dead()) return out;
    int tx = (int)std::floor(pt.x), ty = (int)std::floor(pt.y);
    float dist = distance(player_.pos, Vec2{tx + 0.5f, ty + 0.5f});
    bool near = dist <= kReach + 0.3f;
    const Tile& t = world_->tile(tx, ty);
    int doy = world_->clock().dayOfYear();
    auto add = [&](ActionKind k, const std::string& label, bool enabled = true, const std::string& reason = "") {
        ActionOption o;
        o.kind = k;
        o.label = label;
        o.x = tx;
        o.y = ty;
        o.enabled = enabled && near;
        o.reason = !near ? "Too far away" : reason;
        out.push_back(o);
        return &out.back();
    };

    // Doors
    int dx, dy, ddir;
    if (world_->findDoorNear(pt, 0.9f, dx, dy, ddir)) {
        Vec2 mid = ddir == 0 ? Vec2{dx + 0.5f, (float)dy} : Vec2{(float)dx, dy + 0.5f};
        bool nearDoor = distance(mid, player_.pos) < 2.0f;
        ActionOption o;
        o.kind = ActionKind::ToggleDoor;
        o.label = world_->doorOpen(dx, dy, ddir) ? "Close door" : "Open door";
        o.x = dx; o.y = dy; o.dir = ddir;
        o.enabled = nearDoor;
        o.reason = nearDoor ? "" : "Too far away";
        out.push_back(o);
    }

    // Corpses
    for (auto& a : animals_.animals()) {
        if (a.alive() || a.butchered || distance(a.pos, pt) > std::max(1.0f, a.def->size)) continue;
        bool reach = distance(a.pos, player_.pos) < 2.2f;
        bool knife = player_.inventory.findTag("knife") >= 0 || player_.inventory.findTag("axe") >= 0;
        ActionOption o;
        o.kind = ActionKind::Butcher;
        o.label = "Butcher the " + lower(a.def->name);
        o.targetId = a.id;
        o.enabled = reach && knife;
        o.reason = !reach ? "Too far away" : !knife ? "You need a knife" : "";
        out.push_back(o);
    }

    // Containers
    std::string kind = world_->containerKindAt(tx, ty);
    if (!kind.empty()) {
        const ContainerDef* cd = items_.container(kind);
        std::string name = cd ? cd->name : propName(t.prop);
        auto& c = world_->container(tx, ty);
        if (!c.searched) add(ActionKind::Search, "Search " + lower(name));
        else add(ActionKind::OpenContainer, "Look in " + lower(name) + (c.items.empty() ? " (empty)" : ""));
    }

    // Water
    int wx, wy;
    World::WaterSource src = world_->waterSourceNear(pt, 0.8f, &wx, &wy);
    if (src != World::WaterSource::None) {
        bool reach = distance(Vec2{wx + 0.5f, wy + 0.5f}, player_.pos) < 2.2f;
        ActionOption o;
        o.kind = ActionKind::DrinkSource;
        o.label = std::string("Drink from ") + sourceName(src);
        o.x = wx; o.y = wy; o.enabled = reach; o.reason = reach ? "" : "Too far away";
        out.push_back(o);
        for (const auto& it : player_.inventory.items()) {
            if (it.def->capacityMl <= 0 || it.liquidMl >= it.def->capacityMl - 1) continue;
            ActionOption f = o;
            f.kind = ActionKind::FillBottle;
            f.label = "Fill " + lower(it.def->name) + " from " + sourceName(src);
            f.item = {ItemLoc::Player, 0, 0, 0, it.uid};
            out.push_back(f);
        }
    }

    // Plants
    if (t.prop == Prop::LingonBush || t.prop == Prop::BlueberryBush) {
        bool lingon = t.prop == Prop::LingonBush;
        bool season = lingon ? inSeason(doy, 222, 319) : inSeason(doy, 182, 262);
        bool picked = world_->harvested(tx, ty, 200);
        add(ActionKind::PickBerries, lingon ? "Pick lingonberries" : "Pick blueberries", season && !picked,
            picked ? "Already picked" : "No berries this time of year");
    }
    if (t.prop == Prop::RoseBush) {
        bool picked = world_->harvested(tx, ty, 200);
        add(ActionKind::PickRoseHips, "Pick rose hips", inSeason(doy, 228, 364) && !picked,
            picked ? "Already picked" : "No rose hips yet");
    }
    if (isTree(t.prop)) {
        add(ActionKind::GatherSticks, "Gather dry sticks", !world_->harvested(tx, ty, 4), "Nothing left here for now");
        bool axe = player_.inventory.findTag("axe") >= 0;
        add(ActionKind::ChopTree, std::string("Chop down ") + lower(propName(t.prop)), axe, "You need an axe");
    }
    if (t.prop == Prop::Bed || t.prop == Prop::HospitalBed || t.prop == Prop::Sofa)
        add(ActionKind::Sleep, t.prop == Prop::Sofa ? "Sleep on the sofa" : "Sleep in the bed");

    // Fires
    if (Campfire* f = world_->fireNear(pt, 1.3f, false)) {
        bool reach = distance(f->pos, player_.pos) < 2.4f;
        auto fireOpt = [&](ActionKind k, const std::string& label, bool en, const std::string& why) {
            ActionOption o;
            o.kind = k; o.label = label; o.targetId = f->id;
            o.enabled = en && reach;
            o.reason = !reach ? "Too far away" : why;
            out.push_back(o);
            return &out.back();
        };
        bool igniter = player_.inventory.findTag("igniter") >= 0;
        bool fuel = findFuel() >= 0;
        if (!f->lit) fireOpt(ActionKind::LightFire, "Light the campfire", igniter && (f->fuelMinutes > 0 || fuel),
                             !igniter ? "You need a lighter or matches" : "No fuel");
        fireOpt(ActionKind::AddFuel, "Add fuel to the fire", fuel, "You have nothing to burn");
        if (f->lit) {
            bool pot = player_.inventory.findTag("pot") >= 0;
            for (const auto& it : player_.inventory.items()) {
                if (it.def->cookedInto.empty()) continue;
                bool needsPot = it.def->id == "oats" || it.def->id == "potato_raw";
                auto* o = fireOpt(ActionKind::CookItem, "Cook " + lower(itemName(it)), !needsPot || pot, "You need a pot");
                o->item = {ItemLoc::Player, 0, 0, 0, it.uid};
            }
            bool dirty = player_.inventory.find([](const ItemInstance& i) { return i.def->capacityMl > 0 && i.liquidMl > 0 && !i.liquidSafe; }) >= 0;
            if (dirty) fireOpt(ActionKind::BoilWater, "Boil water", pot, "You need a pot");
        }
    }

    // Snares
    for (auto& s : world_->snares()) {
        if (distance(s.pos, pt) > 1.0f) continue;
        ActionOption o;
        o.kind = ActionKind::TakeSnare;
        o.label = "Take up the snare";
        o.targetId = s.id;
        o.enabled = distance(s.pos, player_.pos) < 2.2f;
        o.reason = o.enabled ? "" : "Too far away";
        out.push_back(o);
    }

    // Open ground outdoors
    bool open = t.building < 0 && t.prop == Prop::None && groundWalkable(t.ground) && t.ground != Ground::WaterShallow &&
                t.ground != Ground::Asphalt;
    if (open) {
        Zone z = map_.zoneAt(tx + 0.5f, ty + 0.5f);
        bool wild = z == Zone::Forest || z == Zone::Woodland;
        if (wild) {
            bool season = inSeason(doy, 182, 320);
            bool done = world_->harvested(tx & ~7, ty & ~7, 10);
            add(ActionKind::Forage, "Forage for mushrooms", season && !done,
                done ? "You've searched this area recently" : "Nothing grows this time of year");
        }
        int fuelCount = 0;
        for (const auto& it : player_.inventory.items())
            if (it.def->hasTag("fuel")) ++fuelCount;
        if (!world_->fireNear(pt, 3.0f, false))
            add(ActionKind::BuildFire, "Build a campfire here", fuelCount >= 2, "You need at least two pieces of wood or sticks");
        bool wire = player_.inventory.findTag("snare") >= 0;
        if (z != Zone::Residential && z != Zone::Commercial)
            add(ActionKind::SetSnare, "Set a snare here", wire, "You need snare wire");
    }

    if (out.empty() || (t.building < 0 && t.prop == Prop::None)) {
        add(ActionKind::Rest, "Rest for an hour");
        if (t.building >= 0 || t.ground != Ground::WaterShallow) add(ActionKind::Sleep, "Sleep here");
    }
    return out;
}

std::vector<ActionOption> Game::actionsForItem(const ItemRef& ref) {
    std::vector<ActionOption> out;
    ItemInstance* it = resolve(ref);
    if (!it || !it->def || dead()) return out;
    const ItemDef& d = *it->def;
    auto add = [&](ActionKind k, const std::string& label, float amount = 1, bool enabled = true, const std::string& reason = "") {
        ActionOption o;
        o.kind = k; o.label = label; o.item = ref; o.amount = amount; o.enabled = enabled; o.reason = reason;
        out.push_back(o);
    };
    bool mine = ref.loc == ItemLoc::Player;

    if (ref.loc == ItemLoc::Container || ref.loc == ItemLoc::Ground) add(ActionKind::PickUp, "Take");

    if (d.needsCanOpener && !it->opened) {
        bool opener = player_.inventory.findTag("can_opener") >= 0;
        bool knife = player_.inventory.findTag("knife") >= 0;
        add(ActionKind::OpenCan, "Open with a can opener", 1, opener, "You need a can opener");
        if (knife) add(ActionKind::OpenCan, "Pry open with a knife", 0);
    } else if (d.isEdible() && d.capacityMl <= 0) {
        if (d.category == ItemCategory::Drink) {
            add(ActionKind::Drink, "Drink");
        } else {
            add(ActionKind::Eat, "Eat all", 1.0f);
            if (it->portion > 0.5f) add(ActionKind::Eat, "Eat half", 0.5f);
            if (it->portion > 0.25f) add(ActionKind::Eat, "Eat a quarter", 0.25f);
        }
    }
    if (d.capacityMl > 0 && it->liquidMl > 0) {
        add(ActionKind::Drink, "Drink");
        bool tablet = player_.inventory.findTag("purify") >= 0;
        if (!it->liquidSafe && tablet) add(ActionKind::Purify, "Purify with a tablet");
        add(ActionKind::PourOut, "Pour out");
    }
    if (mine && (d.category == ItemCategory::Weapon || d.hasTag("knife") || d.hasTag("axe"))) {
        if (player_.weaponUid == it->uid) add(ActionKind::Unequip, "Put away");
        else add(ActionKind::Equip, "Hold as weapon");
    }
    if (d.category == ItemCategory::Clothing) {
        if (ref.loc == ItemLoc::Worn) add(ActionKind::TakeOff, "Take off");
        else if (mine) {
            add(ActionKind::Wear, "Wear");
            if (d.slot != ClothingSlot::Back && d.slot != ClothingSlot::Feet) add(ActionKind::TearIntoRags, "Tear into strips");
        }
    }
    if (mine && d.hasTag("bandage")) {
        int idx = -1;
        const auto& wounds = player_.body.state().wounds;
        for (int i = 0; i < (int)wounds.size(); ++i)
            if (!wounds[i].bandaged || wounds[i].bandageDirt > 0.6f) { idx = i; break; }
        add(ActionKind::Bandage, "Bandage a wound", 1, idx >= 0, "No wounds need a bandage");
    }
    if (mine && d.hasTag("disinfectant")) {
        bool any = false;
        for (const auto& w : player_.body.state().wounds) any |= !w.disinfected;
        add(ActionKind::Disinfect, "Clean a wound", 1, any, "No wounds to clean");
    }
    if (mine && (d.hasTag("painkiller") || d.hasTag("antibiotic") || d.treats != Pathogen::None || d.vitaminCMg > 0))
        add(ActionKind::TakePill, "Take one");
    if (mine && d.hasTag("light")) add(ActionKind::Equip, player_.flashlightOn ? "Switch off" : "Switch on", 2);
    if (mine && d.category == ItemCategory::Literature) add(ActionKind::Read, "Read");
    if (mine) add(ActionKind::Drop, "Drop");
    return out;
}

// ---------------------------------------------------------------------------
// Performing actions
// ---------------------------------------------------------------------------

void Game::startTimed(const ActionOption& o, float seconds, float timeScale) {
    action_.option = o;
    action_.duration = seconds;
    action_.elapsed = 0;
    action_.active = true;
    action_.timeScale = timeScale;
}

void Game::cancelAction() {
    if (action_.active && action_.option.kind == ActionKind::Sleep) {
        player_.asleep = false;
        player_.body.setAsleep(false);
    }
    action_.active = false;
}

void Game::perform(const ActionOption& o) {
    if (!o.enabled || dead()) return;
    if (action_.active) cancelAction();
    ItemInstance* it = (o.item.uid != 0) ? resolve(o.item) : nullptr;
    switch (o.kind) {
        // instant ------------------------------------------------------------
        case ActionKind::ToggleDoor:
            world_->toggleDoor(o.x, o.y, o.dir);
            sound(Sfx::Door, Vec2{(float)o.x, (float)o.y});
            return;
        case ActionKind::OpenContainer:
            openContainer_ = true; openX_ = o.x; openY_ = o.y;
            return;
        case ActionKind::PickUp:
            sound(Sfx::Click, player_.pos);
            transfer(o.item, ItemLoc::Player);
            return;
        case ActionKind::Drop:
            transfer(o.item, ItemLoc::Ground);
            return;
        case ActionKind::Equip:
            if (it && o.amount == 2) {   // flashlight toggle
                player_.flashlightOn = !player_.flashlightOn;
                return;
            }
            if (it) { player_.weaponUid = it->uid; log("You hold the " + lower(it->def->name) + " ready."); }
            return;
        case ActionKind::Unequip:
            player_.weaponUid = 0;
            return;
        case ActionKind::Wear: {
            if (!it) return;
            ItemInstance item = takeItem(o.item);
            ItemInstance& slot = player_.worn[(size_t)item.def->slot];
            if (slot.def) player_.inventory.add(slot);
            slot = item;
            return;
        }
        case ActionKind::TakeOff: {
            ItemInstance item = takeItem(o.item);
            if (item.def) player_.inventory.add(item);
            return;
        }
        case ActionKind::PourOut:
            if (it) { it->liquidMl = 0; it->liquidSafe = true; }
            return;
        case ActionKind::Read:
            if (it && it->def->hasTag("field_guide"))
                log("You leaf through the pictures. With this book you can tell edible mushrooms from deadly ones.", MessageKind::Good);
            return;
        // timed ---------------------------------------------------------------
        case ActionKind::Search: {
            const ContainerDef* cd = items_.container(world_->containerKindAt(o.x, o.y));
            startTimed(o, cd ? cd->searchSeconds : 2.0f);
            return;
        }
        case ActionKind::Sleep:
            player_.asleep = true;
            player_.body.setAsleep(true);
            startTimed(o, 1e9f, 90.0f);
            log("You lie down and close your eyes.");
            return;
        case ActionKind::Rest: startTimed(o, 3.0f, 20.0f); return;
        case ActionKind::Wait: startTimed(o, 3.0f, 20.0f); return;
        case ActionKind::DrinkSource: startTimed(o, 2.5f); return;
        case ActionKind::FillBottle: startTimed(o, 3.0f); return;
        case ActionKind::PickBerries: case ActionKind::PickRoseHips: startTimed(o, 6.0f); return;
        case ActionKind::Forage: startTimed(o, 12.0f); return;
        case ActionKind::GatherSticks: startTimed(o, 7.0f); return;
        case ActionKind::ChopTree: startTimed(o, 15.0f); return;
        case ActionKind::BuildFire: startTimed(o, 8.0f); return;
        case ActionKind::LightFire: startTimed(o, 4.0f); return;
        case ActionKind::AddFuel: startTimed(o, 1.5f); return;
        case ActionKind::CookItem: startTimed(o, 10.0f); return;
        case ActionKind::BoilWater: startTimed(o, 12.0f); return;
        case ActionKind::SetSnare: startTimed(o, 6.0f); return;
        case ActionKind::TakeSnare: startTimed(o, 3.0f); return;
        case ActionKind::Butcher: {
            Animal* a = animals_.find(o.targetId);
            startTimed(o, a ? a->def->butcherSeconds : 20.0f);
            return;
        }
        case ActionKind::Eat: startTimed(o, 3.0f + 4.0f * o.amount); return;
        case ActionKind::Drink: startTimed(o, 2.5f); return;
        case ActionKind::OpenCan: startTimed(o, o.amount > 0 ? 3.0f : 6.0f); return;
        case ActionKind::Bandage: startTimed(o, 4.0f); return;
        case ActionKind::Disinfect: startTimed(o, 3.0f); return;
        case ActionKind::TakePill: startTimed(o, 1.5f); return;
        case ActionKind::Purify: startTimed(o, 2.0f); return;
        case ActionKind::TearIntoRags: startTimed(o, 5.0f); return;
        default: return;
    }
}

void Game::drinkWater(float ml, bool safe, const char* sourceDesc) {
    BodySignals g = player_.body.signals();
    if (g.nausea > 0.85f) { log("You can't keep anything down.", MessageKind::Bad); return; }
    Nutrients n;
    n.water = ml;
    player_.body.ingest(n, 0);
    if (!safe) {
        if (rng_.chance(0.12f)) player_.body.expose(Pathogen::Giardia, rng_.range(0.3f, 0.8f));
        if (rng_.chance(0.05f)) player_.body.expose(Pathogen::FoodPoisoning, rng_.range(0.2f, 0.6f));
    }
    if (sourceDesc) log(std::string("You drink from ") + sourceDesc + ".");
}

void Game::eat(ItemInstance& it, float fraction) {
    const ItemDef& d = *it.def;
    BodySignals g = player_.body.signals();
    if (g.nausea > 0.85f) { log("Your stomach heaves at the thought. You can't eat.", MessageKind::Bad); return; }
    if (player_.body.fullness() > 2.4f) { log("You can't force any more down.", MessageKind::Bad); return; }
    float amount = std::min(fraction, it.portion);
    Nutrients n = d.nutrients.scaled(amount);
    float spoil = it.spoilage();
    player_.body.ingest(n, d.solidMl * amount);
    // Germs
    if (it.carriesPathogen) player_.body.expose(d.pathogen, d.pathogenDose * std::max(0.4f, amount));
    if (d.raw && rng_.chance(0.2f * amount + 0.05f)) player_.body.expose(Pathogen::FoodPoisoning, 0.4f + 0.3f * amount);
    if (spoil > 0.6f && rng_.chance((spoil - 0.5f) * 0.9f))
        player_.body.expose(Pathogen::FoodPoisoning, std::min(1.0f, spoil * 0.7f));
    if (d.toxin != Toxin::None) player_.body.ingestToxin(d.toxin, d.toxinAmount * amount);
    float yuck = d.nauseaOnEat + (spoil >= 1.0f ? 0.35f : 0.0f);
    if (yuck > 0) player_.body.mutableState().nauseaExtra = std::min(1.0f, player_.body.state().nauseaExtra + yuck * amount);

    std::string name = lower(itemName(it));
    std::string msg = amount >= it.portion - 0.01f ? "You finish the " + name + "." : "You eat some of the " + name + ".";
    if (spoil >= 1.0f) msg += " It tastes rotten.";
    else if (spoil > 0.6f) msg += " It tastes a bit off.";
    else if (d.raw) msg += " It's raw.";
    if (d.id == "surstromming") msg += " The smell is indescribable.";
    log(msg);
    it.portion -= amount;
}

void Game::drinkFrom(ItemInstance& it) {
    if (it.def->capacityMl > 0) {
        float ml = std::min(250.0f, it.liquidMl);
        it.liquidMl -= ml;
        drinkWater(ml, it.liquidSafe, nullptr);
        log(it.liquidSafe ? "You drink some water." : "You drink the untreated water.");
        if (it.liquidMl <= 0) it.liquidSafe = true;
        return;
    }
    // Cartons and cans: 250 ml sips
    float grams = it.def->nutrients.water + it.def->nutrients.carbs + it.def->nutrients.protein + it.def->nutrients.fat;
    float frac = grams > 0 ? std::min(it.portion, 250.0f / grams) : it.portion;
    eat(it, frac);
}

void Game::finishAction() {
    action_.active = false;
    const ActionOption& o = action_.option;
    ItemInstance* it = (o.item.uid != 0) ? resolve(o.item) : nullptr;
    auto removeIfUsedUp = [&](const ItemRef& ref) {
        ItemInstance* i = resolve(ref);
        if (!i) return;
        bool empty = (i->def->isEdible() && i->def->capacityMl <= 0 && i->portion <= 0.01f) ||
                     (i->def->uses > 0 && i->usesLeft <= 0) ;
        if (empty) takeItem(ref);
    };
    auto useOne = [&](int index) {
        if (index < 0) return;
        ItemInstance& u = player_.inventory.items()[index];
        if (u.def->uses > 0) {
            if (--u.usesLeft <= 0) player_.inventory.take(index);
        } else {
            player_.inventory.take(index);
        }
    };

    switch (o.kind) {
        case ActionKind::Search: {
            ContainerState& c = world_->container(o.x, o.y);
            c.searched = true;
            openContainer_ = true; openX_ = o.x; openY_ = o.y;
            const ContainerDef* cd = items_.container(c.kind);
            std::string name = cd ? lower(cd->name) : "container";
            if (c.items.empty()) log("You search the " + name + ". Nothing useful.");
            else log("You search the " + name + " and find " + std::to_string(c.items.size()) + (c.items.size() == 1 ? " thing." : " things."), MessageKind::Good);
            break;
        }
        case ActionKind::Sleep:
            player_.asleep = false;
            player_.body.setAsleep(false);
            log("You wake up.");
            break;
        case ActionKind::Rest: case ActionKind::Wait:
            break;
        case ActionKind::DrinkSource: {
            const Tile& t = world_->tile(o.x, o.y);
            if (t.ground == Ground::WaterShallow || t.ground == Ground::WaterDeep) drinkWater(300, false, "the lake");
            else {
                float got = world_->takeTankWater(o.x, o.y, 300);
                bool barrel = t.prop == Prop::RainBarrel;
                if (got > 0) {
                    drinkWater(got, !barrel || rng_.chance(0.8f), barrel ? "the rain barrel" : "the toilet tank");
                } else log("It's empty.");
            }
            break;
        }
        case ActionKind::FillBottle: {
            if (!it) break;
            const Tile& t = world_->tile(o.x, o.y);
            float want = it->def->capacityMl - it->liquidMl;
            bool lake = t.ground == Ground::WaterShallow || t.ground == Ground::WaterDeep;
            float got = lake ? want : world_->takeTankWater(o.x, o.y, want);
            bool safe = !lake && t.prop != Prop::RainBarrel;
            if (got <= 0) { log("There's no water left in it."); break; }
            it->liquidSafe = it->liquidMl > 0 ? (it->liquidSafe && safe) : safe;
            it->liquidMl += got;
            log("You fill the " + lower(it->def->name) + ".");
            break;
        }
        case ActionKind::PickBerries: {
            const Tile& t = world_->tile(o.x, o.y);
            bool lingon = t.prop == Prop::LingonBush;
            int n = rng_.rangeInt(1, 3);
            for (int i = 0; i < n; ++i) giveItem(items_.create(lingon ? "lingonberries" : "blueberries", rng_));
            world_->markHarvested(o.x, o.y);
            log("You pick " + std::to_string(n) + (n == 1 ? " handful of " : " handfuls of ") + (lingon ? "lingonberries." : "blueberries."), MessageKind::Good);
            break;
        }
        case ActionKind::PickRoseHips: {
            int n = rng_.rangeInt(1, 2);
            for (int i = 0; i < n; ++i) giveItem(items_.create("rosehips", rng_));
            world_->markHarvested(o.x, o.y);
            log("You pick rose hips, avoiding the thorns.", MessageKind::Good);
            break;
        }
        case ActionKind::Forage: {
            world_->markHarvested(o.x & ~7, o.y & ~7);
            float r = rng_.uniform();
            const char* found = nullptr;
            if (r < 0.28f) found = "chanterelles";
            else if (r < 0.40f) found = "pale_mushroom_safe";
            else if (r < 0.50f) found = "pale_mushroom_deadly";
            else if (r < 0.64f) found = "fly_agaric";
            if (found) {
                ItemInstance m = items_.create(found, rng_);
                giveItem(m);
                log("You find some " + lower(itemName(m)) + ".", MessageKind::Good);
            } else {
                log("You search the forest floor but find nothing edible.");
            }
            break;
        }
        case ActionKind::GatherSticks: {
            int n = rng_.rangeInt(1, 2);
            for (int i = 0; i < n; ++i) giveItem(items_.create("sticks", rng_));
            world_->markHarvested(o.x, o.y);
            log("You gather some dry sticks.");
            break;
        }
        case ActionKind::ChopTree: {
            sound(Sfx::Chop, player_.pos);
            world_->setProp(o.x, o.y, Prop::Stump);
            int n = rng_.rangeInt(2, 4);
            for (int i = 0; i < n; ++i) world_->dropItem(Vec2{o.x + 0.5f, o.y + 0.5f}, items_.create("firewood", rng_));
            log("The tree comes down. You split it into firewood.", MessageKind::Good);
            break;
        }
        case ActionKind::BuildFire: {
            int used = 0;
            for (int i = (int)player_.inventory.size() - 1; i >= 0 && used < 2; --i)
                if (player_.inventory.items()[i].def->hasTag("fuel")) {
                    float minutes = player_.inventory.items()[i].def->id == "firewood" ? 90.0f : 25.0f;
                    player_.inventory.take(i);
                    ++used;
                    Campfire f;
                    if (used == 1) {
                        f.pos = Vec2{o.x + 0.5f, o.y + 0.5f};
                        f.id = world_->newId();
                        f.fuelMinutes = minutes;
                        world_->campfires().push_back(f);
                    } else {
                        world_->campfires().back().fuelMinutes += minutes;
                    }
                }
            log("You stack wood for a campfire.");
            break;
        }
        case ActionKind::LightFire: {
            for (auto& f : world_->campfires()) {
                if (f.id != o.targetId) continue;
                if (f.fuelMinutes <= 0) {
                    int fuel = findFuel();
                    if (fuel < 0) break;
                    f.fuelMinutes += player_.inventory.items()[fuel].def->id == "firewood" ? 90.0f : 25.0f;
                    player_.inventory.take(fuel);
                }
                float rain = world_->clock().weather().rain;
                int ig = player_.inventory.findTag("igniter");
                useOne(ig);
                if (rng_.chance(0.85f - rain * 0.5f)) { f.lit = true; log("The fire catches.", MessageKind::Good); }
                else log("The kindling smokes but won't catch.", MessageKind::Bad);
            }
            break;
        }
        case ActionKind::AddFuel: {
            int fuel = findFuel();
            if (fuel < 0) break;
            float minutes = player_.inventory.items()[fuel].def->id == "firewood" ? 90.0f : 25.0f;
            for (auto& f : world_->campfires())
                if (f.id == o.targetId) f.fuelMinutes += minutes;
            player_.inventory.take(fuel);
            log("You feed the fire.");
            break;
        }
        case ActionKind::CookItem: {
            if (!it) break;
            const ItemDef* cooked = items_.find(it->def->cookedInto);
            if (!cooked) break;
            if (it->def->id == "oats") {
                // porridge takes a litre of water from your bottles
                float need = 1000;
                for (auto& b : player_.inventory.items())
                    if (b.def->capacityMl > 0 && need > 0) { float t = std::min(need, b.liquidMl); b.liquidMl -= t; need -= t; }
                if (need > 0) { log("You need a litre of water to make porridge.", MessageKind::Bad); break; }
            }
            std::string before = lower(itemName(*it));
            float portion = it->portion;
            it->def = cooked;
            it->portion = portion;
            it->ageHours = 0;
            it->carriesPathogen = false;
            log("You cook the " + before + ".", MessageKind::Good);
            break;
        }
        case ActionKind::BoilWater: {
            for (auto& b : player_.inventory.items())
                if (b.def->capacityMl > 0) b.liquidSafe = true;
            log("You boil your water. It's safe to drink now.", MessageKind::Good);
            break;
        }
        case ActionKind::SetSnare: {
            int w = player_.inventory.findTag("snare");
            if (w < 0) break;
            player_.inventory.take(w);
            Snare s;
            s.pos = Vec2{o.x + 0.5f, o.y + 0.5f};
            s.id = world_->newId();
            world_->snares().push_back(s);
            log("You set a snare. Check back on it later.");
            break;
        }
        case ActionKind::TakeSnare: {
            auto& snares = world_->snares();
            for (size_t i = 0; i < snares.size(); ++i)
                if (snares[i].id == o.targetId) { snares.erase(snares.begin() + i); break; }
            giveItem(items_.create("snare_wire", rng_));
            log("You take up the snare.");
            break;
        }
        case ActionKind::Butcher: {
            Animal* a = animals_.find(o.targetId);
            if (!a || a->butchered) break;
            a->butchered = true;
            for (const auto& y : a->def->yields)
                for (int i = 0; i < y.second; ++i) {
                    ItemInstance m = items_.create(y.first, rng_);
                    m.ageHours = a->deadMinutes / 60.0f;
                    world_->dropItem(a->pos, m);
                }
            bool gloves = player_.worn[(size_t)ClothingSlot::Hands].def != nullptr;
            if (a->def->id == "hare" && !gloves && rng_.chance(0.08f)) player_.body.expose(Pathogen::Tularemia, 0.5f);
            // Reset any snare that caught it
            for (auto& s : world_->snares())
                if (s.caught && distance(s.pos, a->pos) < 1.0f) { s.caught = false; s.minutesSet = 0; }
            log("You butcher the " + lower(a->def->name) + ". The meat lies on the ground beside you.", MessageKind::Good);
            break;
        }
        case ActionKind::Eat:
            sound(Sfx::Eat, player_.pos);
            if (it) { eat(*it, o.amount); removeIfUsedUp(o.item); }
            break;
        case ActionKind::Drink:
            sound(Sfx::Drink, player_.pos);
            if (it) { drinkFrom(*it); removeIfUsedUp(o.item); }
            break;
        case ActionKind::OpenCan:
            if (it) {
                it->opened = true;
                if (o.amount <= 0 && rng_.chance(0.25f)) {
                    Wound w;
                    w.part = BodyPart::LeftHand;
                    w.type = WoundType::Laceration;
                    w.severity = 0.2f;
                    w.bleeding = 4.0f;
                    w.contamination = 0.3f;
                    player_.body.addWound(w);
                    log("The knife slips and slices your hand.", MessageKind::Bad);
                } else {
                    log("You open the " + lower(it->def->name) + ".");
                }
            }
            break;
        case ActionKind::Bandage: {
            if (!it) break;
            const auto& wounds = player_.body.state().wounds;
            int idx = -1;
            float worst = -1;
            for (int i = 0; i < (int)wounds.size(); ++i)
                if ((!wounds[i].bandaged || wounds[i].bandageDirt > 0.6f) && wounds[i].bleeding + wounds[i].severity > worst) {
                    worst = wounds[i].bleeding + wounds[i].severity;
                    idx = i;
                }
            if (idx < 0) break;
            bool sterile = it->def->hasTag("sterile");
            player_.body.bandageWound(idx, sterile);
            log(std::string("You bandage the wound on your ") + bodyPartName(wounds[idx].part) + ".");
            takeItem(o.item);
            break;
        }
        case ActionKind::Disinfect: {
            if (!it) break;
            const auto& wounds = player_.body.state().wounds;
            for (int i = 0; i < (int)wounds.size(); ++i)
                if (!wounds[i].disinfected) { player_.body.disinfectWound(i); break; }
            log("You clean the wound. It stings.");
            if (--it->usesLeft <= 0) takeItem(o.item);
            break;
        }
        case ActionKind::TakePill: {
            if (!it) break;
            const ItemDef& d = *it->def;
            if (d.hasTag("painkiller")) player_.body.takePainkiller();
            if (d.treats != Pathogen::None) player_.body.takeAntibiotic(d.treats);
            if (d.hasTag("treats_wound_infection")) player_.body.takeAntibiotic(Pathogen::WoundInfection);
            if (d.hasTag("treats_food_poisoning")) player_.body.takeAntibiotic(Pathogen::FoodPoisoning);
            if (d.vitaminCMg > 0) player_.body.takeVitaminC(d.vitaminCMg);
            log("You swallow a " + std::string(d.vitaminCMg > 0 ? "tablet." : "pill."));
            if (--it->usesLeft <= 0) takeItem(o.item);
            break;
        }
        case ActionKind::Purify: {
            if (!it) break;
            int tab = player_.inventory.findTag("purify");
            if (tab < 0) break;
            it->liquidSafe = true;
            uint32_t uid = it->uid;
            useOne(tab);
            (void)uid;
            log("You drop a tablet in. The water tastes of chlorine, but it's safe.", MessageKind::Good);
            break;
        }
        case ActionKind::TearIntoRags: {
            if (!it) break;
            takeItem(o.item);
            int n = rng_.rangeInt(2, 4);
            for (int i = 0; i < n; ++i) giveItem(items_.create("rag", rng_));
            log("You tear it into strips of cloth.");
            break;
        }
        default: break;
    }
}

} // namespace alone
