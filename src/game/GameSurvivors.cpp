// Other people: who they are, where they live, how they behave towards you,
// and what you can say to them.
#include "core/Config.h"
#include "core/Log.h"
#include "game/Game.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace alone {

namespace {

std::vector<std::pair<std::string, int>> itemCounts(const std::vector<std::string>& list) {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& e : list) {
        auto p = split(e, ':');
        if (p.empty() || p[0].empty()) continue;
        out.push_back({p[0], p.size() > 1 ? std::max(1, std::atoi(p[1].c_str())) : 1});
    }
    return out;
}

uint32_t parseColour(const std::string& s, uint32_t def) {
    if (s.empty()) return def;
    return (uint32_t)std::strtoul(s.c_str() + (s[0] == '#' ? 1 : 0), nullptr, 16);
}

// "a|b|c" -> one of them
std::string pickLine(const std::string& s, Rng& rng) {
    auto parts = split(s, '|');
    if (parts.empty()) return s;
    return parts[(size_t)rng.rangeInt(0, (int)parts.size() - 1)];
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string firstName(const SurvivorDef& d) {
    auto sp = d.name.find(' ');
    return sp == std::string::npos ? d.name : d.name.substr(0, sp);
}

bool isHome(BuildingType t) {
    return t == BuildingType::House || t == BuildingType::Cottage || t == BuildingType::Farmhouse || t == BuildingType::SummerCottage;
}

} // namespace

const char* dispositionName(Disposition d) {
    switch (d) {
        case Disposition::Friendly: return "friendly";
        case Disposition::Wary: return "wary";
        case Disposition::Hostile: return "hostile";
        case Disposition::Raider: return "raider";
    }
    return "";
}

bool Survivor::hostileTo() const {
    if (!alive() || joined) return false;
    if (angry) return true;
    if (def->disposition == Disposition::Raider) return !truce;
    return def->disposition == Disposition::Hostile && !admitted && state == SurvivorState::Hunt;
}

const ItemInstance* Survivor::weapon() const {
    int i = inventory.indexOfUid(weaponUid);
    return i >= 0 ? &inventory.items()[i] : nullptr;
}

// ---------------------------------------------------------------------------
// Loading and placing
// ---------------------------------------------------------------------------

bool Game::loadSurvivors(const std::string& path) {
    Config cfg;
    if (!cfg.load(path)) return false;
    survivorDefs_.clear();
    for (const auto& s : cfg.sections()) {
        if (s.name.empty()) continue;
        SurvivorDef d;
        d.id = s.name;
        d.name = s.str("name", d.id);
        d.bio = s.str("bio");
        d.age = s.integer("age", 30);
        std::string disp = s.str("disposition", "friendly");
        d.disposition = disp == "wary" ? Disposition::Wary : disp == "hostile" ? Disposition::Hostile
                      : disp == "raider" ? Disposition::Raider : Disposition::Friendly;
        d.home = s.str("home");
        d.roam = s.num("roam", 10);
        d.route = split(s.str("route"), ';');
        d.route.erase(std::remove(d.route.begin(), d.route.end(), std::string()), d.route.end());
        for (auto& r : d.route) r = trim(r);
        d.wear = s.list("wear");
        d.weapon = s.str("weapon");
        d.carry = itemCounts(s.list("carry"));
        d.skin = parseColour(s.str("skin"), d.skin);
        d.hair = parseColour(s.str("hair"), d.hair);
        d.hp = s.num("hp", 60);
        d.aim = s.num("aim", 0.5f);
        d.greet = s.str("greet");
        d.hello = s.str("hello", "Hello again.");
        d.chat = split(s.str("chat"), '|');
        d.warn = s.str("warn");
        d.parleyText = s.str("parley", "...Fine. Talk.");
        d.letInText = s.str("let_in", "All right. Come in. Slowly.");
        d.keepOutText = s.str("keep_out", "No. You stay out there.");
        d.letInTrust = s.integer("let_in_trust", 25);
        d.describe = s.str("describe");
        d.wantItem = s.str("want");
        d.wantCount = s.integer("want_count", d.wantItem.empty() ? 0 : 1);
        d.wantText = s.str("want_text");
        d.thanksText = s.str("thanks", "Thank you.");
        d.reward = itemCounts(s.list("reward"));
        d.teaches = s.str("teaches");
        d.reveal = s.str("reveal");
        d.revealName = s.str("reveal_name", "a place");
        d.revealText = s.str("reveal_text");
        d.revealTrust = s.integer("reveal_trust", 30);
        d.wantPlace = s.str("want_place");
        d.wantPlaceLevel = s.integer("want_place_level", 0);
        d.wantPlaceName = s.str("want_place_name", "What " + d.name + " asked for");
        d.leads = s.list("leads");
        d.leadText = s.str("lead_text");
        d.canJoin = s.flag("can_join");
        d.joinTrust = s.integer("join_trust", 50);
        d.joinText = s.str("join_text", "All right. I'll come with you.");
        d.refuseText = s.str("refuse_text", "No. Not now.");
        d.partners = s.list("partners");
        std::string g = s.str("gender", "man");
        d.gender = g == "woman" ? Gender::Woman : g == "other" ? Gender::Other : Gender::Man;
        d.hairStyle = std::clamp(s.integer("hair_style", d.gender == Gender::Woman ? 1 : 0), 0, kHairStyles - 1);
        auto lines = [&](const char* key) {
            std::vector<std::string> out;
            for (auto& l : split(s.str(key), '|')) {
                std::string t = trim(l);
                if (!t.empty()) out.push_back(t);
            }
            return out;
        };
        d.about = lines("about");
        d.happened = lines("happened");
        d.happenedLore = lines("happened_lore");   // | between them, like the lines they go with
        d.friendChat = lines("friend_chat");
        d.closeChat = lines("close_chat");
        d.idle = lines("idle");
        d.night = lines("night");
        d.morning = lines("morning");
        d.loverChat = lines("lover_chat");
        d.freeze = s.str("freeze");
        d.romanceable = s.flag("romanceable", d.age >= 18 && d.disposition != Disposition::Raider);
        d.flirtYes = s.str("flirt_yes");
        d.flirtNo = s.str("flirt_no");
        d.confessYes = s.str("confess_yes");
        d.confessNo = s.str("confess_no");
        d.familyText = s.str("family_text");
        d.bedtime = s.integer("bedtime", 22);
        d.wakeup = s.integer("wakeup", 7);
        for (const auto& w : d.wear)
            if (!items_.find(w)) Log::warn("Survivor %s wears unknown item %s", d.id.c_str(), w.c_str());
        survivorDefs_.push_back(d);
    }
    Log::info("Loaded %d survivors", (int)survivorDefs_.size());
    return true;
}

bool Game::resolvePlace(const std::string& spec, Vec2& out, std::string* name) {
    auto inside = [&](const Building& b) {
        // A free tile in the biggest living room / shop floor / office
        const Room* best = nullptr;
        int bestScore = -1;
        for (const auto& r : b.rooms) {
            int score = r.rect.area();
            if (r.type == RoomType::Living || r.type == RoomType::Kitchen || r.type == RoomType::ShopFloor || r.type == RoomType::Office ||
                r.type == RoomType::Lobby)
                score += 1000;
            if (score > bestScore) { bestScore = score; best = &r; }
        }
        IRect rr = best ? best->rect : b.rect;
        Vec2 c = rr.centre();
        for (int ring = 0; ring < 8; ++ring)
            for (int dy = -ring; dy <= ring; ++dy)
                for (int dx = -ring; dx <= ring; ++dx) {
                    int x = (int)c.x + dx, y = (int)c.y + dy;
                    if (!rr.contains(x, y) || world_->tileBlocked(x, y)) continue;
                    out = Vec2{x + 0.5f, y + 0.5f};
                    return true;
                }
        out = b.doorPos;
        return true;
    };
    auto houseNear = [&](Vec2 p, float minD, float maxD, uint32_t pickSeed) -> const Building* {
        IRect area{(int)(p.x - maxD), (int)(p.y - maxD), (int)(maxD * 2), (int)(maxD * 2)};
        std::vector<const Building*> cands;
        for (int id : world_->gen().buildingsIn(area)) {
            const Building* b = world_->building(id);
            if (!b || !isHome(b->type) || b->isOutbuilding || b->rooms.empty()) continue;
            float d = distance(b->rect.centre(), p);
            if (d >= minD && d <= maxD) cands.push_back(b);
        }
        if (cands.empty()) return nullptr;
        std::sort(cands.begin(), cands.end(), [](const Building* a, const Building* b) { return a->id < b->id; });
        return cands[pickSeed % cands.size()];
    };

    if (spec.rfind("building:", 0) == 0) {
        std::string want = spec.substr(9);
        for (const auto& b : world_->gen().allBuildings())
            if (b.name == want || b.sign == want) {
                if (name) *name = want;
                return inside(b);
            }
        Log::warn("No building called %s", want.c_str());
        return false;
    }
    if (spec.rfind("at:", 0) == 0) {
        // at:x,y:Name - inside whatever building stands there (metres), called Name
        auto parts = split(spec.substr(3), ':');
        auto xy = split(parts.empty() ? std::string() : parts[0], ',');
        if (xy.size() == 2) {
            Vec2 p{(float)std::atof(xy[0].c_str()), (float)std::atof(xy[1].c_str())};
            for (int id : world_->gen().buildingsIn(IRect{(int)p.x - 2, (int)p.y - 2, 4, 4})) {
                const Building* b = world_->building(id);
                if (!b || !b->rect.contains((int)p.x, (int)p.y) || b->rooms.empty()) continue;
                if (name) *name = parts.size() > 1 ? parts[1] : map_.districtAt(p);
                return inside(*b);
            }
            Log::warn("No building at %s", spec.c_str());
        }
        return false;
    }
    if (spec.rfind("district:", 0) == 0) {
        std::string want = spec.substr(9);
        for (const auto& d : map_.districts())
            if (d.name == want) {
                const Building* b = houseNear(d.centre, 0, 420, hash32(seed_ ^ (uint32_t)std::hash<std::string>{}(want)));
                if (name) *name = "a house in " + want;
                if (b) return inside(*b);
                out = d.centre;
                return true;
            }
        Log::warn("No district called %s", want.c_str());
        return false;
    }
    if (spec.rfind("start:", 0) == 0) {
        float r = (float)std::atof(spec.c_str() + 6);
        const Building* b = houseNear(player_.pos, r * 0.7f, r * 1.4f, hash32(seed_ * 31 + 5));
        if (!b) b = houseNear(player_.pos, 40, r * 3, hash32(seed_ * 31 + 5));
        if (name) *name = "a house " + std::string(r < 400 ? "a few streets away" : "across town");
        if (b) return inside(*b);
        return false;
    }
    auto xy = split(spec, ',');
    if (xy.size() == 2) {
        out = Vec2{(float)std::atof(xy[0].c_str()), (float)std::atof(xy[1].c_str())};
        if (name) *name = map_.districtAt(out);
        return true;
    }
    return false;
}

