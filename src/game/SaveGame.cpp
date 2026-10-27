// Save files: a plain text stream of whitespace separated tokens, written and
// read in the same order. Item and container ids never contain spaces.
#include "core/Log.h"
#include "game/Game.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace alone {

class SaveGame {
public:
    static constexpr int kVersion = 2;

    // --- writing -------------------------------------------------------------
    static void item(std::ostream& o, const ItemInstance& it) {
        o << (it.def ? it.def->id : std::string("-")) << ' ' << it.portion << ' ' << it.ageHours << ' ' << it.condition << ' '
          << it.liquidMl << ' ' << it.liquidSafe << ' ' << it.opened << ' ' << it.usesLeft << ' ' << it.carriesPathogen << ' '
          << it.uid << '\n';
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
        o << p.pos.x << ' ' << p.pos.y << ' ' << p.facing << ' ' << p.stamina << ' ' << p.weaponUid << ' ' << p.flashlightOn << '\n';
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
            o << (int)wd.part << ' ' << (int)wd.type << ' ' << wd.severity << ' ' << wd.bleeding << ' ' << wd.bandaged << ' '
              << wd.disinfected << ' ' << wd.bandageDirt << ' ' << wd.contamination << ' ' << wd.infection << ' ' << wd.healed << '\n';
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
        for (const auto& f : w.fires_) o << f.pos.x << ' ' << f.pos.y << ' ' << f.fuelMinutes << ' ' << f.lit << ' ' << f.id << '\n';
        o << w.snares_.size() << '\n';
        for (const auto& s : w.snares_) o << s.pos.x << ' ' << s.pos.y << ' ' << s.minutesSet << ' ' << s.caught << ' ' << s.id << '\n';
        o << w.nextId_ << '\n';
        o << "END\n";
    }

    // --- reading -------------------------------------------------------------
    static bool item(Game& g, std::istream& in, ItemInstance& it, uint32_t& maxUid) {
        std::string id;
        int safe = 1, opened = 0, path = 0;
        in >> id >> it.portion >> it.ageHours >> it.condition >> it.liquidMl >> safe >> opened >> it.usesLeft >> path >> it.uid;
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
        if (magic != "ALONE_SAVE" || version != kVersion) { Log::warn("Not a compatible save file"); return false; }
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
        int flash = 0;
        in >> p.pos.x >> p.pos.y >> p.facing >> p.stamina >> p.weaponUid >> flash;
        p.flashlightOn = flash;
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
            int part, type, band, dis;
            in >> part >> type >> wd.severity >> wd.bleeding >> band >> dis >> wd.bandageDirt >> wd.contamination >> wd.infection >> wd.healed;
            wd.part = (BodyPart)part; wd.type = (WoundType)type; wd.bandaged = band; wd.disinfected = dis;
            b.wounds.push_back(wd);
        }
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
            Campfire f; int lit;
            in >> f.pos.x >> f.pos.y >> f.fuelMinutes >> lit >> f.id;
            f.lit = lit;
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
        in >> w.nextId_;
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
