#include "world/World.h"
#include "core/Log.h"
#include <algorithm>
#include <cmath>

namespace alone {

namespace {
int64_t doorKey(int x, int y, int dir) { return tileKey(x, y) * 2 + dir; }
}

World::World(const MapData& map, const ItemRegistry& items, uint32_t seed)
    : map_(map), items_(items), seed_(seed), gen_(map, seed), clock_(seed, map.latitude()) {
    // Power fails somewhere between two and twelve days into the game
    powerOffMinutes_ = clock_.totalMinutes() + (2.0 + 10.0 * hashFloat(hash32(seed ^ 0x90E7))) * 1440.0;
}

bool World::buildingLightsOn(int id) const {
    const Building* b = gen_.building(id);
    if (!b || b->isOutbuilding) return false;
    float chance = b->type == BuildingType::House || b->type == BuildingType::Apartment ? 0.3f : 0.55f;
    return hashFloat(hash3(id, 7, 0x116, seed_)) < chance;
}

// ---------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------

Chunk& World::chunk(int cx, int cy) {
    int64_t key = tileKey(cx, cy);
    auto it = chunks_.find(key);
    if (it != chunks_.end()) {
        it->second->lastUsed = clockTime_;
        return *it->second;
    }
    auto c = std::make_unique<Chunk>();
    c->cx = cx;
    c->cy = cy;
    gen_.generateChunk(*c);
    // Re-apply the player's changes
    int x0 = cx * kChunkSize, y0 = cy * kChunkSize;
    for (int ly = 0; ly < kChunkSize; ++ly)
        for (int lx = 0; lx < kChunkSize; ++lx) {
            auto o = overrides_.find(tileKey(x0 + lx, y0 + ly));
            if (o != overrides_.end() && o->second.hasProp) c->at(lx, ly).prop = o->second.prop;
        }
    c->lastUsed = clockTime_;
    c->version = ++versionCounter_;
    Chunk& ref = *c;
    chunks_[key] = std::move(c);
    return ref;
}

Chunk* World::chunkIfLoaded(int cx, int cy) {
    auto it = chunks_.find(tileKey(cx, cy));
    return it == chunks_.end() ? nullptr : it->second.get();
}

const Tile& World::tile(int x, int y) {
    Chunk& c = chunk(floorDiv(x, kChunkSize), floorDiv(y, kChunkSize));
    return c.at(x - c.cx * kChunkSize, y - c.cy * kChunkSize);
}

Tile& World::mutableTile(int x, int y) {
    Chunk& c = chunk(floorDiv(x, kChunkSize), floorDiv(y, kChunkSize));
    return c.at(x - c.cx * kChunkSize, y - c.cy * kChunkSize);
}

void World::touch(int x, int y) {
    // Bump this chunk and neighbours (walls/props render across edges)
    int cx = floorDiv(x, kChunkSize), cy = floorDiv(y, kChunkSize);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (Chunk* c = chunkIfLoaded(cx + dx, cy + dy)) c->version = ++versionCounter_;
}

void World::setProp(int x, int y, Prop p) {
    mutableTile(x, y).prop = p;
    TileOverride& o = overrides_[tileKey(x, y)];
    o.hasProp = true;
    o.prop = p;
    touch(x, y);
}

const Building* World::buildingAt(int x, int y) {
    const Tile& t = tile(x, y);
    return t.building >= 0 ? gen_.building(t.building) : nullptr;
}

bool World::indoors(Vec2 p) {
    return tile((int)std::floor(p.x), (int)std::floor(p.y)).building >= 0;
}

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

bool World::tileBlocked(int x, int y) {
    const Tile& t = tile(x, y);
    if (!groundWalkable(t.ground)) return true;
    return propBlocks(t.prop);
}

bool World::edgeBlocked(int x, int y, int dx, int dy) {
    // Edge between (x,y) and (x+dx, y+dy), 4-neighbours only
    int ex = x, ey = y, dir;
    if (dx == 1) { ex = x + 1; dir = 1; }
    else if (dx == -1) { dir = 1; }
    else if (dy == 1) { ey = y + 1; dir = 0; }
    else { dir = 0; }
    const Tile& t = tile(ex, ey);
    uint8_t wall = dir == 0 ? WallN : WallW;
    uint8_t door = dir == 0 ? DoorN : DoorW;
    if (!(t.walls & wall)) return false;
    if (t.walls & door) return !doorOpen(ex, ey, dir);
    return true;
}

Vec2 World::moveCircle(Vec2 from, Vec2 delta, float r) {
    // Move in small steps, resolving each axis separately so the player slides along walls.
    float len = delta.length();
    int steps = std::max(1, (int)std::ceil(len / 0.1f));
    Vec2 step = delta * (1.0f / steps);
    Vec2 p = from;
    auto collides = [&](Vec2 q) {
        int tx = (int)std::floor(q.x), ty = (int)std::floor(q.y);
        for (int y = ty - 1; y <= ty + 1; ++y)
            for (int x = tx - 1; x <= tx + 1; ++x) {
                if (!tileBlocked(x, y)) continue;
                // circle vs tile box (props are a bit smaller than the tile)
                float inset = isTree(tile(x, y).prop) ? 0.3f : 0.05f;
                float cx = std::clamp(q.x, x + inset, x + 1 - inset), cy = std::clamp(q.y, y + inset, y + 1 - inset);
                if ((q.x - cx) * (q.x - cx) + (q.y - cy) * (q.y - cy) < r * r) return true;
            }
        // walls: check edges around the tile the circle centre is in
        auto wallHit = [&](int x, int y, int dir) {
            const Tile& t = tile(x, y);
            uint8_t wall = dir == 0 ? WallN : WallW;
            uint8_t door = dir == 0 ? DoorN : DoorW;
            if (!(t.walls & wall)) return false;
            if ((t.walls & door) && doorOpen(x, y, dir)) return false;
            if (dir == 0) {   // horizontal segment y = y, x..x+1
                float cx = std::clamp(q.x, (float)x, (float)x + 1);
                float dy = q.y - y, dx = q.x - cx;
                return dx * dx + dy * dy < r * r;
            } else {
                float cy = std::clamp(q.y, (float)y, (float)y + 1);
                float dx = q.x - x, dy = q.y - cy;
                return dx * dx + dy * dy < r * r;
            }
        };
        for (int y = ty - 1; y <= ty + 2; ++y)
            for (int x = tx - 1; x <= tx + 2; ++x)
                if (wallHit(x, y, 0) || wallHit(x, y, 1)) return true;
        return false;
    };
    for (int i = 0; i < steps; ++i) {
        Vec2 tryX{p.x + step.x, p.y};
        if (!collides(tryX)) p = tryX;
        Vec2 tryY{p.x, p.y + step.y};
        if (!collides(tryY)) p = tryY;
    }
    return p;
}

bool World::lineOfSight(Vec2 a, Vec2 b) {
    Vec2 d = b - a;
    float len = d.length();
    int steps = (int)(len / 0.5f);
    int px = (int)std::floor(a.x), py = (int)std::floor(a.y);
    for (int i = 1; i <= steps; ++i) {
        Vec2 p = a + d * (i / (float)steps);
        int x = (int)std::floor(p.x), y = (int)std::floor(p.y);
        if (x != px && y == py && edgeBlocked(px, py, x - px, 0)) return false;
        if (y != py && x == px && edgeBlocked(px, py, 0, y - py)) return false;
        if (x != px && y != py) {
            if (edgeBlocked(px, py, x - px, 0) && edgeBlocked(px, py, 0, y - py)) return false;
        }
        px = x; py = y;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Doors
// ---------------------------------------------------------------------------

bool World::doorAt(int x, int y, int dir) {
    const Tile& t = tile(x, y);
    return (t.walls & (dir == 0 ? DoorN : DoorW)) != 0;
}

bool World::doorOpen(int x, int y, int dir) const {
    auto it = doors_.find(doorKey(x, y, dir));
    return it != doors_.end() && it->second;
}

void World::toggleDoor(int x, int y, int dir) {
    bool& d = doors_[doorKey(x, y, dir)];
    d = !d;
    touch(x, y);
}

bool World::findDoorNear(Vec2 p, float reach, int& ox, int& oy, int& odir) {
    float best = reach;
    bool found = false;
    int tx = (int)std::floor(p.x), ty = (int)std::floor(p.y);
    for (int y = ty - 2; y <= ty + 2; ++y)
        for (int x = tx - 2; x <= tx + 2; ++x)
            for (int dir = 0; dir < 2; ++dir) {
                if (!doorAt(x, y, dir)) continue;
                Vec2 mid = dir == 0 ? Vec2{x + 0.5f, (float)y} : Vec2{(float)x, y + 0.5f};
                float d = distance(mid, p);
                if (d < best) { best = d; ox = x; oy = y; odir = dir; found = true; }
            }
    return found;
}

// ---------------------------------------------------------------------------
// Containers, ground items, water
// ---------------------------------------------------------------------------

std::string World::containerKindAt(int x, int y) {
    const Tile& t = tile(x, y);
    if (t.prop == Prop::None) return "";
    if (const Building* b = buildingAt(x, y)) {
        if (const Furniture* f = b->furnitureAt(x, y)) return f->container;
    }
    const char* c = propContainer(t.prop);
    return c ? c : "";
}

ContainerState& World::container(int x, int y) {
    int64_t key = tileKey(x, y);
    auto it = containers_.find(key);
    if (it != containers_.end()) return it->second;
    ContainerState& c = containers_[key];
    c.kind = containerKindAt(x, y);
    c.items = items_.rollLoot(c.kind, hash3(x, y, 0x1007, seed_));
    return c;
}

std::vector<ItemInstance>& World::groundItems(int x, int y) {
    return ground_[tileKey(x, y)];
}

void World::dropItem(Vec2 p, const ItemInstance& it) {
    if (!it.def) return;
    groundItems((int)std::floor(p.x), (int)std::floor(p.y)).push_back(it);
}

std::vector<std::pair<int64_t, int>> World::groundItemsNear(Vec2 p, float radius) {
    std::vector<std::pair<int64_t, int>> out;
    int r = (int)std::ceil(radius);
    int tx = (int)std::floor(p.x), ty = (int)std::floor(p.y);
    for (int y = ty - r; y <= ty + r; ++y)
        for (int x = tx - r; x <= tx + r; ++x) {
            auto it = ground_.find(tileKey(x, y));
            if (it == ground_.end()) continue;
            if (distance(Vec2{x + 0.5f, y + 0.5f}, p) > radius + 0.5f) continue;
            for (int i = 0; i < (int)it->second.size(); ++i) out.push_back({it->first, i});
        }
    return out;
}

std::vector<ItemInstance>* World::groundStack(int64_t key) {
    auto it = ground_.find(key);
    return it == ground_.end() ? nullptr : &it->second;
}

World::WaterSource World::waterSourceNear(Vec2 p, float reach, int* ox, int* oy) {
    int r = (int)std::ceil(reach);
    int tx = (int)std::floor(p.x), ty = (int)std::floor(p.y);
    WaterSource best = WaterSource::None;
    float bestD = 1e9f;
    for (int y = ty - r; y <= ty + r; ++y)
        for (int x = tx - r; x <= tx + r; ++x) {
            float d = distance(Vec2{x + 0.5f, y + 0.5f}, p);
            if (d > reach + 0.7f || d >= bestD) continue;
            const Tile& t = tile(x, y);
            WaterSource s = WaterSource::None;
            if (t.ground == Ground::WaterShallow || t.ground == Ground::WaterDeep) s = WaterSource::Lake;
            else if (t.prop == Prop::Toilet) s = WaterSource::ToiletTank;
            else if (t.prop == Prop::RainBarrel) s = WaterSource::RainBarrel;
            if (s == WaterSource::None) continue;
            if (s != WaterSource::Lake) {
                auto it = tankWater_.find(tileKey(x, y));
                if (it != tankWater_.end() && it->second <= 1.0f) continue;
            }
            best = s;
            bestD = d;
            if (ox) *ox = x;
            if (oy) *oy = y;
        }
    return best;
}

float World::takeTankWater(int x, int y, float ml) {
    int64_t key = tileKey(x, y);
    auto it = tankWater_.find(key);
    if (it == tankWater_.end()) {
        float start = tile(x, y).prop == Prop::RainBarrel ? 60000.0f * hashFloat(hash2(x, y, seed_)) + 20000.0f : 6000.0f;
        it = tankWater_.emplace(key, start).first;
    }
    float take = std::min(ml, it->second);
    it->second -= take;
    return take;
}

bool World::harvested(int x, int y, float regrowDays) {
    auto it = overrides_.find(tileKey(x, y));
    if (it == overrides_.end() || it->second.harvestedAtDay < 0) return false;
    return (clock_.totalMinutes() / 1440.0) - it->second.harvestedAtDay < regrowDays;
}

void World::markHarvested(int x, int y) {
    overrides_[tileKey(x, y)].harvestedAtDay = (float)(clock_.totalMinutes() / 1440.0);
}

// ---------------------------------------------------------------------------
// Fires
// ---------------------------------------------------------------------------

Campfire* World::fireNear(Vec2 p, float reach, bool litOnly) {
    Campfire* best = nullptr;
    float bestD = reach;
    for (auto& f : fires_) {
        if (litOnly && !f.lit) continue;
        float d = distance(f.pos, p);
        if (d < bestD) { bestD = d; best = &f; }
    }
    return best;
}

float World::fireWarmthAt(Vec2 p) {
    float w = 0;
    for (const auto& f : fires_) {
        if (!f.lit) continue;
        float d = distance(f.pos, p);
        if (d < 6.0f) w += 14.0f * (1.0f - ramp(d, 1.0f, 6.0f));
    }
    return std::min(w, 22.0f);
}

float World::temperatureAt(Vec2 p) {
    float t = indoors(p) ? clock_.indoorTempC() : clock_.outdoorTempC();
    return t + fireWarmthAt(p);
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void World::update(float gameMinutes, Vec2 focus) {
    clock_.advance(gameMinutes);
    clockTime_ += gameMinutes;
    // Snow settles when it snows below freezing and melts in the thaw; rain wets the ground
    const Weather& wx = clock_.weather();
    float t = clock_.outdoorTempC();
    float hours = gameMinutes / 60.0f;
    if (wx.kind == WeatherKind::Snow) snowCover_ = std::min(1.0f, snowCover_ + hours * 0.12f * wx.rain);
    else if (t > 0.5f) snowCover_ = std::max(0.0f, snowCover_ - hours * 0.03f * (t - 0.5f + (wx.rain > 0 ? 2.0f : 0.0f)));
    if (wx.rain > 0.05f && wx.kind != WeatherKind::Snow) wetness_ = std::min(1.0f, wetness_ + hours * 1.5f * wx.rain);
    else wetness_ = std::max(0.0f, wetness_ - hours * (0.15f + 0.02f * std::max(0.0f, t)));
    for (auto& f : fires_) {
        if (!f.lit) continue;
        // Rain makes fires burn down faster
        f.fuelMinutes -= gameMinutes * (1.0f + clock_.weather().rain * 1.5f);
        if (f.fuelMinutes <= 0) { f.fuelMinutes = 0; f.lit = false; }
    }
    // Food on the ground and in containers ages too (cheaply, only near the player)
    float temp = t;
    for (auto& [key, stack] : ground_) {
        for (auto& it : stack)
            if (it.def->spoilHours > 0 && !(it.def->sealed && !it.opened))
                it.ageHours += hours * std::pow(2.0f, (temp - 20.0f) / 10.0f);
    }
    (void)focus;
}

void World::unloadFarChunks(Vec2 focus, float radius) {
    int fcx = floorDiv((int)focus.x, kChunkSize), fcy = floorDiv((int)focus.y, kChunkSize);
    int r = (int)std::ceil(radius / kChunkSize);
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        const Chunk& c = *it->second;
        if (std::abs(c.cx - fcx) > r || std::abs(c.cy - fcy) > r) it = chunks_.erase(it);
        else ++it;
    }
}

} // namespace alone
