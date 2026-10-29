// Everyday life for the people of Alingsås: pottering about their homes,
// going to bed at night and getting up in the morning, and how close you
// are to them (Enemy ... Lover). Also the shared pool of dialogue lines.
#include "core/Config.h"
#include "core/Log.h"
#include "game/Game.h"
#include <algorithm>
#include <cmath>
#include <deque>

namespace alone {

namespace {

std::string firstNameOf(const SurvivorDef& d) {
    auto sp = d.name.find(' ');
    return sp == std::string::npos ? d.name : d.name.substr(0, sp);
}

bool isHomeType(BuildingType t) {
    return t == BuildingType::House || t == BuildingType::Cottage || t == BuildingType::Farmhouse;
}

// The wall edge between a tile and its 4-neighbour: where it is, and what's there
struct Edge { int x, y, dir; };
Edge edgeBetween(int x, int y, int dx, int dy) {
    if (dx == 1) return {x + 1, y, 1};
    if (dx == -1) return {x, y, 1};
    if (dy == 1) return {x, y + 1, 0};
    return {x, y, 0};
}

} // namespace

const char* bondName(Bond b) {
    switch (b) {
        case Bond::Enemy: return "Enemy";
        case Bond::Stranger: return "Stranger";
        case Bond::Acquaintance: return "Acquaintance";
        case Bond::Friend: return "Friend";
        case Bond::BestFriend: return "Best friend";
        case Bond::Family: return "Family";
        case Bond::Lover: return "Lover";
        default: return "";
    }
}

const char* bondBlurb(Bond b) {
    switch (b) {
        case Bond::Enemy: return "They want nothing to do with you - or worse.";
        case Bond::Stranger: return "They don't know you. Talk, help, bring gifts.";
        case Bond::Acquaintance: return "They know your face and your name.";
        case Bond::Friend: return "They like you. Better deals, and they open up.";
        case Bond::BestFriend: return "They'd trust you with their life.";
        case Bond::Family: return "Family now. They'd follow you anywhere.";
        case Bond::Lover: return "You have each other.";
        default: return "";
    }
}

const char* genderName(Gender g) {
    switch (g) {
        case Gender::Man: return "Man";
        case Gender::Woman: return "Woman";
        default: return "Other";
    }
}

const char* hairStyleName(int style) {
    static const char* names[kHairStyles] = {"Short", "Long", "Ponytail", "Bun", "Shaved"};
    return names[std::clamp(style, 0, kHairStyles - 1)];
}

Bond Survivor::bond() const {
    if (angry || trust <= kTrustEnemy) return Bond::Enemy;
    if (def->disposition == Disposition::Raider) return truce ? Bond::Stranger : Bond::Enemy;
    if (lover) return Bond::Lover;
    if (family) return Bond::Family;
    if (!met || trust < kTrustAcquaintance) return Bond::Stranger;
    if (trust < kTrustFriend) return Bond::Acquaintance;
    if (trust < kTrustBestFriend) return Bond::Friend;
    return Bond::BestFriend;
}

Bond Game::bondWith(int id) const {
    const Survivor* s = const_cast<Game*>(this)->survivor(id);
    return s ? s->bond() : Bond::Stranger;
}

// ---------------------------------------------------------------------------
// Dialogue pool
// ---------------------------------------------------------------------------

bool Game::loadDialogue(const std::string& path) {
    dialogue_.clear();
    Config cfg;
    if (!cfg.load(path)) return false;
    int n = 0;
    for (const auto& sec : cfg.sections())
        for (const auto& [k, v] : sec.values) {
            auto& lines = dialogue_[sec.name + "." + k];
            for (auto& l : split(v, '|')) {
                std::string t = trim(l);
                if (!t.empty()) { lines.push_back(t); ++n; }
            }
        }
    Log::info("Loaded %d lines of dialogue", n);
    return true;
}

std::string Game::line(const std::string& key) {
    auto it = dialogue_.find(key);
    if (it == dialogue_.end() || it->second.empty()) {
        // "chat.bestfriend" falls back to "chat.friend", then "chat.any"
        auto dot = key.rfind('.');
        if (dot != std::string::npos) {
            std::string sec = key.substr(0, dot), k = key.substr(dot + 1);
            if (k == "lover" || k == "family") return line(sec + ".bestfriend");
            if (k == "bestfriend") return line(sec + ".friend");
            if (k != "any") return line(sec + ".any");
        }
        return "";
    }
    return it->second[(size_t)rng_.rangeInt(0, (int)it->second.size() - 1)];
}

std::string Game::pickOf(const std::vector<std::string>& v, const std::string& fallbackKey) {
    if (!v.empty()) {
        std::string l = v[(size_t)rng_.rangeInt(0, (int)v.size() - 1)];
        auto parts = split(l, '|');   // a line can hold alternatives too
        return trim(parts[(size_t)rng_.rangeInt(0, (int)parts.size() - 1)]);
    }
    return line(fallbackKey);
}

// {name}: your name. {time}: morning/afternoon/evening/night.
std::string Game::personal(const std::string& text) const {
    std::string out = text;
    auto rep = [&](const std::string& key, const std::string& val) {
        for (size_t p = out.find(key); p != std::string::npos; p = out.find(key, p + val.size())) out.replace(p, key.size(), val);
    };
    rep("{name}", player_.look.name.empty() ? std::string("friend") : player_.look.name);
    float h = world_ ? world_->clock().hourOfDay() : 12;
    rep("{time}", h < 5 ? "night" : h < 12 ? "morning" : h < 18 ? "afternoon" : h < 22 ? "evening" : "night");
    return out;
}

// ---------------------------------------------------------------------------
// Friendship
// ---------------------------------------------------------------------------

void Game::addTrust(Survivor& s, float amount, float cap) {
    if (amount > 0) s.trust = std::max(s.trust, std::min(cap, s.trust + amount));
    else s.trust += amount;
    s.trust = std::clamp(s.trust, -100.0f, 100.0f);
    announceBond(s);
}

void Game::announceBond(Survivor& s) {
    if (!s.met || !s.alive()) return;
    Bond b = s.bond();
    if (b == s.shownBond) return;
    bool up = (int)b > (int)s.shownBond;
    Bond was = s.shownBond;
    s.shownBond = b;
    if (s.def->disposition == Disposition::Raider) return;
    std::string who = firstNameOf(*s.def);
    std::string text;
    switch (b) {
        case Bond::Enemy: text = who + " sees you as an enemy now."; break;
        case Bond::Stranger: text = up ? who + " doesn't hate you any more." : who + " has gone cold on you."; break;
        case Bond::Acquaintance: text = up ? who + " knows you now - an acquaintance." : who + " has cooled towards you."; break;
        case Bond::Friend: text = up ? who + " counts you as a friend." : who + " is still your friend, but less sure of you."; break;
        case Bond::BestFriend: text = up ? who + " trusts you completely. Best friends." : who + " is still close to you."; break;
        case Bond::Family: text = who + " is family now."; break;
        case Bond::Lover: text = "You and " + who + " are together now."; break;
        default: break;
    }
    if (was == Bond::Lover && b != Bond::Lover && b != Bond::Enemy) text = "You and " + who + " are not together any more.";
    if (!text.empty()) log("\xE2\x99\xA5 " + text, up ? MessageKind::Good : MessageKind::Bad);
}

// ---------------------------------------------------------------------------
// Home life
// ---------------------------------------------------------------------------

bool Game::sleepTime(const Survivor& s) const {
    float h = world_->clock().hourOfDay();
    // everyone keeps slightly different hours
    float bed = s.def->bedtime + (s.id % 3) * 0.4f, up = s.def->wakeup + (s.id % 2) * 0.5f;
    if (bed >= 24) bed -= 24;
    return bed > up ? (h >= bed || h < up) : (h >= bed && h < up);
}

void Game::findBed(Survivor& s) {
    s.hasBed = false;
    s.homeBuilding = world_->tile((int)std::floor(s.home.x), (int)std::floor(s.home.y)).building;
    const Building* b = world_->building(s.homeBuilding);
    if (!b) { s.homeBuilding = -1; return; }
    // A real bed in a house; elsewhere a sofa, or a corner of the floor
    const Furniture* best = nullptr;
    int bestScore = -1;
    for (const auto& f : b->furniture) {
        int score = f.prop == Prop::Bed ? 3 : f.prop == Prop::Sofa ? 2 : f.prop == Prop::HospitalBed ? 1 : -1;
        if (score < 0) continue;
        // people sharing a house don't share a bed
        for (const auto& o : survivors_)
            if (&o != &s && o.hasBed && (int)std::floor(o.bed.x) == f.x && (int)std::floor(o.bed.y) == f.y) score = -1;
        if (score > bestScore) { bestScore = score; best = &f; }
    }
    static const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    if (best) {
        for (int k = 0; k < 4; ++k) {
            int x = best->x + dx[k], y = best->y + dy[k];
            if (world_->tileBlocked(x, y) || world_->edgeBlocked(best->x, best->y, dx[k], dy[k])) continue;
            if (world_->tile(x, y).building != s.homeBuilding) continue;
            s.bed = Vec2{best->x + 0.5f, best->y + 0.5f};
            s.bedSide = Vec2{x + 0.5f, y + 0.5f};
            s.hasBed = true;
            return;
        }
    }
    // No bed: a blanket on the floor where they spend their days
    s.bed = s.bedSide = s.home;
    s.hasBed = true;
}

Vec2 Game::roomSpot(const Survivor& s) {
    const Building* b = world_->building(s.homeBuilding);
    if (!b || b->rooms.empty()) return s.home;
    // Mostly the kitchen and the living room
    float total = 0;
    std::vector<float> w;
    for (const auto& r : b->rooms) {
        float k = r.type == RoomType::Living || r.type == RoomType::Kitchen ? 3.0f : r.type == RoomType::Hall ? 0.5f : 1.0f;
        if (!isHomeType(b->type)) k = std::min(4.0f, r.rect.area() / 20.0f + 0.3f);   // big public buildings: the big rooms
        w.push_back(k);
        total += k;
    }
    float pick = rng_.range(0, total);
    size_t ri = 0;
    for (; ri + 1 < w.size() && pick > w[ri]; ++ri) pick -= w[ri];
    const IRect& rr = b->rooms[ri].rect;
    // big buildings: stay within reach of home
    for (int tries = 0; tries < 14; ++tries) {
        int x = rr.x + rng_.rangeInt(0, std::max(0, rr.w - 1)), y = rr.y + rng_.rangeInt(0, std::max(0, rr.h - 1));
        Vec2 p{x + 0.5f, y + 0.5f};
        if (!isHomeType(b->type) && distance(p, s.home) > s.def->roam + 6) continue;
        if (!world_->tileBlocked(x, y)) return p;
    }
    return s.home;
}

// Breadth-first search over the tiles of their house (and the doorstep).
// Doors count as open: they open them on the way.
bool Game::planPath(Survivor& s, Vec2 to) {
    s.path.clear();
    const Building* b = world_->building(s.homeBuilding);
    int sx = (int)std::floor(s.pos.x), sy = (int)std::floor(s.pos.y);
    int gx = (int)std::floor(to.x), gy = (int)std::floor(to.y);
    if (sx == gx && sy == gy) { s.path.push_back(to); return true; }
    IRect area = b ? b->rect.expanded(3) : IRect{std::min(sx, gx) - 6, std::min(sy, gy) - 6, std::abs(sx - gx) + 13, std::abs(sy - gy) + 13};
    if (!area.contains(sx, sy) || !area.contains(gx, gy) || area.area() > 160 * 160) return false;
    std::vector<int> from((size_t)area.area(), -1);
    auto idx = [&](int x, int y) { return (y - area.y) * area.w + (x - area.x); };
    std::deque<int> q;
    from[(size_t)idx(sx, sy)] = idx(sx, sy);
    q.push_back(idx(sx, sy));
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    bool found = false;
    while (!q.empty() && !found) {
        int c = q.front();
        q.pop_front();
        int cx = area.x + c % area.w, cy = area.y + c / area.w;
        for (int k = 0; k < 4; ++k) {
            int nx = cx + dx[k], ny = cy + dy[k];
            if (!area.contains(nx, ny) || from[(size_t)idx(nx, ny)] >= 0) continue;
            if (world_->tileBlocked(nx, ny) && !(nx == gx && ny == gy)) continue;
            Edge e = edgeBetween(cx, cy, dx[k], dy[k]);
            const Tile& t = world_->tile(e.x, e.y);
            uint8_t wall = e.dir == 0 ? WallN : WallW, door = e.dir == 0 ? DoorN : DoorW;
            if ((t.walls & wall) && !(t.walls & door)) continue;
            from[(size_t)idx(nx, ny)] = c;
            if (nx == gx && ny == gy) { found = true; break; }
            q.push_back(idx(nx, ny));
        }
    }
    if (!found) return false;
    std::vector<Vec2> rev{to};
    for (int c = from[(size_t)idx(gx, gy)]; c != idx(sx, sy); c = from[(size_t)c])
        rev.push_back(Vec2{area.x + c % area.w + 0.5f, area.y + c / area.w + 0.5f});
    s.path.assign(rev.rbegin(), rev.rend());
    return true;
}

void Game::wakeSurvivor(Survivor& s) {
    if (!s.asleep()) return;
    s.state = SurvivorState::Home;
    s.pos = s.bedSide;
    s.path.clear();
    s.idleTimer = rng_.range(4, 10);
    s.saidGoodnight = false;
}

// Friendly people (and Jonas when he isn't watching you) at home: walk from
// room to room, go to bed at bedtime, get up in the morning.
void Game::updateHomeLife(Survivor& s, float dt, bool sees, Vec2& goal, float& speed, bool& face) {
    const SurvivorDef& d = *s.def;
    float dist = distance(s.pos, player_.pos);
    s.awakeHold = std::max(0.0f, s.awakeHold - dt);
    if (s.homeBuilding < 0) {
        // Lives outdoors (or the house wasn't found): wander about the place
        if (s.stateTimer <= 0) {
            s.stateTimer = rng_.range(6, 16);
            if (rng_.chance(0.6f)) s.target = s.home + Vec2{rng_.range(-d.roam, d.roam), rng_.range(-d.roam, d.roam)};
            else s.target = s.pos;
        }
        if (distance(s.pos, s.target) > 0.8f && !face) { goal = s.target; speed = 1.2f; }
        return;
    }
    // Asleep: lie still; loud noise close by, or morning, wakes them
    if (s.asleep()) {
        s.pos = s.bed;
        s.vel = {};
        face = false;
        if (!sleepTime(s)) {
            wakeSurvivor(s);
            if (dist < 14 && sees) survivorSay(s, personal(pickOf(d.morning, "morning.any")));
            return;
        }
        // walking past the bed might wake them, running or a gunshot will; creeping won't
        bool loud = dist < player_.noise * 0.25f;
        if (loud && rng_.chance(dt * (player_.noise > 20 ? 0.8f : 0.25f))) {
            wakeSurvivor(s);
            s.awakeHold = 60;
            face = true;
            if (d.disposition == Disposition::Hostile && !s.admitted) {
                s.warned = 6;
                survivorSay(s, "What - WHO'S THERE?!");
            } else {
                survivorSay(s, s.met ? personal(line("woken.noise_known")) : line("woken.noise"));
            }
            return;
        }
        s.snoreTimer -= dt;
        if (s.snoreTimer <= 0 && dist < 16) {
            s.snoreTimer = rng_.range(5, 10);
            s.speech = rng_.chance(0.25f) ? "Zzz... mm..." : "Zzz...";
            s.speechTimer = 2.5f;
        }
        return;
    }
    // Follow the path; open doors on the way, close them behind
    auto walk = [&](float spd) {
        if (s.path.empty()) return;
        while (!s.path.empty() && distance(s.pos, s.path.front()) < 0.3f) s.path.erase(s.path.begin());
        if (s.path.empty()) return;
        Vec2 next = s.path.front();
        int ax = (int)std::floor(s.pos.x), ay = (int)std::floor(s.pos.y);
        int bx = (int)std::floor(next.x), by = (int)std::floor(next.y);
        if (std::abs(ax - bx) + std::abs(ay - by) == 1 && world_->edgeBlocked(ax, ay, bx - ax, by - ay)) {
            Edge e = edgeBetween(ax, ay, bx - ax, by - ay);
            if (world_->doorAt(e.x, e.y, e.dir) && !world_->doorOpen(e.x, e.y, e.dir)) {
                world_->toggleDoor(e.x, e.y, e.dir);
                if (dist < 25) sound(Sfx::Door, s.pos);
                s.doorX = e.x; s.doorY = e.y; s.doorDir = e.dir;
            }
        }
        goal = next;
        speed = spd;
    };
    // Bedtime
    if (sleepTime(s) && s.hasBed && s.awakeHold <= 0) {
        if (!s.saidGoodnight) {
            s.saidGoodnight = true;
            if (s.met && dist < 12 && sees) survivorSay(s, personal(line("goodnight." + std::string(s.bond() >= Bond::Friend ? "friend" : "any"))));
            planPath(s, s.bedSide);
        }
        if (distance(s.pos, s.bedSide) < 0.6f) {
            s.state = SurvivorState::Sleep;
            s.pos = s.bed;
            s.path.clear();
            s.snoreTimer = rng_.range(3, 8);
            s.wokenUp = false;
            return;
        }
        if (s.path.empty() && !planPath(s, s.bedSide)) {
            // can't find the way: lie down where they are
            s.bed = s.bedSide = s.pos;
        }
        face = false;
        walk(1.0f);
        return;
    }
    s.saidGoodnight = false;
    if (face) return;    // watching you
    // Daytime: potter from room to room, sometimes muttering to themselves
    s.idleTimer -= dt;
    if (s.path.empty() && s.idleTimer <= 0) {
        s.idleTimer = rng_.range(12, 40);
        if (!planPath(s, roomSpot(s))) s.idleTimer = 4;
        if (dist < 14 && rng_.chance(0.35f)) survivorSay(s, personal(pickOf(d.idle, "idle.any")), false);
    }
    walk(1.05f);
}

// ---------------------------------------------------------------------------
// Getting about: round props, water and walls, through doors
// ---------------------------------------------------------------------------

namespace {
// A wall between the tile and its neighbour that has no door in it
bool solidWall(World& w, int x, int y, int dx, int dy) {
    Edge e = edgeBetween(x, y, dx, dy);
    const Tile& t = w.tile(e.x, e.y);
    uint8_t wall = e.dir == 0 ? WallN : WallW, door = e.dir == 0 ? DoorN : DoorW;
    return (t.walls & wall) && !(t.walls & door);
}
} // namespace

bool Game::walkClear(Vec2 a, Vec2 b, float radius) {
    Vec2 d = b - a;
    float len = d.length();
    if (len < 0.05f) return true;
    Vec2 dir = d * (1.0f / len), side{-dir.y * radius, dir.x * radius};
    int gx = (int)std::floor(b.x), gy = (int)std::floor(b.y);
    int steps = (int)std::ceil(len / 0.2f);
    // the middle and both shoulders
    for (Vec2 off : {Vec2{0, 0}, side, side * -1.0f}) {
        Vec2 from = a + off;
        int px = (int)std::floor(from.x), py = (int)std::floor(from.y);
        for (int i = 1; i <= steps; ++i) {
            Vec2 p = from + d * (i / (float)steps);
            int x = (int)std::floor(p.x), y = (int)std::floor(p.y);
            if (x == px && y == py) continue;
            if (world_->tileBlocked(x, y) && !(x == gx && y == gy)) return false;
            int sx = x - px, sy = y - py;
            if (std::abs(sx) + std::abs(sy) == 1) {
                if (solidWall(*world_, px, py, sx, sy)) return false;
            } else {
                // a corner: blocked unless one of the two ways round it is open
                bool viaX = !solidWall(*world_, px, py, sx, 0) && !solidWall(*world_, px + sx, py, 0, sy) && !world_->tileBlocked(px + sx, py);
                bool viaY = !solidWall(*world_, px, py, 0, sy) && !solidWall(*world_, px, py + sy, sx, 0) && !world_->tileBlocked(px, py + sy);
                if (!viaX && !viaY) return false;
            }
            px = x; py = y;
        }
    }
    return true;
}

// Breadth-first over tiles (8 ways, no cutting corners) in a box round both
// ends. If the goal can't be reached (it's in a lake, or too far), the way
// leads as close as it gets.
bool Game::planRoute(Vec2 from, Vec2 to, std::vector<Vec2>& out) {
    out.clear();
    int sx = (int)std::floor(from.x), sy = (int)std::floor(from.y);
    int gx = (int)std::floor(to.x), gy = (int)std::floor(to.y);
    // far away: aim at a point on the way there
    const int reach = 60;
    if (std::abs(gx - sx) > reach || std::abs(gy - sy) > reach) {
        Vec2 d = to - from;
        Vec2 mid = from + d * (reach / std::max(std::fabs(d.x), std::fabs(d.y)));
        gx = (int)std::floor(mid.x); gy = (int)std::floor(mid.y);
    }
    const int pad = 14;
    IRect area{std::min(sx, gx) - pad, std::min(sy, gy) - pad, std::abs(sx - gx) + 2 * pad + 1, std::abs(sy - gy) + 2 * pad + 1};
    std::vector<int> prev((size_t)area.area(), -1);
    auto idx = [&](int x, int y) { return (y - area.y) * area.w + (x - area.x); };
    auto open = [&](int x, int y) { return area.contains(x, y) && (!world_->tileBlocked(x, y) || (x == gx && y == gy)); };
    std::deque<int> q;
    int start = idx(sx, sy), goal = idx(gx, gy);
    prev[(size_t)start] = start;
    q.push_back(start);
    int best = start;
    int bestD = std::abs(sx - gx) + std::abs(sy - gy);
    static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!q.empty()) {
        int c = q.front();
        q.pop_front();
        if (c == goal) { best = c; break; }
        int cx = area.x + c % area.w, cy = area.y + c / area.w;
        for (int k = 0; k < 8; ++k) {
            int nx = cx + dx[k], ny = cy + dy[k];
            if (!open(nx, ny) || prev[(size_t)idx(nx, ny)] >= 0) continue;
            if (k < 4) {
                if (solidWall(*world_, cx, cy, dx[k], dy[k])) continue;
            } else {
                // diagonal: both side tiles free and no wall on either way round
                if (!open(nx, cy) || !open(cx, ny) || world_->tileBlocked(nx, cy) || world_->tileBlocked(cx, ny)) continue;
                if (solidWall(*world_, cx, cy, dx[k], 0) || solidWall(*world_, cx, cy, 0, dy[k]) ||
                    solidWall(*world_, nx, cy, 0, dy[k]) || solidWall(*world_, cx, ny, dx[k], 0)) continue;
            }
            int n = idx(nx, ny);
            prev[(size_t)n] = c;
            int dd = std::abs(nx - gx) + std::abs(ny - gy);
            if (dd < bestD) { bestD = dd; best = n; }
            q.push_back(n);
        }
    }
    if (best == start) return false;
    std::vector<Vec2> rev;
    if (best == goal && gx == (int)std::floor(to.x) && gy == (int)std::floor(to.y)) rev.push_back(to);
    else rev.push_back(Vec2{area.x + best % area.w + 0.5f, area.y + best / area.w + 0.5f});
    for (int c = prev[(size_t)best]; c != start; c = prev[(size_t)c])
        rev.push_back(Vec2{area.x + c % area.w + 0.5f, area.y + c / area.w + 0.5f});
    out.assign(rev.rbegin(), rev.rend());
    return true;
}

