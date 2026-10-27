#pragma once
#include "game/Animal.h"
#include "items/Inventory.h"
#include "items/ItemRegistry.h"
#include "survival/Body.h"
#include "survival/Symptoms.h"
#include "world/World.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>

namespace alone {

// Where an item lives.
enum class ItemLoc { Player, Worn, Container, Ground };

struct ItemRef {
    ItemLoc loc = ItemLoc::Player;
    int x = 0, y = 0;            // container tile
    int64_t groundKey = 0;       // ground stack
    uint32_t uid = 0;
};

enum class ActionKind {
    None,
    // world
    ToggleDoor, Search, OpenContainer, DrinkSource, FillBottle, PickBerries, PickRoseHips, Forage,
    GatherSticks, ChopTree, BuildFire, LightFire, AddFuel, CookItem, BoilWater, SetSnare, CheckSnare,
    TakeSnare, Butcher, PickUpCarcass, Sleep, Rest, Wait,
    // items
    Eat, Drink, OpenCan, Equip, Unequip, Wear, TakeOff, Drop, PickUp, Bandage, Disinfect,
    TakePill, Purify, PourOut, TearIntoRags, Read,
};

struct ActionOption {
    std::string label;
    ActionKind kind = ActionKind::None;
    int x = 0, y = 0, dir = 0;   // target tile / door edge
    int targetId = 0;            // animal / fire / snare id
    ItemRef item;                // item involved
    float amount = 1;            // e.g. fraction to eat
    bool enabled = true;
    std::string reason;          // why it's disabled
};

// A timed action in progress, with a progress bar.
struct CurrentAction {
    ActionOption option;
    float duration = 0;          // real seconds
    float elapsed = 0;
    bool active = false;
    float timeScale = 1;         // speeds up the clock (sleeping/waiting)
};

enum class MessageKind { Info, Good, Bad, Body };

// Things the client may want to play a sound for.
enum class Sfx { Footstep, Gunshot, Swing, Hit, Door, Click, Chop, Eat, Drink, Hurt };
struct SoundEvent { Sfx type; Vec2 pos; };
struct Message {
    std::string text;
    MessageKind kind = MessageKind::Info;
    float age = 0;
};

struct PlayerInput {
    Vec2 move;                   // desired direction in world space (length 0..1)
    bool run = false;
    bool sneak = false;
    Vec2 aim;                    // world point under the cursor
    bool attack = false;         // pressed this frame
};

struct Player {
    Vec2 pos;
    float facing = 0;            // radians
    float stamina = 1;           // 0..1 short-term breath
    bool moving = false, running = false, sneaking = false, asleep = false;
    float attackTimer = 0;
    float swing = 0;             // animation 0..1 after an attack
    Body body;
    Inventory inventory;
    std::array<ItemInstance, (size_t)ClothingSlot::Count> worn{};
    uint32_t weaponUid = 0;      // equipped weapon (in inventory)
    float noise = 8;             // metres others can hear you this tick
    float vomitTimer = 0;
    bool flashlightOn = false;
    float lastAttack = 100;      // seconds since the player last fought
};

struct DeathInfo {
    DeathCause cause = DeathCause::None;
    float daysSurvived = 0;
    std::string place;
};

// The whole simulation. Owns the world, the player and the wildlife.
// The client only feeds it input and draws what it exposes.
class SaveGame;

class Game {
    friend class SaveGame;
public:
    bool loadData();                 // items, map, symptoms, animals (once)
    void newGame(uint32_t seed);
    void update(float realDt, const PlayerInput& input);

    // --- state -----------------------------------------------------------
    bool running() const { return world_ != nullptr; }
    World& world() { return *world_; }
    Player& player() { return player_; }
    const Player& player() const { return player_; }
    AnimalSystem& animals() { return animals_; }
    const ItemRegistry& items() const { return items_; }
    const MapData& map() const { return map_; }
    const Symptoms& symptoms() const { return symptoms_; }
    std::vector<ActiveSymptom> currentSymptoms() const;
    const std::deque<Message>& messages() const { return messages_; }
    const CurrentAction& currentAction() const { return action_; }
    bool dead() const { return player_.body.dead(); }
    const DeathInfo& death() const { return death_; }
    std::string location() const;
    float gameMinutesPerSecond() const { return minutesPerSecond_ * speed_; }
    void setTimeSpeed(float s) { speed_ = s; }
    float timeSpeed() const { return speed_; }
    void setBaseTimeScale(float minutesPerSecond) { minutesPerSecond_ = minutesPerSecond; }
    bool knowsMushrooms() const;
    float carryCapacityKg() const;
    float carriedKg() const;
    Environment environment();
    const ItemInstance* weapon() const;

    // Container the player has open (search finished), or none.
    bool hasOpenContainer() const { return openContainer_; }
    int openContainerX() const { return openX_; }
    int openContainerY() const { return openY_; }
    void closeContainer() { openContainer_ = false; }

    // --- actions -------------------------------------------------------------
    std::vector<ActionOption> actionsAt(Vec2 worldPoint);
    std::vector<ActionOption> actionsForItem(const ItemRef& ref);
    void perform(const ActionOption& option);
    void cancelAction();
    bool transfer(const ItemRef& ref, ItemLoc to);   // move between player / container / ground
    ItemInstance* resolve(const ItemRef& ref);
    std::string itemName(const ItemInstance& it) const { return it.displayName(knowsMushrooms()); }

    void log(const std::string& text, MessageKind kind = MessageKind::Info);
    std::vector<SoundEvent> takeSounds() { std::vector<SoundEvent> s; s.swap(sounds_); return s; }
    void sound(Sfx s, Vec2 at) { if (sounds_.size() < 64) sounds_.push_back({s, at}); }

    // Saving: one file, plain text. Returns false (and logs why) on failure.
    bool save(const std::string& path);
    bool load(const std::string& path);
    uint32_t seed() const { return seed_; }

private:
    void updatePlayer(float dt, float gameMinutes, const PlayerInput& in);
    void updateCombat(float dt, const PlayerInput& in);
    void finishAction();
    void startTimed(const ActionOption& o, float seconds, float timeScale = 1);
    void handleBodyEvents();
    void eat(ItemInstance& it, float fraction);
    void drinkFrom(ItemInstance& it);
    void drinkWater(float ml, bool safe, const char* sourceDesc);
    ItemInstance takeItem(const ItemRef& ref);
    void giveItem(const ItemInstance& it);       // to inventory (or ground if too heavy)
    int findFuel() const;
    float fireReach() const { return 2.2f; }
    void playerHurt(float severity, const AnimalDef* by);

    ItemRegistry items_;
    MapData map_;
    Symptoms symptoms_;
    AnimalSystem animals_;
    std::unique_ptr<World> world_;
    Player player_;
    Rng rng_;
    CurrentAction action_;
    std::deque<Message> messages_;
    std::vector<SoundEvent> sounds_;
    float stepDistance_ = 0;
    DeathInfo death_;
    uint32_t seed_ = 0;
    float minutesPerSecond_ = 1.0f;   // 1 game minute per real second: a day lasts 24 minutes
    float speed_ = 1.0f;
    bool openContainer_ = false;
    int openX_ = 0, openY_ = 0;
    float sneezeTimer_ = 0;
    float symptomMsgTimer_ = 0;
    struct Announced { int level = 0; float calm = 0; };   // calm: seconds spent below that level
    std::map<std::string, Announced> lastSymptomLevels_;
    float flashlightAcc_ = 0;
};

} // namespace alone
