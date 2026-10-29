#pragma once
#include "survival/Nutrients.h"
#include "survival/Wound.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace alone {

enum class ItemCategory { Food, Drink, Weapon, Tool, Medical, Clothing, Container, Ammo, Material, Literature, Misc };
enum class ClothingSlot { None, Head, Torso, Sweater, Jacket, Legs, Feet, Hands, Back, Count };

const char* categoryName(ItemCategory c);
const char* slotName(ClothingSlot s);

struct ItemDef {
    std::string id;
    std::string name;            // what the player sees
    std::string trueName;        // shown only with the right knowledge (mushrooms)
    std::string description;
    ItemCategory category = ItemCategory::Misc;
    float weightKg = 0.1f;       // empty weight (food: whole portion)
    uint32_t colour = 0x808080;  // icon / world colour 0xRRGGBB
    std::string icon;            // icon shape hint for the UI
    std::set<std::string> tags;

    // --- food & drink -------------------------------------------------------
    Nutrients nutrients;         // whole item
    float solidMl = 0;           // stomach volume of the whole item
    bool raw = false;            // risky until cooked
    std::string cookedInto;      // item id after cooking
    Pathogen pathogen = Pathogen::None;
    float pathogenChance = 0;    // chance this particular item carries it
    float pathogenDose = 0.5f;
    Toxin toxin = Toxin::None;
    float toxinAmount = 0;       // whole item
    float nauseaOnEat = 0;       // unpleasant to eat (surströmming, raw potato)
    float spoilHours = 0;        // 0 = keeps (practically) forever
    bool needsCanOpener = false;
    bool sealed = false;         // must be opened before spoiling starts

    // --- liquid containers --------------------------------------------------
    float capacityMl = 0;        // > 0: holds liquid

    // --- weapons ------------------------------------------------------------
    float damage = 0;
    float reach = 1.2f;          // metres (firearm: range)
    float cooldown = 1.0f;       // seconds between attacks
    float noise = 5.0f;          // metres animals can hear it
    bool firearm = false;
    std::string ammo;

    // --- clothing -----------------------------------------------------------
    ClothingSlot slot = ClothingSlot::None;
    float clo = 0;
    float waterproof = 0;
    float carryBonusKg = 0;      // backpacks
    bool shortSleeves = false;   // t-shirts: forearms show
    int rags = 0;                // rags you get by ripping it up (0 = can't)
    int tentW = 0, tentL = 0;    // tents: footprint in tiles
    bool ripNeedsKnife = false;  // denim and jackets need cutting

    // --- dressings ----------------------------------------------------------
    DressingKind dressing = DressingKind::None;   // can be tied over a wound
    std::string washesInto;      // a dirty dressing washed in water becomes this
    std::string openedName;      // name once the sterile pack is opened/washed

    // --- tools / medical / stacks ----------------------------------------
    int uses = 0;                // > 0: limited uses (lighter, pills, ammo count)
    Pathogen treats = Pathogen::None;
    float vitaminCMg = 0;        // per use (tablets)

    bool hasTag(const std::string& t) const { return tags.count(t) != 0; }
    bool isEdible() const { return category == ItemCategory::Food || category == ItemCategory::Drink; }
};

// One actual item in the world or an inventory.
struct ItemInstance {
    const ItemDef* def = nullptr;
    float portion = 1.0f;        // food: fraction left
    float ageHours = 0;          // since opened / harvested
    float condition = 1.0f;      // tools/weapons/clothes wear
    float liquidMl = 0;          // liquid containers
    bool liquidSafe = true;      // false: untreated lake water
    uint8_t liquid = 0;          // what's in it: 0 water, 1 petrol
    bool opened = false;         // cans/jars
    uint32_t tint = 0;           // clothes: dyed this colour (0 = the item's own)
    int usesLeft = 0;
    bool carriesPathogen = false;  // rolled when created
    uint32_t uid = 0;

    float weightKg() const;
    float spoilage() const;      // 0 fresh .. 1 rotten
    bool rotten() const { return spoilage() >= 1.0f; }
    std::string displayName(bool knowsMushrooms = false) const;
};

} // namespace alone
