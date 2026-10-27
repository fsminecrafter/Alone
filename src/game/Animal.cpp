#include "game/Animal.h"
#include "core/Config.h"
#include "core/Log.h"
#include "world/World.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace alone {

namespace {
Zone zoneFromString(const std::string& s) {
    static const char* n[] = {"unknown", "water", "forest", "woodland", "park", "residential", "field", "meadow", "commercial", "hospital"};
    for (int i = 0; i < (int)Zone::Count; ++i)
        if (s == n[i]) return (Zone)i;
    return Zone::Unknown;
}
}

bool AnimalSystem::load(const std::string& path) {
    Config cfg;
    if (!cfg.load(path)) return false;
    for (const auto& s : cfg.sections()) {
        if (s.name.empty()) continue;
        AnimalDef d;
        d.id = s.name;
        d.name = s.str("name", d.id);
        d.hp = s.num("hp", 10);
        d.walkSpeed = s.num("walk_speed", 1);
        d.runSpeed = s.num("run_speed", 8);
        d.sense = s.num("sense", 20);
        d.flee = s.num("flee", 30);
        d.aggression = s.num("aggression", 0);
        d.damage = s.num("damage", 0);
        d.attackCooldown = s.num("attack_cooldown", 2);
        d.size = s.num("size", 0.5f);
        d.density = s.num("density", 1);
        d.butcherSeconds = s.num("butcher_seconds", 20);
        std::string col = s.str("colour", "#806040");
        d.colour = (uint32_t)std::strtoul(col.c_str() + (col[0] == '#' ? 1 : 0), nullptr, 16);
        auto g = split(s.str("group", "1-1"), '-');
        d.groupMin = std::atoi(g[0].c_str());
        d.groupMax = g.size() > 1 ? std::atoi(g[1].c_str()) : d.groupMin;
        for (auto& z : s.list("habitat")) d.habitat.push_back(zoneFromString(z));
        for (auto& y : s.list("yields")) {
            auto p = split(y, ':');
            d.yields.push_back({p[0], p.size() > 1 ? std::atoi(p[1].c_str()) : 1});
        }
        defs_.push_back(d);
    }
    Log::info("Loaded %d animal types", (int)defs_.size());
    return true;
}

const AnimalDef* AnimalSystem::find(const std::string& id) const {
    for (const auto& d : defs_)
        if (d.id == id) return &d;
    return nullptr;
}

Animal* AnimalSystem::find(int id) {
    for (auto& a : animals_)
        if (a.id == id) return &a;
    return nullptr;
}

Animal& AnimalSystem::spawn(const AnimalDef* def, Vec2 pos, Rng& rng) {
    Animal a;
    a.def = def;
    a.id = nextId_++;
    a.pos = pos;
    a.hp = def->hp;
    a.heading = rng.range(0, 2 * kPi);
    a.state = AnimalState::Wander;
    a.wanderTarget = pos;
    animals_.push_back(a);
    return animals_.back();
}

void AnimalSystem::scare(Vec2 from, float radius) {
    for (auto& a : animals_) {
        if (!a.alive()) continue;
        if (distance(a.pos, from) < radius && a.state != AnimalState::Charge) {
            a.state = AnimalState::Flee;
            a.stateTimer = 8;
        }
    }
}

void AnimalSystem::spawnAround(World& world, Vec2 centre, Rng& rng) {
    // Despawn far away animals, then top up each species near the player
    animals_.erase(std::remove_if(animals_.begin(), animals_.end(),
                                  [&](const Animal& a) {
                                      float d = distance(a.pos, centre);
                                      return d > 320.0f || (!a.alive() && (a.butchered || a.deadMinutes > 60 * 24 * 6));
                                  }),
                   animals_.end());
    for (const auto& def : defs_) {
        int count = 0;
        for (const auto& a : animals_)
            if (a.def == &def && a.alive() && distance(a.pos, centre) < 200) ++count;
        if (count >= def.density) continue;
        if (!rng.chance(std::min(1.0f, def.density - count) * 0.5f)) continue;
        // Find a spot out of sight in the right habitat
        for (int attempt = 0; attempt < 8; ++attempt) {
            float ang = rng.range(0, 2 * kPi), r = rng.range(80, 190);
            Vec2 p = centre + Vec2{std::cos(ang), std::sin(ang)} * r;
            if (p.x < 0 || p.y < 0 || p.x > world.map().widthM() || p.y > world.map().heightM()) continue;
            Zone z = world.map().zoneAt(p.x, p.y);
            if (std::find(def.habitat.begin(), def.habitat.end(), z) == def.habitat.end()) continue;
            if (world.tileBlocked((int)p.x, (int)p.y) || world.indoors(p)) continue;
            int n = rng.rangeInt(def.groupMin, def.groupMax);
            for (int i = 0; i < n; ++i) {
                Vec2 q = p + Vec2{rng.range(-3, 3), rng.range(-3, 3)};
                if (!world.tileBlocked((int)q.x, (int)q.y)) spawn(&def, q, rng);
            }
            break;
        }
    }
}