Vec2 Game::navigate(Survivor& s, Vec2 goal, float dt) {
    const float r = 0.3f;
    s.navTimer -= dt;
    if (walkClear(s.pos, goal, r)) { s.nav.clear(); return goal; }
    if (s.nav.empty() || s.navTimer <= 0 || distance(goal, s.navGoal) > 1.5f) {
        s.navGoal = goal;
        s.navTimer = rng_.range(0.8f, 1.4f);
        if (!planRoute(s.pos, goal, s.nav)) { s.navTimer = 1.5f; return goal; }
    }
    while (!s.nav.empty() && distance(s.pos, s.nav.front()) < 0.35f) s.nav.erase(s.nav.begin());
    if (s.nav.empty()) return goal;
    // cut corners where the way is clear
    size_t far = 0;
    for (size_t i = std::min<size_t>(s.nav.size(), 8); i-- > 1;)
        if (walkClear(s.pos, s.nav[i], r)) { far = i; break; }
    s.nav.erase(s.nav.begin(), s.nav.begin() + (long)far);
    return s.nav.front();
}

void Game::openDoorAhead(Survivor& s, Vec2 dir) {
    Vec2 ahead = s.pos + dir * 0.5f;
    int ax = (int)std::floor(s.pos.x), ay = (int)std::floor(s.pos.y);
    int bx = (int)std::floor(ahead.x), by = (int)std::floor(ahead.y);
    if (std::abs(ax - bx) + std::abs(ay - by) != 1) return;
    Edge e = edgeBetween(ax, ay, bx - ax, by - ay);
    if (!world_->doorAt(e.x, e.y, e.dir) || world_->doorOpen(e.x, e.y, e.dir)) return;
    world_->toggleDoor(e.x, e.y, e.dir);
    if (distance(s.pos, player_.pos) < 25) sound(Sfx::Door, s.pos);
    s.doorX = e.x; s.doorY = e.y; s.doorDir = e.dir;
}

} // namespace alone