void Game::placeSurvivors() {
    survivors_.clear();
    for (size_t i = 0; i < survivorDefs_.size(); ++i) {
        const SurvivorDef& d = survivorDefs_[i];
        Vec2 home;
        std::string where;
        if (!resolvePlace(d.home, home, &where)) continue;
        Survivor s;
        s.def = &d;
        s.id = (int)i + 1;
        s.pos = s.home = s.target = home;
        s.hp = d.hp;
        const_cast<SurvivorDef&>(d).homeName = where;
        s.trust = d.disposition == Disposition::Friendly ? 15 : d.disposition == Disposition::Wary ? -5
                : d.disposition == Disposition::Hostile ? -20 : -100;
        s.state = SurvivorState::Home;
        for (const auto& w : d.wear) {
            ItemInstance it = items_.create(w, rng_);
            if (it.def && it.def->slot != ClothingSlot::None) s.worn[(size_t)it.def->slot] = it;
        }
        if (!d.weapon.empty()) {
            ItemInstance w = items_.create(d.weapon, rng_);
            if (w.def) { s.weaponUid = w.uid; s.inventory.add(w); }
        }
        for (const auto& [id, n] : d.carry)
            for (int k = 0; k < n; ++k) {
                ItemInstance it = items_.create(id, rng_);
                if (it.def && it.def->hasTag("gascan")) { it.liquid = 1; it.liquidMl = it.def->capacityMl; }
                if (it.def) s.inventory.add(it);
            }
        // Whoever lives closest is the light you saw in the window
        s.known = d.home.rfind("start:", 0) == 0;
        s.shownBond = Bond::Stranger;
        survivors_.push_back(std::move(s));
        if (d.disposition != Disposition::Raider) findBed(survivors_.back());
        survivors_.back().idleTimer = rng_.range(2, 20);
        // already in bed if you start at night
        if (survivors_.back().hasBed && sleepTime(survivors_.back())) {
            survivors_.back().state = SurvivorState::Sleep;
            survivors_.back().pos = survivors_.back().bed;
        }
    }
}

Survivor* Game::survivor(int id) {
    for (auto& s : survivors_)
        if (s.id == id) return &s;
    return nullptr;
}

void Game::learnAbout(const std::string& id) {
    for (auto& s : survivors_)
        if (s.def->id == id && !s.known) {
            s.known = true;
            bool roams = s.def->disposition == Disposition::Raider;
            s.rumoured = true;
            s.rumourPos = roams ? s.pos : s.home;
            log("Journal: " + s.def->name + " (" + (roams ? placeOf(s) : s.def->homeName) + ").",
                roams || s.def->disposition == Disposition::Hostile ? MessageKind::Bad : MessageKind::Good);
        }
}

std::string Game::placeOf(const Survivor& s) const {
    if (const Building* b = world_->building(world_->tile((int)std::floor(s.pos.x), (int)std::floor(s.pos.y)).building))
        if (!b->name.empty()) return "at " + b->name;
    std::string street = map_.streetAt(s.pos);
    std::string district = map_.districtAt(s.pos);
    if (!street.empty() && distance(s.pos, s.home) > 20) return "on " + street + ", " + district;
    return "around " + district;
}

// Someone the teller knows about that you don't, and roughly where they are.
// Their own leads first (in their own words), then anyone dangerous nearby.
std::string Game::rumour(Survivor& teller) {
    const SurvivorDef& d = *teller.def;
    bool newLead = false;
    for (const auto& l : d.leads)
        for (const auto& o : survivors_)
            if (o.def->id == l && !o.known && o.alive()) newLead = true;
    if (newLead && !d.leadText.empty()) {
        for (const auto& l : d.leads) learnAbout(l);
        return d.leadText;
    }
    // Anyone else they'd have heard of: people within a couple of km, the dangerous ones first
    Survivor* pick = nullptr;
    float best = 1e9f;
    for (auto& o : survivors_) {
        if (&o == &teller || o.known || !o.alive()) continue;
        float dist = distance(o.pos, teller.pos);
        bool danger = o.def->disposition == Disposition::Raider || o.def->disposition == Disposition::Hostile;
        float score = dist * (danger ? 0.5f : 1.0f);
        if (dist < 2500 && score < best) { best = score; pick = &o; }
    }
    if (!pick) return "No. That's everyone I know of. Maybe it's just us.";
    learnAbout(pick->def->id);
    std::string what = pick->def->describe.empty() ? "" : " " + pick->def->describe;
    switch (pick->def->disposition) {
        case Disposition::Raider:
            return "Watch out for " + pick->def->name + "." + what + " Last I heard, " + placeOf(*pick) + ".";
        case Disposition::Hostile:
            return "Stay away from " + pick->def->homeName + ". " + pick->def->name + " is holed up in there and shoots at anyone." + what;
        default:
            return "There's someone in " + pick->def->homeName + " - " + pick->def->name + "." + what;
    }
}

// Danger they've seen lately: a raider within a few hundred metres
std::string Game::warningNear(const Survivor& teller) {
    for (auto& o : survivors_) {
        if (&o == &teller || !o.alive() || o.def->disposition != Disposition::Raider || o.truce) continue;
        if (distance(o.pos, teller.pos) > 400) continue;
        bool wasKnown = o.known;
        o.known = true;
        o.rumoured = true;
        o.rumourPos = o.pos;
        if (!wasKnown) log("Journal: " + o.def->name + " (" + placeOf(o) + ").", MessageKind::Bad);
        return " Careful out there - I saw " + o.def->name + " " + placeOf(o) + " not long ago.";
    }
    return "";
}

bool Game::canTalk(int id, std::string* why) const {
    const Survivor* s = const_cast<Game*>(this)->survivor(id);
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    if (!s || !s->alive()) return no("They're dead");
    if (s->joined) return true;
    if (s->angry) return no("They want you dead. There's no talking now");
    if (s->def->disposition == Disposition::Raider && !s->truce && s->state != SurvivorState::Hunt && !s->met)
        return no("They haven't seen you. Talking would change that");
    return true;
}

float Game::talkRange(int id) const {
    const Survivor* s = const_cast<Game*>(this)->survivor(id);
    if (!s) return 3.5f;
    // shouting across the yard at someone who won't let you closer
    if ((s->def->disposition == Disposition::Hostile && !s->admitted) || s->def->disposition == Disposition::Raider) return 30.0f;
    return 3.5f;
}

// Hand a raider your best things: worth enough, and they leave you alone for a day
bool Game::offerTribute(Survivor& s) {
    std::vector<std::pair<int, uint32_t>> worth;
    for (const auto& it : player_.inventory.items()) worth.push_back({itemValue(it), it.uid});
    std::sort(worth.begin(), worth.end(), [](auto& a, auto& b) { return a.first > b.first; });
    int total = 0;
    for (auto& w : worth) total += w.first;
    if (total < 25) return false;
    int need = std::max(25, total * 2 / 5), got = 0;
    std::string names;
    for (auto& [v, uid] : worth) {
        if (got >= need) break;
        int i = player_.inventory.indexOfUid(uid);
        if (i < 0) continue;
        if (uid == player_.weaponUid) player_.weaponUid = 0;
        ItemInstance it = player_.inventory.take(i);
        if (names.size() < 80) names += (names.empty() ? "" : ", ") + it.def->name;
        s.inventory.add(it);
        got += v;
    }
    log("They take: " + names + ".", MessageKind::Bad);
    double until = world_->clock().totalMinutes() + 24 * 60;
    // the whole gang honours it
    for (auto& o : survivors_)
        if (&o == &s || std::find(o.def->partners.begin(), o.def->partners.end(), s.def->id) != o.def->partners.end() ||
            std::find(s.def->partners.begin(), s.def->partners.end(), o.def->id) != s.def->partners.end()) {
            o.truceUntil = until;
            o.truce = true;
            o.state = SurvivorState::Home;
        }
    return true;
}

