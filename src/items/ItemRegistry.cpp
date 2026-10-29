#include "items/ItemRegistry.h"
#include "core/Config.h"
#include "core/Log.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace alone {

namespace {

ItemCategory parseCategory(const std::string& s) {
    static const char* n[] = {"food", "drink", "weapon", "tool", "medical", "clothing",
                              "container", "ammo", "material", "literature", "misc"};
    for (int i = 0; i < 11; ++i)
        if (s == n[i]) return (ItemCategory)i;
    return ItemCategory::Misc;
}

ClothingSlot parseSlot(const std::string& s) {
    static const char* n[] = {"none", "head", "torso", "sweater", "jacket", "legs", "feet", "hands", "back"};
    for (int i = 0; i < (int)ClothingSlot::Count; ++i)
        if (s == n[i]) return (ClothingSlot)i;
    return ClothingSlot::None;
}

uint32_t parseColour(const std::string& s) {
    std::string t = s;
    if (!t.empty() && t[0] == '#') t = t.substr(1);
    return t.empty() ? 0x808080u : (uint32_t)std::strtoul(t.c_str(), nullptr, 16);
}

} // namespace

bool ItemRegistry::loadDirectory(const std::string& dir) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".ini") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        Log::error("No item files in %s", dir.c_str());
        return false;
    }
    for (const auto& f : files) {
        Config cfg;
        if (!cfg.load(f.string())) continue;
        for (const auto& sec : cfg.sections()) {
            if (sec.name.empty()) continue;
            auto def = std::make_unique<ItemDef>();
            ItemDef& d = *def;
            d.id = sec.name;
            d.name = sec.str("name", d.id);
            d.trueName = sec.str("true_name");
            d.description = sec.str("description");
            d.category = parseCategory(sec.str("category", "misc"));
            d.colour = parseColour(sec.str("colour", "808080"));
            d.icon = sec.str("icon", "box");
            for (auto& t : sec.list("tags")) d.tags.insert(t);

            // Food: values per 100 g, scaled by grams
            float grams = sec.num("grams", 0);
            if (grams > 0) {
                float k = grams / 100.0f;
                d.nutrients.protein = sec.num("protein") * k;
                d.nutrients.fat = sec.num("fat") * k;
                d.nutrients.carbs = sec.num("carbs") * k;
                d.nutrients.fiber = sec.num("fiber") * k;
                d.nutrients.water = sec.num("water") * k;
                d.nutrients.vitaminC = sec.num("vitamin_c") * k;
                d.solidMl = d.category == ItemCategory::Drink ? 0.0f : grams * sec.num("volume_per_gram", 1.0f);
                d.weightKg = grams / 1000.0f + sec.num("packaging_kg", 0);
            } else {
                d.weightKg = sec.num("weight", 0.1f);
            }
            d.raw = sec.flag("raw");
            d.cookedInto = sec.str("cooked_into");
            d.driesTo = sec.str("dries_to");
            d.smokesTo = sec.str("smokes_to");
            d.saltsTo = sec.str("salts_to");
            d.dryHours = sec.num("dry_hours", 48);
            d.smokeHours = sec.num("smoke_hours", 10);
            d.pathogen = pathogenFromString(sec.str("pathogen", "none").c_str());
            d.pathogenChance = sec.num("pathogen_chance", 0);
            d.pathogenDose = sec.num("pathogen_dose", 0.5f);
            d.toxin = toxinFromString(sec.str("toxin", "none").c_str());
            d.toxinAmount = sec.num("toxin_amount", 0);
            d.nauseaOnEat = sec.num("nausea_on_eat", 0);
            d.spoilHours = sec.num("spoil_hours", 0);
            d.needsCanOpener = sec.flag("needs_can_opener");
            d.sealed = sec.flag("sealed", d.needsCanOpener);
            d.capacityMl = sec.num("capacity_ml", 0);

            d.damage = sec.num("damage", 0);
            d.reach = sec.num("reach", 1.2f);
            d.cooldown = sec.num("cooldown", 1.0f);
            d.noise = sec.num("noise", 5.0f);
            d.firearm = sec.flag("firearm");
            d.ammo = sec.str("ammo");
            d.magazine = (int)sec.num("magazine", 0);
            d.lore = sec.str("lore");
            d.armour = std::clamp(sec.num("armour", 0), 0.0f, 0.9f);
            d.reloadSeconds = sec.num("reload", 2.5f);

            d.slot = parseSlot(sec.str("slot", "none"));
            d.clo = sec.num("clo", 0);
            d.waterproof = sec.num("waterproof", 0);
            d.carryBonusKg = sec.num("carry_bonus", 0);
            d.shortSleeves = sec.str("sleeves", "long") == "short";
            // Clothes and textiles can be ripped into rags; tough fabric needs a knife
            int defaultRags = d.category == ItemCategory::Clothing && d.slot != ClothingSlot::Feet && d.slot != ClothingSlot::Back
                                  ? std::clamp((int)std::lround(d.weightKg / 0.1f), 1, 5) : 0;
            d.rags = sec.integer("rags", defaultRags);
            d.ripNeedsKnife = sec.flag("rip_needs_knife");
            d.tentW = sec.integer("tent_width", 0);
            d.tentL = sec.integer("tent_length", 0);

            d.dressing = dressingFromString(sec.str("dressing", "none"));
            d.washesInto = sec.str("washes_into");
            d.openedName = sec.str("name_opened");

            d.uses = sec.integer("uses", 0);
            d.treats = pathogenFromString(sec.str("treats", "none").c_str());
            d.vitaminCMg = sec.num("vitamin_c_mg", 0);

            if (byId_.count(d.id)) Log::warn("Duplicate item id %s", d.id.c_str());
            byId_[d.id] = def.get();
            items_.push_back(std::move(def));
        }
    }
    // Validate references
    for (const auto& d : items_) {
        if (!d->cookedInto.empty() && !find(d->cookedInto))
            Log::warn("Item %s cooks into unknown item %s", d->id.c_str(), d->cookedInto.c_str());
        if (!d->ammo.empty() && !find(d->ammo))
            Log::warn("Item %s uses unknown ammo %s", d->id.c_str(), d->ammo.c_str());
        if (!d->cookedInto.empty() && find(d->cookedInto) == nullptr) {}
        if (!d->washesInto.empty() && !find(d->washesInto))
            Log::warn("Item %s washes into unknown item %s", d->id.c_str(), d->washesInto.c_str());
    }
    Log::info("Loaded %d items", (int)items_.size());
    return true;
}

