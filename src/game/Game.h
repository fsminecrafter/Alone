#pragma once
#include "game/Animal.h"
#include "game/Survivor.h"
#include "game/Vehicle.h"
#include "items/Inventory.h"
#include "items/ItemRegistry.h"
#include "survival/Body.h"
#include "survival/Symptoms.h"
#include "world/World.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <set>
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
    // body (health screen)
    RemoveDressing, WashItem, TearUpSheet,
    // camping, fishing, carpentry
    PlaceStove, FillStove, LightStove, StoveOff, PackStove, PitchTent, PackTent,
    SawPlanks, SplitLog, ReplaceBatteries, Fish, Build, Dismantle,
    // people
    Talk,
    // vehicles
    TryCarDoor, SmashCarWindow, EnterVehicle, ExitVehicle, StartEngine, StopEngine, Hotwire, Refuel, SearchGlovebox, MountBike,
    Siphon, FillFromPump,
    // weapons
    Reload, Stow,
    // trapping
    SetFishTrap, EmptyTrap,
    // preserving
    SaltFood,
    // floors
    UseStairs, RepairStairs,
    // windows
    BreakWindow, ClimbWindow,
};

// Heavy enough to smash glass with (axe, hammer, crowbar, bat...)
bool heavyTool(const ItemDef* d);

struct ActionOption {
    std::string label;
    ActionKind kind = ActionKind::None;
    int x = 0, y = 0, dir = 0;   // target tile / door edge
    int targetId = 0;            // animal / fire / snare id
    int part = -1;               // body part (health actions); -1 = pick the worst wound
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
enum class Sfx { Footstep, Gunshot, Swing, Hit, Door, Click, Chop, Eat, Drink, Hurt, Glass, Engine, Bowshot };
struct SoundEvent { Sfx type; Vec2 pos; };
struct Message {
    std::string text;
    MessageKind kind = MessageKind::Info;
    float age = 0;
};

struct PlayerInput {
    Vec2 move;                   // desired direction in world space (length 0..1)
    bool run = false;
    bool sneak = false;          // crouch (C)
    bool aimHold = false;        // Ctrl held: face the cursor and aim
    Vec2 aim;                    // world point under the cursor
    bool attack = false;         // pressed this frame
    // Driving (bike or car): -1..1, from W/S and A/D
    float throttle = 0;          // + accelerate, - brake / reverse
    float steer = 0;             // + turns the heading angle up (right, as seen on screen)
};

// Who you are: picked when you start a new game.
struct PlayerLook {
    std::string name = "Alex";
    int age = 30;
    Gender gender = Gender::Man;
    uint32_t skin = 0xe2b896, hair = 0x4a3626;
    int hairStyle = 0;
};

// The character you make at the start: looks and what you're wearing
// (an item id per clothing slot, empty = nothing; tint 0 = the item's own colour).
struct CharacterChoice {
    PlayerLook look;
    std::array<std::string, (size_t)ClothingSlot::Count> wear{};
    std::array<uint32_t, (size_t)ClothingSlot::Count> tint{};
    static CharacterChoice defaults();
};

struct Player {
    PlayerLook look;
    Vec2 pos;
    float facing = 0;            // radians
    float stamina = 1;           // 0..1 short-term breath
    bool moving = false, running = false, sneaking = false, asleep = false;
    bool aiming = false;          // holding Ctrl: facing and aiming at the cursor
    bool knowsCarpentry = false;  // read a carpentry book: doors, windows, roofs
    float carpentryXp = 0;        // practice: see Game::carpentryLevel
    bool knowsHotwire = false;    // someone showed you how to start a car without the key
    bool knowsMushroomsTaught = false;
    int vehicle = -1;             // id of the bike or car you're on/in, -1 on foot
    float grade = 0;             // slope of the ground you're walking along: rise per metre, + uphill
    float fallen = 0;            // seconds left lying on the ground after tripping
    float attackTimer = 0;
    float swing = 0;             // animation 0..1 after an attack
    Body body = Body(1);
    Inventory inventory;
    std::array<ItemInstance, (size_t)ClothingSlot::Count> worn{};
    uint32_t weaponUid = 0;      // equipped weapon (in inventory)
    uint32_t stowedUid = 0;      // weapon slung on your back (in inventory); 1 swaps it into your hands
    float noise = 8;             // metres others can hear you this tick
    float vomitTimer = 0;
    bool flashlightOn = false;
    float lastAttack = 100;      // seconds since the player last fought
};

// How well you'd sleep where you are: what helps and what doesn't.
struct SleepComfort {
    float score = 0;                    // -1 miserable .. 1 very comfortable
    std::vector<std::string> good, bad;
    const char* word() const;
};

// A piece of what happened to the world (data/text/lore.ini), learned from
// documents and from people
struct LoreEntry {
    std::string id, title, text;
    int order = 0;
};

// A place someone told you about, marked on the map
struct KnownPlace {
    std::string name;
    Vec2 pos;
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
    void newGame(uint32_t seed, const CharacterChoice* who = nullptr);
    // Change who you are (the character screen): looks and clothes, replacing what's worn
    void dressPlayer(const CharacterChoice& who);
    // Sip clean water from an open bottle by yourself when thirsty
    void setAutoDrink(bool on) { autoDrink_ = on; }
    bool autoDrink() const { return autoDrink_; }
    void update(float realDt, const PlayerInput& input);