int Game::followers() const {
    int n = 0;
    for (const auto& s : survivors_) n += s.joined && s.alive();
    return n;
}

std::string Game::objective() const {
    int total = 0, met = 0;
    for (const auto& s : survivors_) {
        if (s.def->disposition != Disposition::Friendly && s.def->disposition != Disposition::Wary) continue;
        ++total;
        met += s.met;
    }
    for (const auto& s : survivors_)
        if (s.known && !s.met && s.alive() && (s.def->disposition == Disposition::Friendly || s.def->disposition == Disposition::Wary))
            return "Find " + s.def->name + " (" + s.def->homeName + ")";
    for (const auto& s : survivors_)
        if (s.requestKnown && !s.requestDone && s.alive()) {
            const ItemDef* d = s.def->wantItem.rfind("tag:", 0) == 0 ? nullptr : items_.find(s.def->wantItem);
            std::string what = d ? d->name : s.def->wantItem.substr(4);
            return firstName(*s.def) + " needs " + (s.def->wantCount > 1 ? std::to_string(s.def->wantCount) + " x " : "") + lower(what);
        }
    if (met < total) return "Find the others still in Alingsås (" + std::to_string(met) + " of " + std::to_string(total) + ")";
    return "Keep your people alive through the winter";
}

int Game::countFor(const std::string& want) const {
    int n = 0;
    bool tag = want.rfind("tag:", 0) == 0;
    std::string key = tag ? want.substr(4) : want;
    for (const auto& it : player_.inventory.items())
        if (tag ? it.def->hasTag(key) : it.def->id == key) ++n;
    return n;
}

void Game::takeFor(const std::string& want, int count) {
    bool tag = want.rfind("tag:", 0) == 0;
    std::string key = tag ? want.substr(4) : want;
    auto& v = player_.inventory.items();
    for (size_t i = 0; i < v.size() && count > 0;) {
        if (tag ? v[i].def->hasTag(key) : v[i].def->id == key) {
            if (v[i].uid == player_.weaponUid) player_.weaponUid = 0;
            v.erase(v.begin() + i);
            --count;
        } else {
            ++i;
        }
    }
}

// ---------------------------------------------------------------------------
// Talking and trading
// ---------------------------------------------------------------------------

namespace {
const char* bondKey(Bond b) {
    switch (b) {
        case Bond::Enemy: return "enemy";
        case Bond::Stranger: return "stranger";
        case Bond::Acquaintance: return "acquaintance";
        case Bond::Friend: return "friend";
        case Bond::BestFriend: return "bestfriend";
        case Bond::Family: return "family";
        case Bond::Lover: return "lover";
        default: return "any";
    }
}
} // namespace

std::string Game::greeting(int id) {
    Survivor* s = survivor(id);
    if (!s) return "";
    if (inTentWith(id) && !player_.asleep) return personal(line(std::string("tent_in.") + bondKey(s->bond())));
    const SurvivorDef& d = *s->def;
    bool hostile = d.disposition == Disposition::Hostile && !s->admitted;
    bool raider = d.disposition == Disposition::Raider;
    // Woken up in the night
    if (s->asleep()) {
        wakeSurvivor(*s);
        s->awakeHold = 120;
        s->wokenUp = true;
        s->rumourPos = s->pos;
        if (!s->met) {
            s->met = true;
            s->known = true;
            s->rumoured = false;
            log("You met " + d.name + ".", hostile ? MessageKind::Info : MessageKind::Good);
            if (hostile) { s->warned = 4; return "WHAT - who's there?! How did you get in here? Don't move!"; }
            addTrust(*s, -4);
            return personal(line("night.firstmeet"));
        }
        Bond b = s->bond();
        if (hostile) { s->warned = 4; return "You?! In here, at night? Give me one reason not to shoot."; }
        if (b <= Bond::Acquaintance) addTrust(*s, -3);
        else if (b == Bond::Friend) addTrust(*s, -1);
        return personal(pickOf(b >= Bond::Friend ? std::vector<std::string>{} : d.night, std::string("night.") + bondKey(b)));
    }
    if (hostile) s->warned = std::min(s->warned, -15.0f);   // he'll hear you out for a moment
    if (raider && !s->truce)   // a moment before the first blow
        for (auto& o : survivors_)
            if (o.def->disposition == Disposition::Raider && distance(o.pos, s->pos) < 30) o.attackTimer = std::max(o.attackTimer, 5.0f);
    std::string warn = (hostile || raider) ? "" : warningNear(*s);
    s->rumourPos = s->pos;   // you saw them here
    if (!s->met) {
        s->met = true;
        s->known = true;
        s->rumoured = false;
        log("You met " + d.name + ".", raider || hostile ? MessageKind::Info : MessageKind::Good);
        if (raider) return s->truce ? "You again. We had a deal - so walk." : "Ha! Now you want to talk? Empty that bag on the ground.";
        std::string intro = hostile ? "" : " " + line("intro.any");
        return personal((d.greet.empty() ? std::string(hostile ? "Stop right there! What do you want?" : "Hello.") : d.greet) + intro) + warn;
    }
    s->rumoured = false;
    if (raider) return s->truce ? "Keep walking. The deal holds till tomorrow." : "Back again? Then you know what we want.";
    if (hostile) return s->trust > 0 ? "You. What now? Say it from there." : "I told you to stay away!";
    Bond b = s->bond();
    if (b == Bond::Enemy) return personal(line("hello.enemy")) + warn;
    std::string hello = b >= Bond::Friend && rng_.chance(0.6f) ? line(std::string("hello.") + bondKey(b)) : pickLine(d.hello, rng_);
    if (hello.empty()) hello = pickLine(d.hello, rng_);
    // They notice how you look
    std::string concern;
    if (b >= Bond::Acquaintance && rng_.chance(0.6f)) {
        BodySignals g = player_.body.signals();
        const char* key = g.bleeding > 0.1f ? "concern.bleeding" : g.pain > 0.45f ? "concern.hurt" : g.cold > 0.45f ? "concern.cold"
                        : g.fever > 0.4f ? "concern.sick" : g.tired > 0.7f ? "concern.tired" : g.hunger > 0.75f ? "concern.hungry" : nullptr;
        if (key) concern = " " + line(key);
    }
    // Family and lovers keep something aside for you now and then
    std::string gift;
    int today = world_->clock().day();
    if ((b == Bond::Family || b == Bond::Lover) && s->lastGiftDay != today && rng_.chance(0.45f)) {
        s->lastGiftDay = today;
        int pick = -1;
        for (size_t i = 0; i < s->inventory.items().size(); ++i) {
            const ItemInstance& it = s->inventory.items()[i];
            if (it.uid == s->weaponUid) continue;
            ItemCategory c = it.def->category;
            if ((c == ItemCategory::Food && !it.rotten()) || c == ItemCategory::Medical || c == ItemCategory::Drink) { pick = (int)i; if (rng_.chance(0.5f)) break; }
        }
        if (pick >= 0) {
            ItemInstance it = s->inventory.take(pick);
            gift = " " + line(b == Bond::Lover ? "gift.lover" : "gift.family");
            log(firstName(d) + " gives you: " + it.def->name + ".", MessageKind::Good);
            giveItem(it);
        }
    }
    return personal(hello + concern + gift) + warn;
}

