#include "game/Game.h"
#include "core/Log.h"
#include "core/Paths.h"
#include <algorithm>
#include <set>
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
    loadSurvivors(Paths::data("survivors/survivors.ini"));   // optional: the town can be empty
    loadDialogue(Paths::data("text/dialogue.ini"));          // optional too
    return ok;
}

CharacterChoice CharacterChoice::defaults() {
    CharacterChoice c;
    c.wear[(size_t)ClothingSlot::Torso] = "tshirt";
    c.wear[(size_t)ClothingSlot::Sweater] = "fleece";
    c.wear[(size_t)ClothingSlot::Legs] = "jeans";
    c.wear[(size_t)ClothingSlot::Feet] = "sneakers";
    return c;
}

void Game::dressPlayer(const CharacterChoice& who) {
    player_.look = who.look;
    if (player_.look.name.empty()) player_.look.name = "Alex";
    for (size_t sl = 1; sl < who.wear.size(); ++sl) {
        if (sl == (size_t)ClothingSlot::Back) continue;    // a backpack is found, not chosen
        player_.worn[sl] = ItemInstance{};
        if (who.wear[sl].empty()) continue;
        ItemInstance it = items_.create(who.wear[sl], rng_);
        if (!it.def || it.def->slot != (ClothingSlot)sl) continue;
        it.tint = who.tint[sl];
        player_.worn[sl] = it;
    }
}

void Game::newGame(uint32_t seed, const CharacterChoice* who) {
    seed_ = seed;
    world_ = std::make_unique<World>(map_, items_, seed);
    rng_.reseed(seed * 7919ULL + 17);
    player_ = Player{};
    player_.body = Body(seed + 99);
    animals_.animals().clear();
    vehicles_.clear();
    lockedCars_.clear();
    propFuel_.clear();
    pumpFuel_.clear();
    nextVehicleId_ = 1;
    placeCache_.clear();
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

    dressPlayer(who ? *who : CharacterChoice::defaults());
    ItemInstance bottle = items_.create("water_bottle", rng_);
    bottle.liquidMl = 600;
    player_.inventory.add(bottle);
    ItemInstance bread = items_.create("crispbread", rng_);
    bread.portion = 0.4f;
    player_.inventory.add(bread);

    placeSurvivors();
    log("It has been eleven days since you last saw another person.", MessageKind::Info);
    log("But last night there was candlelight in a window a few streets away. Someone else is alive.", MessageKind::Good);
    log("Right-click things to interact. Tab: inventory, J: journal, M: map.", MessageKind::Info);
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
                else if (!r->label.empty() && r->type != RoomType::Hall)
                    what = r->label + " \xE2\x80\x93 " + what;   // "Kläder – Gallerian Storken"
            }
            return what + ", " + district;
        }
    }
    std::string street = map_.streetAt(player_.pos);
    if (!street.empty() && street != district) return street + ", " + district;
    return district;
}

float Game::threat() const {
    float t = 0;
    for (const auto& a : const_cast<AnimalSystem&>(animals_).animals()) {
        if (!a.alive()) continue;
        if (a.state != AnimalState::Charge && a.state != AnimalState::Recover) continue;
        float d = distance(a.pos, player_.pos);
        t = std::max(t, std::clamp(1.0f - (d - 15.0f) / 30.0f, 0.0f, 1.0f));
    }
    for (const auto& s : survivors_) {
        if (!s.alive() || s.state != SurvivorState::Hunt || !s.hostileTo()) continue;
        float d = distance(s.pos, player_.pos);
        t = std::max(t, std::clamp(1.0f - (d - 20.0f) / 40.0f, 0.0f, 1.0f));
    }
    return t;
}

bool Game::knowsMushrooms() const {
    return player_.knowsMushroomsTaught || player_.inventory.findTag("field_guide") >= 0;
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

void Game::checkTrip(float dt, float grade, float limp) {
    Player& p = player_;
    float steep = std::fabs(grade);
    if (steep < 0.2f || p.sneaking || p.fallen > 0) return;   // careful steps don't slip
    float risk = (steep - 0.2f) * (p.running ? 1.2f : 0.12f);
    if (grade < 0) risk *= 1.6f;                              // downhill is worse
    if (!world_->indoors(p.pos)) risk *= 1.0f + 0.8f * world_->groundWetness() + 0.6f * world_->snowCover();
    if (p.stamina < 0.25f) risk *= 1.5f;                      // tired legs
    if (world_->clock().daylight() < 0.15f && !p.flashlightOn) risk *= 1.4f;   // can't see your feet
    risk *= 1.0f + limp;
    if (!rng_.chance(risk * dt)) return;
    p.fallen = p.running ? 1.8f : 1.2f;
    p.running = false;
    p.stamina = std::max(0.0f, p.stamina - 0.2f);
    cancelAction();
    sound(Sfx::Hurt, p.pos);
    bool slippery = world_->groundWetness() > 0.5f || world_->snowCover() > 0.3f;
    if (rng_.chance(p.running ? 0.75f : 0.5f)) {
        // scraped hands or knees on the stones
        static const BodyPart parts[] = {BodyPart::LeftHand, BodyPart::RightHand, BodyPart::LeftLeg, BodyPart::RightLeg};
        Wound w;
        w.part = parts[rng_.rangeInt(0, 3)];
        w.type = WoundType::Scratch;
        w.severity = clamp01(rng_.range(0.06f, 0.2f) * (1.0f + steep));
        w.bleeding = 1.0f + 6.0f * w.severity;
        w.contamination = 0.55f;   // dirt in it
        p.body.addWound(w);
        log(std::string(slippery ? "You slip on the steep, wet ground and fall. " : "You trip on the steep slope and fall. ") +
                "Your " + lower(bodyPartName(w.part)) + " is scraped.", MessageKind::Bad);
    } else {
        log(slippery ? "You slip on the steep, wet ground and fall." : "You trip on the steep slope and fall.", MessageKind::Bad);
    }
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
    // In a car: out of the wind and rain (unless you smashed the window), and the heater works
    if (const Vehicle* v = playerVehicle(); v && v->kind == VehicleKind::Car) {
        env.wind *= v->windowSmashed ? 0.4f : 0.0f;
        env.wetness *= v->windowSmashed ? 0.3f : 0.0f;
        env.airTempC += v->running ? 12.0f : 2.0f;
    }
    // A tent keeps the wind and rain off and holds some warmth
    if (world_->tentAt(player_.pos)) {
        env.wind = 0;
        env.wetness = 0;
        env.airTempC += 4.0f;
    }
    // A sleeping bag and a mat, wherever you sleep
    if (player_.asleep) {
        if (player_.inventory.findTag("sleeping_bag") >= 0) env.clothingClo += 2.5f;
        if (player_.inventory.findId("sleeping_mat") >= 0) env.clothingClo += 0.4f;
    }
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
        env.sleepComfort = sleepComfortIn(env).score;
    }
    return env;
}

const char* SleepComfort::word() const {
    return score >= 0.45f ? "Very comfortable" : score >= 0.15f ? "Comfortable" : score > -0.15f ? "Bearable"
         : score > -0.45f ? "Uncomfortable" : "Miserable";
}

SleepComfort Game::sleepComfort() { return sleepComfortIn(environment()); }

SleepComfort Game::sleepComfortIn(const Environment& env) {
    SleepComfort c;
    auto add = [&](float v, const std::string& why) {
        c.score += v;
        (v >= 0 ? c.good : c.bad).push_back(why);
    };
    const Player& p = player_;
    int tx = (int)std::floor(p.pos.x), ty = (int)std::floor(p.pos.y);
    const Vehicle* v = playerVehicle();
    bool inCar = v && v->kind == VehicleKind::Car;
    bool tent = world_->tentAt(p.pos) != nullptr;
    bool inside = world_->indoors(p.pos);
    bool bed = false, sofa = false;
    if (!inCar)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                Prop pr = world_->tile(tx + dx, ty + dy).prop;
                bed |= pr == Prop::Bed || pr == Prop::HospitalBed;
                sofa |= pr == Prop::Sofa;
            }
    // What you lie on
    if (inCar) {
        add(-0.1f, "a cramped car seat");
        double since = sleepStartMin_ >= 0 && p.asleep ? (world_->clock().totalMinutes() - sleepStartMin_) / 60.0 : 0.0;
        if (since > 2.0) add(-std::min(0.45f, (float)(since - 2.0) * 0.08f), "hours curled up in the car");
    } else if (bed) add(0.45f, "a proper bed");
    else if (sofa) add(0.25f, "a soft sofa");
    else if (tent) add(-0.1f, "hard ground under the tent");
    else if (inside) add(-0.15f, "a hard floor");
    else add(-0.3f, "the bare ground");
    if (tentMate_) {
        if (Survivor* m = survivor(tentMate_); m && m->alive() && distance(m->pos, p.pos) < 2.0f)
            add(m->bond() == Bond::Lover ? 0.25f : 0.08f, m->bond() == Bond::Lover ? m->def->name + " beside you" : "company in the tent");
    }
    bool soft = bed || sofa;
    bool bag = p.inventory.findTag("sleeping_bag") >= 0;
    if (p.inventory.findTag("pillow") >= 0) add(soft ? 0.05f : 0.2f, "your pillow");
    if (bag && !soft) add(0.15f, "your sleeping bag");
    if (!soft && !inCar && p.inventory.findId("sleeping_mat") >= 0) add(0.15f, "a sleeping mat under you");
    // Wet clothes, rain, cold and heat
    float wet = p.body.state().wetness;
    if (wet > 0.15f) add(-0.5f * wet, wet > 0.6f ? "soaking wet clothes" : "damp clothes");
    bool sheltered = inside || inCar || tent;
    if (!sheltered && env.wetness > 0.2f) add(-0.3f, "rain falling on you");
    float air = env.airTempC;
    if (air > 27.0f) add(-0.2f, "too hot");
    else if (sheltered && air >= 16.0f) add(0.15f, "warm and out of the weather");
    else if (air < 8.0f && !bag) add(-0.2f, "cold air");
    c.score = std::clamp(c.score, -1.0f, 1.0f);
    return c;
}

