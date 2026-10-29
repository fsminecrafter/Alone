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

// How close you are to someone, from worst to closest. Most steps come from
// trust alone; Family and Lover are bonds you have to ask for.
enum class Bond { Enemy, Stranger, Acquaintance, Friend, BestFriend, Family, Lover, Count };
const char* bondName(Bond b);
const char* bondBlurb(Bond b);      // one line on what it means
// Trust needed for each step from trust alone
constexpr float kTrustAcquaintance = 10, kTrustFriend = 35, kTrustBestFriend = 65;
// Family and Lover need a best friend and enough of their own meter
constexpr float kBondNeeded = 50;
constexpr float kTrustEnemy = -30;

enum class Gender { Man, Woman, Other };
const char* genderName(Gender g);
// Hair: 0 short, 1 long, 2 ponytail, 3 bun, 4 shaved
constexpr int kHairStyles = 5;
const char* hairStyleName(int style);

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
    Gender gender = Gender::Man;
    int hairStyle = 0;
    float hp = 60;
    float aim = 0.5f;                  // 0..1 how well they shoot
    // Talking
    std::string greet;                 // the first time you meet
    std::string hello;                 // later
    std::vector<std::string> chat;     // things they say when you ask how they're doing
    std::string warn;                  // hostile: shouted when you come close
    std::string freeze;                // hostile: shouted when they first see you ("Freeze!")
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
    // More to say (all optional; generic lines are used when empty)
    std::vector<std::string> about;    // their story, told one piece at a time as they trust you
    std::vector<std::string> friendChat, closeChat;   // chat once you're friends / best friends or closer
    std::vector<std::string> idle;     // muttered at home
    std::vector<std::string> night;    // woken up at night
    std::vector<std::string> morning;  // just woken up
    // Romance
    bool romanceable = false;
    std::string flirtYes, flirtNo, confessYes, confessNo;
    std::vector<std::string> loverChat;
    std::string familyText;            // when you tell them they're family
    int bedtime = 22, wakeup = 7;      // hours
};

// (saved as numbers: only ever add to the end)
enum class SurvivorState { Home, Wander, Follow, Wait, Alert, Hunt, Flee, Dead, Sleep };

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
    float freezeCooldown = 0;          // hostile: seconds before they'll stop you again
    bool calledYou = false;            // hostile: walked up to you and asked what you want
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
    // Relationship
    // Two ways to become closest: flirting builds romance (towards Lover),
    // looking out for each other builds kinship (towards Family). Each pulls
    // the other down, so you have to choose.
    float romance = 0;                 // 0..100
    float kinship = 0;                 // 0..100
    int lastKinDay = -1;
    bool family = false, lover = false;
    int lastChatDay = -1;              // the day you last chatted (daily trust)
    int lastFlirtDay = -1, lastGiftDay = -1, lastJokeDay = -1, lastSorryDay = -1;
    int storiesTold = 0;               // how much of `about` you've heard
    Bond shownBond = Bond::Stranger;   // last step announced in the log
    float companyMinutes = 0;          // travelling together: trust grows
    // Daily life at home
    int homeBuilding = -1;
    Vec2 bed, bedSide;                 // where they sleep, and the free tile beside it
    bool hasBed = false;
    std::vector<Vec2> path;            // tiles to walk through (indoors, round walls)
    std::vector<Vec2> nav;             // a way round whatever stands between them and where they're going
    Vec2 navGoal;                      // where `nav` leads
    float navTimer = 0;                // seconds until the way is planned again
    float idleTimer = 0;               // seconds until they wander somewhere else in the house
    float snoreTimer = 0;
    int doorX = 0, doorY = 0, doorDir = -1;   // a door they opened and will close behind them
    float awakeHold = 0;               // woken up: seconds before they go back to bed
    bool wokenUp = false;              // you woke them (they'll mention it)
    bool saidGoodnight = false;
    bool dozing = false;               // a companion napping while you sleep
    bool asleep() const { return state == SurvivorState::Sleep; }
    Bond bond() const;
    bool alive() const { return state != SurvivorState::Dead; }
    bool hostileTo() const;            // fights the player on sight
    const ItemInstance* weapon() const;
};

// What you can say in a conversation.
enum class Topic { Chat, Others, Request, Give, Gift, Trade, Join, Follow, Wait, Dismiss, Bye, Parley, LetIn, PayOff, Threaten,
                   About, Joke, Flirt, Confess, Family, Hug, Sorry, BackToSleep, Breakup, Kin };

} // namespace alone
