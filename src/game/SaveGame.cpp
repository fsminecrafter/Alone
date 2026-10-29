// Save files: a plain text stream of whitespace separated tokens, written and
// read in the same order. Item and container ids never contain spaces.
#include "core/Log.h"
#include "game/Game.h"
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace alone {

class SaveGame {
public:
    // 3: dressings per body part, 4: tents, carpentry, stoves, 5: people, vehicles, 6: petrol, 7: deals and rumours,
    // 8: who you are (name, looks, dyed clothes), friendships, people's beds, 9: kinship (the way to Family)
    static constexpr int kVersion = 10;
    static int& readVersion() { static int v = kVersion; return v; }   // of the file being read

    // --- writing -------------------------------------------------------------
    static void item(std::ostream& o, const ItemInstance& it) {
        o << (it.def ? it.def->id : std::string("-")) << ' ' << it.portion << ' ' << it.ageHours << ' ' << it.condition << ' '
          << it.liquidMl << ' ' << it.liquidSafe << ' ' << it.opened << ' ' << it.usesLeft << ' ' << it.carriesPathogen << ' '
          << it.uid << ' ' << (int)it.liquid << ' ' << it.tint << '\n';
    }
    static void items(std::ostream& o, const std::vector<ItemInstance>& v) {
        o << v.size() << '\n';
        for (const auto& it : v) item(o, it);
    }
    static void nutrients(std::ostream& o, const Nutrients& n) {
        o << n.protein << ' ' << n.fat << ' ' << n.carbs << ' ' << n.fiber << ' ' << n.water << ' ' << n.vitaminC << '\n';
    }

    static void write(Game& g, std::ostream& o) {
        o.precision(9);
        World& w = *g.world_;
        o << "ALONE_SAVE " << kVersion << '\n';
        o << g.seed_ << ' ' << w.clock().totalMinutes() << ' ' << w.snowCover_ << ' ' << w.wetness_ << '\n';
        // player
        const Player& p = g.player_;
        o << p.pos.x << ' ' << p.pos.y << ' ' << p.facing << ' ' << p.stamina << ' ' << p.weaponUid << ' ' << p.flashlightOn << ' '
          << p.knowsCarpentry << '\n';
        items(o, p.inventory.items());
        for (const auto& wi : p.worn) item(o, wi);
        // body
        const BodyState& b = p.body.state();
        o << b.leanMassKg << ' ' << b.leanBaselineKg << ' ' << b.fatMassKg << ' ' << b.liverGlycogenG << ' ' << b.muscleGlycogenG << '\n';
        nutrients(o, b.stomach);
        o << b.stomachSolidMl << ' ' << b.bloodSugarG << ' ' << b.dietaryFatG << ' ' << b.aminoPoolG << ' ' << b.energyCreditKcal << ' '
          << b.ketoAdaptation << ' ' << b.starvationAdaptation << ' ' << b.ammonia << ' ' << b.vitaminCMg << '\n';
        o << b.avgKcalProtein << ' ' << b.avgKcalFat << ' ' << b.avgKcalCarbs << ' ' << b.energyDebtKcal << '\n';
        o << b.waterDeficitL << ' ' << b.bloodVolumeL << ' ' << b.coreTempC << ' ' << b.feverC << ' ' << b.wetness << '\n';
        o << b.sleepPressure << ' ' << b.hoursAwake << ' ' << b.systemicDamage << ' ' << b.liverDamage << ' ' << b.diarrhea << ' '
          << b.nauseaExtra << ' ' << b.painkiller << ' ' << b.minutesAlive << ' ' << b.woundAntibioticH << '\n';
        o << b.wounds.size() << '\n';
        for (const auto& wd : b.wounds)
            o << (int)wd.part << ' ' << (int)wd.type << ' ' << wd.severity << ' ' << wd.bleeding << ' ' << wd.contamination << ' '
              << wd.infection << ' ' << wd.healed << '\n';
        for (const auto& d : b.dressings)
            o << (int)d.kind << ' ' << d.dirt << ' ' << d.blood << ' ' << d.hours << ' ' << d.sterile << '\n';
        o << b.sting << '\n';
        o << b.infections.size() << '\n';
        for (const auto& inf : b.infections)
            o << (int)inf.type << ' ' << inf.incubationH << ' ' << inf.load << ' ' << inf.peak << ' ' << inf.immunity << ' ' << inf.ageH
              << ' ' << inf.treated << '\n';
        o << b.toxins.size() << '\n';
        for (const auto& t : b.toxins) o << (int)t.type << ' ' << t.amount << ' ' << t.ageH << '\n';
        // world changes
        o << w.overrides_.size() << '\n';
        for (const auto& [k, ov] : w.overrides_) o << k << ' ' << ov.hasProp << ' ' << (int)ov.prop << ' ' << ov.harvestedAtDay << '\n';
        o << w.doors_.size() << '\n';
        for (const auto& [k, open] : w.doors_) o << k << ' ' << open << '\n';
        o << w.containers_.size() << '\n';
        for (const auto& [k, c] : w.containers_) {
            o << k << ' ' << (c.kind.empty() ? std::string("-") : c.kind) << ' ' << c.searched << '\n';
            items(o, c.items);
        }
        o << w.ground_.size() << '\n';
        for (const auto& [k, v] : w.ground_) {
            o << k << '\n';
            items(o, v);
        }
        o << w.tankWater_.size() << '\n';
        for (const auto& [k, ml] : w.tankWater_) o << k << ' ' << ml << '\n';
        o << w.fires_.size() << '\n';
        for (const auto& f : w.fires_)
            o << f.pos.x << ' ' << f.pos.y << ' ' << f.fuelMinutes << ' ' << f.lit << ' ' << f.id << ' ' << f.stove << ' ' << f.fuelMl << '\n';
        o << w.snares_.size() << '\n';
        for (const auto& s : w.snares_) o << s.pos.x << ' ' << s.pos.y << ' ' << s.minutesSet << ' ' << s.caught << ' ' << s.id << '\n';
        o << w.tents_.size() << '\n';
        for (const auto& t : w.tents_)
            o << t.id << ' ' << t.rect.x << ' ' << t.rect.y << ' ' << t.rect.w << ' ' << t.rect.h << ' ' << t.dir << ' ' << t.item << '\n';
        o << w.builds_.size() << '\n';
        for (const auto& b : w.builds_) o << (int)b.kind << ' ' << b.x << ' ' << b.y << ' ' << b.dir << '\n';
        o << w.nextId_ << '\n';
        // v5: other people, vehicles, what you've learned
        o << "PEOPLE " << g.survivors_.size() << '\n';
        for (const auto& sv : g.survivors_) {
            o << sv.def->id << ' ' << sv.pos.x << ' ' << sv.pos.y << ' ' << sv.hp << ' ' << (int)sv.state << ' ' << sv.trust << ' '
              << sv.met << ' ' << sv.known << ' ' << sv.joined << ' ' << sv.angry << ' ' << sv.requestKnown << ' ' << sv.requestDone << ' '
              << sv.routeIndex << ' ' << sv.weaponUid << ' ' << sv.target.x << ' ' << sv.target.y << ' ' << sv.admitted << ' '
              << sv.truceUntil << ' ' << sv.rumoured << ' ' << sv.rumourPos.x << ' ' << sv.rumourPos.y << '\n';
            items(o, sv.inventory.items());
            for (const auto& wi : sv.worn) item(o, wi);
        }
        o << "VEHICLES " << g.vehicles_.size() << ' ' << g.nextVehicleId_ << '\n';
        for (const auto& v : g.vehicles_)
            o << v.id << ' ' << (int)v.kind << ' ' << v.pos.x << ' ' << v.pos.y << ' ' << v.heading << ' ' << v.variant << ' '
              << v.keyInIgnition << ' ' << v.hotwired << ' ' << v.windowSmashed << ' ' << v.fuelL << ' ' << v.damage << ' ' << v.searched << ' '
              << v.tankL << '\n';
        o << g.lockedCars_.size() << '\n';
        for (int64_t k : g.lockedCars_) o << k << '\n';
        o << p.knowsHotwire << ' ' << p.knowsMushroomsTaught << ' ' << p.vehicle << '\n';
        o << g.propFuel_.size() << '\n';
        for (const auto& [k, l] : g.propFuel_) o << k << ' ' << l << '\n';
        o << g.pumpFuel_.size() << '\n';
        for (const auto& [k, l] : g.pumpFuel_) o << k << ' ' << l << '\n';
        // v8: who you are, and where you stand with people
        o << "SELF " << std::quoted(p.look.name) << ' ' << (int)p.look.gender << ' ' << p.look.skin << ' ' << p.look.hair << ' '
          << p.look.hairStyle << ' ' << p.look.age << '\n';   // age: v10
        o << "BONDS " << g.survivors_.size() << '\n';
        for (const auto& sv : g.survivors_)
            o << sv.def->id << ' ' << sv.romance << ' ' << sv.family << ' ' << sv.lover << ' ' << sv.lastChatDay << ' ' << sv.lastFlirtDay << ' '
              << sv.lastGiftDay << ' ' << sv.lastJokeDay << ' ' << sv.lastSorryDay << ' ' << sv.storiesTold << ' ' << (int)sv.shownBond << ' '
              << sv.companyMinutes << ' ' << sv.kinship << ' ' << sv.lastKinDay << ' ' << sv.happenedTold << '\n';
        // v10: broken windows
        o << "WINDOWS " << w.brokenWindows_.size() << '\n';
        for (int64_t k : w.brokenWindows_) o << k << '\n';
        o << "STOWED " << p.stowedUid << '\n';
        o << "LORE " << g.loreKnown_.size();
        for (const auto& l : g.loreKnown_) o << ' ' << l;
        o << '\n';
        o << "END\n";
    }

    // --- reading -------------------------------------------------------------
    static bool item(Game& g, std::istream& in, ItemInstance& it, uint32_t& maxUid) {
        std::string id;
        int safe = 1, opened = 0, path = 0;
        in >> id >> it.portion >> it.ageHours >> it.condition >> it.liquidMl >> safe >> opened >> it.usesLeft >> path >> it.uid;
        if (readVersion() >= 6) { int liq = 0; in >> liq; it.liquid = (uint8_t)liq; }
        if (readVersion() >= 8) in >> it.tint;
        it.liquidSafe = safe; it.opened = opened; it.carriesPathogen = path;
        maxUid = std::max(maxUid, it.uid);
        if (id == "-") { it.def = nullptr; return true; }
        it.def = g.items_.find(id);
        if (!it.def) Log::warn("Save refers to unknown item %s, dropped", id.c_str());
        return (bool)in;
    }
    static bool items(Game& g, std::istream& in, std::vector<ItemInstance>& v, uint32_t& maxUid) {
        size_t n = 0;
        in >> n;
        v.clear();
        for (size_t i = 0; i < n && in; ++i) {
            ItemInstance it;
            item(g, in, it, maxUid);
            if (it.def) v.push_back(it);
        }
        return (bool)in;
    }
    static void nutrients(std::istream& in, Nutrients& n) {
        in >> n.protein >> n.fat >> n.carbs >> n.fiber >> n.water >> n.vitaminC;
    }

    static bool read(Game& g, std::istream& in) {
        std::string magic;
        int version = 0;
        in >> magic >> version;
        if (magic != "ALONE_SAVE" || version < 4 || version > kVersion) { Log::warn("Not a compatible save file"); return false; }
        readVersion() = version;
        uint32_t seed = 0;
        double minutes = 0;
        float snow = 0, wet = 0;
        in >> seed >> minutes >> snow >> wet;
        g.newGame(seed);
        g.messages_.clear();
        World& w = *g.world_;
        w.clock().setTotalMinutes(minutes);
        w.snowCover_ = snow;
        w.wetness_ = wet;
        uint32_t maxUid = 0;
        Player& p = g.player_;
        int flash = 0, carpentry = 0;
        in >> p.pos.x >> p.pos.y >> p.facing >> p.stamina >> p.weaponUid >> flash >> carpentry;
        p.flashlightOn = flash;
        p.knowsCarpentry = carpentry;
        items(g, in, p.inventory.items(), maxUid);
        for (auto& wi : p.worn) { item(g, in, wi, maxUid); }
        BodyState& b = p.body.mutableState();
        in >> b.leanMassKg >> b.leanBaselineKg >> b.fatMassKg >> b.liverGlycogenG >> b.muscleGlycogenG;
        nutrients(in, b.stomach);
        in >> b.stomachSolidMl >> b.bloodSugarG >> b.dietaryFatG >> b.aminoPoolG >> b.energyCreditKcal >> b.ketoAdaptation >>
            b.starvationAdaptation >> b.ammonia >> b.vitaminCMg;
        in >> b.avgKcalProtein >> b.avgKcalFat >> b.avgKcalCarbs >> b.energyDebtKcal;
        in >> b.waterDeficitL >> b.bloodVolumeL >> b.coreTempC >> b.feverC >> b.wetness;
        in >> b.sleepPressure >> b.hoursAwake >> b.systemicDamage >> b.liverDamage >> b.diarrhea >> b.nauseaExtra >> b.painkiller >>
            b.minutesAlive >> b.woundAntibioticH;
        size_t n = 0;
        in >> n;
        b.wounds.clear();
        for (size_t i = 0; i < n && in; ++i) {
            Wound wd;
            int part, type;
            in >> part >> type >> wd.severity >> wd.bleeding >> wd.contamination >> wd.infection >> wd.healed;
            wd.part = (BodyPart)part; wd.type = (WoundType)type;
            b.wounds.push_back(wd);
        }
        for (auto& d : b.dressings) {
            int kind, sterile;
            in >> kind >> d.dirt >> d.blood >> d.hours >> sterile;
            d.kind = (DressingKind)std::clamp(kind, 0, (int)DressingKind::Count - 1);
            d.sterile = sterile;
        }
        in >> b.sting;
        in >> n;
        b.infections.clear();
        for (size_t i = 0; i < n && in; ++i) {
            Infection inf;
            int type, treated;
            in >> type >> inf.incubationH >> inf.load >> inf.peak >> inf.immunity >> inf.ageH >> treated;
            inf.type = (Pathogen)type; inf.treated = treated;
            b.infections.push_back(inf);
        }
        in >> n;
        b.toxins.clear();
        for (size_t i = 0; i < n && in; ++i) {
            ToxinDose t;
            int type;
            in >> type >> t.amount >> t.ageH;
            t.type = (Toxin)type;
            b.toxins.push_back(t);
        }
        in >> n;
        for (size_t i = 0; i < n && in; ++i) {
            int64_t k; int has, prop; float day;
            in >> k >> has >> prop >> day;
            TileOverride ov; ov.hasProp = has; ov.prop = (Prop)prop; ov.harvestedAtDay = day;
            w.overrides_[k] = ov;
        }
        in >> n;
        for (size_t i = 0; i < n && in; ++i) { int64_t k; int open; in >> k >> open; w.doors_[k] = open; }
        in >> n;
        for (size_t i = 0; i < n && in; ++i) {
            int64_t k; std::string kind; int searched;
            in >> k >> kind >> searched;
            ContainerState c;
            c.kind = kind == "-" ? "" : kind;
            c.searched = searched;
            items(g, in, c.items, maxUid);
            w.containers_[k] = c;
        }
        in >> n;
        for (size_t i = 0; i < n && in; ++i) {
            int64_t k; in >> k;
            items(g, in, w.ground_[k], maxUid);
        }
        in >> n;
        for (size_t i = 0; i < n && in; ++i) { int64_t k; float ml; in >> k >> ml; w.tankWater_[k] = ml; }
        in >> n;
        w.fires_.clear();
        for (size_t i = 0; i < n && in; ++i) {
            Campfire f; int lit, stove;
            in >> f.pos.x >> f.pos.y >> f.fuelMinutes >> lit >> f.id >> stove >> f.fuelMl;
            f.lit = lit;
            f.stove = stove;
            w.fires_.push_back(f);
        }
        in >> n;
        w.snares_.clear();
        for (size_t i = 0; i < n && in; ++i) {
            Snare s; int caught;
            in >> s.pos.x >> s.pos.y >> s.minutesSet >> caught >> s.id;
            s.caught = false;   // the catch itself isn't saved; the snare is reset
            (void)caught;
            w.snares_.push_back(s);
        }
        in >> n;
        w.tents_.clear();
        for (size_t i = 0; i < n && in; ++i) {
            Tent t;
            in >> t.id >> t.rect.x >> t.rect.y >> t.rect.w >> t.rect.h >> t.dir >> t.item;
            w.tents_.push_back(t);
        }
        in >> n;
        w.builds_.clear();
        for (size_t i = 0; i < n && in; ++i) {
            Built b; int kind;
            in >> kind >> b.x >> b.y >> b.dir;
            b.kind = (BuildKind)std::clamp(kind, 0, (int)BuildKind::Count - 1);
            w.builds_.push_back(b);
        }
        in >> w.nextId_;
        if (version >= 5) {
            std::string tag;
            in >> tag >> n;
            for (size_t i = 0; i < n && in; ++i) {
                std::string id;
                Survivor tmp;
                int state = 0, met, known, joined, angry, rk, rd;
                in >> id >> tmp.pos.x >> tmp.pos.y >> tmp.hp >> state >> tmp.trust >> met >> known >> joined >> angry >> rk >> rd >>
                    tmp.routeIndex >> tmp.weaponUid >> tmp.target.x >> tmp.target.y;
                int admitted = 0, rumoured = 0;
                if (version >= 7) in >> admitted >> tmp.truceUntil >> rumoured >> tmp.rumourPos.x >> tmp.rumourPos.y;
                items(g, in, tmp.inventory.items(), maxUid);
                for (auto& wi : tmp.worn) item(g, in, wi, maxUid);
                for (auto& sv : g.survivors_) {
                    if (sv.def->id != id) continue;
                    sv.pos = tmp.pos; sv.hp = tmp.hp; sv.state = (SurvivorState)state; sv.trust = tmp.trust;
                    sv.met = met; sv.known = known; sv.joined = joined; sv.angry = angry; sv.requestKnown = rk; sv.requestDone = rd;
                    sv.routeIndex = tmp.routeIndex; sv.weaponUid = tmp.weaponUid; sv.target = tmp.target;
                    sv.inventory = tmp.inventory; sv.worn = tmp.worn;
                    sv.admitted = admitted; sv.truceUntil = tmp.truceUntil; sv.rumoured = rumoured; sv.rumourPos = tmp.rumourPos;
                }
            }
            size_t nv = 0;
            in >> tag >> nv >> g.nextVehicleId_;
            g.vehicles_.clear();
            for (size_t i = 0; i < nv && in; ++i) {
                Vehicle v; int kind, key, hot, smashed, searched;
                in >> v.id >> kind >> v.pos.x >> v.pos.y >> v.heading >> v.variant >> key >> hot >> smashed >> v.fuelL >> v.damage >> searched;
                if (version >= 6) in >> v.tankL;
                v.kind = (VehicleKind)kind; v.keyInIgnition = key; v.hotwired = hot; v.windowSmashed = smashed; v.searched = searched;
                g.vehicles_.push_back(v);
            }
            in >> n;
            g.lockedCars_.clear();
            for (size_t i = 0; i < n && in; ++i) { int64_t k; in >> k; g.lockedCars_.insert(k); }
            int hw, mush;
            in >> hw >> mush >> p.vehicle;
            p.knowsHotwire = hw; p.knowsMushroomsTaught = mush;
            if (version >= 6) {
                g.propFuel_.clear();
                g.pumpFuel_.clear();
                in >> n;
                for (size_t i = 0; i < n && in; ++i) { int64_t k; float l; in >> k >> l; g.propFuel_[k] = l; }
                in >> n;
                for (size_t i = 0; i < n && in; ++i) { int64_t k; float l; in >> k >> l; g.pumpFuel_[k] = l; }
            }
            if (p.vehicle >= 0 && !g.vehicle(p.vehicle)) p.vehicle = -1;
        }
        if (version >= 8) {
            std::string tag;
            int gender = 0;
            in >> tag >> std::quoted(p.look.name) >> gender >> p.look.skin >> p.look.hair >> p.look.hairStyle;
            if (version >= 10) { in >> p.look.age; p.look.age = std::clamp(p.look.age, 18, 90); }
            p.look.gender = (Gender)std::clamp(gender, 0, 2);
            size_t nb = 0;
            in >> tag >> nb;
            for (size_t i = 0; i < nb && in; ++i) {
                std::string id;
                Survivor t;
                int fam = 0, lov = 0, shown = 1;
                in >> id >> t.romance >> fam >> lov >> t.lastChatDay >> t.lastFlirtDay >> t.lastGiftDay >> t.lastJokeDay >> t.lastSorryDay >>
                    t.storiesTold >> shown >> t.companyMinutes;
                if (version >= 9) in >> t.kinship >> t.lastKinDay;
                if (version >= 10) in >> t.happenedTold;
                else if (fam) t.kinship = kBondNeeded;
                for (auto& sv : g.survivors_) {
                    if (sv.def->id != id) continue;
                    sv.romance = t.romance; sv.family = fam; sv.lover = lov;
                    sv.lastChatDay = t.lastChatDay; sv.lastFlirtDay = t.lastFlirtDay; sv.lastGiftDay = t.lastGiftDay;
                    sv.lastJokeDay = t.lastJokeDay; sv.lastSorryDay = t.lastSorryDay; sv.storiesTold = t.storiesTold;
                    sv.shownBond = (Bond)std::clamp(shown, 0, (int)Bond::Count - 1); sv.companyMinutes = t.companyMinutes;
                    sv.kinship = t.kinship; sv.lastKinDay = t.lastKinDay; sv.happenedTold = t.happenedTold;
                }
            }
            if (version >= 10) {
                size_t nw = 0;
                in >> tag >> nw;
                w.brokenWindows_.clear();
                for (size_t i = 0; i < nw && in; ++i) { int64_t k; in >> k; w.brokenWindows_.insert(k); }
                in >> tag >> p.stowedUid;
                size_t nl = 0;
                in >> tag >> nl;
                g.loreKnown_.clear();
                for (size_t i = 0; i < nl && in; ++i) { std::string l; in >> l; g.loreKnown_.push_back(l); }
            }
        }
        // Saved before people had a bed: whatever they were doing, they're awake
        for (auto& sv : g.survivors_) {
            if (sv.asleep() && !sv.hasBed) sv.state = SurvivorState::Home;
            if (version < 8) sv.shownBond = sv.bond();
        }
        std::string end;
        in >> end;
        if (end != "END") { Log::warn("Save file is truncated or corrupt"); return false; }
        g.items_.ensureUidAbove(maxUid);
        // Chunks were generated before the overrides were known: rebuild them
        w.chunks_.clear();
        return true;
    }
};

bool Game::save(const std::string& path) {
    if (!world_ || dead()) return false;
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) { Log::warn("Can't write save file %s", tmp.c_str()); return false; }
        SaveGame::write(*this, f);
        if (!f) return false;
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { Log::warn("Can't replace save file %s", path.c_str()); return false; }
    Log::info("Saved to %s", path.c_str());
    return true;
}

bool Game::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    if (!SaveGame::read(*this, f)) return false;
    log("You wake up where you left off.");
    Log::info("Loaded %s", path.c_str());
    return true;
}

} // namespace alone
