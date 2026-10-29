#pragma once
#include "core/Math.h"
#include "items/Inventory.h"
#include <array>
#include <string>
#include <vector>

namespace alone {

// How someone treats you when you meet.
//   Friendly: glad to see another person; trades, asks for help, may join you
//   Wary:     keeps you at arm's length until you've proved yourself
//   Hostile:  holds a place and shoots at anyone who comes too close
//   Raider:   roams the town and hunts other people for what they carry
enum class Disposition { Friendly, Wary, Hostile, Raider };
const char* dispositionName(Disposition d);

// One person, from data/survivors/survivors.ini.
struct SurvivorDef {
    std::string id, name, bio;
    int age = 30;
    Disposition disposition = Disposition::Friendly;
    // Where they live: "building:Name", "district:Name", "start:<metres>" (a
    // house about that far from where you wake up) or "x,y" in metres.
    std::string home;
    std::string homeName;              // what the journal calls the place
    float roam = 12;                   // metres they wander from home
    std::vector<std::string> route;    // raiders: places they walk between (same forms as home)
    std::vector<std::string> wear;     // clothing item ids
    std::string weapon;                // item id they fight with
    std::vector<std::pair<std::string, int>> carry;   // what they have (to trade, or to drop)
    uint32_t skin = 0xe2b896, hair = 0x4a3626;
    float hp = 60;
    float aim = 0.5f;                  // 0..1 how well they shoot
    // Talking
    std::string greet;                 // the first time you meet
    std::string hello;                 // later
    std::vector<std::string> chat;     // things they say when you ask how they're doing
    std::string warn;                  // hostile: shouted when you come close
    // Talking to someone who'd rather shoot you
    std::string parleyText;            // when you say you're not looking for trouble
    std::string letInText, keepOutText;   // hostile: letting you in, or not
    int letInTrust = 25;               // hostile: how far they must trust you to let you come close
    std::string describe;              // how others describe them (rumours)
    // What they need (one request each)
    std::string wantItem;              // item id, or tag:<tag>
    int wantCount = 0;
    std::string wantText, thanksText;
    std::vector<std::pair<std::string, int>> reward;
    std::string teaches;               // knowledge the reward gives: "carpentry", "hotwire", "mushrooms"
    // Who they tell you about once they trust you a little
    std::vector<std::string> leads;
    std::string leadText;
    bool canJoin = false;
    int joinTrust = 50;
    std::string joinText, refuseText;
    std::vector<std::string> partners; // raiders who travel together
};

enum class SurvivorState { Home, Wander, Follow, Wait, Alert, Hunt, Flee, Dead };

struct Survivor {
    const SurvivorDef* def = nullptr;
    int id = 0;
    Vec2 pos, home, vel, target;
    float facing = 0;
    float hp = 60;
    SurvivorState state = SurvivorState::Home;
    float stateTimer = 0, attackTimer = 0;
    float swing = 0;                   // 1 -> 0 after a blow
    float lastAttack = 100;            // seconds since they fought
    float trust = 0;                   // -100 .. 100
    bool met = false;                  // you've talked to them
    bool known = false;                // someone told you about them (journal, map)
    bool joined = false;               // travelling with you
    bool angry = false;                // turned on you
    bool requestKnown = false, requestDone = false;
    int routeIndex = 0;
    float warned = 0;                  // hostile: seconds you've ignored a warning
    float alert = 0;                   // 0..1 how sure they are you're near
    bool admitted = false;             // hostile: let you in; treats you like anyone else now
    double truceUntil = 0;             // raiders: game minute until which they leave you alone (you paid)
    bool truce = false;                // (truceUntil is in the future)
    bool rumoured = false;             // you only know roughly where they are
    Vec2 rumourPos;                    // where someone said they were
    std::string speech;                // said out loud (shown over their head)
    float speechTimer = 0;
    Inventory inventory;
    std::array<ItemInstance, 9> worn{};   // by ClothingSlot
    uint32_t weaponUid = 0;
    bool alive() const { return state != SurvivorState::Dead; }
    bool hostileTo() const;            // fights the player on sight
    const ItemInstance* weapon() const;
};

// What you can say in a conversation.
enum class Topic { Chat, Others, Request, Give, Gift, Trade, Join, Follow, Wait, Dismiss, Bye, Parley, LetIn, PayOff, Threaten };

} // namespace alone