std::vector<std::pair<Topic, std::string>> Game::talkTopics(int id) {
    std::vector<std::pair<Topic, std::string>> out;
    Survivor* s = survivor(id);
    if (!s || !canTalk(id)) return out;
    const SurvivorDef& d = *s->def;
    if (d.disposition == Disposition::Raider) {
        if (!s->truce) {
            out.push_back({Topic::PayOff, "Take what you want. Just let me go."});
            out.push_back({Topic::Threaten, "Back off, or I'll put you down."});
        } else {
            out.push_back({Topic::Others, "Seen anyone else around?"});
        }
        out.push_back({Topic::Bye, "(Walk away.)"});
        return out;
    }
    if (d.disposition == Disposition::Hostile && !s->admitted) {
        out.push_back({Topic::Parley, "Easy! I'm not here to fight."});
        if (s->trust > -30) {
            out.push_back({Topic::Trade, "I've got things you might need. Let's make a deal."});
            if (d.wantCount > 0 && !s->requestDone) {
                if (!s->requestKnown) out.push_back({Topic::Request, "What do you need? Maybe I can get it."});
                else if (countFor(d.wantItem) >= d.wantCount) out.push_back({Topic::Give, "I brought what you asked for."});
                else out.push_back({Topic::Request, "What was it you needed?"});
            }
            out.push_back({Topic::Others, "Have you seen anyone else out here?"});
            out.push_back({Topic::LetIn, "Let me in. I just want to talk face to face."});
        }
        out.push_back({Topic::Bye, "(Back away.)"});
        return out;
    }
    Bond b = s->bond();
    int today = world_->clock().day();
    if (s->wokenUp && sleepTime(*s)) out.push_back({Topic::BackToSleep, "Sorry I woke you. Go back to sleep."});
    if (b == Bond::Enemy) {
        if (s->lastSorryDay != today) out.push_back({Topic::Sorry, "I'm sorry. For whatever I did."});
        out.push_back({Topic::Bye, "(Leave them be.)"});
        return out;
    }
    // Lying in the tent together
    if (inTentWith(id) && !player_.asleep) {
        out.push_back({Topic::TentSleep, "Let's sleep now."});
        if (b == Bond::Lover) {
            if (s->lastTentHoldDay != today) out.push_back({Topic::TentHold, "Come here. (Hold them close.)"});
            if (s->lastTentWhisperDay != today) out.push_back({Topic::TentWhisper, "Tell me something. Anything. (Whisper in the dark.)"});
        }
        out.push_back({Topic::Chat, b >= Bond::Friend ? "So, how are you, really?" : "How are you holding up?"});
        out.push_back({Topic::Bye, "(Not yet. Lie awake a while.)"});
        return out;
    }
    out.push_back({Topic::Chat, b >= Bond::Friend ? "So, how are you, really?" : "How are you holding up?"});
    if (s->storiesTold < (int)d.about.size())
        out.push_back({Topic::About, s->storiesTold == 0 ? "Tell me about yourself." : "Tell me more about you."});
    out.push_back({Topic::WhatHappened, s->happenedTold == 0 ? "What happened? How did it all end so fast?" : "Tell me more about what happened."});
    if (s->lastJokeDay != today) out.push_back({Topic::Joke, "Want to hear a joke?"});
    out.push_back({Topic::Others, "Have you seen anyone else?"});
    if (d.wantCount > 0 && !s->requestDone) {
        if (!s->requestKnown) out.push_back({Topic::Request, "Is there anything you need?"});
        else {
            int have = countFor(d.wantItem);
            std::string what = d.wantItem.rfind("tag:", 0) == 0 ? d.wantItem.substr(4) : (items_.find(d.wantItem) ? lower(items_.find(d.wantItem)->name) : d.wantItem);
            if (have >= d.wantCount) out.push_back({Topic::Give, "Here - I brought what you needed (" + what + ")"});
            else out.push_back({Topic::Request, "What was it you needed again?"});
        }
    }
    out.push_back({Topic::Trade, "Do you want to trade?"});
    // Getting closer
    if (d.romanceable && b != Bond::Lover && !s->family && s->trust >= 20 && s->lastFlirtDay != today) {
        static const char* flirts[] = {"You look good today, you know.", "I like spending time with you.", "Has anyone told you that you have a lovely smile?",
                                       "I find myself thinking about you out there.", "You make this place feel less empty."};
        out.push_back({Topic::Flirt, flirts[(today + s->id) % 5]});
    }
    // ...or growing close the other way: looking out for each other
    if (b != Bond::Lover && !s->family && s->trust >= 20 && s->lastKinDay != today) {
        static const char* kin[] = {"Whatever happens out there, I've got your back.", "Tell me about the people you lost. I want to know.",
                                    "Let's eat together tonight. Like a proper family.", "Can I tell you about my family? Before all this.",
                                    "If you ever need anything, you come to me. Anything."};
        out.push_back({Topic::Kin, kin[(today * 3 + s->id) % 5]});
    }
    if (d.romanceable && b != Bond::Lover && !s->family && s->romance >= kBondNeeded && s->trust >= kTrustBestFriend)
        out.push_back({Topic::Confess, "I think I'm falling for you."});
    if (!s->family && b != Bond::Lover && s->kinship >= kBondNeeded && s->trust >= kTrustBestFriend)
        out.push_back({Topic::Family, "You're family to me now. You know that?"});
    if (b >= Bond::BestFriend) out.push_back({Topic::Hug, b == Bond::Lover ? "Come here. (Hold them close.)" : "Come here. (Hug them.)"});
    if (s->joined) {
        if (s->state == SurvivorState::Wait) out.push_back({Topic::Follow, "Come with me."});
        else out.push_back({Topic::Wait, "Wait here."});
        out.push_back({Topic::Dismiss, "Go back home for now."});
    } else if (d.canJoin || b == Bond::Family || b == Bond::Lover) {
        out.push_back({Topic::Join, b == Bond::Lover ? "Come with me. I want you by my side." : "Come with me. We're safer together."});
    }
    if (b == Bond::Lover) out.push_back({Topic::Breakup, "I think we should just be friends."});
    out.push_back({Topic::Bye, b >= Bond::Friend ? "See you later." : "Goodbye."});
    return out;
}

std::string Game::romanceHint(int id) {
    Survivor* s = survivor(id);
    if (!s || !canTalk(id)) return "";
    const SurvivorDef& d = *s->def;
    if (d.disposition == Disposition::Raider || (d.disposition == Disposition::Hostile && !s->admitted)) return "";
    Bond b = s->bond();
    if (b == Bond::Lover || b == Bond::Enemy) return "";
    if (!d.romanceable) return d.age < 18 ? "" : "(Romance: they're not looking for that.)";
    if (s->family) return "(Romance: you're family now.)";
    if (s->trust < 20) return "(Romance: they need to trust you more first.)";
    if (s->lastFlirtDay == world_->clock().day()) {
        if (s->romance >= kBondNeeded && s->trust < kTrustBestFriend) return "(Romance: you've flirted today. Be best friends to say more.)";
        return "(Romance: you've flirted today. Try again tomorrow.)";
    }
    return "";
}