    // --- state -----------------------------------------------------------
    bool running() const { return world_ != nullptr; }
    World& world() { return *world_; }
    Player& player() { return player_; }
    const Player& player() const { return player_; }
    AnimalSystem& animals() { return animals_; }
    // 0..1: how much the player is being hunted right now (drives the danger music)
    float threat() const;
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
    SleepComfort sleepComfort();     // sleeping here, now
    const ItemInstance* weapon() const;

    // --- other people ---------------------------------------------------------
    std::vector<Survivor>& survivors() { return survivors_; }
    const std::vector<Survivor>& survivors() const { return survivors_; }
    Survivor* survivor(int id);
    const std::vector<SurvivorDef>& survivorDefs() const { return survivorDefs_; }
    // What you can say to them right now, with labels
    std::vector<std::pair<Topic, std::string>> talkTopics(int id);
    // Why flirting isn't on offer right now (empty when it is, or it can't ever be)
    std::string romanceHint(int id);
    // In a tent with a companion: who (0 if nobody)
    int tentMate() const { return tentMate_; }
    const std::vector<KnownPlace>& knownPlaces() const { return knownPlaces_; }
    bool knowsPlace(const std::string& name) const;
    // What happened to the world: entries in story order, and the ones you know
    const std::vector<LoreEntry>& lore() const { return lore_; }
    bool knowsLore(const std::string& id) const;
    // Adds it to the journal; fromDocument: the client opens the journal on it
    bool learnLore(const std::string& id, bool fromDocument);
    std::string takeLoreOpened() { std::string s = loreOpened_; loreOpened_.clear(); return s; }
    static int carSeats(const Vehicle& v) { return ((v.variant >> 4) % 4) == 3 ? 7 : 5; }   // a van seats seven
    // Weapons: the one on your back, swapping (key 1), reloading (R)
    const ItemInstance* stowedWeapon() const;
    float armourAt(BodyPart part) const;           // what your helmet / body armour takes off a hit there
    void toggleStow();
    int ammoCarried(const ItemDef* gun) const;    // rounds for this gun in your bags
    bool canReload() const;                        // holding a gun that isn't full, with rounds for it
    void reload();
    bool inTentWith(int survivorId);
    // Someone walked up to you and wants to talk (0 if nobody); taking it clears it
    int takeTalkRequest() { int id = talkRequest_; talkRequest_ = 0; return id; }
    // Say it; returns their answer
    std::string talk(int id, Topic t);
    std::string greeting(int id);      // what they say when you walk up (meets them the first time)
    // How you'd describe where you stand with them (Enemy ... Lover)
    Bond bondWith(int id) const;
    // A line with {name} etc. filled in for the player
    std::string personal(const std::string& line) const;
    // Can you talk to them from where you are? (hostile people: shout from a distance)
    bool canTalk(int id, std::string* why = nullptr) const;
    float talkRange(int id) const;
    // Trading: what an item is worth to people now (food, medicine, ammo...)
    int itemValue(const ItemInstance& it) const;
    // Would they take this? why: what they say if not
    bool tradeFair(int id, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs, std::string* why) const;
    bool trade(int id, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs);
    int followers() const;
    // The goal shown in the journal and the HUD
    std::string objective() const;