const ItemInstance* Game::stowedWeapon() const {
    int i = player_.inventory.indexOfUid(player_.stowedUid);
    return i >= 0 ? &player_.inventory.items()[(size_t)i] : nullptr;
}

void Game::toggleStow() {
    Player& p = player_;
    if (p.asleep || dead()) return;
    const ItemInstance* back = stowedWeapon();
    const ItemInstance* hand = weapon();
    if (!back && !hand) return;
    if (action_.active && action_.option.kind == ActionKind::Reload) cancelAction();
    uint32_t h = hand ? hand->uid : 0, b = back ? back->uid : 0;
    std::string handName = hand ? lower(hand->def->name) : "", backName = back ? lower(back->def->name) : "";
    p.weaponUid = b;
    p.stowedUid = h;
    p.attackTimer = std::max(p.attackTimer, 0.6f);
    sound(Sfx::Click, p.pos);
    if (b && h) log("You sling the " + handName + " and take the " + backName + " off your back.");
    else if (b) log("You take the " + backName + " off your back.");
    else log("You sling the " + handName + " on your back.");
}

int Game::ammoCarried(const ItemDef* gun) const {
    if (!gun || gun->ammo.empty()) return 0;
    int n = 0;
    for (const auto& it : player_.inventory.items())
        if (it.def->id == gun->ammo) n += std::max(0, it.usesLeft);
    return n;
}

bool Game::canReload() const {
    const ItemInstance* w = weapon();
    return w && w->def->magazine > 0 && w->usesLeft < w->def->magazine && ammoCarried(w->def) > 0;
}

void Game::reload() {
    if (!canReload()) return;
    ActionOption o;
    o.kind = ActionKind::Reload;
    o.label = "Reloading";
    perform(o);
}

bool Game::inTentWith(int survivorId) {
    if (!tentMate_ || tentMate_ != survivorId) return false;
    Survivor* m = survivor(survivorId);
    if (!m || !m->alive() || !m->joined) { tentMate_ = 0; return false; }
    Tent* t = world_->tentAt(player_.pos);
    if (!t || !t->rect.contains((int)std::floor(m->pos.x), (int)std::floor(m->pos.y))) { tentMate_ = 0; return false; }
    return true;
}

