#pragma once
#include "core/Math.h"
#include "core/Rng.h"
#include "world/Terrain.h"
#include <string>
#include <vector>

namespace alone {

class World;

struct AnimalDef {
    std::string id, name;
    float hp = 10, walkSpeed = 1, runSpeed = 8, sense = 20, flee = 30, aggression = 0;
    float damage = 0, attackCooldown = 2, size = 0.5f, density = 1;
    int groupMin = 1, groupMax = 1;
    float butcherSeconds = 20;
    uint32_t colour = 0x806040;
    std::vector<Zone> habitat;
    std::vector<std::pair<std::string, int>> yields;
};

enum class AnimalState { Idle, Wander, Flee, Charge, Recover, Dead };

struct Animal {
    const AnimalDef* def = nullptr;
    int id = 0;
    Vec2 pos;
    Vec2 vel;
    float heading = 0;
    float hp = 1;
    AnimalState state = AnimalState::Idle;
    float stateTimer = 0;
    float attackTimer = 0;
    Vec2 wanderTarget;
    bool butchered = false;
    float deadMinutes = 0;   // game minutes since death (corpses rot)
    bool alive() const { return state != AnimalState::Dead; }
};

// What an animal knows about the player each tick.
struct PlayerPresence {
    Vec2 pos;
    float noise = 10;        // metres a sound carries this tick
    bool alive = true;
};

class AnimalSystem {
public:
    bool load(const std::string& path);
    const std::vector<AnimalDef>& defs() const { return defs_; }
    const AnimalDef* find(const std::string& id) const;

    std::vector<Animal>& animals() { return animals_; }
    const std::vector<Animal>& animals() const { return animals_; }

    // Returns damage dealt to the player this tick (0..1 severity) and by whom.
    struct Attack { float severity = 0; const AnimalDef* by = nullptr; };
    std::vector<Attack> update(float realDt, float gameMinutes, World& world, const PlayerPresence& player, Rng& rng);

    void spawnAround(World& world, Vec2 centre, Rng& rng);
    Animal& spawn(const AnimalDef* def, Vec2 pos, Rng& rng);
    void scare(Vec2 from, float radius);     // gunshot etc.
    Animal* find(int id);

private:
    std::vector<AnimalDef> defs_;
    std::vector<Animal> animals_;
    int nextId_ = 1;
    float spawnTimer_ = 0;
};

} // namespace alone