    // --- vehicles -------------------------------------------------------------
    std::vector<Vehicle>& vehicles() { return vehicles_; }
    const std::vector<Vehicle>& vehicles() const { return vehicles_; }
    Vehicle* vehicle(int id);
    Vehicle* playerVehicle() { return player_.vehicle >= 0 ? vehicle(player_.vehicle) : nullptr; }
    const Vehicle* playerVehicle() const { return const_cast<Game*>(this)->playerVehicle(); }
    float carFuelAt(int x, int y) const;   // litres in a parked car's tank (0-40% of it)

    // Container the player has open (search finished), or none.
    bool hasOpenContainer() const { return openContainer_; }
    int openContainerX() const { return openX_; }
    int openContainerY() const { return openY_; }
    void closeContainer() { openContainer_ = false; }

    // --- actions -------------------------------------------------------------
    std::vector<ActionOption> actionsAt(Vec2 worldPoint);
    std::vector<ActionOption> actionsForItem(const ItemRef& ref);
    // Carpentry (the build menu)
    struct BuildRecipe {
        BuildKind kind;
        const char* name;
        int planks, nails;
        bool saw, tarp, book;
        float seconds;
        const char* description;
        int level = 0;                 // carpentry level needed (see carpentryLevel)
        const char* extraTag = nullptr;   // one more thing it uses up (an item tag), e.g. rope
        const char* extraName = nullptr;
    };
    // Carpentry skill: 0 Clumsy .. 5 Master. Building, sawing and repairs teach it.
    static int carpentryLevelFor(float xp);
    static float carpentryXpFor(int level);
    static const char* carpentryLevelName(int level);
    int carpentryLevel() const { return carpentryLevelFor(player_.carpentryXp); }
    void carpentryPractice(float xp);
    // What a raw food becomes when dried/smoked/salted: several pieces, as
    // much as is left of it once the water's gone
    std::vector<ItemInstance> preserve(const ItemInstance& raw, const std::string& productId);
    void updateDryingRacks(float gameMinutes);
    // Upper floors and basements: the floor made (if needed), the way there
    int ensureFloor(int baseId, int level);
    int floorLevel() const { return floorLevelAt(player_.pos.x); }
    static const std::vector<BuildRecipe>& buildRecipes();
    static const BuildRecipe* recipe(BuildKind k);
    bool canAffordBuild(BuildKind k, std::string* why) const;
    // An option that builds a piece there (enabled or with the reason it can't)
    ActionOption buildOption(BuildKind k, int x, int y, int dir);
    // Right-click on a body part in the health screen
    std::vector<ActionOption> actionsForBodyPart(BodyPart part);
    // What you can see when you look at a body part (wounds and dressings)
    std::vector<std::string> describeBodyPart(BodyPart part) const;
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
    SleepComfort sleepComfortIn(const Environment& env);
    void sleepReport();              // on waking: how you slept
    void updatePlayer(float dt, float gameMinutes, const PlayerInput& in);
    void updateCombat(float dt, const PlayerInput& in);
    void finishAction();
    void startTimed(const ActionOption& o, float seconds, float timeScale = 1);
    void handleBodyEvents();
    void soilDressings(float gameMinutes);        // dirt from walking and working gets into dressings
    bool waterForWashing(bool consume);           // a lake, tap or bottle within reach
    int worstWoundPart(DressingKind forKind) const;   // -1 if none needs it
    int facingDir() const;                        // 0 N, 1 E, 2 S, 3 W
    IRect tentFootprint(const ItemDef& tent) const;
    int dirtiestWoundPart() const;
    void returnUsedDressing(const Dressing& d);    // what you get back when you take one off
    void eat(ItemInstance& it, float fraction);
    void drinkFrom(ItemInstance& it);
    void drinkWater(float ml, bool safe, const char* sourceDesc);
    ItemInstance takeItem(const ItemRef& ref);
    void giveItem(const ItemInstance& it);       // to inventory (or ground if too heavy)
    int findFuel() const;
    float fireReach() const { return 2.2f; }
    void playerHurt(float severity, const AnimalDef* by);
    void playerHurtBy(float severity, const std::string& who, bool gunshot);