std::vector<AnimalSystem::Attack> AnimalSystem::update(float dt, float gameMinutes, World& world,
                                                       const PlayerPresence& player, Rng& rng) {
    std::vector<Attack> attacks;
    spawnTimer_ -= dt;
    if (spawnTimer_ <= 0) {
        spawnTimer_ = 4.0f;
        spawnAround(world, player.pos, rng);
    }
    float daylight = world.clock().daylight();
    for (auto& a : animals_) {
        if (!a.alive()) {
            a.deadMinutes += gameMinutes;
            continue;
        }
        const AnimalDef& d = *a.def;
        float dist = distance(a.pos, player.pos);
        // Hearing, plus sight in the open during the day
        float notice = std::max(player.noise * (d.sense / 18.0f), d.sense * (0.4f + 0.6f * daylight));
        bool aware = player.alive && dist < notice && (dist < player.noise || world.lineOfSight(a.pos, player.pos));

        a.stateTimer -= dt;
        a.attackTimer -= dt;
        switch (a.state) {
            case AnimalState::Idle:
            case AnimalState::Wander:
                if (aware && dist < d.flee) {
                    if (d.aggression > 0 && dist < 14 && rng.chance(d.aggression)) { a.state = AnimalState::Charge; a.stateTimer = 10; }
                    else { a.state = AnimalState::Flee; a.stateTimer = rng.range(5, 10); }
                } else if (a.stateTimer <= 0) {
                    if (rng.chance(0.5f)) { a.state = AnimalState::Idle; a.stateTimer = rng.range(3, 12); }
                    else {
                        a.state = AnimalState::Wander;
                        a.stateTimer = rng.range(4, 10);
                        a.wanderTarget = a.pos + Vec2{rng.range(-15, 15), rng.range(-15, 15)};
                    }
                }
                break;
            case AnimalState::Flee:
                if (a.stateTimer <= 0 && (!aware || dist > d.flee * 1.5f)) { a.state = AnimalState::Wander; a.stateTimer = 3; }
                break;
            case AnimalState::Charge:
                if (dist < d.size * 0.5f + 0.9f && a.attackTimer <= 0) {
                    attacks.push_back({d.damage * rng.range(0.6f, 1.2f), &d});
                    a.attackTimer = d.attackCooldown;
                    if (rng.chance(0.4f)) { a.state = AnimalState::Recover; a.stateTimer = rng.range(2, 4); }
                }
                if (a.stateTimer <= 0 || dist > 30 || !player.alive) { a.state = AnimalState::Flee; a.stateTimer = 5; }
                break;
            case AnimalState::Recover:
                if (a.stateTimer <= 0) { a.state = AnimalState::Charge; a.stateTimer = 6; }
                break;
            default: break;
        }

        Vec2 desired;
        float speed = 0;
        switch (a.state) {
            case AnimalState::Wander:
                if (distance(a.pos, a.wanderTarget) > 1) { desired = (a.wanderTarget - a.pos).normalized(); speed = d.walkSpeed; }
                break;
            case AnimalState::Flee: {
                Vec2 away = (a.pos - player.pos).normalized();
                float jink = std::sin((a.id * 13.0f) + a.stateTimer * 1.7f) * 0.5f;
                desired = Vec2{away.x - away.y * jink, away.y + away.x * jink}.normalized();
                speed = d.runSpeed * (a.hp < d.hp * 0.5f ? 0.6f : 1.0f);
                break;
            }
            case AnimalState::Charge:
                desired = (player.pos - a.pos).normalized();
                speed = d.runSpeed * 0.8f;
                break;
            case AnimalState::Recover: {
                Vec2 away = (a.pos - player.pos).normalized();
                desired = away; speed = d.walkSpeed * 2;
                break;
            }
            default: break;
        }
        // smooth turning / acceleration
        Vec2 targetVel = desired * speed;
        float k = std::min(1.0f, dt * 4.0f);
        a.vel = a.vel + (targetVel - a.vel) * k;
        if (a.vel.lengthSq() > 0.01f) {
            Vec2 before = a.pos;
            a.pos = world.moveCircle(a.pos, a.vel * dt, std::min(0.45f, d.size * 0.35f));
            if (distance(before, a.pos) < a.vel.length() * dt * 0.3f) {
                // stuck: pick another direction
                a.wanderTarget = a.pos + Vec2{rng.range(-10, 10), rng.range(-10, 10)};
                if (a.state == AnimalState::Wander) a.stateTimer = 0;
                a.vel = Vec2{rng.range(-1, 1), rng.range(-1, 1)} * speed;
            }
            a.heading = std::atan2(a.vel.y, a.vel.x);
        }
    }
    return attacks;
}

} // namespace alone