void Game::sleepReport() {
    if (sleepMinutes_ >= 30) {
        float avg = sleepComfortSum_ / sleepMinutes_;
        if (avg >= 0.3f) log("You slept well.", MessageKind::Good);
        else if (avg <= -0.3f) log("You slept badly. You don't feel rested.", MessageKind::Bad);
        bool inCar = playerVehicle() && playerVehicle()->kind == VehicleKind::Car;
        if (inCar && sleepMinutes_ > 4 * 60) log("Your neck and back are stiff from the car seat.", MessageKind::Body);
    }
    sleepStartMin_ = -1;
    sleepComfortSum_ = sleepMinutes_ = 0;
    tentMate_ = 0;
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
    updateAutoDrink(dt);

    // Wildlife
    PlayerPresence presence{player_.pos, player_.noise, !dead()};
    // Animals move in real time; when the clock is sped up they keep up
    float animalDt = std::min(dt * std::max(1.0f, speed_ * timeScale * 0.25f), 0.25f);
    for (const auto& atk : animals_.update(animalDt, gameMinutes, *world_, presence, rng_))
        playerHurt(atk.severity, atk.by);
    updateSurvivors(animalDt, gameMinutes);

    // Snares: chance of catching a hare each game hour in good habitat
    for (auto& s : world_->snares()) {
        if (s.caught) continue;
        s.minutesSet += gameMinutes;
        Zone z = map_.zoneAt(s.pos.x, s.pos.y);
        float habitat = (z == Zone::Woodland || z == Zone::Meadow || z == Zone::Field) ? 1.0f
                      : (z == Zone::Forest || z == Zone::ForestDense || z == Zone::Marsh) ? 0.6f : z == Zone::Thicket ? 0.4f : 0.15f;
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
    if (p.stowedUid && (p.stowedUid == p.weaponUid || p.inventory.indexOfUid(p.stowedUid) < 0)) p.stowedUid = 0;
    Capacity cap = p.body.capacity();

    if (p.vehicle >= 0) {
        // Riding or driving instead of walking
        if (in.move.lengthSq() > 0.01f && action_.active && action_.option.kind != ActionKind::Hotwire) cancelAction();
        updateVehicle(dt, in);
    } else {
    // Movement
    float len = in.move.length();
    bool wantsMove = len > 0.1f;
    // Lying where you fell: you have to get up first
    if (p.fallen > 0) {
        p.fallen = std::max(0.0f, p.fallen - dt);
        wantsMove = false;
    }
    if (wantsMove && action_.active && action_.option.kind != ActionKind::Reload) {   // you can reload on the move
        if (action_.option.kind == ActionKind::Sleep) log("You get up.");
        cancelAction();
    }
    p.moving = wantsMove;
    p.sneaking = in.sneak;
    p.aiming = in.aimHold && !p.asleep && p.fallen <= 0;
    bool canRun = p.stamina > 0.05f && !in.sneak && !p.aiming;
    p.running = wantsMove && in.run && canRun;

    // A hurt leg or foot: you limp, and can't run properly
    float limp = std::max(p.body.impairment(BodyPart::LeftLeg) + 0.7f * p.body.impairment(BodyPart::LeftFoot),
                          p.body.impairment(BodyPart::RightLeg) + 0.7f * p.body.impairment(BodyPart::RightFoot));
    limp = std::min(1.0f, limp);
    if (limp > 0.65f) p.running = false;
    float speed = p.sneaking ? 1.0f : p.running ? 4.6f : 2.1f;
    if (p.aiming) speed *= 0.65f;
    speed *= cap.moveSpeed * (1.0f - 0.45f * limp);
    float over = carriedKg() / carryCapacityKg();
    if (over > 1.0f) speed *= std::max(0.35f, 1.0f - (over - 1.0f));
    int tx = (int)std::floor(p.pos.x), ty = (int)std::floor(p.pos.y);
    const Tile& t0 = world_->tile(tx, ty);
    Ground g = t0.ground;
    if (g == Ground::WaterShallow) speed *= 0.45f;
    else if (g == Ground::TallGrass || g == Ground::FieldSoil) speed *= 0.85f;
    // Thick forest, thickets and bogs are slow going (off the paths)
    if (t0.building < 0 && g != Ground::Asphalt && g != Ground::Gravel && g != Ground::Dirt && g != Ground::Sidewalk) {
        Zone zone = map_.zoneAt(p.pos.x, p.pos.y);
        if (zone == Zone::ForestDense) speed *= 0.85f;
        else if (zone == Zone::Thicket) speed *= 0.6f;
        else if (zone == Zone::Marsh) speed *= 0.7f;
        else if (zone == Zone::Rock) speed *= 0.9f;
    }

    p.grade = 0;
    if (wantsMove) {
        Vec2 dir = in.move * (1.0f / std::max(1.0f, len));
        // Hills: slow going uphill, and steeply downhill too (Tobler's hiking
        // function: fastest on a gentle downhill)
        Vec2 unit = dir * (1.0f / std::max(0.001f, dir.length()));
        p.grade = world_->slope(p.pos, unit);
        float hill = std::exp(-3.5f * std::fabs(p.grade + 0.05f)) / std::exp(-3.5f * 0.05f);
        speed *= std::clamp(hill, 0.3f, 1.1f);
        checkTrip(dt, p.grade, limp);
        if (p.fallen > 0) speed = 0;
        Vec2 before = p.pos;
        p.pos = world_->moveCircle(p.pos, dir * (speed * dt), 0.28f);
        stepDistance_ += distance(before, p.pos);
        float stride = p.running ? 1.5f : p.sneaking ? 0.6f : 0.85f;
        if (stepDistance_ > stride) { stepDistance_ = 0; sound(Sfx::Footstep, p.pos); }
        p.facing = std::atan2(dir.y, dir.x);
    }
    // Aiming: you turn to the cursor and walk (more slowly) however you like
    if (p.aiming) {
        Vec2 to = in.aim - p.pos;
        if (to.lengthSq() > 0.01f) p.facing = std::atan2(to.y, to.x);
    }

    // Stamina (real-time). Climbing takes your breath, running uphill most of all.
    float uphill = std::max(0.0f, p.grade);
    if (p.running) p.stamina -= dt / 26.0f * (1.0f + 2.0f * uphill);
    else if (p.moving && uphill > 0.12f) p.stamina -= dt * (uphill - 0.12f) * 0.25f;
    else p.stamina += dt / 9.0f * cap.endurance * (p.moving ? 0.6f : 1.0f);
    p.stamina = clamp01(p.stamina);

    p.noise = p.sneaking ? 3.0f : p.running ? 26.0f : p.moving ? 11.0f : 4.0f;
    }   // on foot
    float uphill = std::max(0.0f, p.grade);
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
    else if (const Vehicle* v = playerVehicle())
        act = v->kind == VehicleKind::Bike && std::fabs(v->speed) > 0.5f ? (std::fabs(v->speed) > 4.5f ? Activity::Running : Activity::Walking)
                                                                         : Activity::Resting;
    else if (p.running) act = Activity::Running;
    else if (p.moving) act = uphill > 0.15f ? Activity::Running : Activity::Walking;   // a steep climb is hard work
    else if (action_.active) act = action_.option.kind == ActionKind::Rest || action_.option.kind == ActionKind::Wait ? Activity::Resting : Activity::Working;
    Environment env = environment();
    p.body.update(gameMinutes, env, act);
    if (p.asleep) {
        if (sleepStartMin_ < 0) sleepStartMin_ = world_->clock().totalMinutes();
        sleepComfortSum_ += env.sleepComfort * gameMinutes;
        sleepMinutes_ += gameMinutes;
    }
    soilDressings(gameMinutes);
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
    if (!in.attack || p.attackTimer > 0 || dead() || p.asleep || p.vehicle >= 0) return;
    if (action_.active && action_.option.kind == ActionKind::Reload) return;   // still reloading
    if (action_.active) cancelAction();
    Capacity cap = p.body.capacity();
    const ItemInstance* w = weapon();
    Vec2 aimDir = (in.aim - p.pos).normalized();
    if (aimDir.lengthSq() < 0.1f) aimDir = {std::cos(p.facing), std::sin(p.facing)};
    p.facing = std::atan2(aimDir.y, aimDir.x);
    p.lastAttack = 0;
    p.swing = 1.0f;

    if (w && w->def->firearm) {
        p.attackTimer = w->def->cooldown;
        const ItemDef* ammoDef = nullptr;
        if (w->def->magazine > 0) {
            // rounds in the gun; empty, it reloads from your bags
            ItemInstance& gun = p.inventory.items()[p.inventory.indexOfUid(w->uid)];
            if (gun.usesLeft <= 0) {
                if (canReload()) {
                    log("Empty. You reload.");
                    ActionOption o;
                    o.kind = ActionKind::Reload;
                    o.label = "Reloading";
                    perform(o);
                } else {
                    log("Click. Empty, and no rounds left.");
                }
                return;
            }
            --gun.usesLeft;
            ammoDef = items_.find(w->def->ammo);
        } else {
            int ammo = p.inventory.find([&](const ItemInstance& it) { return it.def->id == w->def->ammo && it.usesLeft > 0; });
            if (ammo < 0) { log(w->def->noise > 100 ? "Click. No cartridges." : "Nothing to shoot."); return; }
            ItemInstance& a = p.inventory.items()[ammo];
            ammoDef = a.def;
            if (--a.usesLeft <= 0) p.inventory.take(ammo);
        }
        bool loud = w->def->noise > 100;
        p.noise = w->def->noise;
        // aim sway from dizziness, cold hands, exhaustion
        float sway = (1.0f - cap.accuracy) * 0.12f + (p.running ? 0.08f : 0.0f) + (p.moving ? 0.03f : 0.0f);
        if (p.aiming) sway *= 0.4f;   // taking your time
        float ang = p.facing + rng_.range(-sway, sway);
        Vec2 dir{std::cos(ang), std::sin(ang)};
        // the first animal or person along the line of fire
        Animal* hit = nullptr;
        Survivor* hitS = nullptr;
        float bestT = w->def->reach;
        for (auto& an : animals_.animals()) {
            if (!an.alive()) continue;
            Vec2 rel = an.pos - p.pos;
            float t = dot(rel, dir);
            if (t < 0 || t > bestT) continue;
            float off = (rel - dir * t).length();
            if (off < an.def->size * 0.6f && world_->lineOfSight(p.pos, an.pos)) { bestT = t; hit = &an; }
        }
        for (auto& sv : survivors_) {
            if (!sv.alive() || sv.joined) continue;
            Vec2 rel = sv.pos - p.pos;
            float t = dot(rel, dir);
            if (t < 0 || t > bestT) continue;
            float off = (rel - dir * t).length();
            if (off < 0.4f && world_->lineOfSight(p.pos, sv.pos)) { bestT = t; hitS = &sv; hit = nullptr; }
        }
        if (loud) {
            animals_.scare(p.pos, 350.0f);
            sound(Sfx::Gunshot, p.pos);
            log("BANG!");
            // a shot carries: raiders come to see
            for (auto& sv : survivors_)
                if (sv.alive() && sv.def->disposition == Disposition::Raider && distance(sv.pos, p.pos) < 250 && sv.state != SurvivorState::Hunt) {
                    sv.state = SurvivorState::Hunt;
                    sv.stateTimer = 40;
                }
        } else {
            sound(Sfx::Bowshot, p.pos);
        }
        Vec2 landed = p.pos + dir * (hit || hitS ? bestT : std::min(w->def->reach, rng_.range(10.0f, 30.0f)));
        if (hitS) {
            hurtSurvivor(*hitS, w->def->damage * rng_.range(0.6f, 1.1f) * 0.6f, true);
            if (hitS->alive()) log("You hit " + (hitS->met ? hitS->def->name : std::string("them")) + ".");
        } else if (hit) {
            hit->hp -= w->def->damage * rng_.range(0.6f, 1.2f);
            if (hit->hp <= 0) { hit->state = AnimalState::Dead; log("The " + lower(hit->def->name) + " drops.", MessageKind::Good); }
            else log("You hit the " + lower(hit->def->name) + ". It runs.");
            if (!loud && hit->alive()) { hit->state = AnimalState::Flee; hit->stateTimer = 8; }
        }
        // Arrows and bolts can often be found again
        if (ammoDef->hasTag("arrow") && rng_.chance(hit || hitS ? 0.5f : 0.7f)) {
            ItemInstance back = items_.create(ammoDef, rng_);
            back.usesLeft = 1;
            if (!world_->tileBlocked((int)std::floor(landed.x), (int)std::floor(landed.y))) world_->dropItem(landed, back);
        }
        return;
    }

    float reach = w ? w->def->reach : 0.9f;
    float arm = std::min(1.0f, p.body.impairment(BodyPart::RightArm) + p.body.impairment(BodyPart::RightHand));
    float damage = (w ? w->def->damage : 2.0f) * cap.strength * (1.0f - 0.5f * arm);
    p.attackTimer = (w ? w->def->cooldown : 0.7f) * (1.0f + 0.8f * arm);
    p.stamina = std::max(0.0f, p.stamina - 0.08f);
    p.noise = w ? w->def->noise : 5.0f;
    sound(Sfx::Swing, p.pos);
    Animal* target = nullptr;
    Survivor* targetS = nullptr;
    float best = 1e9f;
    for (auto& an : animals_.animals()) {
        if (!an.alive()) continue;
        Vec2 rel = an.pos - p.pos;
        float d = rel.length();
        if (d > reach + an.def->size * 0.5f + 0.3f) continue;
        if (d > 0.3f && dot(rel * (1.0f / d), aimDir) < 0.35f) continue;
        if (d < best) { best = d; target = &an; }
    }
    for (auto& sv : survivors_) {
        if (!sv.alive() || sv.joined) continue;
        Vec2 rel = sv.pos - p.pos;
        float d = rel.length();
        if (d > reach + 0.5f) continue;
        if (d > 0.3f && dot(rel * (1.0f / d), aimDir) < 0.35f) continue;
        if (d < best) { best = d; targetS = &sv; target = nullptr; }
    }
    if (targetS) {
        if (!rng_.chance(0.75f * cap.accuracy)) { log("You miss."); return; }
        hurtSurvivor(*targetS, damage * rng_.range(0.7f, 1.3f), true);
        return;
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
            if (ref.loc == ItemLoc::Player && it.uid == player_.stowedUid) player_.stowedUid = 0;
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

// ---------------------------------------------------------------------------
// Carpentry
// ---------------------------------------------------------------------------

const std::vector<Game::BuildRecipe>& Game::buildRecipes() {
    static const std::vector<BuildRecipe> r = {
        {BuildKind::Wall, "Wooden wall", 2, 4, false, false, false, 8.0f, "A plank wall along the edge of a tile. Keeps wind and animals out."},
        {BuildKind::Door, "Door", 3, 6, true, false, true, 12.0f, "A door in a frame. Open and close it like any other."},
        {BuildKind::Window, "Wall with a window", 2, 4, true, false, true, 10.0f, "A wall with a small glazed window from an old frame."},
        {BuildKind::Floor, "Plank floor", 1, 2, false, false, false, 5.0f, "Boards on joists: dry underfoot."},
        {BuildKind::Roof, "Roof", 2, 4, true, false, true, 8.0f, "A plank roof over a tile. Under a roof you're out of the rain and wind."},
        {BuildKind::Crate, "Storage crate", 2, 4, false, false, false, 6.0f, "A big lidded box to keep things in."},
        {BuildKind::RainCollector, "Rain collector", 1, 2, false, true, false, 6.0f, "A barrel under a tarp funnel. Fills when it rains."},
        {BuildKind::Bed, "Plank bed", 3, 6, false, false, false, 10.0f, "A bed frame off the cold ground."},
        {BuildKind::Fence, "Fence", 1, 2, false, false, false, 4.0f, "A short stretch of fence."},
    };
    return r;
}

const Game::BuildRecipe* Game::recipe(BuildKind k) {
    for (const auto& r : buildRecipes())
        if (r.kind == k) return &r;
    return nullptr;
}

bool Game::canAffordBuild(BuildKind k, std::string* why) const {
    const BuildRecipe* r = recipe(k);
    if (!r) return false;
    auto no = [&](const std::string& w) { if (why) *why = w; return false; };
    const Inventory& inv = player_.inventory;
    if (r->book && !player_.knowsCarpentry) return no("You don't know how. A carpentry book would teach you");
    if (inv.findTag("hammer") < 0 && inv.findId("hammer") < 0 && inv.findId("sledgehammer") < 0) return no("You need a hammer");
    if (r->saw && inv.findTag("saw") < 0) return no("You need a saw");
    if (r->tarp && inv.findTag("tarp") < 0) return no("You need a tarpaulin");
    int planks = 0, nails = 0;
    for (const auto& it : inv.items()) {
        if (it.def->hasTag("plank")) ++planks;
        if (it.def->hasTag("nails")) nails += it.usesLeft;
    }
    if (planks < r->planks) return no("You need " + std::to_string(r->planks) + " planks (you have " + std::to_string(planks) + ")");
    if (nails < r->nails) return no("You need " + std::to_string(r->nails) + " nails (you have " + std::to_string(nails) + ")");
    return true;
}

ActionOption Game::buildOption(BuildKind k, int x, int y, int dir) {
    ActionOption o;
    o.kind = ActionKind::Build;
    o.targetId = (int)k;
    o.x = x; o.y = y; o.dir = dir;
    const BuildRecipe* r = recipe(k);
    o.label = std::string("Build ") + (r ? lower(r->name) : "it");
    std::string why;
    Built b{k, x, y, dir};
    Vec2 c = isEdgeBuild(k) ? (dir == 0 ? Vec2{x + 0.5f, (float)y} : Vec2{(float)x, y + 0.5f}) : Vec2{x + 0.5f, y + 0.5f};
    if (distance(c, player_.pos) > 2.8f) { o.enabled = false; o.reason = "Too far away"; return o; }
    if (!world_->canBuild(b, &why) || !canAffordBuild(k, &why)) { o.enabled = false; o.reason = why; }
    return o;
}

// ---------------------------------------------------------------------------
// Wounds and dressings
// ---------------------------------------------------------------------------

int Game::facingDir() const {
    // 0 north, 1 east, 2 south, 3 west (the map's y grows southwards)
    int k = (int)std::lround(player_.facing / 1.5707963f);
    k = ((k % 4) + 4) % 4;
    return (k + 1) % 4;
}

IRect Game::tentFootprint(const ItemDef& d) const {
    int dir = facingDir();
    int dx = dir == 1 ? 1 : dir == 3 ? -1 : 0, dy = dir == 2 ? 1 : dir == 0 ? -1 : 0;
    int px = (int)std::floor(player_.pos.x), py = (int)std::floor(player_.pos.y);
    int fx = px + dx, fy = py + dy;               // the tile in front of you
    int L = std::max(1, d.tentL), W = std::max(1, d.tentW);
    int x0 = fx, y0 = fy, x1 = fx, y1 = fy;
    for (int i = 0; i < L; ++i)
        for (int j = 0; j < W; ++j) {
            int x = fx + dx * i - dy * j, y = fy + dy * i + dx * j;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
    return IRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

int Game::worstWoundPart(DressingKind forKind) const {
    const Body& b = player_.body;
    int best = -1;
    float bestScore = 0;
    for (const auto& w : b.state().wounds) {
        if (w.healed > 0.95f) continue;
        if (forKind == DressingKind::Bandaid && w.severity > 0.35f) continue;   // a plaster won't do
        const Dressing& d = b.dressing(w.part);
        // Needs one if it has none, or the old one is soaked or filthy
        if (d.on() && d.soiling() < 0.55f) continue;
        float score = w.bleeding * 0.2f + w.severity + (d.on() ? 0.0f : 0.5f);
        if (score > bestScore) { bestScore = score; best = (int)w.part; }
    }
    return best;
}

int Game::dirtiestWoundPart() const {
    int best = -1;
    float bestC = 0.02f;
    for (const auto& w : player_.body.state().wounds)
        if (w.healed < 0.95f && w.contamination + w.infection > bestC) { bestC = w.contamination + w.infection; best = (int)w.part; }
    return best;
}

void Game::returnUsedDressing(const Dressing& d) {
    if (!d.on()) return;
    // Plasters lose their stick; bandages and rags can be washed and used again
    if (d.kind == DressingKind::Bandaid) return;
    bool dirty = d.soiling() > 0.25f;
    std::string id = d.kind == DressingKind::Bandage ? (dirty ? "bandage_dirty" : "bandage") : (dirty ? "rag_dirty" : "rag");
    ItemInstance it = items_.create(id, rng_);
    if (!dirty && d.kind == DressingKind::Bandage) it.opened = true;   // used: no longer sterile
    giveItem(it);
}

bool Game::waterForWashing(bool consume) {
    // A lake, a tap with water in the tank, a rain barrel...
    int wx, wy;
    if (world_->waterSourceNear(player_.pos, 1.8f, &wx, &wy) != World::WaterSource::None) {
        const Tile& t = world_->tile(wx, wy);
        if (t.ground == Ground::WaterShallow || t.ground == Ground::WaterDeep) return true;
        if (!consume) return world_->tankWater(wx, wy) >= 250;
        return world_->takeTankWater(wx, wy, 250) > 0;
    }
    // ...or what you carry
    for (auto& it : player_.inventory.items()) {
        if (it.def->capacityMl <= 0 || it.liquidMl < 250) continue;
        if (consume) it.liquidMl -= 250;
        return true;
    }
    return false;
}

// Dirt reaching each dressing (or open wound) from walking, working and lying
// on the ground. Mud and forest floor soil it fast; a wooden floor indoors hardly.
void Game::soilDressings(float gameMinutes) {
    Player& p = player_;
    if (gameMinutes <= 0 || dead()) return;
    float h = gameMinutes / 60.0f;
    int tx = (int)std::floor(p.pos.x), ty = (int)std::floor(p.pos.y);
    const Tile& t = world_->tile(tx, ty);
    float ground;
    switch (t.ground) {
        case Ground::Dirt: case Ground::FieldSoil: ground = 1.5f; break;
        case Ground::ForestFloor: case Ground::Moss: ground = 1.0f; break;
        case Ground::WaterShallow: ground = 1.3f; break;
        case Ground::Grass: case Ground::TallGrass: case Ground::Stubble: ground = 0.6f; break;
        case Ground::Sand: case Ground::Gravel: ground = 0.7f; break;
        case Ground::Asphalt: case Ground::Sidewalk: case Ground::Cobble: case Ground::Concrete: case Ground::Rail: ground = 0.35f; break;
        case Ground::FloorWood: ground = 0.12f; break;
        case Ground::FloorTile: ground = 0.08f; break;
        case Ground::FloorCarpet: ground = 0.15f; break;
        case Ground::FloorConcrete: ground = 0.2f; break;
        default: ground = 0.6f; break;
    }
    bool indoors = t.building >= 0;
    float wet = indoors ? 1.0f : 1.0f + 0.6f * std::max(world_->groundWetness(), world_->clock().weather().rain);
    float moving = p.running ? 2.0f : p.moving ? 1.0f : 0.25f;
    auto worn = [&](ClothingSlot sl) { return p.worn[(size_t)sl].def != nullptr; };
    // Per body part: how exposed it is, and how much clothing shields it
    float feet = 0.15f * (worn(ClothingSlot::Feet) ? 1.0f : 4.0f);
    float legs = 0.08f * (worn(ClothingSlot::Legs) ? 0.5f : 1.0f);
    float hands = 0.03f * (worn(ClothingSlot::Hands) ? 0.3f : 1.0f);
    float sleeves = worn(ClothingSlot::Jacket) || worn(ClothingSlot::Sweater) ? 0.5f : 1.0f;
    float rate[(size_t)BodyPart::Count] = {};
    rate[(size_t)BodyPart::Head] = 0.02f;
    rate[(size_t)BodyPart::Torso] = 0.015f;
    rate[(size_t)BodyPart::LeftArm] = rate[(size_t)BodyPart::RightArm] = 0.02f * sleeves;
    rate[(size_t)BodyPart::LeftHand] = rate[(size_t)BodyPart::RightHand] = hands;
    rate[(size_t)BodyPart::LeftLeg] = rate[(size_t)BodyPart::RightLeg] = legs;
    rate[(size_t)BodyPart::LeftFoot] = rate[(size_t)BodyPart::RightFoot] = feet;
    for (auto& r : rate) r *= ground * wet * moving;
    // Working with your hands
    if (action_.active) {
        ActionKind k = action_.option.kind;
        float work = k == ActionKind::Butcher ? 1.2f : k == ActionKind::Search ? 0.25f
                   : (k == ActionKind::GatherSticks || k == ActionKind::ChopTree || k == ActionKind::BuildFire ||
                      k == ActionKind::Forage || k == ActionKind::PickBerries || k == ActionKind::SetSnare) ? 0.4f : 0.0f;
        float glove = worn(ClothingSlot::Hands) ? 0.3f : 1.0f;
        rate[(size_t)BodyPart::LeftHand] += work * glove;
        rate[(size_t)BodyPart::RightHand] += work * glove;
        if (k == ActionKind::Butcher) rate[(size_t)BodyPart::LeftArm] += 0.2f, rate[(size_t)BodyPart::RightArm] += 0.2f;
    }
    // Sleeping on the bare ground
    if (p.asleep && !indoors && t.prop != Prop::Bed && p.vehicle < 0)
        for (int i : {1, 2, 3, 6, 7}) rate[i] += 0.04f * ground;
    for (int i = 0; i < (int)BodyPart::Count; ++i) p.body.soil((BodyPart)i, rate[i] * h);
}

std::vector<ActionOption> Game::actionsForBodyPart(BodyPart part) {
    std::vector<ActionOption> out;
    if (dead()) return out;
    const Body& b = player_.body;
    bool wounded = false;
    float worst = 0;
    for (const auto& w : b.state().wounds)
        if (w.part == part) { wounded = true; worst = std::max(worst, w.severity); }
    const Dressing& cur = b.dressing(part);
    auto opt = [&](ActionKind k, const std::string& label, const ItemInstance* it, bool en = true, const std::string& why = "") {
        ActionOption o;
        o.kind = k; o.label = label; o.part = (int)part; o.enabled = en; o.reason = why;
        if (it) o.item = {ItemLoc::Player, 0, 0, 0, it->uid};
        out.push_back(o);
    };
    if (wounded) {
        // One option per kind of disinfectant and dressing you carry
        std::set<std::string> seen;
        for (const auto& it : player_.inventory.items()) {
            const ItemDef& d = *it.def;
            if (d.hasTag("disinfectant") && !seen.count("dis:" + d.id)) {
                seen.insert("dis:" + d.id);
                opt(ActionKind::Disinfect, "Disinfect with " + lower(d.name), &it);
            }
        }
        for (const auto& it : player_.inventory.items()) {
            const ItemDef& d = *it.def;
            if (d.dressing == DressingKind::None) continue;
            std::string key = "dr:" + d.id + (it.opened ? "o" : "");
            if (seen.count(key)) continue;
            seen.insert(key);
            std::string name = lower(it.displayName());
            if (it.def->uses > 0) name = lower(d.dressing == DressingKind::Bandaid && d.uses > 1 ? "bandaid" : d.name);
            std::string label = std::string(cur.on() ? "Replace with " : "Apply ") + (d.dressing == DressingKind::Bandaid && d.uses > 1 ? "a bandaid" : name);
            bool small = d.dressing != DressingKind::Bandaid || worst <= 0.35f;
            opt(ActionKind::Bandage, label, &it, small, "The wound is too big for a bandaid");
        }
        if (seen.empty()) opt(ActionKind::None, "You have nothing to treat it with", nullptr, false, "Look in bathrooms, pharmacies and hospitals. Clothes rip into rags.");
    }
    if (cur.on()) opt(ActionKind::RemoveDressing, std::string("Take off the ") + dressingName(cur.kind), nullptr);
    if (!wounded && !cur.on()) opt(ActionKind::None, "Nothing wrong here", nullptr, false);
    return out;
}

std::vector<std::string> Game::describeBodyPart(BodyPart part) const {
    std::vector<std::string> out;
    const Body& b = player_.body;
    const Dressing& d = b.dressing(part);
    for (const auto& w : b.state().wounds) {
        if (w.part != part) continue;
        std::string what;
        switch (w.type) {
            case WoundType::Scratch: what = w.severity > 0.3f ? "Long scratches" : "A scratch"; break;
            case WoundType::Laceration: what = w.severity > 0.6f ? "A deep gash" : w.severity > 0.3f ? "A nasty cut" : "A small cut"; break;
            case WoundType::Bite: what = w.severity > 0.5f ? "A deep, torn bite" : "A bite"; break;
            case WoundType::Puncture: what = "A puncture wound"; break;
            case WoundType::Burn: what = w.severity > 0.5f ? "A blistered burn" : "A burn"; break;
            default: what = "A wound"; break;
        }
        std::vector<std::string> state;
        float bleedOut = w.bleeding * (d.on() ? (d.blood > 0.75f ? 0.8f : 0.3f) : 1.0f);
        if (w.bleeding > 0.3f) state.push_back(d.on() ? (d.blood > 0.75f ? "blood is seeping through" : "bleeding under the dressing")
                                                       : bleedOut > 8 ? "bleeding heavily" : "bleeding");
        // Only what you can see: redness, swelling, pus - never the word "infection"
        if (w.infection > 0.6f) state.push_back("swollen and weeping yellow pus, it smells");
        else if (w.infection > 0.35f) state.push_back("red, swollen and hot to the touch");
        else if (w.infection > 0.12f) state.push_back("red around the edges");
        if (!d.on() && w.contamination > 0.5f && w.healed < 0.8f) state.push_back("there's dirt in it");
        if (w.healed > 0.75f) state.push_back("almost healed");
        else if (w.healed > 0.4f && w.bleeding <= 0) state.push_back("scabbed over");
        std::string line = what;
        for (size_t i = 0; i < state.size(); ++i) line += (i == 0 ? ", " : i + 1 == state.size() ? " and " : ", ") + state[i];
        out.push_back(line + ".");
    }
    if (d.on()) {
        std::string kind = d.kind == DressingKind::Bandaid ? "bandaid" : d.kind == DressingKind::Bandage ? "bandage" : "rag";
        std::string look = d.soiling() > 0.8f ? "A filthy " : d.soiling() > 0.5f ? "A dirty " : d.soiling() > 0.25f ? "A grubby " : "A clean ";
        std::string line = look + kind;
        if (d.blood > 0.75f) line += ", soaked through with blood";
        else if (d.blood > 0.35f) line += ", stained with blood";
        out.push_back(line + ".");
    }
    return out;
}

std::vector<ActionOption> Game::actionsAt(Vec2 pt) {
    std::vector<ActionOption> out;
    if (!world_ || dead()) return out;
    // On a bike or in a car you can only do what you can do from the seat
    if (player_.vehicle >= 0) return vehicleActionsAt(pt);
    // People
    for (const auto& s : survivors_) {
        if (distance(s.pos, pt) > 0.9f || !s.alive()) continue;
        float d = distance(s.pos, player_.pos);
        ActionOption o;
        o.kind = ActionKind::Talk;
        o.targetId = s.id;
        std::string name = s.met ? s.def->name : "the stranger";
        float range = talkRange(s.id);
        o.label = (range > 5 && d > 4 ? "Call out to " : "Talk to ") + name;
        std::string why;
        o.enabled = canTalk(s.id, &why) && d < range;
        o.reason = !why.empty() ? why : "Too far away";
        out.push_back(o);
    }
    for (auto& o : vehicleActionsAt(pt)) out.push_back(o);
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

    // Windows: smash one, then climb through
    int winX, winY, winDir;
    if (world_->findWindowNear(pt, 0.9f, winX, winY, winDir)) {
        Vec2 mid = winDir == 0 ? Vec2{winX + 0.5f, (float)winY} : Vec2{(float)winX, winY + 0.5f};
        bool nearWin = distance(mid, player_.pos) < 1.6f;
        ActionOption o;
        o.x = winX; o.y = winY; o.dir = winDir;
        o.enabled = nearWin;
        o.reason = nearWin ? "" : "Too far away";
        if (!world_->windowBroken(winX, winY, winDir)) {
            bool tool = false;
            for (const auto& it : player_.inventory.items()) tool |= heavyTool(it.def);
            o.kind = ActionKind::BreakWindow;
            o.label = tool ? "Smash the window" : "Smash the window (with your elbow)";
        } else {
            o.kind = ActionKind::ClimbWindow;
            o.label = "Climb through the window";
        }
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
            if (it.liquid == 1 && it.liquidMl > 0) { f.enabled = false; f.reason = "It has petrol in it. Pour that out first"; }
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
    if (t.prop == Prop::Bed || t.prop == Prop::HospitalBed) {
        bool done = world_->harvested(tx, ty, 100000);
        add(ActionKind::TearUpSheet, "Tear up the bed sheet for rags", !done, "Already torn up");
    }

    // Fires and camping stoves
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
        if (f->stove) {
            bool spirit = player_.inventory.findTag("stove_fuel") >= 0;
            // You can pour while it burns. You shouldn't.
            fireOpt(ActionKind::FillStove, f->lit ? "Top up the burner with red alcohol" : "Fill the burner with red alcohol",
                    spirit && f->fuelMl < 99.0f, !spirit ? "You have no red alcohol" : "The burner is full");
            if (!f->lit) {
                fireOpt(ActionKind::LightStove, "Light the storm kitchen", igniter && f->fuelMl > 1.0f,
                        !igniter ? "You need a lighter or matches" : "The burner is empty");
                fireOpt(ActionKind::PackStove, "Pack up the storm kitchen", true, "");
            } else {
                fireOpt(ActionKind::StoveOff, "Put out the flame", true, "");
            }
        } else {
            bool fuel = findFuel() >= 0;
            if (!f->lit) fireOpt(ActionKind::LightFire, "Light the campfire", igniter && (f->fuelMinutes > 0 || fuel),
                                 !igniter ? "You need a lighter or matches" : "No fuel");
            fireOpt(ActionKind::AddFuel, "Add fuel to the fire", fuel, "You have nothing to burn");
        }
        if (f->lit) {
            bool pot = player_.inventory.findTag("pot") >= 0 || f->stove;   // a storm kitchen comes with its pots
            for (const auto& it : player_.inventory.items()) {
                if (it.def->cookedInto.empty()) continue;
                bool needsPot = it.def->id == "oats" || it.def->id == "potato_raw" || it.def->id == "pasta" || it.def->id == "rice";
                auto* o = fireOpt(ActionKind::CookItem, "Cook " + lower(itemName(it)), !needsPot || pot, "You need a pot");
                o->item = {ItemLoc::Player, 0, 0, 0, it.uid};
            }
            bool dirty = player_.inventory.find([](const ItemInstance& i) { return i.def->capacityMl > 0 && i.liquidMl > 0 && !i.liquidSafe; }) >= 0;
            if (dirty) fireOpt(ActionKind::BoilWater, "Boil water", pot, "You need a pot");
        }
    }

    // Tents
    if (Tent* tent = world_->tentAt(pt)) {
        bool reach = distance(tent->rect.centre(), player_.pos) < 3.0f;
        ActionOption o;
        o.targetId = tent->id; o.x = tx; o.y = ty;
        o.enabled = reach; o.reason = reach ? "" : "Too far away";
        o.kind = ActionKind::Sleep; o.label = "Crawl in and sleep";
        out.push_back(o);
        o.kind = ActionKind::PackTent; o.label = "Pack up the tent";
        out.push_back(o);
    }

    // Fishing from the shore
    if ((t.ground == Ground::WaterShallow || t.ground == Ground::WaterDeep) && player_.inventory.findTag("fishing") >= 0) {
        ActionOption o;
        o.kind = ActionKind::Fish; o.label = "Fish here for an hour"; o.x = tx; o.y = ty;
        o.enabled = dist < 4.0f; o.reason = o.enabled ? "" : "Too far away";
        out.push_back(o);
    }

    // Things you built: take them apart again
    {
        bool tool = player_.inventory.findId("hammer") >= 0 || player_.inventory.findId("crowbar") >= 0 ||
                    player_.inventory.findId("sledgehammer") >= 0;
        auto dismantle = [&](const Built& b) {
            ActionOption o;
            o.kind = ActionKind::Dismantle; o.targetId = (int)b.kind;
            o.x = b.x; o.y = b.y; o.dir = b.dir;
            o.label = std::string("Dismantle the ") + buildKindName(b.kind);
            o.enabled = tool && near; o.reason = !near ? "Too far away" : "You need a hammer or a crowbar";
            out.push_back(o);
        };
        float fx = pt.x - tx, fy = pt.y - ty;
        const Built* e = nullptr;
        if (fy < 0.3f) e = world_->builtEdgeAt(tx, ty, 0);
        if (!e && fy > 0.7f) e = world_->builtEdgeAt(tx, ty + 1, 0);
        if (!e && fx < 0.3f) e = world_->builtEdgeAt(tx, ty, 1);
        if (!e && fx > 0.7f) e = world_->builtEdgeAt(tx + 1, ty, 1);
        if (e) dismantle(*e);
        for (BuildKind k : {BuildKind::Crate, BuildKind::RainCollector, BuildKind::Bed, BuildKind::Fence, BuildKind::Roof, BuildKind::Floor})
            if (const Built* b = world_->builtAt(tx, ty, k)) dismantle(*b);
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
        bool wild = z == Zone::Forest || z == Zone::Woodland || z == Zone::ForestDense || z == Zone::Thicket || z == Zone::Marsh;
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
    if (d.capacityMl > 0 && it->liquidMl > 0 && it->liquid == 1) {
        add(ActionKind::Drink, "Drink", 1, false, "It's petrol");
        add(ActionKind::PourOut, "Pour it out");
    } else if (d.capacityMl > 0 && it->liquidMl > 0) {
        add(ActionKind::Drink, "Drink");
        bool tablet = player_.inventory.findTag("purify") >= 0;
        if (!it->liquidSafe && tablet) add(ActionKind::Purify, "Purify with a tablet");
        add(ActionKind::PourOut, "Pour out");
    }
    if (mine && (d.category == ItemCategory::Weapon || d.hasTag("knife") || d.hasTag("axe"))) {
        if (player_.weaponUid == it->uid) add(ActionKind::Unequip, "Put away");
        else add(ActionKind::Equip, "Hold as weapon");
        if (player_.stowedUid != it->uid) add(ActionKind::Stow, "Sling on your back (1 swaps)");
        if (d.magazine > 0 && player_.weaponUid == it->uid)
            add(ActionKind::Reload, "Reload", canReload(), it->usesLeft >= d.magazine ? "It's full" : "No rounds for it");
    }
    if (d.category == ItemCategory::Clothing) {
        if (ref.loc == ItemLoc::Worn) add(ActionKind::TakeOff, "Take off");
        else if (mine) add(ActionKind::Wear, "Wear");
    }
    if (mine && d.rags > 0) {
        bool knife = player_.inventory.findTag("knife") >= 0 || player_.inventory.findTag("scissors") >= 0;
        add(ActionKind::TearIntoRags, "Rip into rags", 1, !d.ripNeedsKnife || knife, "The fabric is too tough to rip by hand. You need a knife or scissors");
    }
    if (mine && d.dressing != DressingKind::None) {
        const char* label = d.dressing == DressingKind::Bandaid ? "Put on the worst small cut"
                          : d.dressing == DressingKind::Bandage ? "Bandage the worst wound" : "Tie over the worst wound";
        add(ActionKind::Bandage, label, 1, worstWoundPart(d.dressing) >= 0,
            d.dressing == DressingKind::Bandaid && !player_.body.state().wounds.empty() ? "Your wounds are too big for a bandaid"
                                                                                       : "No wounds need it");
    }
    if (mine && !d.washesInto.empty())
        add(ActionKind::WashItem, "Wash in water", 1, waterForWashing(false), "You need water: a lake, a tap or a full bottle");
    if (mine && d.hasTag("disinfectant"))
        add(ActionKind::Disinfect, "Clean the dirtiest wound", 1, dirtiestWoundPart() >= 0, "No open wounds to clean");
    if (mine && d.hasTag("stove")) add(ActionKind::PlaceStove, "Set up the storm kitchen here");
    if (mine && d.hasTag("tent")) {
        std::string why;
        IRect r = tentFootprint(d);
        bool ok = world_->canPitch(r, &why);
        add(ActionKind::PitchTent, "Pitch the tent (in front of you)", 1, ok, why);
    }
    if (mine && d.hasTag("log")) {
        add(ActionKind::SawPlanks, "Saw into planks", 1, player_.inventory.findTag("saw") >= 0, "You need a saw");
        add(ActionKind::SplitLog, "Split into firewood", 1, player_.inventory.findTag("axe") >= 0, "You need an axe");
    }
    if (mine && d.id == "batteries") {
        int light = player_.inventory.find([](const ItemInstance& i) { return i.def->hasTag("light") && i.usesLeft < i.def->uses; });
        add(ActionKind::ReplaceBatteries, "Put new batteries in a torch", 1, light >= 0, "Your lights are full");
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
        sleepReport();
    }
    action_.active = false;
}

void Game::perform(const ActionOption& o) {
    if (!o.enabled || dead()) return;
    if (action_.active) cancelAction();
    ItemInstance* it = (o.item.uid != 0) ? resolve(o.item) : nullptr;
    switch (o.kind) {
        // people and vehicles -------------------------------------------------
        case ActionKind::Talk: return;   // the client opens the conversation
        case ActionKind::TryCarDoor: case ActionKind::SmashCarWindow: case ActionKind::EnterVehicle: case ActionKind::ExitVehicle:
        case ActionKind::StartEngine: case ActionKind::StopEngine: case ActionKind::Hotwire: case ActionKind::Refuel:
        case ActionKind::SearchGlovebox: case ActionKind::MountBike: case ActionKind::Siphon: case ActionKind::FillFromPump:
            performVehicle(o);
            return;
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
            if (it) {
                if (player_.stowedUid == it->uid) player_.stowedUid = 0;
                player_.weaponUid = it->uid;
                log("You hold the " + lower(it->def->name) + " ready.");
            }
            return;
        case ActionKind::Unequip:
            player_.weaponUid = 0;
            return;
        case ActionKind::Stow:
            if (it) {
                if (player_.weaponUid == it->uid) player_.weaponUid = 0;
                player_.stowedUid = it->uid;
                log("You sling the " + lower(it->def->name) + " on your back. (1 to swap)");
            }
            return;
        case ActionKind::Reload:
            if (canReload()) startTimed(o, weapon()->def->reloadSeconds);
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
            if (it && it->def->hasTag("teaches_carpentry") && !player_.knowsCarpentry) {
                player_.knowsCarpentry = true;
                log("You read about framing, door hanging and roofing. You could build doors, windows and roofs now (B).",
                    MessageKind::Good);
                return;
            }
            if (it && it->def->hasTag("field_guide"))
                log("You leaf through the pictures. With this book you can tell edible mushrooms from deadly ones.", MessageKind::Good);
            return;
        // timed ---------------------------------------------------------------
        case ActionKind::Search: {
            const ContainerDef* cd = items_.container(world_->containerKindAt(o.x, o.y));
            startTimed(o, cd ? cd->searchSeconds : 2.0f);
            return;
        }
        case ActionKind::Sleep: {
            if (o.targetId != 0 && player_.vehicle < 0) {   // crawling into a tent
                const Tent* tent = nullptr;
                for (const auto& t : world_->tents())
                    if (t.id == o.targetId) tent = &t;
                if (tent) {
                    player_.pos = tent->rect.centre();
                    // room for two: a companion nearby crawls in too, and you
                    // talk before you sleep ("Let's sleep now")
                    bool two = tent->rect.w * tent->rect.h >= 4;
                    Survivor* mate = nullptr;
                    for (auto& s : survivors_)
                        if (two && s.joined && s.alive() && s.state != SurvivorState::Wait && distance(s.pos, player_.pos) < 20 &&
                            (!mate || s.bond() > mate->bond()))
                            mate = &s;
                    if (mate) {
                        const IRect& r = tent->rect;
                        bool wide = r.w >= r.h;
                        player_.pos = wide ? Vec2{r.x + r.w * 0.27f, r.y + r.h * 0.5f} : Vec2{r.x + r.w * 0.5f, r.y + r.h * 0.27f};
                        mate->pos = wide ? Vec2{r.x + r.w * 0.73f, r.y + r.h * 0.5f} : Vec2{r.x + r.w * 0.5f, r.y + r.h * 0.73f};
                        mate->vel = {};
                        mate->path.clear();
                        mate->nav.clear();
                        tentMate_ = mate->id;
                        talkRequest_ = mate->id;
                        log(mate->def->name + " crawls in beside you.");
                        return;
                    }
                }
            }
            bool car = playerVehicle() && playerVehicle()->kind == VehicleKind::Car;
            if (car) if (Vehicle* v = playerVehicle()) v->speed = 0;
            player_.asleep = true;
            player_.body.setAsleep(true);
            sleepStartMin_ = world_->clock().totalMinutes();
            sleepComfortSum_ = sleepMinutes_ = 0;
            startTimed(o, 1e9f, 90.0f);
            log(car ? "You tip the seat back and close your eyes." : "You lie down and close your eyes.");
            // how this will be
            SleepComfort c = sleepComfort();
            std::string why;
            for (size_t i = 0; i < c.good.size(); ++i) why += (i ? ", " : "") + c.good[i];
            if (!c.bad.empty()) {
                why += why.empty() ? "" : ". But: ";
                for (size_t i = 0; i < c.bad.size(); ++i) why += (i ? ", " : "") + c.bad[i];
            }
            log(std::string(c.word()) + (why.empty() ? "." : " - " + why + "."),
                c.score >= 0.15f ? MessageKind::Good : c.score <= -0.15f ? MessageKind::Bad : MessageKind::Info);
            return;
        }
        case ActionKind::Rest: startTimed(o, 3.0f, 20.0f); return;
        case ActionKind::BreakWindow: startTimed(o, 1.0f); return;
        case ActionKind::ClimbWindow: startTimed(o, 2.5f); return;
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
        case ActionKind::Bandage: {
            ItemInstance* d = resolve(o.item);
            DressingKind k = d ? d->def->dressing : DressingKind::Bandage;
            startTimed(o, k == DressingKind::Bandaid ? 2.0f : k == DressingKind::Rag ? 5.0f : 4.0f);
            return;
        }
        case ActionKind::Disinfect: startTimed(o, 3.0f); return;
        case ActionKind::RemoveDressing: startTimed(o, 2.0f); return;
        case ActionKind::PlaceStove: startTimed(o, 3.0f); return;
        case ActionKind::FillStove: startTimed(o, 2.5f); return;
        case ActionKind::LightStove: startTimed(o, 1.5f); return;
        case ActionKind::StoveOff: startTimed(o, 1.0f); return;
        case ActionKind::PackStove: startTimed(o, 3.0f); return;
        case ActionKind::PitchTent: {
            ItemInstance* t = resolve(o.item);
            startTimed(o, t && t->def->tentW > 1 ? 18.0f : 12.0f);
            return;
        }
        case ActionKind::PackTent: startTimed(o, 10.0f); return;
        case ActionKind::SawPlanks: startTimed(o, 15.0f); return;
        case ActionKind::SplitLog: startTimed(o, 10.0f); return;
        case ActionKind::ReplaceBatteries: startTimed(o, 2.0f); return;
        case ActionKind::Fish: startTimed(o, 4.0f, 15.0f); return;
        case ActionKind::Build: {
            const BuildRecipe* r = recipe((BuildKind)o.targetId);
            ActionOption check = buildOption((BuildKind)o.targetId, o.x, o.y, o.dir);
            if (!r || !check.enabled) { log(check.reason.empty() ? "You can't build that here." : check.reason + "."); return; }
            startTimed(o, r->seconds);
            return;
        }
        case ActionKind::Dismantle: startTimed(o, 6.0f); return;
        case ActionKind::WashItem: startTimed(o, 6.0f); return;
        case ActionKind::TearUpSheet: startTimed(o, 6.0f); return;
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

// Thirsty, and there's clean water in an open bottle: take a sip without
// being asked. Never untreated water, petrol, or anything still sealed.
void Game::updateAutoDrink(float dt) {
    autoDrinkTimer_ -= dt;
    if (!autoDrink_ || autoDrinkTimer_ > 0 || dead() || player_.asleep || action_.active) return;
    autoDrinkTimer_ = 2.0f;
    BodySignals g = player_.body.signals();
    if (g.thirst < 0.3f || g.nausea > 0.7f) return;
    // the emptiest bottle first, so the full ones stay full
    int best = -1;
    auto& v = player_.inventory.items();
    for (size_t i = 0; i < v.size(); ++i) {
        const ItemInstance& it = v[i];
        const ItemDef& d = *it.def;
        if (d.capacityMl <= 0 || it.liquid != 0 || !it.liquidSafe || it.liquidMl < 1) continue;
        if (d.sealed && !it.opened) continue;
        if (d.hasTag("gascan")) continue;
        if (best < 0 || it.liquidMl < v[best].liquidMl) best = (int)i;
    }
    if (best < 0) return;
    ItemInstance& it = v[best];
    float ml = std::min(250.0f, it.liquidMl);
    it.liquidMl -= ml;
    drinkWater(ml, true, nullptr);
    sound(Sfx::Drink, player_.pos);
    log("Thirsty, you take a few mouthfuls from your " + lower(itemName(it)) + ".");
    autoDrinkTimer_ = 25.0f;   // give it a moment to help before the next sip
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
        case ActionKind::Hotwire: {
            Vehicle* v = playerVehicle();
            if (!v) break;
            if (rng_.chance(0.7f)) {
                v->hotwired = true;
                log("Two wires touch, the dash lights up. You can start it now.", MessageKind::Good);
            } else {
                log("A spark bites your fingers. Nothing. Try again.", MessageKind::Bad);
            }
            break;
        }
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
            sleepReport();
            break;
        case ActionKind::Reload: {
            int gi = player_.inventory.indexOfUid(player_.weaponUid);
            if (gi < 0) break;
            const ItemDef* gd = player_.inventory.items()[(size_t)gi].def;
            int want = gd->magazine - player_.inventory.items()[(size_t)gi].usesLeft;
            int got = 0;
            while (want > 0) {
                int a = player_.inventory.find([&](const ItemInstance& it) { return it.def->id == gd->ammo && it.usesLeft > 0; });
                if (a < 0) break;
                ItemInstance& box = player_.inventory.items()[(size_t)a];
                int take = std::min(want, box.usesLeft);
                box.usesLeft -= take;
                want -= take;
                got += take;
                if (box.usesLeft <= 0) player_.inventory.take(a);
            }
            gi = player_.inventory.indexOfUid(player_.weaponUid);   // taking boxes out moves things
            if (gi >= 0) player_.inventory.items()[(size_t)gi].usesLeft += got;
            sound(Sfx::Click, player_.pos);
            break;
        }
        case ActionKind::BreakWindow: {
            if (!world_->windowAt(o.x, o.y, o.dir) || world_->windowBroken(o.x, o.y, o.dir)) break;
            world_->breakWindow(o.x, o.y, o.dir);
            Vec2 mid = o.dir == 0 ? Vec2{o.x + 0.5f, (float)o.y} : Vec2{(float)o.x, o.y + 0.5f};
            sound(Sfx::Glass, mid);
            animals_.scare(mid, 80);
            player_.noise = std::max(player_.noise, 45.0f);
            for (auto& s : survivors_)   // raiders come to see what broke
                if (s.alive() && s.def->disposition == Disposition::Raider && !s.truce && distance(s.pos, mid) < 70) {
                    s.state = SurvivorState::Hunt;
                    s.stateTimer = 30;
                    s.target = player_.pos;
                }
            bool tool = false;
            for (const auto& it : player_.inventory.items()) tool |= heavyTool(it.def);
            bool gloves = player_.worn[(size_t)ClothingSlot::Hands].def != nullptr;
            log(tool ? "The glass shatters." : "You drive your elbow through the glass. It shatters.");
            float cut = tool ? 0.1f : gloves ? 0.2f : 0.45f;
            if (rng_.chance(cut)) {
                Wound w;
                w.part = tool || rng_.chance(0.5f) ? BodyPart::RightHand : BodyPart::RightArm;
                w.type = WoundType::Laceration;
                w.severity = rng_.range(0.08f, 0.25f);
                w.bleeding = 6;
                w.contamination = 0.25f;
                player_.body.addWound(w);
                log("You cut yourself on the glass.", MessageKind::Bad);
            }
            break;
        }
        case ActionKind::ClimbWindow: {
            // out the other side of the wall
            Vec2 p = player_.pos;
            Vec2 to = o.dir == 0 ? Vec2{o.x + 0.5f, p.y < o.y ? o.y + 0.55f : o.y - 0.55f}
                                 : Vec2{p.x < o.x ? o.x + 0.55f : o.x - 0.55f, o.y + 0.5f};
            if (world_->tileBlocked((int)std::floor(to.x), (int)std::floor(to.y))) {
                log("Something's in the way on the other side.", MessageKind::Bad);
                break;
            }
            player_.pos = to;
            log("You climb through the broken window.");
            if (rng_.chance(player_.worn[(size_t)ClothingSlot::Hands].def ? 0.03f : 0.1f)) {
                Wound w;
                w.part = rng_.chance(0.5f) ? BodyPart::LeftHand : BodyPart::LeftLeg;
                w.type = WoundType::Laceration;
                w.severity = rng_.range(0.05f, 0.15f);
                w.bleeding = 3;
                w.contamination = 0.2f;
                player_.body.addWound(w);
                log("A shard left in the frame catches you.", MessageKind::Bad);
            }
            break;
        }
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
            it->liquid = 0;
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
            int n = rng_.rangeInt(1, 3);
            for (int i = 0; i < n; ++i) world_->dropItem(Vec2{o.x + 0.5f, o.y + 0.5f}, items_.create("firewood", rng_));
            world_->dropItem(Vec2{o.x + 0.5f, o.y + 0.5f}, items_.create("log", rng_));
            log("The tree comes down. You cut a log from the trunk and split the rest for firewood.", MessageKind::Good);
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
            const ItemDef& d = *it->def;
            int part = o.part >= 0 ? o.part : worstWoundPart(d.dressing);
            if (part < 0) break;
            bool dirty = d.hasTag("dirty");
            bool sterile = d.hasTag("sterile") && !it->opened && !dirty;
            float dirt = dirty ? 0.65f : sterile ? 0.0f : d.dressing == DressingKind::Rag ? 0.12f : 0.06f;
            Dressing old = player_.body.applyDressing((BodyPart)part, d.dressing, sterile, dirt);
            useOne(player_.inventory.indexOfUid(it->uid));   // (before giving anything back: `it` may move)
            std::string where = bodyPartName((BodyPart)part);
            const char* verb = d.dressing == DressingKind::Bandaid ? "You stick a bandaid on your "
                             : d.dressing == DressingKind::Bandage ? "You wrap a bandage around your " : "You tie a rag around your ";
            log(verb + where + (dirty ? ". It's filthy." : "."));
            if (d.dressing == DressingKind::Bandaid && player_.body.worstSeverityAt((BodyPart)part) > 0.35f)
                log("It's far too small for this.", MessageKind::Bad);
            returnUsedDressing(old);
            break;
        }
        case ActionKind::RemoveDressing: {
            if (o.part < 0) break;
            Dressing old = player_.body.removeDressing((BodyPart)o.part);
            if (!old.on()) break;
            log(std::string("You take the ") + dressingName(old.kind) + " off your " + bodyPartName((BodyPart)o.part) + ".");
            returnUsedDressing(old);
            break;
        }
        case ActionKind::Disinfect: {
            if (!it) break;
            int part = o.part >= 0 ? o.part : dirtiestWoundPart();
            if (part < 0) break;
            bool weak = it->def->hasTag("weak_disinfectant");
            player_.body.disinfect((BodyPart)part, weak ? 0.75f : 1.0f);
            log(std::string("You clean the wound on your ") + bodyPartName((BodyPart)part) + (weak ? ". The alcohol burns." : ". It stings."));
            if (--it->usesLeft <= 0) takeItem(o.item);
            break;
        }
        case ActionKind::WashItem: {
            if (!it || it->def->washesInto.empty()) break;
            if (!waterForWashing(true)) { log("There's no water here to wash it in."); break; }
            ItemInstance clean = items_.create(it->def->washesInto, rng_);
            clean.opened = true;   // clean, but not sterile any more
            std::string name = lower(it->def->name);
            takeItem(o.item);
            giveItem(clean);
            log("You scrub the " + name + " clean in the water and wring it out.");
            break;
        }
        case ActionKind::PlaceStove: {
            if (!it) break;
            Vec2 at = player_.pos + Vec2{std::cos(player_.facing), std::sin(player_.facing)} * 0.8f;
            Campfire f;
            f.pos = Vec2{std::floor(at.x) + 0.5f, std::floor(at.y) + 0.5f};
            f.id = world_->newId();
            f.stove = true;
            f.fuelMl = it->liquidMl;   // whatever was left in the burner
            world_->campfires().push_back(f);
            takeItem(o.item);
            log("You set up the storm kitchen: burner, windshield, pot on top.");
            break;
        }
        case ActionKind::FillStove: {
            Campfire* f = nullptr;
            for (auto& c : world_->campfires()) if (c.id == o.targetId) f = &c;
            int fuel = player_.inventory.findTag("stove_fuel");
            if (!f || fuel < 0) break;
            if (f->lit) {
                // Pouring into a lit burner: the invisible flame runs up the stream into the bottle
                ItemInstance bottle = player_.inventory.take(fuel);
                (void)bottle;
                Wound w;
                w.part = BodyPart::RightHand; w.type = WoundType::Burn;
                w.severity = rng_.range(0.4f, 0.65f); w.bleeding = 0; w.contamination = 0.15f;
                player_.body.addWound(w);
                Wound w2 = w;
                w2.part = BodyPart::RightArm; w2.severity = rng_.range(0.15f, 0.35f);
                player_.body.addWound(w2);
                player_.body.mutableState().sting = 1.0f;
                Campfire spill;
                spill.pos = f->pos + Vec2{0.6f, 0.3f};
                spill.id = world_->newId();
                spill.lit = true;
                spill.fuelMinutes = 6.0f;   // the spilled alcohol burns out on the ground
                world_->campfires().push_back(spill);
                log("You pour while it's still burning. The flame races up into the bottle - it bursts alight in your hand!",
                    MessageKind::Bad);
                break;
            }
            ItemInstance& b = player_.inventory.items()[fuel];
            f->fuelMl = 100.0f;
            if (--b.usesLeft <= 0) player_.inventory.take(fuel);
            log("You fill the burner with red alcohol.");
            break;
        }
        case ActionKind::LightStove:
            for (auto& f : world_->campfires())
                if (f.id == o.targetId && f.fuelMl > 0) {
                    useOne(player_.inventory.findTag("igniter"));
                    f.lit = true;
                    f.fuelMinutes = f.fuelMl / 4.0f;
                    log("A pale blue flame rises from the burner.", MessageKind::Good);
                }
            break;
        case ActionKind::StoveOff:
            for (auto& f : world_->campfires())
                if (f.id == o.targetId) { f.lit = false; log("You put the simmer lid over the burner. The flame dies."); }
            break;
        case ActionKind::PackStove: {
            auto& fires = world_->campfires();
            for (size_t i = 0; i < fires.size(); ++i)
                if (fires[i].id == o.targetId && fires[i].stove && !fires[i].lit) {
                    ItemInstance st = items_.create("storm_kitchen", rng_);
                    st.liquidMl = fires[i].fuelMl;
                    fires.erase(fires.begin() + i);
                    giveItem(st);
                    log("You pack the storm kitchen away.");
                    break;
                }
            break;
        }
        case ActionKind::PitchTent: {
            if (!it || !it->def->hasTag("tent")) break;
            IRect r = tentFootprint(*it->def);
            std::string why;
            if (!world_->canPitch(r, &why)) { log(why + "."); break; }
            Tent t;
            t.id = world_->newId();
            t.rect = r;
            t.dir = facingDir();
            t.item = it->def->id;
            world_->tents().push_back(t);
            takeItem(o.item);
            log("You pitch the tent and peg it down.", MessageKind::Good);
            break;
        }
        case ActionKind::PackTent: {
            auto& tents = world_->tents();
            for (size_t i = 0; i < tents.size(); ++i)
                if (tents[i].id == o.targetId) {
                    giveItem(items_.create(tents[i].item, rng_));
                    tents.erase(tents.begin() + i);
                    log("You pull up the pegs and roll the tent up.");
                    break;
                }
            break;
        }
        case ActionKind::SawPlanks: {
            if (!it) break;
            takeItem(o.item);
            for (int i = 0; i < 3; ++i) giveItem(items_.create("plank", rng_));
            sound(Sfx::Chop, player_.pos);
            log("You saw the log into three planks.", MessageKind::Good);
            break;
        }
        case ActionKind::SplitLog: {
            if (!it) break;
            takeItem(o.item);
            for (int i = 0; i < 4; ++i) giveItem(items_.create("firewood", rng_));
            sound(Sfx::Chop, player_.pos);
            log("You split the log into firewood.");
            break;
        }
        case ActionKind::ReplaceBatteries: {
            int light = player_.inventory.find([](const ItemInstance& i) { return i.def->hasTag("light") && i.usesLeft < i.def->uses; });
            if (light < 0 || !it) break;
            ItemInstance& l = player_.inventory.items()[light];
            l.usesLeft = l.def->uses;
            std::string name = lower(l.def->name);
            useOne(player_.inventory.indexOfUid(it->uid));
            log("You put fresh batteries in the " + name + ".");
            break;
        }
        case ActionKind::Fish: {
            float h = world_->clock().hourOfDay();
            bool bite = (h < 8.0f || h > 18.0f) ? rng_.chance(0.5f) : rng_.chance(0.3f);
            if (bite) {
                world_->dropItem(player_.pos, items_.create("fish_raw", rng_));
                log("Something pulls the line. You land a perch!", MessageKind::Good);
            } else {
                log("Not a single bite.");
            }
            break;
        }
        case ActionKind::Build: {
            BuildKind k = (BuildKind)o.targetId;
            const BuildRecipe* r = recipe(k);
            ActionOption check = buildOption(k, o.x, o.y, o.dir);
            if (!r || !check.enabled) { log(check.reason + "."); break; }
            // use the materials
            int planks = r->planks, nails = r->nails;
            for (int i = (int)player_.inventory.size() - 1; i >= 0 && planks > 0; --i)
                if (player_.inventory.items()[i].def->hasTag("plank")) { player_.inventory.take(i); --planks; }
            for (int i = (int)player_.inventory.size() - 1; i >= 0 && nails > 0; --i) {
                ItemInstance& n = player_.inventory.items()[i];
                if (!n.def->hasTag("nails")) continue;
                int use = std::min(nails, n.usesLeft);
                n.usesLeft -= use;
                nails -= use;
                if (n.usesLeft <= 0) player_.inventory.take(i);
            }
            if (r->tarp) player_.inventory.take(player_.inventory.findTag("tarp"));
            world_->addBuild(Built{k, o.x, o.y, o.dir});
            sound(Sfx::Chop, player_.pos);
            log(std::string("You finish the ") + buildKindName(k) + ".", MessageKind::Good);
            break;
        }
        case ActionKind::Dismantle: {
            Built b{(BuildKind)o.targetId, o.x, o.y, o.dir};
            const BuildRecipe* r = recipe(b.kind);
            if (b.kind == BuildKind::Crate) {   // tip out what was in it
                for (auto& item : world_->container(b.x, b.y).items) world_->dropItem(Vec2{b.x + 0.5f, b.y + 0.5f}, item);
                world_->container(b.x, b.y).items.clear();
            }
            if (!world_->removeBuild(b)) break;
            int back = r ? (r->planks + 1) / 2 : 1;
            for (int i = 0; i < back; ++i) world_->dropItem(player_.pos, items_.create("plank", rng_));
            sound(Sfx::Chop, player_.pos);
            log(std::string("You take the ") + buildKindName(b.kind) + " apart and save " + std::to_string(back) +
                (back == 1 ? " plank." : " planks."));
            break;
        }
        case ActionKind::TearUpSheet: {
            int n = rng_.rangeInt(4, 6);
            for (int i = 0; i < n; ++i) giveItem(items_.create("rag", rng_));
            world_->markHarvested(o.x, o.y);
            log("You strip the bed and tear the sheet into " + std::to_string(n) + " rags.");
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
            int n = std::max(1, it->def->rags);
            std::string name = lower(it->def->name);
            takeItem(o.item);
            for (int i = 0; i < n; ++i) giveItem(items_.create("rag", rng_));
            log("You rip the " + name + " into " + std::to_string(n) + (n == 1 ? " rag." : " rags."));
            break;
        }
        default: break;
    }
}

} // namespace alone