    // people (GameSurvivors.cpp)
    bool loadSurvivors(const std::string& path);
    void placeSurvivors();
    bool resolvePlace(const std::string& spec, Vec2& out, std::string* name);
    void updateSurvivors(float dt, float gameMinutes);
    void updateHomeLife(Survivor& s, float dt, bool sees, Vec2& goal, float& speed, bool& face);
    bool sleepTime(const Survivor& s) const;      // their night (bedtime .. wake-up)
    void findBed(Survivor& s);
    bool planPath(Survivor& s, Vec2 to);           // tiles through the house, round walls (doors are fine)
    Vec2 roomSpot(const Survivor& s);              // somewhere to stand in their house
    // Where to head next on the way to `goal`: the goal itself when the way is
    // clear, otherwise the next turn of a way round props, water and walls
    Vec2 navigate(Survivor& s, Vec2 goal, float dt);
    bool walkClear(Vec2 a, Vec2 b, float radius);  // a body of `radius` could walk straight from a to b (doors count as open)
    bool planRoute(Vec2 from, Vec2 to, std::vector<Vec2>& out);
    void openDoorAhead(Survivor& s, Vec2 dir);     // opens a closed door they're about to walk into
    void wakeSurvivor(Survivor& s);
    void announceBond(Survivor& s);                // logs when you've become friends, etc.
    void addTrust(Survivor& s, float amount, float cap = 100);
    std::string line(const std::string& key);     // a generic line from data/text/dialogue.ini
    std::string pickOf(const std::vector<std::string>& v, const std::string& fallbackKey);
    bool loadDialogue(const std::string& path);
    std::map<std::string, std::vector<std::string>> dialogue_;
    void survivorSay(Survivor& s, const std::string& line, bool log = true);
    void hurtSurvivor(Survivor& s, float damage, bool byPlayer);
    void survivorAttack(Survivor& s, Vec2 targetPos, bool atPlayer, Animal* animal, Survivor* other);
    void learnAbout(const std::string& id);
    std::string rumour(Survivor& teller);           // someone they know of, and roughly where
    std::string warningNear(const Survivor& teller); // danger they've seen lately (or empty)
    std::string placeOf(const Survivor& s) const;   // "around Stampen"
    bool offerTribute(Survivor& s);                 // raiders: hand over valuables for a truce
    int countFor(const std::string& want) const;    // how many of a wanted item you carry
    void takeFor(const std::string& want, int count);
    bool resolvePlaceCache(Survivor& s, Vec2& out);   // the next stop on a raider's round
    std::map<std::string, Vec2> placeCache_;
    std::vector<SurvivorDef> survivorDefs_;
    std::vector<Survivor> survivors_;

    // vehicles (GameVehicles.cpp)
    void updateVehicle(float dt, const PlayerInput& in);
    Vehicle& vehicleFromProp(int x, int y, VehicleKind kind);
    std::vector<ActionOption> vehicleActionsAt(Vec2 pt);
    void performVehicle(const ActionOption& o);
    // Companions in cars and on bikes (GameVehicles.cpp)
    void companionsBoard(Vehicle& v);              // you got in or on: they come along
    bool findBikeFor(Survivor& s);
    bool updateCompanionRide(Survivor& s, float dt);   // true: moved them this frame
    std::vector<Vehicle> vehicles_;
    std::set<int64_t> lockedCars_;    // car props whose doors you've tried
    std::map<int64_t, float> propFuel_;   // parked cars (props) someone siphoned: litres left
    std::map<int64_t, float> pumpFuel_;   // fuel stations: litres taken from the underground tank
    int gasCan(bool wantPetrol, bool wantSpace) const;   // a gas can in the inventory (-1 none)
    int nextVehicleId_ = 1;

    ItemRegistry items_;
    MapData map_;
    Symptoms symptoms_;
    AnimalSystem animals_;
    std::unique_ptr<World> world_;
    // This night's sleep: when it began, and how comfortable it has been
    int talkRequest_ = 0;
    int tentMate_ = 0;
    std::vector<LoreEntry> lore_;
    std::vector<std::string> loreKnown_;
    std::vector<KnownPlace> knownPlaces_;
    std::string loreOpened_;               // the companion who crawled into the tent with you (survivor id)
    double sleepStartMin_ = -1;
    float sleepComfortSum_ = 0, sleepMinutes_ = 0;
    Player player_;
    Rng rng_;
    void checkTrip(float dt, float grade, float limp);   // steep ground can bring you down
    CurrentAction action_;
    std::deque<Message> messages_;
    std::vector<SoundEvent> sounds_;
    float stepDistance_ = 0;
    float unloadTimer_ = 0;
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
    bool autoDrink_ = true;
    float autoDrinkTimer_ = 0;
    void updateAutoDrink(float dt);
};

} // namespace alone