std::string Game::talk(int id, Topic t) {
    Survivor* s = survivor(id);
    if (!s || !s->alive()) return "";
    const SurvivorDef& d = *s->def;
    bool wary = d.disposition == Disposition::Wary;
    Bond b = s->bond();
    int today = world_->clock().day();
    std::string bk = bondKey(b);
    auto say = [&](const std::string& l) { return personal(l); };
    switch (t) {
        case Topic::Chat: {
            // Once a day, a proper talk brings you closer
            if (s->lastChatDay != today) { s->lastChatDay = today; addTrust(*s, 3, kTrustBestFriend + 5); }
            else addTrust(*s, 0.5f, 25);
            // Something about right now, or something of their own
            float rain = world_->clock().weather().rain;
            float h = world_->clock().hourOfDay();
            if (rng_.chance(0.3f)) {
                std::string now = rain > 0.3f ? line(world_->clock().weather().kind == WeatherKind::Snow ? "chat.snow" : "chat.rain")
                                : world_->outdoorTemp() < 2 ? line("chat.cold") : h >= 20 || h < 5 ? line("chat.night") : h < 10 ? line("chat.morning") : "";
                if (!now.empty()) return say(now);
            }
            const std::vector<std::string>* own = &d.chat;
            if (b == Bond::Lover && !d.loverChat.empty()) own = &d.loverChat;
            else if (b >= Bond::BestFriend && !d.closeChat.empty() && rng_.chance(0.6f)) own = &d.closeChat;
            else if (b >= Bond::Friend && !d.friendChat.empty() && rng_.chance(0.6f)) own = &d.friendChat;
            if (own->empty() || rng_.chance(0.35f)) {
                std::string g = line("chat." + bk);
                if (!g.empty()) return say(g);
            }
            if (own->empty()) return "We get by.";
            return say(pickOf(*own, "chat.any"));
        }
        case Topic::About: {
            if (s->storiesTold >= (int)d.about.size()) return say(line("about.none"));
            // each piece of their story needs a little more trust
            float need = s->storiesTold * 10.0f - (wary ? 0.0f : 5.0f);
            if (s->trust < need) return say(line("about.notyet"));
            std::string story = d.about[(size_t)s->storiesTold++];
            addTrust(*s, 2, 90);
            return say(story);
        }
        case Topic::Joke: {
            s->lastJokeDay = today;
            float odds = 0.55f + s->trust / 200.0f - (wary ? 0.15f : 0.0f);
            if (rng_.chance(odds)) { addTrust(*s, 2, 95); return say(line("joke.good")); }
            addTrust(*s, -0.5f);
            return say(line("joke.bad"));
        }
        case Topic::Flirt: {
            s->lastFlirtDay = today;
            float odds = std::clamp((s->trust - 15) / 55.0f + s->romance / 200.0f, 0.1f, 0.95f);
            if (rng_.chance(odds)) {
                s->romance = std::min(100.0f, s->romance + rng_.range(10, 16));
                s->kinship = std::max(0.0f, s->kinship - 8);   // it stops feeling like family
                addTrust(*s, 2, 100);
                std::string l = !d.flirtYes.empty() && rng_.chance(0.5f) ? pickLine(d.flirtYes, rng_) : line(s->romance >= 50 ? "flirt.warm" : "flirt.yes");
                return say(l);
            }
            addTrust(*s, -2);
            return say(!d.flirtNo.empty() && rng_.chance(0.5f) ? pickLine(d.flirtNo, rng_) : line("flirt.no"));
        }
        case Topic::Kin: {
            s->lastKinDay = today;
            float odds = std::clamp((s->trust - 10) / 50.0f + s->kinship / 200.0f, 0.15f, 0.95f);
            if (rng_.chance(odds)) {
                s->kinship = std::min(100.0f, s->kinship + rng_.range(10, 16));
                s->romance = std::max(0.0f, s->romance - 8);   // and less like romance
                addTrust(*s, 2, 100);
                return say(line(s->kinship >= kBondNeeded ? "kin.warm" : "kin.yes"));
            }
            addTrust(*s, -1);
            return say(line("kin.no"));
        }
        case Topic::Confess:
            if (s->kinship >= 40) {   // they see you as family, not like that
                s->romance = std::max(0.0f, s->romance - 20);
                return say(line("confess.family"));
            }
            if (s->romance >= kBondNeeded && s->trust >= kTrustBestFriend) {
                s->lover = true;
                s->romance = std::max(s->romance, 70.0f);
                addTrust(*s, 5, 100);
                sound(Sfx::Click, player_.pos);
                return say(d.confessYes.empty() ? line("confess.yes") : d.confessYes);
            }
            s->romance = std::max(0.0f, s->romance - 20);
            return say(d.confessNo.empty() ? line("confess.no") : d.confessNo);
        case Topic::Family:
            if (s->romance >= 40) return say(line("family.romance"));   // they hoped for something else
            if (s->kinship >= kBondNeeded && s->trust >= kTrustBestFriend) {
                s->family = true;
                addTrust(*s, 3, 100);
                return say(d.familyText.empty() ? line("family.yes") : d.familyText);
            }
            return say(line("family.no"));
        case Topic::Hug:
            addTrust(*s, 0.5f, 100);
            if (player_.body.signals().cold > 0.3f) player_.body.mutableState().coreTempC += 0.05f;   // a little warmth
            return say(line(b == Bond::Lover ? "hug.lover" : b == Bond::Family ? "hug.family" : "hug.friend"));
        case Topic::Breakup:
            s->lover = false;
            s->romance = 20;
            addTrust(*s, -10);
            announceBond(*s);
            return say(line("breakup.any"));
        case Topic::Sorry:
            s->lastSorryDay = today;
            if (s->angry) return "...";
            addTrust(*s, 6);
            return say(line(s->bond() == Bond::Enemy ? "sorry.no" : "sorry.ok"));
        case Topic::BackToSleep:
            s->awakeHold = 0;
            s->wokenUp = false;
            return say(line("sleep.back"));
        case Topic::Others:
            if (s->trust < (wary ? 15 : d.disposition == Disposition::Hostile ? 0 : -50))
                return d.disposition == Disposition::Hostile ? "Why would I tell you anything?" : "I don't know you well enough to tell you that.";
            // the place they've kept to themselves
            if (!d.reveal.empty() && s->trust >= d.revealTrust && !knowsPlace(d.revealName)) {
                Vec2 at;
                if (resolvePlace(d.reveal, at, nullptr)) {
                    knownPlaces_.push_back({d.revealName, at});
                    log("Marked on your map (M): " + d.revealName + ".", MessageKind::Good);
                    return personal(d.revealText);
                }
            }
            return say(rumour(*s));
        case Topic::Parley:
            s->warned = -40;
            if (s->state == SurvivorState::Hunt) s->state = SurvivorState::Home;
            s->trust = std::min(s->trust + 8.0f, 20.0f);
            return d.parleyText;
        case Topic::LetIn:
            if (s->trust >= d.letInTrust) {
                s->admitted = true;
                s->state = SurvivorState::Home;
                s->warned = 0;
                log(d.name + " lets you in.", MessageKind::Good);
                return d.letInText;
            }
            return d.keepOutText;
        case Topic::PayOff:
            if (offerTribute(*s)) return "Smart. Now get lost - and if we see you tomorrow, it starts again.";
            s->state = SurvivorState::Hunt;
            s->stateTimer = 25;
            return "That's it? That's NOTHING. Get them!";
        case Topic::Threaten: {
            // a loaded gun in your hands, against what they're carrying
            const ItemInstance* mine = weapon();
            bool loaded = mine && mine->def->firearm &&
                          player_.inventory.find([&](const ItemInstance& it) { return it.def->id == mine->def->ammo && it.usesLeft > 0; }) >= 0;
            const ItemInstance* theirs = s->weapon();
            float odds = loaded ? (theirs && theirs->def->firearm ? 0.45f : 0.8f) : 0.08f;
            odds *= std::clamp(s->hp / s->def->hp + 0.3f, 0.3f, 1.0f);
            if (rng_.chance(odds)) {
                for (auto& o : survivors_)
                    if (&o == s || std::find(o.def->partners.begin(), o.def->partners.end(), d.id) != o.def->partners.end() ||
                        std::find(d.partners.begin(), d.partners.end(), o.def->id) != d.partners.end()) {
                        o.state = SurvivorState::Flee;
                        o.stateTimer = 20;
                    }
                s->truceUntil = world_->clock().totalMinutes() + 90;
                return loaded ? "Whoa - okay, OKAY. We're going." : "...You're crazy. Fine. Not worth it.";
            }
            s->state = SurvivorState::Hunt;
            s->stateTimer = 25;
            return loaded ? "You won't. But I will." : "With what? Your fists? Get them!";
        }
        case Topic::Request:
            s->requestKnown = true;
            // the story put it somewhere: make that floor (and the item on it) and mark it on your map
            if (!d.wantPlace.empty() && !s->requestDone && !knowsPlace(d.wantPlaceName)) {
                int base = baseBuildingAt(d.wantPlace);
                if (base >= 0 && ensureFloor(base, d.wantPlaceLevel) >= 0) {
                    auto spot = storySpots_.find(d.id);
                    if (spot != storySpots_.end()) {
                        knownPlaces_.push_back(spot->second);
                        log("Marked on your map (M), " + lower(floorName(spot->second.level)) + ": " + spot->second.name + ".", MessageKind::Good);
                    }
                }
            }
            return say(d.wantText);
        case Topic::Give: {
            if (countFor(d.wantItem) < d.wantCount) return "You don't have it.";
            takeFor(d.wantItem, d.wantCount);
            s->requestDone = true;
            // found: its mark comes off the map
            knownPlaces_.erase(std::remove_if(knownPlaces_.begin(), knownPlaces_.end(),
                                              [&](const KnownPlace& k) { return k.name == d.wantPlaceName; }), knownPlaces_.end());
            addTrust(*s, 40);
            std::string got;
            for (const auto& [rid, n] : d.reward)
                for (int k = 0; k < n; ++k) {
                    ItemInstance it = items_.create(rid, rng_);
                    if (!it.def) continue;
                    // the reward comes out of their pack when they have it
                    int idx = s->inventory.findId(rid);
                    if (idx >= 0 && s->inventory.items()[idx].uid != s->weaponUid) it = s->inventory.take(idx);
                    giveItem(it);
                    if (k == 0) got += (got.empty() ? "" : ", ") + it.def->name + (n > 1 ? " x" + std::to_string(n) : "");
                }
            if (!got.empty()) log(firstName(d) + " gave you: " + got + ".", MessageKind::Good);
            if (d.teaches == "hotwire" && !player_.knowsHotwire) {
                player_.knowsHotwire = true;
                log("You learned to hotwire a car (you need a screwdriver).", MessageKind::Good);
            } else if (d.teaches == "carpentry" && !player_.knowsCarpentry) {
                player_.knowsCarpentry = true;
                log("You learned to build doors, windows and roofs.", MessageKind::Good);
            } else if (d.teaches == "mushrooms" && !player_.knowsMushroomsTaught) {
                player_.knowsMushroomsTaught = true;
                log("You can tell the mushrooms apart now.", MessageKind::Good);
            }
            for (const auto& l : d.leads) learnAbout(l);
            if (d.disposition == Disposition::Hostile && s->trust >= d.letInTrust && !s->admitted) {
                s->admitted = true;
                log(d.name + " lets you in.", MessageKind::Good);
            }
            return say(d.thanksText);
        }
        case Topic::Trade:
            if (b == Bond::Enemy) return "Trade? With you? No.";
            if (wary && s->trust < 0) return "Trade? I don't even know your name.";
            if (d.disposition == Disposition::Hostile && !s->admitted)
                return s->trust > -30 ? "Let's see what you've got. Leave it by the door." : "Get lost.";
            return "Let's see what you've got.";
        case Topic::Join: {
            bool close = b == Bond::Family || b == Bond::Lover;
            if (!close && (!d.canJoin || s->trust < d.joinTrust)) return say(d.refuseText);
            s->joined = true;
            s->state = SurvivorState::Follow;
            s->path.clear();
            log(d.name + " is coming with you.", MessageKind::Good);
            if (close && !d.canJoin) return say(line(b == Bond::Lover ? "join.lover" : "join.family"));
            return say(d.joinText);
        }
        case Topic::Follow:
            s->state = SurvivorState::Follow;
            return say(b >= Bond::Friend ? line("follow.friend") : "Right behind you.");
        case Topic::Wait:
            s->state = SurvivorState::Wait;
            s->target = s->pos;
            return say(b >= Bond::Friend ? line("wait.friend") : "I'll wait here. Don't be long.");
        case Topic::Dismiss:
            s->joined = false;
            s->state = SurvivorState::Home;
            s->path.clear();
            return say(b == Bond::Lover ? line("dismiss.lover") : "I'll head home. You know where to find me.");
        case Topic::WhatHappened: {
            int i = s->happenedTold;
            if (i < (int)d.happened.size()) {
                float need = (wary ? 15.0f : 5.0f) + 12.0f * i;
                if (s->trust < need) return i == 0 ? "...Not now. I don't know you." : "There's more. Ask me again when I know you better.";
                s->happenedTold++;
                if (i < (int)d.happenedLore.size() && !d.happenedLore[(size_t)i].empty() && d.happenedLore[(size_t)i] != "-")
                    learnLore(d.happenedLore[(size_t)i], false);
                return personal(d.happened[(size_t)i]);
            }
            return personal(line(std::string("happened.") + bondKey(b)));
        }
        case Topic::TentSleep: {
            if (!inTentWith(id)) return "";
            std::string l = personal(line(std::string("tent_sleep.") + bondKey(b)));
            ActionOption o;
            o.kind = ActionKind::Sleep;
            o.label = "Sleeping";
            perform(o);
            tentMate_ = s->id;   // still beside you
            survivorSay(*s, l, false);
            return l;
        }
        case Topic::TentHold:
            s->lastTentHoldDay = today;
            addTrust(*s, 2);
            player_.body.mutableState().wetness = std::max(0.0f, player_.body.state().wetness - 0.1f);   // dry warmth
            return personal(line("tent_hold.lover"));
        case Topic::TentWhisper:
            s->lastTentWhisperDay = today;
            addTrust(*s, 2);
            if (!d.loverChat.empty() && rng_.chance(0.4f)) return personal(pickLine(d.loverChat[rng_.rangeInt(0, (int)d.loverChat.size() - 1)], rng_));
            return personal(line("tent_whisper.lover"));
        case Topic::Bye:
            if (inTentWith(id)) return "Mm. Say when you're ready to sleep.";
            if (d.disposition == Disposition::Hostile && !s->admitted) {
                // back to his post; he leaves you be for a while
                if (s->state == SurvivorState::Alert) s->state = SurvivorState::Home;
                s->calledYou = false;
                s->freezeCooldown = 90;
                return "Go on, then. Slowly.";
            }
            if (b >= Bond::Friend) return say(line("bye." + bk));
            return wary ? "Mm." : "Take care out there.";
        default: break;
    }
    return "";
}

