#include "world/WorldGen.h"
#include "core/Log.h"
#include "core/Noise.h"
#include "core/Rng.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace alone {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char* buildingTypeName(BuildingType t) {
    static const char* n[] = {"House", "Cottage", "Farmhouse", "Barn", "Shed", "Apartment block", "Office",
                              "Warehouse", "Grocery store", "Pharmacy", "Gas station", "Hardware store",
                              "Hospital", "School", "Church", "Town hall", "Police station", "Fire station",
                              "Railway station", "Hostel", "Manor", "Sports hall", "Shop"};
    static_assert(sizeof(n) / sizeof(n[0]) == (size_t)BuildingType::Count, "building names");
    return n[(int)t];
}

const char* roomTypeName(RoomType t) {
    static const char* n[] = {"Hallway", "Living room", "Kitchen", "Bedroom", "Bathroom", "Storage", "Shop",
                              "Office", "Ward", "Classroom", "Nave", "Lobby", "Changing room"};
    return n[(int)t];
}

const Room* Building::roomAt(int x, int y) const {
    for (const auto& r : rooms)
        if (r.rect.contains(x, y)) return &r;
    return nullptr;
}

const Furniture* Building::furnitureAt(int x, int y) const {
    for (const auto& f : furniture)
        if (f.x == x && f.y == y) return &f;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

float rnd(uint32_t& h) {
    h = hash32(h + 0x9E3779B9u);
    return hashFloat(h);
}
int rint(uint32_t& h, int lo, int hi) {   // inclusive
    if (hi <= lo) return lo;
    return lo + (int)(rnd(h) * (hi - lo + 1)) % (hi - lo + 1);
}
template <typename T>
const T& pick(uint32_t& h, const std::vector<T>& v) { return v[rint(h, 0, (int)v.size() - 1)]; }

BuildingType landmarkType(const std::string& kind) {
    if (kind == "church") return BuildingType::Church;
    if (kind == "townhall") return BuildingType::TownHall;
    if (kind == "station") return BuildingType::Station;
    if (kind == "hospital") return BuildingType::Hospital;
    if (kind == "hostel") return BuildingType::Hostel;
    if (kind == "manor") return BuildingType::Manor;
    if (kind == "grocery") return BuildingType::Grocery;
    if (kind == "gasstation") return BuildingType::GasStation;
    if (kind == "school") return BuildingType::School;
    if (kind == "pharmacy") return BuildingType::Pharmacy;
    if (kind == "hardware") return BuildingType::Hardware;
    if (kind == "police") return BuildingType::Police;
    if (kind == "firestation") return BuildingType::FireStation;
    if (kind == "sports") return BuildingType::Sports;
    return BuildingType::Office;
}

bool isUrban(Zone z) {
    return z == Zone::Residential || z == Zone::Commercial || z == Zone::Hospital;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

WorldGen::WorldGen(const MapData& map, uint32_t seed) : map_(map), seed_(seed) {
    gridW_ = (int)(map.widthM() / 64) + 2;
    gridH_ = (int)(map.heightM() / 64) + 2;
    buildingGrid_.assign((size_t)gridW_ * gridH_, {});
    makeLandmarks();
    makeDenseBlocks();
}

int WorldGen::addBuilding(Building b) {
    b.id = (int)buildings_.size();
    IRect r = b.rect.expanded(3);
    if (b.lot.w > 0) r = {std::min(r.x, b.lot.x), std::min(r.y, b.lot.y),
                          std::max(r.right(), b.lot.right()) - std::min(r.x, b.lot.x),
                          std::max(r.bottom(), b.lot.bottom()) - std::min(r.y, b.lot.y)};
    if (b.hasPath) {
        int px0 = (int)std::min(b.doorPos.x, b.pathEnd.x) - 2, px1 = (int)std::max(b.doorPos.x, b.pathEnd.x) + 2;
        int py0 = (int)std::min(b.doorPos.y, b.pathEnd.y) - 2, py1 = (int)std::max(b.doorPos.y, b.pathEnd.y) + 2;
        int nx0 = std::min(r.x, px0), ny0 = std::min(r.y, py0);
        r = {nx0, ny0, std::max(r.right(), px1) - nx0, std::max(r.bottom(), py1) - ny0};
    }
    for (int gy = std::max(0, r.y / 64); gy <= std::min(gridH_ - 1, r.bottom() / 64); ++gy)
        for (int gx = std::max(0, r.x / 64); gx <= std::min(gridW_ - 1, r.right() / 64); ++gx)
            buildingGrid_[(size_t)gy * gridW_ + gx].push_back(b.id);
    buildings_.push_back(std::move(b));
    return buildings_.back().id;
}

void WorldGen::makeLandmarks() {
    int i = 0;
    for (const auto& lm : map_.landmarks()) {
        Building b;
        b.type = landmarkType(lm.kind);
        b.name = lm.name;
        b.rect = {(int)(lm.centre.x - lm.size.x / 2), (int)(lm.centre.y - lm.size.y / 2), (int)lm.size.x, (int)lm.size.y};
        b.seed = hash3(i, 0, 0xB111D, seed_);
        b.lot = b.rect.expanded(4);
        layout(b);
        reserved_.push_back(b.rect.expanded(6));
        addBuilding(std::move(b));
        ++i;
    }
}

void WorldGen::makeDenseBlocks() {
    // Old town: rows of buildings along both sides of every street.
    int placed = 0;
    for (const auto& area : map_.denseAreas()) {
        for (size_t si = 0; si < map_.roads().size(); ++si) {
            const RoadSegment& seg = map_.roads()[si];
            if (seg.cls == RoadClass::Motorway) continue;
            Vec2 mid = (seg.a + seg.b) * 0.5f;
            if (distance(mid, area.centre) > area.radius) continue;
            Vec2 dir = (seg.b - seg.a).normalized();
            Vec2 n{-dir.y, dir.x};
            float len = (seg.b - seg.a).length();
            bool axisX = std::fabs(dir.x) >= std::fabs(dir.y);
            for (int side = -1; side <= 1; side += 2) {
                uint32_t h = hash3((int)si, side, 0xDE45E, seed_);
                float t = 5.0f;
                while (t < len - 6.0f) {
                    int along = rint(h, 7, 13), depth = rint(h, 9, 13);
                    int bw = axisX ? along : depth, bh = axisX ? depth : along;
                    float offset = seg.halfWidth + 3.5f + std::fabs(n.x) * bw * 0.5f + std::fabs(n.y) * bh * 0.5f;
                    Vec2 c = seg.a + dir * (t + along * 0.5f) + n * (offset * side);
                    IRect r{(int)std::lround(c.x - bw * 0.5f), (int)std::lround(c.y - bh * 0.5f), bw, bh};
                    bool ok = distance(c, area.centre) < area.radius * 1.1f && siteIsClear(r);
                    for (const auto& pl : map_.plazas()) {
                        IRect pr{(int)(pl.centre.x - pl.size.x / 2), (int)(pl.centre.y - pl.size.y / 2), (int)pl.size.x, (int)pl.size.y};
                        if (pr.expanded(2).intersects(r)) ok = false;
                    }
                    if (ok)
                        for (const auto& d : denseRects_)
                            if (d.expanded(1).intersects(r)) { ok = false; break; }
                    if (!ok) { t += 2.0f; continue; }
                    Building b;
                    float k = rnd(h);
                    b.type = k < 0.4f ? BuildingType::Shop : k < 0.7f ? BuildingType::House : k < 0.9f ? BuildingType::Apartment
                                                                                                    : BuildingType::Office;
                    if (b.type == BuildingType::Apartment && (bw < 12 && bh < 12)) b.type = BuildingType::House;
                    b.rect = r;
                    b.lot = r.expanded(1);
                    b.seed = h;
                    layout(b);
                    if (b.type == BuildingType::House) { b.floors = 2; b.roof = RoofKind::Gable; }
                    denseRects_.push_back(r);
                    addBuilding(std::move(b));
                    ++placed;
                    t += along + rint(h, 1, 3);
                }
            }
        }
    }
    Log::info("Old town: %d buildings", placed);
}

// ---------------------------------------------------------------------------
// Lots
// ---------------------------------------------------------------------------

bool WorldGen::siteIsClear(const IRect& r, bool allowRoads) const {
    for (const auto& res : reserved_)
        if (res.intersects(r)) return false;
    for (int y = r.y; y <= r.bottom(); y += 2) {
        for (int x = r.x; x <= r.right(); x += 2) {
            if (map_.waterCoverage((float)x, (float)y) > 0.3f) return false;
            if (allowRoads) continue;
            RoadHit h = map_.nearestRoad((float)x, (float)y);
            if (h.cls != RoadClass::None && h.distance < h.halfWidth + 3.0f) return false;
            RoadHit rh = map_.nearestRoad((float)x, (float)y, true);
            if (rh.cls != RoadClass::None && rh.distance < rh.halfWidth + 6.0f) return false;
        }
    }
    return true;
}

bool WorldGen::tryPlace(BuildingType type, const IRect& area, uint32_t h, Building& out) {
    int w, d;
    switch (type) {
        case BuildingType::House:     w = rint(h, 8, 12); d = rint(h, 7, 10); break;
        case BuildingType::Cottage:   w = rint(h, 6, 8);  d = rint(h, 5, 7); break;
        case BuildingType::Farmhouse: w = rint(h, 10, 13); d = rint(h, 8, 10); break;
        case BuildingType::Barn:      w = rint(h, 14, 20); d = rint(h, 9, 12); break;
        case BuildingType::Shed:      w = rint(h, 3, 5);  d = rint(h, 3, 4); break;
        case BuildingType::Apartment: w = rint(h, 26, 38); d = rint(h, 11, 14); break;
        case BuildingType::Office:    w = rint(h, 16, 28); d = rint(h, 12, 16); break;
        case BuildingType::Warehouse: w = rint(h, 20, 34); d = rint(h, 16, 24); break;
        default:                      w = rint(h, 10, 16); d = rint(h, 8, 12); break;
    }
    if (rnd(h) < 0.5f) std::swap(w, d);
    if (w > area.w - 4 || d > area.h - 4) { std::swap(w, d); }
    if (w > area.w - 4 || d > area.h - 4) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        IRect r{area.x + rint(h, 2, area.w - w - 2), area.y + rint(h, 2, area.h - d - 2), w, d};
        if (!siteIsClear(r)) continue;
        out.type = type;
        out.rect = r;
        out.lot = area;
        out.seed = h;
        return true;
    }
    return false;
}

void WorldGen::ensureLot(int lx, int ly) {
    // Old town: single lots filling the space between the street rows
    {
        Vec2 c{(lx + 0.5f) * kLot, (ly + 0.5f) * kLot};
        bool inDense = false;
        for (const auto& d : map_.denseAreas())
            if (distance(c, d.centre) < d.radius) inDense = true;
        if (inDense) {
            int64_t key = tileKey(lx, ly);
            if (lotDone_.count(key)) return;
            lotDone_[key] = 1;
            IRect area{lx * kLot, ly * kLot, kLot, kLot};
            uint32_t h = hash3(lx, ly, 0xBAC, seed_);
            if (rnd(h) > 0.8f) return;
            BuildingType type = rnd(h) < 0.6f ? BuildingType::House : BuildingType::Apartment;
            for (int attempt = 0; attempt < 6; ++attempt) {
                Building cand;
                if (!tryPlace(type == BuildingType::Apartment ? BuildingType::Office : BuildingType::House, area,
                              hash32(h + attempt * 31), cand)) continue;
                bool clash = false;
                for (const auto& d : denseRects_)
                    if (d.expanded(2).intersects(cand.rect)) { clash = true; break; }
                if (clash) continue;
                cand.type = type;
                cand.lot = cand.rect.expanded(1);
                layout(cand);
                if (type == BuildingType::House) cand.floors = 2;
                denseRects_.push_back(cand.rect);
                addBuilding(cand);
                break;
            }
            return;
        }
    }

    // Dense zones use 2x2 lots as one big block.
    int ax = lx & ~1, ay = ly & ~1;
    float bcx = (ax + 1.0f) * kLot, bcy = (ay + 1.0f) * kLot;
    Zone blockZone = map_.zoneAtPixel((int)(bcx / map_.metresPerPixel()), (int)(bcy / map_.metresPerPixel()));
    bool nearCentre = false;
    for (const auto& d : map_.districts())
        if (d.name == "Centrum" && distance(d.centre, Vec2{bcx, bcy}) < d.radius * 1.4f) nearCentre = true;
    bool big = blockZone == Zone::Commercial || blockZone == Zone::Hospital || (nearCentre && blockZone == Zone::Residential);
    if (big) { lx = ax; ly = ay; }

    int64_t key = tileKey(lx, ly);
    if (lotDone_.count(key)) return;
    lotDone_[key] = 1;
    if (big) {
        lotDone_[tileKey(ax + 1, ay)] = 1;
        lotDone_[tileKey(ax, ay + 1)] = 1;
        lotDone_[tileKey(ax + 1, ay + 1)] = 1;
    }

    IRect area{lx * kLot, ly * kLot, big ? 2 * kLot : kLot, big ? 2 * kLot : kLot};
    for (const auto& d : map_.denseAreas())
        if (distance(area.centre(), d.centre) < d.radius + (big ? 30.0f : 0.0f)) return;
    if (area.x < 0 || area.y < 0 || area.right() > map_.widthM() || area.bottom() > map_.heightM()) return;
    Vec2 c = area.centre();
    Zone z = big ? blockZone : map_.zoneAt(c.x, c.y);
    uint32_t h = hash3(lx, ly, 0x107, seed_);
    RoadHit road = map_.nearestRoad(c.x, c.y);
    float roadDist = road.cls == RoadClass::None ? 1e9f : road.distance;
    if (road.cls == RoadClass::Motorway) roadDist += 30;   // nobody builds facing the E20

    BuildingType type;
    float chance;
    switch (z) {
        case Zone::Residential:
            type = big ? (rnd(h) < 0.75f ? BuildingType::Apartment : BuildingType::Office) : BuildingType::House;
            chance = big ? 0.9f : 0.78f;
            if (roadDist > 40) chance = 0.0f;
            break;
        case Zone::Commercial: {
            float r = rnd(h);
            type = r < 0.45f ? BuildingType::Warehouse : r < 0.8f ? BuildingType::Office : BuildingType::Apartment;
            chance = roadDist < 60 ? 0.85f : 0.0f;
            break;
        }
        case Zone::Hospital: type = BuildingType::Office; chance = 0.5f; break;
        case Zone::Field:    type = BuildingType::Farmhouse; chance = roadDist < 50 ? 0.07f : 0.0f; break;
        case Zone::Meadow:   type = BuildingType::Cottage; chance = roadDist < 50 ? 0.06f : 0.0f; break;
        case Zone::Woodland: type = BuildingType::Cottage; chance = roadDist < 40 ? 0.05f : 0.0f; break;
        case Zone::Forest:   type = BuildingType::Cottage; chance = roadDist < 30 ? 0.01f : 0.0f; break;
        default: return;
    }
    if (rnd(h) >= chance) return;

    Building b;
    if (!tryPlace(type, area, h, b)) return;
    layout(b);
    if (type == BuildingType::House || type == BuildingType::Farmhouse || type == BuildingType::Cottage) {
        float r = rnd(h);
        b.lotBorder = r < 0.35f ? 1 : r < 0.6f ? 2 : 0;
    }
    int mainId = addBuilding(b);
    (void)mainId;

    // Outbuildings: sheds by houses, barns on farms
    if (type == BuildingType::House || type == BuildingType::Cottage || type == BuildingType::Farmhouse) {
        bool farm = type == BuildingType::Farmhouse;
        if (rnd(h) < (farm ? 0.9f : 0.45f)) {
            Building s;
            BuildingType st = farm ? BuildingType::Barn : BuildingType::Shed;
            const Building& main = buildings_.back();
            for (int attempt = 0; attempt < 6; ++attempt) {
                Building cand;
                IRect sub = area;
                if (!tryPlace(st, sub, hash32(h + attempt * 7919), cand)) continue;
                if (cand.rect.expanded(2).intersects(main.rect)) continue;
                cand.isOutbuilding = true;
                cand.lot = IRect{};
                layout(cand);
                addBuilding(cand);
                break;
            }
        }
    }
}

std::vector<int> WorldGen::buildingsIn(const IRect& r) {
    int lx0 = floorDiv(r.x - 2, kLot), lx1 = floorDiv(r.right() + 2, kLot);
    int ly0 = floorDiv(r.y - 2, kLot), ly1 = floorDiv(r.bottom() + 2, kLot);
    for (int ly = ly0 - 1; ly <= ly1 + 1; ++ly)
        for (int lx = lx0 - 1; lx <= lx1 + 1; ++lx) ensureLot(lx, ly);
    std::set<int> ids;
    for (int gy = std::max(0, floorDiv(r.y, 64)); gy <= std::min(gridH_ - 1, floorDiv(r.bottom(), 64)); ++gy)
        for (int gx = std::max(0, floorDiv(r.x, 64)); gx <= std::min(gridW_ - 1, floorDiv(r.right(), 64)); ++gx)
            for (int id : buildingGrid_[(size_t)gy * gridW_ + gx]) ids.insert(id);
    return std::vector<int>(ids.begin(), ids.end());
}

const Building* WorldGen::building(int id) const {
    return id >= 0 && id < (int)buildings_.size() ? &buildings_[id] : nullptr;
}

const Building* WorldGen::findStartHouse(Vec2 near) {
    IRect area{(int)near.x - 300, (int)near.y - 300, 600, 600};
    const Building* best = nullptr;
    float bestD = 1e9f;
    for (int id : buildingsIn(area)) {
        const Building& b = buildings_[id];
        if (b.type != BuildingType::House) continue;
        float d = distance(b.rect.centre(), near);
        if (d < bestD) { bestD = d; best = &b; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Floor plans
// ---------------------------------------------------------------------------

void WorldGen::layout(Building& b) {
    uint32_t h = b.seed ^ 0x51ED;
    b.rooms.clear();
    b.edges.clear();
    b.furniture.clear();
    const IRect& r = b.rect;

    // Exterior walls with windows
    auto windowAt = [&](int i, int len) { return i > 0 && i < len - 1 && (i % 3 == 1) && rnd(h) < 0.7f; };
    for (int x = r.x; x < r.right(); ++x) {
        b.edges.push_back({x, r.y, (uint8_t)(WallN | (windowAt(x - r.x, r.w) ? WindowN : 0))});
        b.edges.push_back({x, r.bottom(), (uint8_t)(WallN | (windowAt(x - r.x, r.w) ? WindowN : 0))});
    }
    for (int y = r.y; y < r.bottom(); ++y) {
        b.edges.push_back({r.x, y, (uint8_t)(WallW | (windowAt(y - r.y, r.h) ? WindowW : 0))});
        b.edges.push_back({r.right(), y, (uint8_t)(WallW | (windowAt(y - r.y, r.h) ? WindowW : 0))});
    }

    switch (b.type) {
        case BuildingType::House: case BuildingType::Cottage: case BuildingType::Farmhouse:
        case BuildingType::Manor:
            splitRooms(b, r, 0, h);
            break;
        case BuildingType::Apartment: case BuildingType::Office: case BuildingType::School:
        case BuildingType::Hospital: case BuildingType::Hostel: case BuildingType::Police:
        case BuildingType::TownHall: case BuildingType::FireStation: {
            // Corridor down the long axis, rooms either side
            bool alongX = r.w >= r.h;
            int len = alongX ? r.w : r.h, depth = alongX ? r.h : r.w;
            int cw = 2;
            int c0 = (depth - cw) / 2;
            IRect corridor = alongX ? IRect{r.x, r.y + c0, r.w, cw} : IRect{r.x + c0, r.y, cw, r.h};
            b.rooms.push_back({corridor, RoomType::Hall});
            for (int side = 0; side < 2; ++side) {
                int sd = side == 0 ? c0 : depth - c0 - cw;
                if (sd < 2) continue;
                int pos = 0;
                while (pos < len) {
                    int rl = std::min(len - pos, rint(h, 4, 6));
                    if (len - pos - rl < 3) rl = len - pos;
                    IRect room = alongX ? IRect{r.x + pos, side == 0 ? r.y : r.y + c0 + cw, rl, sd}
                                        : IRect{side == 0 ? r.x : r.x + c0 + cw, r.y + pos, sd, rl};
                    b.rooms.push_back({room, RoomType::Office});
                    // wall between this room and the next along the corridor
                    if (pos + rl < len) {
                        for (int i = 0; i < sd; ++i) {
                            if (alongX) b.edges.push_back({r.x + pos + rl, room.y + i, WallW});
                            else b.edges.push_back({room.x + i, r.y + pos + rl, WallN});
                        }
                    }
                    // wall to the corridor with a door
                    int door = rint(h, 1, std::max(1, rl - 2));
                    for (int i = 0; i < rl; ++i) {
                        uint8_t bits = (uint8_t)(i == door ? 1 : 0);
                        if (alongX) {
                            int wy = side == 0 ? r.y + c0 : r.y + c0 + cw;
                            b.edges.push_back({r.x + pos + i, wy, (uint8_t)(WallN | (bits ? DoorN : 0))});
                        } else {
                            int wx = side == 0 ? r.x + c0 : r.x + c0 + cw;
                            b.edges.push_back({wx, r.y + pos + i, (uint8_t)(WallW | (bits ? DoorW : 0))});
                        }
                    }
                    pos += rl;
                }
            }
            break;
        }
        case BuildingType::Grocery: case BuildingType::Pharmacy: case BuildingType::Hardware:
        case BuildingType::GasStation: case BuildingType::Shop: {
            // Shop floor with a storeroom along the back
            bool alongX = r.w >= r.h;
            int depth = alongX ? r.h : r.w;
            int back = std::max(3, depth / 4);
            IRect shop = alongX ? IRect{r.x, r.y + back, r.w, r.h - back} : IRect{r.x + back, r.y, r.w - back, r.h};
            IRect store = alongX ? IRect{r.x, r.y, r.w, back} : IRect{r.x, r.y, back, r.h};
            b.rooms.push_back({shop, RoomType::ShopFloor});
            b.rooms.push_back({store, RoomType::Storage});
            int len = alongX ? r.w : r.h;
            int door = rint(h, 1, len - 2);
            for (int i = 0; i < len; ++i) {
                if (alongX) b.edges.push_back({r.x + i, r.y + back, (uint8_t)(WallN | (i == door ? DoorN : 0))});
                else b.edges.push_back({r.x + back, r.y + i, (uint8_t)(WallW | (i == door ? DoorW : 0))});
            }
            break;
        }
        case BuildingType::Church:   b.rooms.push_back({r, RoomType::Nave}); break;
        case BuildingType::Station:  b.rooms.push_back({r, RoomType::Lobby}); break;
        case BuildingType::Sports:   b.rooms.push_back({r, RoomType::Locker}); break;
        default:                     b.rooms.push_back({r, RoomType::Storage}); break;
    }

    assignRooms(b, h);
    addExteriorDoor(b, h);
    furnish(b, h);
    styleBuilding(b, h);
}

void WorldGen::splitRooms(Building& b, IRect r, int depth, uint32_t& h) {
    int maxArea = b.type == BuildingType::Manor ? 30 : 22;
    bool canX = r.w >= 6, canY = r.h >= 6;
    bool leaf = (!canX && !canY) || depth >= 4 || (r.area() <= maxArea && rnd(h) < 0.7f);
    if (leaf) {
        b.rooms.push_back({r, RoomType::Bedroom});
        return;
    }
    bool splitX = canX && (!canY || r.w > r.h || (r.w == r.h && rnd(h) < 0.5f));
    if (splitX) {
        int s = rint(h, 3, r.w - 3);
        int door = rint(h, 1, std::max(1, r.h - 2));
        for (int y = 0; y < r.h; ++y)
            b.edges.push_back({r.x + s, r.y + y, (uint8_t)(WallW | (y == door ? DoorW : 0))});
        splitRooms(b, {r.x, r.y, s, r.h}, depth + 1, h);
        splitRooms(b, {r.x + s, r.y, r.w - s, r.h}, depth + 1, h);
    } else {
        int s = rint(h, 3, r.h - 3);
        int door = rint(h, 1, std::max(1, r.w - 2));
        for (int x = 0; x < r.w; ++x)
            b.edges.push_back({r.x + x, r.y + s, (uint8_t)(WallN | (x == door ? DoorN : 0))});
        splitRooms(b, {r.x, r.y, r.w, s}, depth + 1, h);
        splitRooms(b, {r.x, r.y + s, r.w, r.h - s}, depth + 1, h);
    }
}

void WorldGen::assignRooms(Building& b, uint32_t& h) {
    auto& rooms = b.rooms;
    switch (b.type) {
        case BuildingType::House: case BuildingType::Cottage: case BuildingType::Farmhouse:
        case BuildingType::Manor: {
            std::vector<int> order(rooms.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
            std::sort(order.begin(), order.end(), [&](int a, int c) { return rooms[a].rect.area() > rooms[c].rect.area(); });
            if (rooms.size() == 1) { rooms[0].type = RoomType::Living; break; }
            rooms[order[0]].type = RoomType::Living;
            rooms[order.back()].type = RoomType::Bathroom;
            if (order.size() >= 3) rooms[order[1]].type = RoomType::Kitchen;
            else rooms[order[0]].type = RoomType::Kitchen;   // tiny cottage: kitchen-living
            if (order.size() >= 6) rooms[order[order.size() - 2]].type = RoomType::Storage;
            if (b.type == BuildingType::Manor && order.size() >= 5) rooms[order[2]].type = RoomType::Office;
            break;
        }
        case BuildingType::Apartment:
            for (auto& r : rooms) if (r.type == RoomType::Office) {
                float x = rnd(h);
                r.type = x < 0.35f ? RoomType::Living : x < 0.6f ? RoomType::Kitchen : x < 0.85f ? RoomType::Bedroom : RoomType::Bathroom;
            }
            break;
        case BuildingType::Hospital:
            for (auto& r : rooms) if (r.type == RoomType::Office) r.type = rnd(h) < 0.6f ? RoomType::Ward : RoomType::Office;
            break;
        case BuildingType::School:
            for (auto& r : rooms) if (r.type == RoomType::Office) r.type = rnd(h) < 0.7f ? RoomType::Classroom : RoomType::Locker;
            break;
        case BuildingType::Hostel:
            for (auto& r : rooms) if (r.type == RoomType::Office) r.type = rnd(h) < 0.75f ? RoomType::Bedroom : RoomType::Bathroom;
            break;
        case BuildingType::FireStation: case BuildingType::Police:
            for (auto& r : rooms) if (r.type == RoomType::Office) r.type = rnd(h) < 0.6f ? RoomType::Office : RoomType::Locker;
            break;
        default: break;
    }
}

void WorldGen::addExteriorDoor(Building& b, uint32_t& h) {
    const IRect& r = b.rect;
    // Which side faces the closest road?
    struct Side { int dir; Vec2 probe; };
    Side sides[4] = {
        {0, {r.x + r.w * 0.5f, (float)r.y - 6}}, {1, {(float)r.right() + 6, r.y + r.h * 0.5f}},
        {2, {r.x + r.w * 0.5f, (float)r.bottom() + 6}}, {3, {(float)r.x - 6, r.y + r.h * 0.5f}},
    };
    int best = 2;
    float bestD = 1e9f;
    for (const auto& s : sides) {
        RoadHit hit = map_.nearestRoad(s.probe.x, s.probe.y);
        float d = hit.cls == RoadClass::None ? 1e6f : hit.distance - hit.halfWidth + (hit.cls == RoadClass::Motorway ? 200.0f : 0.0f);
        if (d < bestD) { bestD = d; best = s.dir; }
    }
    // Prefer a door into a living space
    auto goodRoom = [](RoomType t) {
        return t == RoomType::Living || t == RoomType::Hall || t == RoomType::Lobby || t == RoomType::ShopFloor ||
               t == RoomType::Kitchen || t == RoomType::Nave || t == RoomType::Locker || t == RoomType::Storage;
    };
    std::vector<int> candidates;
    int len = (best == 0 || best == 2) ? r.w : r.h;
    for (int i = 1; i < len - 1; ++i) {
        int tx = (best == 0 || best == 2) ? r.x + i : (best == 1 ? r.right() - 1 : r.x);
        int ty = (best == 1 || best == 3) ? r.y + i : (best == 2 ? r.bottom() - 1 : r.y);
        const Room* room = b.roomAt(tx, ty);
        if (room && goodRoom(room->type)) candidates.push_back(i);
    }
    if (candidates.empty())
        for (int i = 1; i < len - 1; ++i) candidates.push_back(i);
    int i = candidates[rint(h, 0, (int)candidates.size() - 1)];
    if (candidates.size() > 2) i = candidates[candidates.size() / 2 + rint(h, -1, 1)];

    int ex, ey;
    uint8_t bits;
    Vec2 outward;
    switch (best) {
        case 0: ex = r.x + i; ey = r.y; bits = WallN | DoorN; outward = {0, -1}; b.doorPos = {ex + 0.5f, ey - 0.5f}; break;
        case 1: ex = r.right(); ey = r.y + i; bits = WallW | DoorW; outward = {1, 0}; b.doorPos = {ex + 0.5f, ey + 0.5f}; break;
        case 2: ex = r.x + i; ey = r.bottom(); bits = WallN | DoorN; outward = {0, 1}; b.doorPos = {ex + 0.5f, ey + 0.5f}; break;
        default: ex = r.x; ey = r.y + i; bits = WallW | DoorW; outward = {-1, 0}; b.doorPos = {ex - 0.5f, ey + 0.5f}; break;
    }
    // replace the plain wall edge with a door
    for (auto& e : b.edges)
        if (e.x == ex && e.y == ey && (e.bits & (bits & (WallN | WallW)))) e.bits = bits;

    // Big buildings get a back door too
    if (r.w * r.h > 150) {
        int bx = r.x + r.w / 2, by = r.y + r.h / 2;
        uint8_t bb;
        int tx, ty;
        switch ((best + 2) % 4) {
            case 0: tx = bx; ty = r.y; bb = WallN | DoorN; break;
            case 1: tx = r.right(); ty = by; bb = WallW | DoorW; break;
            case 2: tx = bx; ty = r.bottom(); bb = WallN | DoorN; break;
            default: tx = r.x; ty = by; bb = WallW | DoorW; break;
        }
        for (auto& e : b.edges)
            if (e.x == tx && e.y == ty && (e.bits & (bb & (WallN | WallW)))) e.bits = bb;
    }

    // Driveway / path to the road
    Vec2 p = b.doorPos;
    for (int step = 0; step < 45; ++step) {
        RoadHit hit = map_.nearestRoad(p.x, p.y);
        if (hit.cls != RoadClass::None && hit.cls != RoadClass::Motorway && hit.distance < hit.halfWidth) {
            b.pathEnd = p;
            b.hasPath = step > 1;
            break;
        }
        if (map_.waterCoverage(p.x, p.y) > 0.4f) break;
        p += outward;
    }
}

void WorldGen::furnish(Building& b, uint32_t& h) {
    // Tiles next to doors stay free
    std::set<int64_t> blocked;
    for (const auto& e : b.edges) {
        if (e.bits & DoorN) { blocked.insert(tileKey(e.x, e.y)); blocked.insert(tileKey(e.x, e.y - 1)); }
        if (e.bits & DoorW) { blocked.insert(tileKey(e.x, e.y)); blocked.insert(tileKey(e.x - 1, e.y)); }
    }
    auto place = [&](int x, int y, Prop p, uint8_t facing, const std::string& container = "") {
        if (blocked.count(tileKey(x, y))) return false;
        if (!b.rect.contains(x, y)) return false;
        blocked.insert(tileKey(x, y));
        std::string c = container;
        if (c.empty() && propContainer(p)) c = propContainer(p);
        b.furniture.push_back({x, y, p, facing, c});
        return true;
    };
    // Place along the walls of a room, in a shuffled order
    auto alongWalls = [&](const Room& room, const std::vector<std::pair<Prop, std::string>>& items) {
        const IRect& r = room.rect;
        std::vector<std::pair<int, int>> spots;   // x, y, facing encoded later
        std::vector<uint8_t> facings;
        for (int x = r.x; x < r.right(); ++x) { spots.push_back({x, r.y}); facings.push_back(2); }
        for (int x = r.x; x < r.right(); ++x) { spots.push_back({x, r.bottom() - 1}); facings.push_back(0); }
        for (int y = r.y + 1; y < r.bottom() - 1; ++y) { spots.push_back({r.x, y}); facings.push_back(1); }
        for (int y = r.y + 1; y < r.bottom() - 1; ++y) { spots.push_back({r.right() - 1, y}); facings.push_back(3); }
        for (const auto& it : items) {
            for (int tries = 0; tries < 12; ++tries) {
                int k = rint(h, 0, (int)spots.size() - 1);
                if (place(spots[k].first, spots[k].second, it.first, facings[k], it.second)) break;
            }
        }
    };
    auto rows = [&](const Room& room, Prop p, const std::string& container, int spacing) {
        const IRect& r = room.rect;
        bool alongX = r.w >= r.h;
        if (alongX) {
            for (int y = r.y + 2; y < r.bottom() - 2; y += spacing)
                for (int x = r.x + 2; x < r.right() - 2; ++x) place(x, y, p, 2, container);
        } else {
            for (int x = r.x + 2; x < r.right() - 2; x += spacing)
                for (int y = r.y + 2; y < r.bottom() - 2; ++y) place(x, y, p, 1, container);
        }
    };

    std::string shelfLoot = "garage_shelf";
    switch (b.type) {
        case BuildingType::Grocery: shelfLoot = "grocery_shelf"; break;
        case BuildingType::Pharmacy: shelfLoot = "pharmacy_shelf"; break;
        case BuildingType::Hardware: shelfLoot = "hardware_shelf"; break;
        case BuildingType::GasStation: shelfLoot = "gas_shelf"; break;
        case BuildingType::Shed: shelfLoot = "toolbox"; break;
        case BuildingType::Shop: {
            static const std::vector<std::string> shops = {"gas_shelf", "wardrobe", "bookshelf", "hardware_shelf",
                                                           "grocery_shelf", "pharmacy_shelf", "dresser"};
            shelfLoot = pick(h, shops);
            break;
        }
        default: break;
    }
    bool hunterHome = rnd(h) < 0.18f;   // lots of Swedes hunt

    for (const auto& room : b.rooms) {
        switch (room.type) {
            case RoomType::Kitchen:
                alongWalls(room, {{Prop::Fridge, ""}, {Prop::Stove, ""}, {Prop::Counter, ""}, {Prop::Counter, ""},
                                  {Prop::Sink, ""}, {Prop::Counter, "pantry"}});
                if (rnd(h) < 0.4f) alongWalls(room, {{Prop::Freezer, ""}});
                if (room.rect.w >= 4 && room.rect.h >= 4) {
                    Vec2 c = room.rect.centre();
                    place((int)c.x, (int)c.y, Prop::Table, 0);
                    place((int)c.x + 1, (int)c.y, Prop::Chair, 3);
                }
                break;
            case RoomType::Living:
                alongWalls(room, {{Prop::Sofa, ""}, {Prop::Bookshelf, ""}, {Prop::Chair, ""}});
                if (room.rect.w >= 4 && room.rect.h >= 4) {
                    Vec2 c = room.rect.centre();
                    place((int)c.x, (int)c.y, Prop::Table, 0);
                }
                if (b.type == BuildingType::Cottage || b.rooms.size() == 1)
                    alongWalls(room, {{Prop::Fridge, ""}, {Prop::Counter, ""}, {Prop::Bed, ""}, {Prop::Wardrobe, ""}});
                break;
            case RoomType::Bedroom:
                alongWalls(room, {{Prop::Bed, ""}, {Prop::Wardrobe, ""}, {Prop::Dresser, ""}});
                if (hunterHome && b.type != BuildingType::Hostel) { alongWalls(room, {{Prop::GunCabinet, ""}}); hunterHome = false; }
                break;
            case RoomType::Bathroom:
                alongWalls(room, {{Prop::Toilet, ""}, {Prop::Sink, ""}, {Prop::BathroomCabinet, ""}, {Prop::Bathtub, ""}});
                break;
            case RoomType::Storage:
                if (b.type == BuildingType::Grocery || b.type == BuildingType::Pharmacy || b.type == BuildingType::Hardware ||
                    b.type == BuildingType::GasStation || b.type == BuildingType::Shop) {
                    alongWalls(room, {{Prop::Shelf, shelfLoot}, {Prop::Shelf, shelfLoot}, {Prop::Shelf, shelfLoot},
                                      {Prop::Desk, b.type == BuildingType::Pharmacy ? "pharmacy_back" : "desk"}});
                } else if (b.type == BuildingType::Barn) {
                    alongWalls(room, {{Prop::HayBale, ""}, {Prop::HayBale, ""}, {Prop::Workbench, ""}, {Prop::Woodpile, ""}});
                } else {
                    alongWalls(room, {{Prop::Workbench, b.type == BuildingType::Shed ? "toolbox" : "garage_shelf"},
                                      {Prop::Shelf, b.type == BuildingType::Shed ? "toolbox" : "pantry"}});
                    if (b.type == BuildingType::Shed) alongWalls(room, {{Prop::Woodpile, ""}});
                    if (hunterHome) { alongWalls(room, {{Prop::GunCabinet, ""}}); hunterHome = false; }
                }
                break;
            case RoomType::ShopFloor: {
                rows(room, Prop::Shelf, shelfLoot, 3);
                std::vector<std::pair<Prop, std::string>> wall;
                if (b.type == BuildingType::Grocery || b.type == BuildingType::GasStation)
                    for (int i = 0; i < 4; ++i) wall.push_back({Prop::Cooler, "grocery_cooler"});
                wall.push_back({Prop::Checkout, "checkout"});
                alongWalls(room, wall);
                break;
            }
            case RoomType::Office:
                alongWalls(room, {{Prop::Desk, ""}, {Prop::Bookshelf, ""}, {Prop::Chair, ""}});
                if (b.type == BuildingType::Police && rnd(h) < 0.3f) alongWalls(room, {{Prop::GunCabinet, ""}});
                break;
            case RoomType::Ward:
                alongWalls(room, {{Prop::HospitalBed, ""}, {Prop::HospitalBed, ""}, {Prop::MedicalCabinet, ""}});
                break;
            case RoomType::Classroom:
                rows(room, Prop::Desk, "desk", 2);
                break;
            case RoomType::Locker:
                alongWalls(room, {{Prop::Locker, ""}, {Prop::Locker, ""}, {Prop::Locker, ""}, {Prop::Bench, ""}});
                break;
            case RoomType::Nave:
                rows(room, Prop::Pew, "", 2);
                alongWalls(room, {{Prop::Altar, ""}});
                break;
            case RoomType::Lobby:
                alongWalls(room, {{Prop::Bench, ""}, {Prop::Bench, ""}, {Prop::Desk, ""}, {Prop::Bin, ""}});
                break;
            case RoomType::Hall:
                if (b.type == BuildingType::Hospital) alongWalls(room, {{Prop::MedicalCabinet, ""}});
                break;
            default: break;
        }
    }
}

void WorldGen::styleBuilding(Building& b, uint32_t& h) {
    b.floors = 1;
    b.roof = RoofKind::Gable;
    b.roofAlongX = b.rect.w >= b.rect.h;
    b.trimColour = 0xf2efe6;
    static const std::vector<uint32_t> houseWalls = {0x8b2a1e, 0x8b2a1e, 0x8b2a1e, 0x9a3324, 0xe3c565, 0xe8d38a,
                                                     0xf0ede4, 0xe6e6e0, 0xa9b9c4, 0x6f8f9a};
    static const std::vector<uint32_t> roofs = {0x2c2c2e, 0x343436, 0x2a2f33, 0x7a2e22, 0x8a3a2a, 0x4a3a30};
    static const std::vector<uint32_t> blocks = {0xd9cbb0, 0xe8e2d4, 0xb85c3c, 0xc9b79a, 0xf0ece2, 0xa0a4a8};
    switch (b.type) {
        case BuildingType::House:
            b.wallColour = pick(h, houseWalls); b.roofColour = pick(h, roofs); b.floors = rnd(h) < 0.4f ? 2 : 1; break;
        case BuildingType::Cottage:
            b.wallColour = rnd(h) < 0.7f ? 0x8b2a1e : 0xe3c565; b.roofColour = pick(h, roofs); break;
        case BuildingType::Farmhouse:
            b.wallColour = rnd(h) < 0.6f ? 0xf0ede4 : 0x8b2a1e; b.roofColour = pick(h, roofs); b.floors = 2; break;
        case BuildingType::Barn:
            b.wallColour = 0x84271b; b.roofColour = 0x2f2f30; b.floors = 2; b.floor = Ground::FloorConcrete; break;
        case BuildingType::Shed:
            b.wallColour = rnd(h) < 0.8f ? 0x8b2a1e : 0x6a5a48; b.roofColour = 0x2c2c2e; b.floor = Ground::FloorConcrete; break;
        case BuildingType::Apartment:
            b.wallColour = pick(h, blocks); b.roofColour = 0x3a3a3c; b.floors = rint(h, 3, 4);
            b.roof = rnd(h) < 0.6f ? RoofKind::Hip : RoofKind::Flat; break;
        case BuildingType::Office: case BuildingType::TownHall: case BuildingType::Police:
            b.wallColour = pick(h, blocks); b.roofColour = 0x45474a; b.floors = rint(h, 2, 3); b.roof = RoofKind::Flat;
            b.floor = Ground::FloorCarpet; break;
        case BuildingType::Warehouse:
            b.wallColour = rnd(h) < 0.5f ? 0x9aa0a6 : 0xb0a890; b.roofColour = 0x55585c; b.floors = 2;
            b.roof = RoofKind::Flat; b.floor = Ground::FloorConcrete; break;
        case BuildingType::Shop: {
            static const std::vector<uint32_t> shopWalls = {0xe3c565, 0xf0ede4, 0x8b2a1e, 0xd9cbb0, 0xa9b9c4, 0xe8d38a};
            b.wallColour = pick(h, shopWalls); b.roofColour = pick(h, roofs); b.floors = 2;
            b.trimColour = 0xf2efe6; b.floor = Ground::FloorWood; b.roof = RoofKind::Gable;
            break;
        }
        case BuildingType::Grocery: case BuildingType::Pharmacy: case BuildingType::Hardware: case BuildingType::GasStation:
            b.wallColour = 0xe8e6e0; b.trimColour = b.type == BuildingType::Pharmacy ? 0x2a8a4a : 0xc0302a;
            b.roofColour = 0x45474a; b.roof = RoofKind::Flat; b.floor = Ground::FloorTile; break;
        case BuildingType::Hospital:
            b.wallColour = 0xc9a88a; b.roofColour = 0x505254; b.floors = 4; b.roof = RoofKind::Flat; b.floor = Ground::FloorTile; break;
        case BuildingType::School:
            b.wallColour = 0xb85c3c; b.roofColour = 0x3a3a3c; b.floors = 2; b.floor = Ground::FloorTile; break;
        case BuildingType::Church:
            b.wallColour = 0xf2efe8; b.roofColour = 0x2a2c30; b.floors = 3; b.floor = Ground::FloorTile; break;
        case BuildingType::Station:
            b.wallColour = 0xd9c49a; b.roofColour = 0x5a3028; b.floors = 2; b.floor = Ground::FloorTile; break;
        case BuildingType::Hostel: case BuildingType::Manor:
            b.wallColour = 0xf0ede4; b.roofColour = 0x2c2c2e; b.floors = 2; b.roof = RoofKind::Hip; break;
        case BuildingType::FireStation:
            b.wallColour = 0xb85c3c; b.roofColour = 0x3a3a3c; b.floors = 2; b.roof = RoofKind::Flat; break;
        case BuildingType::Sports:
            b.wallColour = 0xa9b9c4; b.roofColour = 0x4a4c50; b.floors = 2; b.roof = RoofKind::Flat; b.floor = Ground::FloorConcrete; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Chunks
// ---------------------------------------------------------------------------

void WorldGen::groundFor(Tile& t, int x, int y, Zone z, float water) {
    uint32_t h = hash2(x, y, seed_);
    float r = hashFloat(h);
    float r2 = hashFloat(hash32(h ^ 0xABCD));
    t.variant = (uint8_t)(h >> 24);
    float n = fbm(x * 0.06f, y * 0.06f, seed_ + 5, 3);
    t.prop = Prop::None;

    // Water and shorelines
    if (water >= 0.5f) {
        t.ground = water > 0.68f ? Ground::WaterDeep : Ground::WaterShallow;
        if (t.ground == Ground::WaterShallow && r < 0.12f) t.prop = Prop::Reeds;
        return;
    }
    bool shore = water > 0.3f;

    auto pickTree = [&](float a, float b, float c, float d) {   // spruce, pine, birch, oak (rest rowan)
        if (r2 < a) return Prop::Spruce;
        if (r2 < a + b) return Prop::Pine;
        if (r2 < a + b + c) return Prop::Birch;
        if (r2 < a + b + c + d) return Prop::Oak;
        return Prop::Rowan;
    };

    switch (z) {
        case Zone::Forest:
            t.ground = n > 0.55f ? Ground::Moss : (n < 0.35f ? Ground::Dirt : Ground::ForestFloor);
            if (r < 0.085f) t.prop = pickTree(0.5f, 0.3f, 0.15f, 0.02f);
            else if (r < 0.17f) t.prop = Prop::BlueberryBush;
            else if (r < 0.215f) t.prop = Prop::LingonBush;
            else if (r < 0.222f) t.prop = Prop::Rock;
            else if (r < 0.2245f) t.prop = Prop::Boulder;
            else if (r < 0.227f) t.prop = Prop::Stump;
            break;
        case Zone::Woodland:
            t.ground = n > 0.6f ? Ground::TallGrass : (n < 0.3f ? Ground::ForestFloor : Ground::Grass);
            if (r < 0.03f) t.prop = pickTree(0.15f, 0.15f, 0.5f, 0.15f);
            else if (r < 0.045f) t.prop = Prop::Shrub;
            else if (r < 0.05f) t.prop = Prop::RoseBush;
            else if (r < 0.07f) t.prop = Prop::BlueberryBush;
            else if (r < 0.072f) t.prop = Prop::Boulder;
            break;
        case Zone::Park:
            t.ground = Ground::Grass;
            if (r < 0.012f) t.prop = r2 < 0.4f ? Prop::Oak : r2 < 0.7f ? Prop::Maple : Prop::Birch;
            else if (r < 0.015f) t.prop = Prop::RoseBush;
            else if (r < 0.017f) t.prop = Prop::Shrub;
            break;
        case Zone::Residential:
            t.ground = Ground::Grass;
            if (r < 0.008f) t.prop = r2 < 0.3f ? Prop::Birch : r2 < 0.55f ? Prop::Maple : r2 < 0.8f ? Prop::Rowan : Prop::Spruce;
            else if (r < 0.016f) t.prop = Prop::Shrub;
            break;
        case Zone::Field: {
            // ploughed rows / stubble in big patches
            float patch = fbm(x * 0.008f, y * 0.008f, seed_ + 9, 2);
            bool row = ((x + y / 3) % 3) == 0;
            t.ground = patch > 0.5f ? (row ? Ground::Dirt : Ground::FieldSoil) : Ground::Stubble;
            if (patch <= 0.5f && r < 0.0015f) t.prop = Prop::HayBale;
            break;
        }
        case Zone::Meadow:
            t.ground = n > 0.45f ? Ground::TallGrass : Ground::Grass;
            if (r < 0.004f) t.prop = pickTree(0.2f, 0.1f, 0.5f, 0.15f);
            else if (r < 0.014f) t.prop = Prop::Shrub;
            else if (r < 0.02f) t.prop = Prop::RoseBush;
            else if (r < 0.022f) t.prop = Prop::Boulder;
            break;
        case Zone::Commercial:
            t.ground = n > 0.62f ? Ground::Grass : Ground::Concrete;
            if (t.ground == Ground::Concrete && r < 0.0015f) t.prop = Prop::Car;
            else if (t.ground == Ground::Grass && r < 0.01f) t.prop = Prop::Shrub;
            break;
        case Zone::Hospital:
            t.ground = n > 0.5f ? Ground::Grass : Ground::Concrete;
            if (t.ground == Ground::Grass && r < 0.01f) t.prop = Prop::Maple;
            break;
        default:
            t.ground = Ground::Grass;
            break;
    }
    if (shore) {
        if (z == Zone::Park || z == Zone::Meadow || z == Zone::Residential) t.ground = Ground::Sand;
        if (isTree(t.prop) || t.prop == Prop::Boulder) t.prop = Prop::None;
        if (r < 0.06f) t.prop = Prop::Reeds;
    }
}

void WorldGen::applyRoads(Tile& t, int x, int y, Zone z) {
    float px = x + 0.5f, py = y + 0.5f;
    RoadHit rail = map_.nearestRoad(px, py, true);
    if (rail.cls != RoadClass::None && rail.distance < rail.halfWidth) {
        t.ground = Ground::Rail;
        t.prop = Prop::None;
        return;
    }
    if (rail.cls != RoadClass::None && rail.distance < rail.halfWidth + 3 && isTree(t.prop)) t.prop = Prop::None;

    RoadHit road = map_.nearestRoad(px, py);
    if (road.cls == RoadClass::None) return;
    float d = road.distance, hw = road.halfWidth;
    bool urban = isUrban(z) || z == Zone::Park;
    if (d < hw) {
        t.ground = Ground::Asphalt;
        t.prop = Prop::None;
        uint32_t h = hash2(x, y, seed_ ^ 0xCA2);
        if (road.cls != RoadClass::Motorway && d < hw * 0.5f && hashFloat(h) < 0.0008f) {
            t.prop = Prop::Car;
            t.variant = (uint8_t)(std::fabs(road.dir.x) > std::fabs(road.dir.y) ? 0 : 1);
        }
        return;
    }
    if (road.cls == RoadClass::Motorway) {
        if (d < hw + 2.0f) { t.ground = Ground::Gravel; t.prop = Prop::None; }
        else if (d < hw + 6.0f && isTree(t.prop)) t.prop = Prop::None;
        return;
    }
    if (urban && d < hw + 1.8f) {
        t.ground = Ground::Sidewalk;
        t.prop = Prop::None;
        if (road.cls == RoadClass::Major && d > hw + 1.0f && std::fmod(road.along, 28.0f) < 1.0f) t.prop = Prop::Lamppost;
        return;
    }
    if (!urban && d < hw + 1.0f) {
        t.ground = Ground::Gravel;
        t.prop = Prop::None;
        return;
    }
    if (d < hw + 3.0f && (isTree(t.prop) || t.prop == Prop::Boulder)) t.prop = Prop::None;
}

void WorldGen::applyBuilding(Chunk& c, const Building& b) {
    int x0 = c.cx * kChunkSize, y0 = c.cy * kChunkSize;
    auto inChunk = [&](int x, int y) { return x >= x0 && y >= y0 && x < x0 + kChunkSize && y < y0 + kChunkSize; };
    auto tileAt = [&](int x, int y) -> Tile& { return c.at(x - x0, y - y0); };

    // Garden: clear trees in the plot, border with hedge or fence
    IRect clear = b.rect.expanded(b.isOutbuilding ? 1 : 3);
    for (int y = std::max(clear.y, y0); y < std::min(clear.bottom(), y0 + kChunkSize); ++y)
        for (int x = std::max(clear.x, x0); x < std::min(clear.right(), x0 + kChunkSize); ++x) {
            Tile& t = tileAt(x, y);
            if (t.prop != Prop::Car && t.prop != Prop::Lamppost) t.prop = Prop::None;
            if (t.ground == Ground::TallGrass || t.ground == Ground::Moss || t.ground == Ground::ForestFloor)
                t.ground = Ground::Grass;
        }
    if (b.lotBorder && b.lot.w > 0) {
        const IRect& l = b.lot;
        for (int y = std::max(l.y, y0); y < std::min(l.bottom(), y0 + kChunkSize); ++y)
            for (int x = std::max(l.x, x0); x < std::min(l.right(), x0 + kChunkSize); ++x) {
                bool edge = x == l.x || y == l.y || x == l.right() - 1 || y == l.bottom() - 1;
                if (!edge) continue;
                Tile& t = tileAt(x, y);
                if (t.ground != Ground::Grass) continue;
                if (b.hasPath && distToSegment({x + 0.5f, y + 0.5f}, b.doorPos, b.pathEnd) < 2.5f) continue;
                t.prop = b.lotBorder == 1 ? Prop::Hedge : Prop::Fence;
                t.variant = (uint8_t)((x == l.x || x == l.right() - 1) ? 1 : 0);
            }
    }

    // Path to the road
    if (b.hasPath) {
        int px0 = (int)std::floor(std::min(b.doorPos.x, b.pathEnd.x)) - 2;
        int px1 = (int)std::ceil(std::max(b.doorPos.x, b.pathEnd.x)) + 2;
        int py0 = (int)std::floor(std::min(b.doorPos.y, b.pathEnd.y)) - 2;
        int py1 = (int)std::ceil(std::max(b.doorPos.y, b.pathEnd.y)) + 2;
        float width = (b.type == BuildingType::House || b.type == BuildingType::Cottage || b.type == BuildingType::Farmhouse) ? 1.4f : 2.2f;
        for (int y = std::max(py0, y0); y < std::min(py1, y0 + kChunkSize); ++y)
            for (int x = std::max(px0, x0); x < std::min(px1, x0 + kChunkSize); ++x) {
                if (distToSegment({x + 0.5f, y + 0.5f}, b.doorPos, b.pathEnd) > width) continue;
                Tile& t = tileAt(x, y);
                if (t.ground == Ground::Asphalt || t.ground == Ground::Sidewalk || t.building >= 0) continue;
                t.ground = width > 2 ? Ground::Concrete : Ground::Gravel;
                if (t.prop != Prop::Car) t.prop = Prop::None;
            }
        // A mailbox by the road for houses
        if ((b.type == BuildingType::House || b.type == BuildingType::Farmhouse) && !b.isOutbuilding) {
            Vec2 dir = (b.doorPos - b.pathEnd).normalized();
            Vec2 side = {-dir.y, dir.x};
            Vec2 mp = b.pathEnd + dir * 2.0f + side * 1.6f;
            int mx = (int)std::floor(mp.x), my = (int)std::floor(mp.y);
            if (inChunk(mx, my) && tileAt(mx, my).ground != Ground::Asphalt && tileAt(mx, my).building < 0)
                tileAt(mx, my).prop = Prop::Mailbox;
        }
    }

    // Woodpile against a house wall, rain barrel
    if ((b.type == BuildingType::House || b.type == BuildingType::Cottage || b.type == BuildingType::Farmhouse) &&
        (b.seed & 3) != 0) {
        int wx = b.rect.right(), wy = b.rect.y + 1;
        if (inChunk(wx, wy)) tileAt(wx, wy).prop = Prop::Woodpile;
        if ((b.seed & 12) == 4 && inChunk(b.rect.x - 1, b.rect.bottom() - 1)) tileAt(b.rect.x - 1, b.rect.bottom() - 1).prop = Prop::RainBarrel;
    }

    // Floor and furniture
    for (int y = std::max(b.rect.y, y0); y < std::min(b.rect.bottom(), y0 + kChunkSize); ++y)
        for (int x = std::max(b.rect.x, x0); x < std::min(b.rect.right(), x0 + kChunkSize); ++x) {
            Tile& t = tileAt(x, y);
            t.building = b.id;
            t.prop = Prop::None;
            const Room* room = b.roomAt(x, y);
            Ground g = b.floor;
            if (room) {
                if (room->type == RoomType::Bathroom || room->type == RoomType::Kitchen) g = Ground::FloorTile;
                else if (room->type == RoomType::Storage && b.floor == Ground::FloorWood) g = Ground::FloorConcrete;
                else if (room->type == RoomType::Bedroom && (b.seed & 16)) g = Ground::FloorCarpet;
            }
            t.ground = g;
        }
    for (const auto& e : b.edges)
        if (inChunk(e.x, e.y)) tileAt(e.x, e.y).walls |= e.bits;
    for (const auto& f : b.furniture)
        if (inChunk(f.x, f.y)) {
            Tile& t = tileAt(f.x, f.y);
            t.prop = f.prop;
            t.variant = f.facing;
        }
}

void WorldGen::generateChunk(Chunk& c) {
    int x0 = c.cx * kChunkSize, y0 = c.cy * kChunkSize;
    c.buildings.clear();
    for (int ly = 0; ly < kChunkSize; ++ly) {
        for (int lx = 0; lx < kChunkSize; ++lx) {
            int x = x0 + lx, y = y0 + ly;
            Tile& t = c.at(lx, ly);
            t = Tile{};
            if (x < 0 || y < 0 || x >= map_.widthM() || y >= map_.heightM()) {
                t.ground = Ground::ForestFloor;
                t.prop = (hash2(x, y, seed_) & 7) == 0 ? Prop::Spruce : Prop::None;
                continue;
            }
            float water = map_.waterCoverage(x + 0.5f, y + 0.5f);
            Zone z = water >= 0.5f ? Zone::Water : map_.zoneAt(x + 0.5f, y + 0.5f);
            Vec2 p{x + 0.5f, y + 0.5f};
            bool dense = water < 0.3f && map_.denseAreaAt(p) != nullptr;
            if (dense) z = Zone::Residential;
            groundFor(t, x, y, z, water);
            if (dense) {
                // Courtyards: paved with some grass and the odd tree
                float n = fbm(x * 0.08f, y * 0.08f, seed_ + 21, 2);
                if (t.ground != Ground::Sand) t.ground = n > 0.55f ? Ground::Grass : Ground::Cobble;
                if (t.ground == Ground::Cobble && t.prop != Prop::None) t.prop = Prop::None;
                RoadHit rh = map_.nearestRoad(p.x, p.y);
                if (rh.cls != RoadClass::None && rh.distance >= rh.halfWidth && rh.distance < rh.halfWidth + 3.2f) {
                    t.ground = Ground::Sidewalk;
                    t.prop = (std::fmod(rh.along, 22.0f) < 1.0f && rh.distance > rh.halfWidth + 2.2f) ? Prop::Lamppost : Prop::None;
                }
            }
            applyRoads(t, x, y, z);
            if (const Plaza* pl = map_.plazaAt(p)) {
                t.ground = Ground::Cobble;
                t.prop = Prop::None;
                float ex = std::fabs(p.x - pl->centre.x) - pl->size.x / 2, ey = std::fabs(p.y - pl->centre.y) - pl->size.y / 2;
                uint32_t hh = hash2(x, y, seed_ ^ 0x91A2);
                if (ex > -2.0f && ey < -3 && (y % 6) == 0) t.prop = Prop::Bench;
                else if ((ex > -1.0f || ey > -1.0f) && (hh & 15) == 0) t.prop = Prop::Lamppost;
                else if (ex > -4.0f && ey > -4.0f) t.prop = Prop::Maple;   // trees at the corners
            }
        }
    }
    IRect area{x0, y0, kChunkSize, kChunkSize};
    for (int id : buildingsIn(area)) {
        const Building& b = buildings_[id];
        IRect touch = b.rect.expanded(4);
        if (b.lot.w > 0) touch = IRect{std::min(touch.x, b.lot.x), std::min(touch.y, b.lot.y),
                                       std::max(touch.right(), b.lot.right()) - std::min(touch.x, b.lot.x),
                                       std::max(touch.bottom(), b.lot.bottom()) - std::min(touch.y, b.lot.y)};
        bool pathTouches = false;
        if (b.hasPath) {
            IRect pr{(int)std::min(b.doorPos.x, b.pathEnd.x) - 3, (int)std::min(b.doorPos.y, b.pathEnd.y) - 3,
                     (int)std::fabs(b.doorPos.x - b.pathEnd.x) + 6, (int)std::fabs(b.doorPos.y - b.pathEnd.y) + 6};
            pathTouches = pr.intersects(area);
        }
        if (!touch.intersects(area) && !pathTouches) continue;
        applyBuilding(c, b);
        if (b.rect.expanded(1).intersects(area)) c.buildings.push_back(id);
    }
    ++c.version;
}

} // namespace alone