bool ItemRegistry::loadContainers(const std::string& path) {
    Config cfg;
    if (!cfg.load(path)) return false;
    for (const auto& sec : cfg.sections()) {
        if (sec.name.empty()) continue;
        ContainerDef c;
        c.id = sec.name;
        c.name = sec.str("name", c.id);
        c.searchSeconds = sec.num("search_seconds", 2.0f);
        auto rolls = split(sec.str("rolls", "0-3"), '-');
        c.minRolls = std::atoi(rolls[0].c_str());
        c.maxRolls = rolls.size() > 1 ? std::atoi(rolls[1].c_str()) : c.minRolls;
        c.emptyChance = sec.num("empty_chance", 0.2f);
        // items = id:weight[:min-max], ...
        for (const auto& entry : sec.list("items")) {
            auto parts = split(entry, ':');
            LootEntry le;
            le.itemId = parts[0];
            if (parts.size() > 1) le.weight = (float)std::atof(parts[1].c_str());
            if (parts.size() > 2) {
                auto mm = split(parts[2], '-');
                le.minCount = std::atoi(mm[0].c_str());
                le.maxCount = mm.size() > 1 ? std::atoi(mm[1].c_str()) : le.minCount;
            }
            if (!find(le.itemId)) {
                Log::warn("Container %s lists unknown item %s", c.id.c_str(), le.itemId.c_str());
                continue;
            }
            c.totalWeight += le.weight;
            c.loot.push_back(le);
        }
        containers_[c.id] = c;
    }
    Log::info("Loaded %d container types", (int)containers_.size());
    return true;
}

const ItemDef* ItemRegistry::find(const std::string& id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
}

const ContainerDef* ItemRegistry::container(const std::string& id) const {
    auto it = containers_.find(id);
    return it == containers_.end() ? nullptr : &it->second;
}

ItemInstance ItemRegistry::create(const std::string& id, Rng& rng) const {
    const ItemDef* d = find(id);
    if (!d) {
        Log::warn("create: unknown item %s", id.c_str());
        return {};
    }
    return create(d, rng);
}

ItemInstance ItemRegistry::create(const ItemDef* d, Rng& rng) const {
    ItemInstance it;
    it.def = d;
    it.uid = nextUid_++;
    it.usesLeft = d->uses;
    if (d->capacityMl > 0) it.liquidMl = d->hasTag("starts_empty") ? 0.0f : d->capacityMl;
    if (d->hasTag("gascan")) {
        // found cans usually have some petrol left in them
        it.liquid = 1;
        it.liquidMl = rng.chance(0.35f) ? 0.0f : std::round(rng.range(0.1f, 1.0f) * d->capacityMl / 500.0f) * 500.0f;
    }
    if (d->pathogen != Pathogen::None) it.carriesPathogen = rng.chance(d->pathogenChance);
    if (d->category == ItemCategory::Weapon || d->category == ItemCategory::Tool) it.condition = rng.range(0.5f, 1.0f);
    return it;
}

std::vector<ItemInstance> ItemRegistry::rollLoot(const std::string& containerId, uint32_t seed) const {
    std::vector<ItemInstance> out;
    const ContainerDef* c = container(containerId);
    if (!c || c->loot.empty()) return out;
    Rng rng(seed);
    if (rng.chance(c->emptyChance)) return out;
    int rolls = rng.rangeInt(c->minRolls, c->maxRolls);
    for (int r = 0; r < rolls; ++r) {
        float pick = rng.uniform() * c->totalWeight;
        for (const auto& le : c->loot) {
            pick -= le.weight;
            if (pick <= 0) {
                int count = rng.rangeInt(le.minCount, le.maxCount);
                for (int i = 0; i < count; ++i) {
                    ItemInstance it = create(le.itemId, rng);
                    // Things in abandoned houses have been sitting for a while
                    if (it.def && it.def->spoilHours > 0) it.ageHours = rng.range(0.0f, it.def->spoilHours * 0.7f);
                    // Scraps were left behind when everyone went: already well on their way
                    if (it.def && it.def->hasTag("leftovers")) it.ageHours = rng.range(0.3f, 0.95f) * it.def->spoilHours;
                    out.push_back(it);
                }
                break;
            }
        }
    }
    return out;
}

} // namespace alone