int Game::itemValue(const ItemInstance& it) const {
    const ItemDef& d = *it.def;
    float v = 1;
    switch (d.category) {
        case ItemCategory::Food:
            if (it.rotten()) return 0;
            v = 2 + d.nutrients.scaled(it.portion).kcal() / 120.0f;
            break;
        case ItemCategory::Drink: v = 3; break;
        case ItemCategory::Medical: v = d.hasTag("antibiotic") ? 35 : d.treats != Pathogen::None ? 25 : 10; break;
        case ItemCategory::Weapon: v = d.damage * 0.6f + (d.firearm ? 45 : 0); break;
        case ItemCategory::Ammo: v = std::max(1, it.usesLeft) * (d.id == "arrows" || d.id == "crossbow_bolts" ? 2.5f : 2.0f); break;
        case ItemCategory::Tool: v = 8; break;
        case ItemCategory::Clothing: v = 3 + d.clo * 12 + d.waterproof * 4 + d.carryBonusKg * 2; break;
        case ItemCategory::Container: v = 5; break;
        case ItemCategory::Material: v = 2; break;
        case ItemCategory::Literature: v = d.id == "carpentry_book" || d.id == "field_guide" ? 12 : 2; break;
        default: v = 1; break;
    }
    if (d.capacityMl > 0 && it.liquid == 0) v += std::min(it.liquidMl, 2000.0f) / 200.0f;
    if (it.liquid == 1) v += it.liquidMl / 1000.0f * 1.5f;   // petrol is precious
    if (d.uses > 0 && d.category != ItemCategory::Ammo && d.category != ItemCategory::Food && d.uses > 1)
        v *= 0.3f + 0.7f * it.usesLeft / (float)d.uses;
    v *= 0.4f + 0.6f * it.condition;
    return std::max(1, (int)std::lround(v));
}

bool Game::tradeFair(int id, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs, std::string* why) const {
    const Survivor* s = const_cast<Game*>(this)->survivor(id);
    if (!s || !s->alive()) return false;
    int give = 0, take = 0;
    for (uint32_t u : mine) {
        int i = player_.inventory.indexOfUid(u);
        if (i >= 0) give += itemValue(player_.inventory.items()[i]);
    }
    for (uint32_t u : theirs) {
        int i = s->inventory.indexOfUid(u);
        if (i < 0) continue;
        if (u == s->weaponUid && !s->joined) { if (why) *why = "\"Not the " + lower(s->inventory.items()[i].def->name) + ". I need that.\""; return false; }
        take += itemValue(s->inventory.items()[i]);
    }
    if (s->def->disposition == Disposition::Wary && s->trust < 0) { if (why) *why = "\"I don't trade with strangers.\""; return false; }
    if (s->def->disposition == Disposition::Raider) { if (why) *why = "They don't trade."; return false; }
    if (give == 0 && take == 0) { if (why) *why = "Pick what to give and what to take."; return false; }
    // Friends give you a better deal; companions share
    Bond b = s->bond();
    if (b == Bond::Enemy) { if (why) *why = "\"I'm not trading with you.\""; return false; }
    float markup = s->joined ? 0.0f
                 : b == Bond::Lover || b == Bond::Family ? 0.6f
                 : b == Bond::BestFriend ? 0.85f
                 : b == Bond::Friend ? 1.0f
                 : b == Bond::Acquaintance ? 1.25f : 1.5f;
    if (s->def->disposition == Disposition::Hostile && !s->admitted) markup = 1.8f;   // he names the price
    if (give < take * markup) {
        if (why) *why = "\"That's not enough. I'd want more for that.\"";
        return false;
    }
    return true;
}

bool Game::trade(int id, const std::vector<uint32_t>& mine, const std::vector<uint32_t>& theirs) {
    std::string why;
    if (!tradeFair(id, mine, theirs, &why)) return false;
    Survivor* s = survivor(id);
    int give = 0, take = 0;
    for (uint32_t u : mine) {
        int i = player_.inventory.indexOfUid(u);
        if (i < 0) continue;
        give += itemValue(player_.inventory.items()[i]);
        if (u == player_.weaponUid) player_.weaponUid = 0;
        s->inventory.add(player_.inventory.take(i));
    }
    for (uint32_t u : theirs) {
        int i = s->inventory.indexOfUid(u);
        if (i < 0) continue;
        take += itemValue(s->inventory.items()[i]);
        if (u == s->weaponUid) s->weaponUid = 0;
        giveItem(s->inventory.take(i));
    }
    // A gift (or a generous deal) makes friends
    if (give > take) addTrust(*s, std::min(20.0f, 2.0f + (give - take) / 4.0f));
    else addTrust(*s, 1, 60);
    sound(Sfx::Click, player_.pos);
    // A companion without a weapon picks up the best one they have
    if (s->weaponUid == 0) {
        float best = 0;
        for (const auto& it : s->inventory.items())
            if (it.def->category == ItemCategory::Weapon && it.def->damage > best) { best = it.def->damage; s->weaponUid = it.uid; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

void Game::survivorSay(Survivor& s, const std::string& line, bool toLog) {
    if (line.empty() || line == "...") return;
    s.speech = line;
    s.speechTimer = 4.5f;
    if (toLog && distance(s.pos, player_.pos) < 40) log(firstName(*s.def) + ": \"" + line + "\"", MessageKind::Info);
}

void Game::playerHurtBy(float severity, const std::string& who, bool gunshot) {
    sound(Sfx::Hurt, player_.pos);
    if (player_.asleep) { cancelAction(); log("You wake with a start!", MessageKind::Bad); }
    static const BodyPart parts[] = {BodyPart::LeftLeg, BodyPart::RightLeg, BodyPart::LeftArm, BodyPart::RightArm, BodyPart::Torso,
                                     BodyPart::Torso, BodyPart::LeftHand, BodyPart::RightHand, BodyPart::Head};
    Wound w;
    w.part = parts[rng_.rangeInt(0, 8)];
    w.type = WoundType::Laceration;
    // a helmet or body armour takes part of it
    float armour = armourAt(w.part);
    if (armour > 0) {
        severity *= 1.0f - armour;
        if (severity < 0.06f) {
            log(std::string(gunshot ? "A shot slams into your " : "A blow glances off your ") + (w.part == BodyPart::Head ? "helmet." : "armour."),
                MessageKind::Info);
            return;
        }
    }
    w.severity = clamp01(severity);
    w.bleeding = (gunshot ? 14.0f : 8.0f) + 45.0f * severity * severity;
    w.contamination = gunshot ? 0.35f : 0.3f;
    player_.body.addWound(w);
    log((gunshot ? "You're hit! " : "") + who + (gunshot ? " got your " : " cuts your ") + bodyPartName(w.part) + "!", MessageKind::Bad);
}

void Game::hurtSurvivor(Survivor& s, float damage, bool byPlayer) {
    if (!s.alive()) return;
    s.hp -= damage;
    s.lastAttack = 0;
    sound(Sfx::Hit, s.pos);
    if (s.hp <= 0) {
        s.state = SurvivorState::Dead;
        s.hp = 0;
        s.joined = false;
        // what they had falls where they lie
        for (const auto& it : s.inventory.items()) world_->dropItem(s.pos, it);
        for (const auto& it : s.worn)
            if (it.def) world_->dropItem(s.pos, it);
        s.inventory.items().clear();
        for (auto& it : s.worn) it = ItemInstance{};
        s.weaponUid = 0;
        log(s.def->name + " falls and doesn't get up.", byPlayer && !s.hostileTo() && !s.angry ? MessageKind::Bad : MessageKind::Info);
        return;
    }
    if (byPlayer && !s.angry && s.def->disposition != Disposition::Raider) {
        bool wasHunting = s.def->disposition == Disposition::Hostile;
        s.angry = true;
        s.joined = false;
        s.trust = -100;
        s.lover = s.family = false;
        if (s.asleep()) wakeSurvivor(s);
        if (!wasHunting) survivorSay(s, rng_.chance(0.5f) ? "What are you doing?!" : "Stop! STOP!");
        // the others hear about it
        for (auto& o : survivors_)
            if (&o != &s && o.alive() && distance(o.pos, s.pos) < 30) o.trust -= 30;
    }
    bool armed = s.weapon() != nullptr;
    if (s.hp < s.def->hp * (s.def->disposition == Disposition::Raider ? 0.25f : 0.45f) || (!armed && !s.joined)) {
        s.state = SurvivorState::Flee;
        s.stateTimer = 12;
    } else if (!s.joined) {
        s.state = SurvivorState::Hunt;
    }
}

// One attack against the player (atPlayer), an animal or another person.
void Game::survivorAttack(Survivor& s, Vec2 targetPos, bool atPlayer, Animal* animal, Survivor* other) {
    if (s.attackTimer > 0) return;
    const ItemInstance* w = s.weapon();
    float dist = distance(s.pos, targetPos);
    s.facing = std::atan2(targetPos.y - s.pos.y, targetPos.x - s.pos.x);
    bool ranged = false;
    int ammo = -1;
    if (w && w->def->firearm) {
        ammo = s.inventory.find([&](const ItemInstance& it) { return it.def->id == w->def->ammo && it.usesLeft > 0; });
        ranged = ammo >= 0;
    }
    if (ranged) {
        float range = std::min(w->def->reach, 32.0f);
        if (dist > range || !world_->lineOfSight(s.pos, targetPos)) return;
        s.attackTimer = w->def->cooldown * 1.6f + rng_.range(0.2f, 0.9f);
        ItemInstance& a = s.inventory.items()[ammo];
        if (--a.usesLeft <= 0) s.inventory.take(ammo);
        bool loud = w->def->noise > 100;
        sound(loud ? Sfx::Gunshot : Sfx::Bowshot, s.pos);
        if (loud) animals_.scare(s.pos, 300);
        s.lastAttack = 0;
        float moving = atPlayer && player_.moving ? (player_.running ? 0.6f : 0.8f) : 1.0f;
        float p = s.def->aim * (1.0f - 0.6f * dist / range) * moving;
        if (atPlayer && player_.vehicle >= 0) p *= 0.6f;   // through the car
        if (!rng_.chance(p)) {
            if (atPlayer && loud) log("A shot cracks past you!", MessageKind::Bad);
            return;
        }
        if (atPlayer) playerHurtBy(std::clamp(w->def->damage / 120.0f, 0.2f, 0.85f), s.def->name, true);
        else if (animal) { animal->hp -= w->def->damage; if (animal->hp <= 0) animal->state = AnimalState::Dead; }
        else if (other) hurtSurvivor(*other, w->def->damage * 0.6f, false);
        return;
    }
    float reach = w && !w->def->firearm ? w->def->reach : 0.9f;
    if (dist > reach + 0.55f || !world_->lineOfSight(s.pos, targetPos)) return;
    float damage = w ? (w->def->firearm ? 6.0f : w->def->damage) : 3.0f;
    s.attackTimer = (w && !w->def->firearm ? w->def->cooldown : 0.9f) * 1.9f + rng_.range(0.3f, 0.9f);
    s.swing = 1.0f;
    s.lastAttack = 0;
    sound(Sfx::Swing, s.pos);
    // dodging is easier when you're moving
    if (!rng_.chance(atPlayer && player_.moving ? 0.4f : 0.6f)) return;
    if (atPlayer) playerHurtBy(std::clamp(damage / 40.0f, 0.08f, 0.7f), s.def->name, false);
    else if (animal) {
        animal->hp -= damage;
        sound(Sfx::Hit, animal->pos);
        if (animal->hp <= 0) animal->state = AnimalState::Dead;
    } else if (other) hurtSurvivor(*other, damage, false);
}

void Game::updateSurvivors(float dt, float gameMinutes) {
    Player& p = player_;
    float daylight = world_->clock().daylight();
    for (auto& s : survivors_) {
        if (!s.alive()) continue;
        const SurvivorDef& d = *s.def;
        s.speechTimer = std::max(0.0f, s.speechTimer - dt);
        s.attackTimer -= dt;
        s.stateTimer -= dt;
        s.lastAttack += dt;
        s.swing = std::max(0.0f, s.swing - dt * 3.0f);
        float dist = distance(s.pos, p.pos);
        bool raider = d.disposition == Disposition::Raider;

        // Far away: only the raiders keep walking their rounds (without
        // bumping into anything - nobody sees them)
        if (dist > 150 && !(s.joined && s.state == SurvivorState::Follow && dist < 400)) {
            if (raider && !d.route.empty()) {
                Vec2 goal;
                if (resolvePlaceCache(s, goal)) {
                    Vec2 to = goal - s.pos;
                    float l = to.length();
                    if (l < 3) s.routeIndex = (s.routeIndex + 1) % (int)d.route.size();
                    else s.pos = s.pos + to * (std::min(l, 1.3f * dt) / l);
                }
            } else if (raider && !d.partners.empty()) {
                for (auto& o : survivors_)
                    if (o.alive() && o.def->id == d.partners[0] && distance(o.pos, s.pos) > 3)
                        s.pos = o.pos + Vec2{1.5f, 1.0f};
            } else if (!raider && !s.joined && s.hasBed && !s.angry) {
                // Nobody's watching: to bed at night, up in the morning
                s.awakeHold = 0;
                s.path.clear();
                if (sleepTime(s)) { s.state = SurvivorState::Sleep; s.pos = s.bed; }
                else if (s.asleep()) { wakeSurvivor(s); s.pos = s.home; }
            }
            continue;
        }
        announceBond(s);
        s.dozing = false;
        // stepped into view inside something solid: out of it (a bed is fine while they're in it)
        if (!s.asleep() && world_->tileBlocked((int)std::floor(s.pos.x), (int)std::floor(s.pos.y)))
            for (int k = 0; k < 12; ++k) {
                Vec2 q = s.pos + Vec2{rng_.range(-3, 3), rng_.range(-3, 3)};
                if (!world_->tileBlocked((int)std::floor(q.x), (int)std::floor(q.y))) { s.pos = q; break; }
            }

        s.truce = world_->clock().totalMinutes() < s.truceUntil;
        // Do they know you're there? They see you only with nothing solid in
        // between (worse at night); a noise only tells them where to look.
        float sight = (raider ? 38.0f : 28.0f) * (0.35f + 0.65f * daylight) * (p.sneaking ? 0.6f : 1.0f);
        bool hears = !dead() && dist < p.noise;
        if (p.vehicle >= 0 && dist < 60) hears = true;   // an engine is hard to miss
        bool los = !dead() && dist < 60 && world_->lineOfSight(s.pos, p.pos);
        bool sees = los && (dist < sight || hears);
        if (s.asleep()) sees = hears = false;
        s.freezeCooldown = std::max(0.0f, s.freezeCooldown - dt);
        if (s.asleep() && s.joined) wakeSurvivor(s);

        // Close a door behind them once they're through
        if (s.doorDir >= 0) {
            Vec2 dc = s.doorDir == 0 ? Vec2{s.doorX + 0.5f, (float)s.doorY} : Vec2{(float)s.doorX, s.doorY + 0.5f};
            if (distance(s.pos, dc) > 1.4f) {
                if (distance(p.pos, dc) > 1.4f && world_->doorOpen(s.doorX, s.doorY, s.doorDir)) {
                    world_->toggleDoor(s.doorX, s.doorY, s.doorDir);
                    if (dist < 25) sound(Sfx::Door, s.pos);
                }
                s.doorDir = -1;
            }
        }

        // Pick what to do
        Vec2 goal = s.pos;
        float speed = 0;
        bool face = false;
        Vec2 faceAt;
        if (s.joined && updateCompanionRide(s, dt)) continue;   // in the car, on a bike, or off to find one
        if (s.joined) {
            // Time together brings you closer
            if (dist < 30) {
                s.companyMinutes += gameMinutes;
                while (s.companyMinutes >= 60) { s.companyMinutes -= 60; addTrust(s, 1, 80); }
                // now and then they say something
                if (!p.asleep && s.speechTimer <= 0 && rng_.chance(dt / 150.0f)) {
                    float rain = world_->clock().weather().rain;
                    BodySignals g = p.body.signals();
                    std::string key = g.bleeding > 0.1f ? "companion.hurt" : rain > 0.3f ? "companion.rain"
                                    : daylight < 0.2f ? "companion.night" : "companion." + std::string(s.bond() >= Bond::BestFriend ? "close" : "any");
                    survivorSay(s, personal(line(key)));
                }
            }
            // Companions: protect you first, then follow
            Animal* foeA = nullptr;
            Survivor* foeS = nullptr;
            float best = 18;
            for (auto& o : survivors_)
                if (&o != &s && o.hostileTo() && distance(o.pos, p.pos) < best) { best = distance(o.pos, p.pos); foeS = &o; }
            for (auto& a : animals_.animals())
                if (a.alive() && (a.state == AnimalState::Charge || a.state == AnimalState::Recover) && distance(a.pos, s.pos) < best) {
                    best = distance(a.pos, s.pos);
                    foeA = &a;
                    foeS = nullptr;
                }
            if (foeS || foeA) {
                Vec2 fp = foeS ? foeS->pos : foeA->pos;
                const ItemInstance* w = s.weapon();
                float want = w && w->def->firearm ? 12.0f : 0.8f;
                if (distance(s.pos, fp) > want) { goal = fp; speed = 3.8f; }
                survivorAttack(s, fp, false, foeA, foeS);
                face = true; faceAt = fp;
                if (rng_.chance(dt * 0.15f)) survivorSay(s, foeS ? "Behind you!" : "Watch out!");
            } else if (s.state == SurvivorState::Wait) {
                goal = s.target;
                speed = distance(s.pos, goal) > 1 ? 1.4f : 0;
                s.dozing = p.asleep && dist < 25;
            } else if (p.asleep && dist < 6) {
                s.dozing = true;    // you're sleeping: they doze off beside you
            } else {
                if (dist > 2.6f) { goal = p.pos; speed = dist > 7 ? 4.2f : 1.9f; }
                else if (p.running) { goal = p.pos; speed = 3; }
            }
        } else if (s.state == SurvivorState::Flee) {
            Vec2 away = (s.pos - p.pos).normalized();
            goal = s.pos + away * 10;
            speed = 4.0f;
            if (s.stateTimer <= 0 && dist > 30) {
                s.state = SurvivorState::Home;
                if (!raider && !s.angry) s.trust -= 10;
            }
        } else if (s.hostileTo() || (d.disposition == Disposition::Raider)) {
            // Raiders and anyone you've angered: hunt you down (unless you paid them off)
            if (s.truce && s.state == SurvivorState::Hunt) s.state = SurvivorState::Home;
            // someone holding a place only defends it
            if (d.disposition == Disposition::Hostile && !s.angry && distance(s.pos, s.home) > 35) s.state = SurvivorState::Home;
            if (s.state != SurvivorState::Hunt && !s.truce) {
                bool heardClose = raider && hears && dist < 40;
                if ((sees && dist < (raider ? 36.0f : 25.0f)) || heardClose) {
                    s.state = SurvivorState::Hunt;
                    s.stateTimer = 25;
                    s.target = p.pos;
                    survivorSay(s, sees ? pickLine(d.warn.empty() ? std::string("You!") : d.warn, rng_) : std::string("What was that?"));
                }
            }
            if (s.state == SurvivorState::Hunt) {
                // where they think you are: seen, or heard
                if (sees || hears) s.target = p.pos;
                if (sees) s.stateTimer = 25;
                if (s.stateTimer <= 0 || dist > 80 || dead()) {
                    s.state = SurvivorState::Home;
                    if (!s.angry && rng_.chance(0.5f)) survivorSay(s, "Where did they go...", false);
                } else {
                    const ItemInstance* w = s.weapon();
                    bool gun = w && w->def->firearm &&
                               s.inventory.find([&](const ItemInstance& it) { return it.def->id == w->def->ammo && it.usesLeft > 0; }) >= 0;
                    float want = gun ? std::min(14.0f, w->def->reach * 0.5f) : 0.8f;
                    if (!sees) {
                        // go and look where they last saw or heard you
                        if (distance(s.pos, s.target) > 1.2f) { goal = s.target; speed = 2.6f; }
                        face = true; faceAt = s.target;
                    } else {
                        if (dist > want) { goal = p.pos; speed = dist > 6 ? 3.9f : 2.4f; }
                        if (p.vehicle < 0 || gun) survivorAttack(s, p.pos, true, nullptr, nullptr);
                        face = true; faceAt = p.pos;
                        if (rng_.chance(dt * 0.08f)) survivorSay(s, pickLine(d.warn.empty() ? std::string("You!") : d.warn, rng_));
                    }
                }
            } else if (raider) {
                // On their rounds, partners stick together
                Survivor* lead = nullptr;
                for (auto& o : survivors_)
                    if (!d.partners.empty() && o.alive() && o.def->id == d.partners[0] && d.route.empty()) lead = &o;
                if (lead) {
                    if (distance(s.pos, lead->pos) > 2.5f) { goal = lead->pos; speed = 1.6f; }
                    if (lead->state == SurvivorState::Hunt && dist < 60) { s.state = SurvivorState::Hunt; s.stateTimer = 25; s.target = lead->target; }
                } else if (!d.route.empty()) {
                    Vec2 rp;
                    if (resolvePlaceCache(s, rp)) {
                        if (distance(s.pos, rp) < 4) s.routeIndex = (s.routeIndex + 1) % (int)d.route.size();
                        goal = rp; speed = 1.3f;
                    }
                }
                // partners join the hunt
                if (s.state == SurvivorState::Hunt && !s.truce)
                    for (auto& o : survivors_)
                        if (&o != &s && o.alive() && std::find(o.def->partners.begin(), o.def->partners.end(), d.id) != o.def->partners.end() &&
                            o.state != SurvivorState::Hunt)
                            o.state = SurvivorState::Hunt, o.stateTimer = 25, o.target = s.target;
            }
        } else if (d.disposition == Disposition::Hostile && !s.admitted) {
            // Holds a place. Sees you, shouts "Freeze!", walks up with the gun
            // on you, and then wants to know what you want. (Hurt him, and
            // he's angry: the hunt above.)
            if (s.state == SurvivorState::Alert) {
                if (sees) { s.target = p.pos; s.stateTimer = 20; }
                face = true; faceAt = sees ? p.pos : s.target;
                if (dist > 2.8f) { goal = sees ? p.pos : s.target; speed = 1.5f; }
                else if (sees && !s.calledYou) {
                    s.calledYou = true;
                    talkRequest_ = s.id;   // the client opens the conversation
                }
                if (sees && s.speechTimer <= 0 && dist > 6 && rng_.chance(dt * 0.15f)) survivorSay(s, "I said FREEZE!");
                // you got away, or off his patch: he lets you go
                if (s.stateTimer <= 0 || distance(s.pos, s.home) > 35 || dead()) {
                    s.state = SurvivorState::Home;
                    s.calledYou = false;
                    s.freezeCooldown = 60;
                    if (!dead()) survivorSay(s, "And stay away!", false);
                }
            } else if (sees && dist < 30 && s.freezeCooldown <= 0) {
                s.state = SurvivorState::Alert;
                s.stateTimer = 20;
                s.target = p.pos;
                s.calledYou = false;
                face = true; faceAt = p.pos;
                survivorSay(s, d.freeze.empty() ? std::string("Freeze! Hands where I can see them!") : pickLine(d.freeze, rng_));
            } else {
                if (sees && dist < 14) { face = true; faceAt = p.pos; }   // keeps an eye on you
                updateHomeLife(s, dt, sees, goal, speed, face);
            }
        } else {
            // Friendly and wary people keep to their homes, and watch you come
            bool bedtime = sleepTime(s) && s.met && s.awakeHold <= 0 && s.bond() >= Bond::Acquaintance;
            if (sees && dist < 14 && !s.asleep() && !bedtime) {
                face = true; faceAt = p.pos;
                if (!s.met && s.speechTimer <= 0 && s.stateTimer <= 0) {
                    survivorSay(s, d.disposition == Disposition::Wary ? "Who's there? Stay where you are!" : "Hello? Is someone there?");
                    s.stateTimer = 20;
                }
                // a weapon in your hand makes the wary nervous
                if (d.disposition == Disposition::Wary && p.weaponUid && dist < 4 && s.trust < 20) {
                    if (s.speechTimer <= 0) survivorSay(s, "Put that away. Now.");
                    goal = s.pos + (s.pos - p.pos).normalized() * 3;
                    speed = 1.6f;
                }
            }
            if (speed == 0) updateHomeLife(s, dt, sees, goal, speed, face);
        }

        // Move
        Vec2 to = goal - s.pos;
        float l = to.length();
        if (speed > 0 && l > 0.3f) {
            // round whatever's in the way, not into it
            Vec2 step = navigate(s, goal, dt) - s.pos;
            float sl = step.length();
            Vec2 dir = sl > 1e-4f ? step * (1.0f / sl) : to * (1.0f / l);
            Vec2 want = dir * std::min(speed, l / std::max(dt, 0.001f));
            s.vel = s.vel + (want - s.vel) * std::min(1.0f, dt * 6.0f);
            openDoorAhead(s, dir);
        } else {
            s.vel = s.vel * std::max(0.0f, 1.0f - dt * 8.0f);
        }
        if (s.vel.lengthSq() > 0.01f) {
            Vec2 before = s.pos;
            s.pos = world_->moveCircle(s.pos, s.vel * dt, 0.28f);
            float moved = distance(before, s.pos);
            if (moved < s.vel.length() * dt * 0.3f) {
                // stuck on a wall: slide sideways for a moment
                s.navTimer = 0;   // plan the way again
                Vec2 side{-s.vel.y, s.vel.x};
                s.pos = world_->moveCircle(s.pos, side * (dt * (s.id % 2 ? 1.0f : -1.0f)), 0.28f);
                if (!s.joined && s.state != SurvivorState::Hunt) {
                    s.target = s.pos;
                    if (!s.path.empty() && rng_.chance(dt * 0.7f)) { s.path.clear(); s.idleTimer = 2; }
                }
            }
            s.facing = std::atan2(s.vel.y, s.vel.x);
        }
        if (face) s.facing = std::atan2(faceAt.y - s.pos.y, faceAt.x - s.pos.x);
    }
}

bool Game::resolvePlaceCache(Survivor& s, Vec2& out) {
    const auto& route = s.def->route;
    if (route.empty()) { out = s.home; return true; }
    s.routeIndex = s.routeIndex % (int)route.size();
    std::string key = s.def->id + "#" + std::to_string(s.routeIndex);
    auto it = placeCache_.find(key);
    if (it != placeCache_.end()) { out = it->second; return true; }
    Vec2 p;
    if (!resolvePlace(route[(size_t)s.routeIndex], p, nullptr)) p = s.home;
    placeCache_[key] = p;
    out = p;
    return true;
}

} // namespace alone
