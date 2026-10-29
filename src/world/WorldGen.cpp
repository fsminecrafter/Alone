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
                              "Railway station", "Hostel", "Manor", "Sports hall", "Shop", "Shopping centre",
                              "Restaurant", "Car park", "Shelter", "Military container", "Summer cottage"};
    static_assert(sizeof(n) / sizeof(n[0]) == (size_t)BuildingType::Count, "building names");
    return n[(int)t];
}

const char* roomTypeName(RoomType t) {
    static const char* n[] = {"Hallway", "Living room", "Kitchen", "Bedroom", "Bathroom", "Storage", "Shop",
                              "Office", "Ward", "Classroom", "Nave", "Lobby", "Changing room", "Dining area"};
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
    if (kind == "bunker") return BuildingType::Bunker;
    if (kind == "container") return BuildingType::MilitaryContainer;
    return BuildingType::Office;
}

bool isHomeType(BuildingType t) {
    return t == BuildingType::House || t == BuildingType::Cottage || t == BuildingType::Farmhouse || t == BuildingType::Manor ||
           t == BuildingType::SummerCottage;
}

bool isUrban(Zone z) {
    return z == Zone::Residential || z == Zone::Commercial || z == Zone::Hospital || z == Zone::Paved || z == Zone::Parking;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

WorldGen::WorldGen(const MapData& map, uint32_t seed) : map_(map), seed_(seed) {
    gridW_ = (int)(map.widthM() / 64) + 2;
    gridH_ = (int)(map.heightM() / 64) + 2;
    buildingGrid_.assign((size_t)gridW_ * gridH_, {});
    makeOsmBuildings();
    makeLandmarks();
    makeDenseBlocks();
    for (const auto& t : map.osmTrees()) osmProps_[tileKey((int)std::floor(t.x), (int)std::floor(t.y))] = 1;
    for (const auto& l : map.osmLamps()) osmProps_[tileKey((int)std::floor(l.x), (int)std::floor(l.y))] = 2;
}

// ---------------------------------------------------------------------------
// Real buildings from OpenStreetMap
// ---------------------------------------------------------------------------

namespace {

BuildingType osmType(const std::string& k, std::string& shop) {
    static const std::pair<const char*, BuildingType> direct[] = {
        {"house", BuildingType::House}, {"terrace", BuildingType::Apartment}, {"apartments", BuildingType::Apartment},
        {"shed", BuildingType::Shed}, {"garages", BuildingType::Shed}, {"barn", BuildingType::Barn},
        {"warehouse", BuildingType::Warehouse}, {"office", BuildingType::Office}, {"grocery", BuildingType::Grocery},
        {"hardware", BuildingType::Hardware}, {"pharmacy", BuildingType::Pharmacy}, {"mall", BuildingType::Mall},
        {"cafe", BuildingType::Restaurant}, {"restaurant", BuildingType::Restaurant}, {"kiosk", BuildingType::Restaurant},
        {"clinic", BuildingType::Hospital}, {"hospital", BuildingType::Hospital}, {"school", BuildingType::School},
        {"townhall", BuildingType::TownHall}, {"police", BuildingType::Police}, {"firestation", BuildingType::FireStation},
        {"church", BuildingType::Church}, {"gasstation", BuildingType::GasStation}, {"station", BuildingType::Station},
        {"sports", BuildingType::Sports}, {"parking_garage", BuildingType::ParkingGarage},
    };
    for (const auto& d : direct)
        if (k == d.first) return d.second;
    // everything else is a speciality shop
    shop = k;
    return BuildingType::Shop;
}

// Shelf contents for each kind of shop
const char* shopLoot(const std::string& shop) {
    if (shop == "clothes") return "clothes_rack";
    if (shop == "shoes") return "shoe_shelf";
    if (shop == "books") return "bookstore_shelf";
    if (shop == "electronics") return "electronics_shelf";
    if (shop == "outdoor") return "outdoor_shelf";
    if (shop == "alcohol") return "alcohol_shelf";
    if (shop == "watches") return "watch_shelf";
    if (shop == "variety") return "variety_shelf";
    return nullptr;
}

} // namespace

void WorldGen::makeOsmBuildings() {
    const auto& src = map_.osmBuildings();
    if (src.empty()) return;
    // Biggest first: small sheds give way to the buildings they were drawn against
    std::vector<int> order(src.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return src[a].w * src[a].h > src[b].w * src[b].h; });

    const int cell = 64;
    int gw = gridW_, gh = gridH_;
    std::vector<std::vector<IRect>> placed((size_t)gw * gh);
    auto cellsOf = [&](const IRect& r, auto fn) {
        for (int gy = std::max(0, r.y / cell); gy <= std::min(gh - 1, r.bottom() / cell); ++gy)
            for (int gx = std::max(0, r.x / cell); gx <= std::min(gw - 1, r.right() / cell); ++gx) fn(placed[(size_t)gy * gw + gx]);
    };
    int made = 0, dropped = 0;
    for (int idx : order) {
        const OsmBuilding& ob = src[idx];
        IRect r{(int)std::lround(ob.centre.x - ob.w * 0.5f), (int)std::lround(ob.centre.y - ob.h * 0.5f), ob.w, ob.h};
        int area0 = r.area();
        // Trim against buildings already placed, keeping as much as possible
        bool ok = true;
        for (int pass = 0; pass < 6 && ok; ++pass) {
            const IRect* hit = nullptr;
            cellsOf(r, [&](std::vector<IRect>& v) {
                for (const auto& o : v)
                    if (!hit && o.intersects(r)) hit = &o;
            });
            if (!hit) break;
            const IRect o = *hit;
            IRect cand[4] = {
                {r.x, r.y, o.x - r.x, r.h},                     // keep the west part
                {o.right(), r.y, r.right() - o.right(), r.h},   // keep the east part
                {r.x, r.y, r.w, o.y - r.y},                     // north
                {r.x, o.bottom(), r.w, r.bottom() - o.bottom()} // south
            };
            int best = -1, bestArea = 0;
            for (int i = 0; i < 4; ++i)
                if (cand[i].w >= 3 && cand[i].h >= 3 && cand[i].area() > bestArea) { bestArea = cand[i].area(); best = i; }
            if (best < 0 || bestArea < area0 * 0.45f) ok = false;
            else r = cand[best];
        }
        if (!ok || r.w < 3 || r.h < 3) { ++dropped; continue; }
        // Snapping can push a building onto a street: drop it
        RoadHit rh = map_.nearestStreet(r.centre().x, r.centre().y);
        if (rh.cls != RoadClass::None && rh.distance < rh.halfWidth && r.area() < 400) { ++dropped; continue; }
        if (map_.waterCoverage(r.centre().x, r.centre().y) > 0.5f) { ++dropped; continue; }

        Building b;
        b.type = osmType(ob.kind, b.shop);
        b.name = ob.name;
        if (b.type == BuildingType::House) {
            Vec2 c = r.centre();
            Zone oz = map_.zoneAt(c.x, c.y);
            if (oz == Zone::Field && r.area() >= 60) b.type = BuildingType::Farmhouse;
            else if ((oz == Zone::Meadow || oz == Zone::Woodland || oz == Zone::Forest) && r.area() < 55) {
                bool water = false;
                for (int k = 0; k < 8 && !water; ++k)
                    if (map_.waterCoverage(c.x + std::cos(k * 0.785f) * 30.0f, c.y + std::sin(k * 0.785f) * 30.0f) > 0.5f) water = true;
                if (water) b.type = BuildingType::SummerCottage;
            }
        }
        if (b.type == BuildingType::House && r.area() < 45) b.type = BuildingType::Cottage;
        if (b.type == BuildingType::Shed && r.area() > 160) b.type = BuildingType::Warehouse;
        if (b.type == BuildingType::Apartment && r.area() < 90) b.type = BuildingType::House;
        b.rect = r;
        b.lot = r.expanded(2);
        b.seed = hash3(idx, 0x05A, 0xB01D, seed_);
        b.osm = true;
        b.levels = ob.levels;
        b.osmColour = ob.colour;
        b.osmRoofColour = ob.roofColour;
        b.osmRoof = ob.roof;
        // The old wooden town along Kungsgatan and around the squares
        {
            static const char* oldStreets[] = {"Kungsgatan", "Drottninggatan", "Nygatan", "Färgaregatan", "Norra Strömgatan",
                                               "Södra Strömgatan", "Bryggaregatan", "Torggatan", "Plangatan", "Stora torget",
                                               "Lilla torget", "Bankgatan", "Östra Kyrkogatan", "Västra Kyrkogatan"};
            std::string street = map_.streetAt(r.centre());
            bool onOld = false;
            for (const char* n : oldStreets) if (street == n) onOld = true;
            bool small = r.area() < 650 && (ob.levels == 0 || ob.levels <= 3);
            bool kind = b.type == BuildingType::House || b.type == BuildingType::Shop || b.type == BuildingType::Restaurant ||
                        b.type == BuildingType::Office || b.type == BuildingType::Apartment || b.type == BuildingType::Grocery ||
                        b.type == BuildingType::Pharmacy;
            b.oldTown = onOld && small && kind && distance(r.centre(), Vec2{4456.4f, 2534.7f}) < 520.0f;
        }
        layout(b);
        cellsOf(r, [&](std::vector<IRect>& v) { v.push_back(r); });
        addBuilding(std::move(b));
        ++made;
    }
    Log::info("OpenStreetMap: %d buildings placed, %d dropped as overlaps", made, dropped);
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
        case BuildingType::SummerCottage: w = rint(h, 6, 8); d = rint(h, 5, 6); break;
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
    // Mapped areas only have their real buildings
    {
        float ax = (float)(lx * kLot), ay = (float)(ly * kLot), m = 30.0f;
        if (map_.osmCovered(ax - m, ay - m) || map_.osmCovered(ax + kLot + m, ay - m) ||
            map_.osmCovered(ax - m, ay + kLot + m) || map_.osmCovered(ax + kLot + m, ay + kLot + m)) return;
    }
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
        case Zone::Field:    type = BuildingType::Farmhouse; chance = roadDist < 60 ? 0.12f : 0.0f; break;
        case Zone::Meadow:   type = BuildingType::Cottage; chance = roadDist < 50 ? 0.06f : 0.0f; break;
        case Zone::Woodland: type = BuildingType::Cottage; chance = roadDist < 40 ? 0.05f : 0.0f; break;
        case Zone::Forest:   type = BuildingType::Cottage; chance = roadDist < 30 ? 0.01f : 0.0f; break;
        default: return;
    }
    // Summer cottages: by the lakes, and here and there in the woods along a gravel road
    if (z == Zone::Meadow || z == Zone::Woodland || z == Zone::Forest) {
        bool water = false;
        for (int k = 0; k < 8 && !water; ++k)
            for (float rr : {35.0f, 70.0f}) {
                float a = k * 0.785f;
                if (map_.waterCoverage(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr) > 0.5f) water = true;
            }
        if (water) { type = BuildingType::SummerCottage; chance = roadDist < 120 ? 0.22f : 0.05f; }
        else if (rnd(h) < 0.2f) { type = BuildingType::SummerCottage; chance = std::max(chance, roadDist < 90 ? 0.03f : 0.0f); }
    }
    if (rnd(h) >= chance) return;

    Building b;
    if (!tryPlace(type, area, h, b)) return;
    layout(b);
    if (type == BuildingType::House || type == BuildingType::Farmhouse || type == BuildingType::Cottage || type == BuildingType::SummerCottage) {
        float r = rnd(h);
        b.lotBorder = r < 0.35f ? 1 : r < 0.6f ? 2 : 0;
    }
    int mainId = addBuilding(b);
    (void)mainId;

    // Outbuildings: sheds by houses and cottages (the woodshed), a barn and a
    // woodshed on farms
    if (type == BuildingType::House || type == BuildingType::Cottage || type == BuildingType::Farmhouse || type == BuildingType::SummerCottage) {
        bool farm = type == BuildingType::Farmhouse;
        std::vector<BuildingType> want;
        if (rnd(h) < (farm ? 0.95f : type == BuildingType::SummerCottage ? 0.7f : 0.45f)) want.push_back(farm ? BuildingType::Barn : BuildingType::Shed);
        if (farm && rnd(h) < 0.7f) want.push_back(BuildingType::Shed);
        std::vector<IRect> taken{buildings_.back().rect};
        for (size_t wi = 0; wi < want.size(); ++wi) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                Building cand;
                IRect sub = area;
                if (!tryPlace(want[wi], sub, hash32(h + attempt * 7919 + (uint32_t)wi * 104729u), cand)) continue;
                bool clash = false;
                for (const auto& t : taken) clash |= cand.rect.expanded(2).intersects(t);
                if (clash) continue;
                cand.isOutbuilding = true;
                cand.lot = IRect{};
                layout(cand);
                taken.push_back(cand.rect);
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
    if (r.right() >= 15000) {
        for (int id : levelIds_)
            if (buildings_[(size_t)id].rect.expanded(3).intersects(r)) ids.insert(id);
        return std::vector<int>(ids.begin(), ids.end());
    }
    for (int gy = std::max(0, floorDiv(r.y, 64)); gy <= std::min(gridH_ - 1, floorDiv(r.bottom(), 64)); ++gy)
        for (int gx = std::max(0, floorDiv(r.x, 64)); gx <= std::min(gridW_ - 1, floorDiv(r.right(), 64)); ++gx)
            for (int id : buildingGrid_[(size_t)gy * gridW_ + gx]) ids.insert(id);
    return std::vector<int>(ids.begin(), ids.end());
}

int WorldGen::levelBuilding(int baseId, int level) {
    if (baseId < 0 || baseId >= (int)buildings_.size() || level == 0) return -1;
    int64_t key = (int64_t)baseId * 4 + (level + 1);
    auto it = levels_.find(key);
    if (it != levels_.end()) return it->second;
    const Building& base = buildings_[(size_t)baseId];
    bool any = false;
    for (const auto& s : base.stairs) any |= (s.dir > 0) == (level > 0);
    if (!any) return -1;
    int off = floorOffset(level);
    Building f;
    f.type = base.type;
    f.shop = base.shop;
    f.rect = IRect{base.rect.x + off, base.rect.y, base.rect.w, base.rect.h};
    f.lot = f.rect;
    f.seed = hash3(baseId, level, 0xF1004, seed_);
    f.levelOf = baseId;
    f.level = level;
    f.osm = base.osm;
    f.levels = 1;
    // the same staircases, the other way
    for (const auto& s : base.stairs)
        if ((s.dir > 0) == (level > 0)) f.stairs.push_back({s.x + off, s.y, -s.dir, s.main});
    layout(f);
    // no way out up here (or down there) but the stairs; no windows underground
    for (auto& e : f.edges) {
        bool outer = e.x == f.rect.x || e.x == f.rect.right() || e.y == f.rect.y || e.y == f.rect.bottom();
        if (outer) e.bits &= (uint8_t)~(DoorN | DoorW);
        if (level < 0) e.bits &= (uint8_t)~(WindowN | WindowW);
    }
    f.hasPath = false;
    f.wallColour = base.wallColour; f.roofColour = base.roofColour; f.trimColour = base.trimColour;
    f.roof = RoofKind::Flat;
    f.floors = 1;
    f.shopFront = false;
    f.sign.clear();
    f.floor = level < 0 ? Ground::FloorConcrete : base.floor;
    f.floorY = base.floorY + (level > 0 ? 3.0f : -3.0f);
    f.doorPos = f.stairs.empty() ? f.rect.centre() : Vec2{f.stairs[0].x + 0.5f, f.stairs[0].y + 0.5f};
    f.id = (int)buildings_.size();
    buildings_.push_back(std::move(f));
    levelIds_.push_back((int)buildings_.size() - 1);
    levels_[key] = (int)buildings_.size() - 1;
    return (int)buildings_.size() - 1;
}

std::vector<std::pair<int, int>> WorldGen::madeLevels() const {
    std::vector<std::pair<int, int>> out;
    for (int id : levelIds_) out.push_back({buildings_[(size_t)id].levelOf, buildings_[(size_t)id].level});
    return out;
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
    auto windowAt = [&](int i, int len) { return b.type != BuildingType::Bunker && b.type != BuildingType::MilitaryContainer && i > 0 && i < len - 1 && (i % 3 == 1) && rnd(h) < 0.7f; };
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
        case BuildingType::Manor: case BuildingType::SummerCottage:
            layoutHome(b, h);   // places its own front door
            break;
        case BuildingType::Apartment: case BuildingType::Office: case BuildingType::School:
        case BuildingType::Hospital: case BuildingType::Hostel: case BuildingType::Police:
        case BuildingType::TownHall: case BuildingType::FireStation: case BuildingType::Bunker:
            layoutCorridors(b, h);
            break;
        case BuildingType::Mall:
            layoutMall(b, h);
            break;
        case BuildingType::ParkingGarage: {
            // Open decks on pillars: no walls, a car here and there
            b.edges.clear();
            b.rooms.push_back({r, RoomType::Storage});
            break;
        }
        case BuildingType::Restaurant: {
            // Dining room at the front, kitchen behind
            bool alongX = r.w >= r.h;
            int depth = alongX ? r.h : r.w;
            int back = std::max(3, depth / 3);
            IRect dine = alongX ? IRect{r.x, r.y + back, r.w, r.h - back} : IRect{r.x + back, r.y, r.w - back, r.h};
            IRect kitchen = alongX ? IRect{r.x, r.y, r.w, back} : IRect{r.x, r.y, back, r.h};
            b.rooms.push_back({dine, RoomType::Dining});
            b.rooms.push_back({kitchen, RoomType::Kitchen});
            int len = alongX ? r.w : r.h;
            int door = rint(h, 1, len - 2);
            for (int i = 0; i < len; ++i) {
                if (alongX) b.edges.push_back({r.x + i, r.y + back, (uint8_t)(WallN | (i == door ? DoorN : 0))});
                else b.edges.push_back({r.x + back, r.y + i, (uint8_t)(WallW | (i == door ? DoorW : 0))});
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
    if (!isHomeType(b.type)) addExteriorDoor(b, h);
    // how many storeys it has first: the stairs depend on it
    styleBuilding(b, h);
    b.floorY = floorHeight(b.rect);
    if (b.levels > 0) b.floors = std::clamp(b.levels, 1, 8);
    else if (b.osm && b.type == BuildingType::Apartment && r.area() > 300) b.floors = std::max(b.floors, 3);
    dressBuilding(b, h);
    furnish(b, h);
    // A pitched roof over a wide building would be taller than the house: hip or flatten it
    float span = (float)std::min(r.w, r.h);
    bool keepsPitch = b.type == BuildingType::Church || b.type == BuildingType::Station || b.type == BuildingType::Barn;
    if (b.roof == RoofKind::Gable && span > 13 && !keepsPitch) b.roof = (span > 20 || !(b.type == BuildingType::House || b.type == BuildingType::Farmhouse || b.type == BuildingType::Barn)) ? RoofKind::Flat : RoofKind::Hip;
    if (b.roof == RoofKind::Hip && span > 26) b.roof = RoofKind::Flat;
}

// The look of a real building: mapped colours, the wooden old town, shop
// fronts with awnings, and the two shopping centres.
void WorldGen::dressBuilding(Building& b, uint32_t& h) {
    const IRect& r = b.rect;
    std::string low = b.name;
    for (auto& ch : low) ch = (char)std::tolower((unsigned char)ch);
    bool vimpeln = low.find("vimpeln") != std::string::npos;
    bool storken = low.find("storken") != std::string::npos;
    if (b.oldTown) {
        // Pastel wooden houses with clay tile roofs, white trim
        static const std::vector<uint32_t> walls = {0xe9d48e, 0xf1e6c8, 0xc9cfc0, 0xa9b49e, 0xefe2b0, 0xd8c6a8, 0xf2efe6, 0xc8bfa8};
        static const std::vector<uint32_t> roofs = {0xc4562e, 0xb84a2a, 0xa83a28, 0xc86a3a, 0x8e3a2a};
        b.wallColour = pick(h, walls);
        b.roofColour = pick(h, roofs);
        b.trimColour = 0xf4f2ea;
        b.floors = std::max(2, std::min(b.floors, 3));
        b.roof = r.w * r.h > 260 ? RoofKind::Hip : RoofKind::Gable;
        b.roofAlongX = r.w >= r.h;
        if (b.type != BuildingType::House && b.type != BuildingType::Apartment) b.shopFront = true;
        else if (rnd(h) < 0.45f) b.shopFront = true;   // a shop on the ground floor, flats above
    }
    if (b.osmColour) b.wallColour = b.osmColour & 0xffffff;
    if (b.osmRoofColour) b.roofColour = b.osmRoofColour & 0xffffff;
    if (b.osmRoof == 0 && std::min(r.w, r.h) <= 16) b.roof = RoofKind::Gable;
    else if (b.osmRoof == 1) b.roof = RoofKind::Flat;
    else if (b.osmRoof == 2) b.roof = RoofKind::Hip;
    if (b.type == BuildingType::Mall) {
        b.roof = RoofKind::Flat;
        b.floors = std::max(b.floors, 2);
        b.floor = Ground::FloorTile;
        if (vimpeln) { b.wallColour = 0xbcc0c2; b.roofColour = 0xd6d6d2; b.trimColour = 0x3a3c3e; b.sign = "Vimpeln"; }
        if (storken) { b.wallColour = 0xc2654a; b.roofColour = 0x5a5a5c; b.trimColour = 0xf0e6d6; b.sign = "Storken"; b.shopFront = true; }
    }
    if (b.type == BuildingType::ParkingGarage) {
        b.wallColour = 0xb8b8b2; b.roofColour = 0x8a8a86; b.roof = RoofKind::Flat; b.floor = Ground::FloorConcrete;
        b.floors = b.levels > 0 ? std::clamp(b.levels, 2, 5) : 3;
    }
    if (b.sign.empty()) {
        switch (b.type) {
            case BuildingType::Grocery: b.sign = "Livs"; break;
            case BuildingType::Pharmacy: b.sign = "Apotek"; break;
            case BuildingType::Hardware: b.sign = "Järnhandel"; break;
            case BuildingType::GasStation: b.sign = "Bensin"; break;
            case BuildingType::Police: b.sign = "Polis"; break;
            case BuildingType::FireStation: b.sign = "Räddningstjänst"; break;
            case BuildingType::Bunker: b.sign = "Skyddsrum"; break;
            case BuildingType::MilitaryContainer: b.sign = "FÖRSVARSMAKTEN"; break;
            case BuildingType::Restaurant: b.sign = b.shop == "cafe" ? "Kafé" : b.shop == "kiosk" ? "Kiosk" : "Restaurang"; break;
            case BuildingType::Shop: {
                const std::string& k = b.shop;
                b.sign = k == "clothes" ? "Kläder" : k == "books" ? "Bokhandel" : k == "electronics" ? "Elektronik"
                       : k == "outdoor" ? "Sport & Fritid" : k == "alcohol" ? "Vin & Sprit" : k == "variety" ? "Presenter" : "";
                break;
            }
            default: break;
        }
    }
    if (b.type == BuildingType::Grocery || b.type == BuildingType::Pharmacy || b.type == BuildingType::Restaurant ||
        b.type == BuildingType::Shop || b.type == BuildingType::Hardware)
        b.shopFront = b.shopFront || r.area() < 900;
}

// Corridors down the long axis with rooms either side. Deep buildings get
// several parallel corridors so rooms stay a sensible size.
void WorldGen::layoutCorridors(Building& b, uint32_t& h) {
    const IRect& r = b.rect;
    bool alongX = r.w >= r.h;
    int len = alongX ? r.w : r.h, depth = alongX ? r.h : r.w;
    int strips = depth > 18 ? std::max(1, (int)std::lround(depth / 14.0f)) : 1;
    int cw = 2;
    auto rect = [&](int a, int d, int al, int dl) { return alongX ? IRect{r.x + a, r.y + d, al, dl} : IRect{r.x + d, r.y + a, dl, al}; };
    // wall running along the length at depth d
    auto lenWall = [&](int a, int d, uint8_t extra) {
        if (alongX) b.edges.push_back({r.x + a, r.y + d, (uint8_t)(WallN | (extra ? DoorN : 0))});
        else b.edges.push_back({r.x + d, r.y + a, (uint8_t)(WallW | (extra ? DoorW : 0))});
    };
    // wall across at length position a
    auto crossWall = [&](int a, int d) {
        if (alongX) b.edges.push_back({r.x + a, r.y + d, WallW});
        else b.edges.push_back({r.x + d, r.y + a, WallN});
    };
    for (int s = 0; s < strips; ++s) {
        int s0 = depth * s / strips, s1 = depth * (s + 1) / strips;
        int sd = s1 - s0;
        if (s > 0)
            for (int a = 0; a < len; ++a) lenWall(a, s0, (a == len / 2 || a == len / 4) ? 1 : 0);
        int c0 = s0 + (sd - cw) / 2;
        b.rooms.push_back({rect(0, c0, len, cw), RoomType::Hall});
        for (int side = 0; side < 2; ++side) {
            int d0 = side == 0 ? s0 : c0 + cw;
            int dl = side == 0 ? c0 - s0 : s1 - c0 - cw;
            if (dl < 2) continue;
            int pos = 0;
            while (pos < len) {
                int rl = std::min(len - pos, rint(h, 4, 6));
                if (len - pos - rl < 3) rl = len - pos;
                b.rooms.push_back({rect(pos, d0, rl, dl), RoomType::Office});
                if (pos + rl < len)
                    for (int i = 0; i < dl; ++i) crossWall(pos + rl, d0 + i);
                int door = rint(h, 1, std::max(1, rl - 2));
                int wallD = side == 0 ? c0 : c0 + cw;
                for (int i = 0; i < rl; ++i) lenWall(pos + i, wallD, i == door ? 1 : 0);
                pos += rl;
            }
        }
    }
}

// A shopping centre: glass-roofed walkways with shop units either side, a
// food court, and doors at the ends of every walkway.
void WorldGen::layoutMall(Building& b, uint32_t& h) {
    const IRect& r = b.rect;
    for (auto& e : b.edges) e.bits &= (uint8_t)~(WindowN | WindowW);   // no house windows on a shopping centre
    bool alongX = r.w >= r.h;
    int len = alongX ? r.w : r.h, depth = alongX ? r.h : r.w;
    int strips = std::max(1, (int)std::lround(depth / 32.0f));
    const int ww = 4;   // walkway width
    auto rect = [&](int a, int d, int al, int dl) { return alongX ? IRect{r.x + a, r.y + d, al, dl} : IRect{r.x + d, r.y + a, dl, al}; };
    auto lenWall = [&](int a, int d) {
        if (alongX) b.edges.push_back({r.x + a, r.y + d, WallN});
        else b.edges.push_back({r.x + d, r.y + a, WallW});
    };
    auto crossWall = [&](int a, int d) {
        if (alongX) b.edges.push_back({r.x + a, r.y + d, WallW});
        else b.edges.push_back({r.x + d, r.y + a, WallN});
    };
    auto exteriorDoor = [&](int a, int d) {   // a = 0 or len (end walls)
        int tx = alongX ? r.x + a : r.x + d, ty = alongX ? r.y + d : r.y + a;
        uint8_t bits = alongX ? (WallW | DoorW) : (WallN | DoorN);
        for (auto& e : b.edges)
            if (e.x == tx && e.y == ty && (e.bits & (bits & (WallN | WallW)))) e.bits = bits;
    };
    struct Unit { const char* label; RoomType type; const char* loot; };
    std::string lowName = b.name;
    for (auto& ch : lowName) ch = (char)std::tolower((unsigned char)ch);
    // Vimpeln: the big centre by Gerdsken. Electronics, home goods, a pharmacy,
    // a tool and hardware store, clothes, a supermarket and a food court.
    static const Unit vimpelnUnits[] = {
        {"Stormarknad", RoomType::ShopFloor, "grocery_shelf"},   {"Elektronik", RoomType::ShopFloor, "electronics_shelf"},
        {"Hem & Inredning", RoomType::ShopFloor, "home_shelf"},  {"Apotek", RoomType::ShopFloor, "pharmacy_shelf"},
        {"Järn & Hem", RoomType::ShopFloor, "tool_store_shelf"}, {"Kläder", RoomType::ShopFloor, "clothes_rack"},
        {"Sport & Friluft", RoomType::ShopFloor, "outdoor_shelf"}, {"Skor", RoomType::ShopFloor, "shoe_shelf"},
        {"Kläder", RoomType::ShopFloor, "clothes_rack"},         {"Kafé", RoomType::Dining, "cafe_counter"},
        {"Leksaker", RoomType::ShopFloor, "toy_shelf"},          {"Ur & Optik", RoomType::ShopFloor, "watch_shelf"},
        {"Djurbutik", RoomType::ShopFloor, "pet_shelf"},         {"Godis", RoomType::ShopFloor, "candy_shelf"},
    };
    // Storken: the small gallery on Kungsgatan
    static const Unit storkenUnits[] = {
        {"Kafé", RoomType::Dining, "cafe_counter"},       {"Kläder", RoomType::ShopFloor, "clothes_rack"},
        {"Bokhandel", RoomType::ShopFloor, "bookstore_shelf"}, {"Presenter", RoomType::ShopFloor, "variety_shelf"},
        {"Ur & Optik", RoomType::ShopFloor, "watch_shelf"}, {"Godis", RoomType::ShopFloor, "candy_shelf"},
        {"Skor", RoomType::ShopFloor, "shoe_shelf"},      {"Apotek", RoomType::ShopFloor, "pharmacy_shelf"},
    };
    static const Unit units[] = {
        {"Livs", RoomType::ShopFloor, "grocery_shelf"},        {"Kläder", RoomType::ShopFloor, "clothes_rack"},
        {"Apotek", RoomType::ShopFloor, "pharmacy_shelf"},     {"Ur & Optik", RoomType::ShopFloor, "watch_shelf"},
        {"Skor", RoomType::ShopFloor, "shoe_shelf"},           {"Elektronik", RoomType::ShopFloor, "electronics_shelf"},
        {"Kläder", RoomType::ShopFloor, "clothes_rack"},       {"Bokhandel", RoomType::ShopFloor, "bookstore_shelf"},
        {"Sport & Fritid", RoomType::ShopFloor, "outdoor_shelf"}, {"Järnhandel", RoomType::ShopFloor, "hardware_shelf"},
        {"Kafé", RoomType::Dining, "cafe_counter"},            {"Systembolaget", RoomType::ShopFloor, "alcohol_shelf"},
        {"Kläder", RoomType::ShopFloor, "clothes_rack"},       {"Presenter", RoomType::ShopFloor, "variety_shelf"},
    };
    const Unit* list = units;
    int nUnits = (int)(sizeof(units) / sizeof(units[0]));
    bool bigCentre = lowName.find("vimpeln") != std::string::npos;
    if (bigCentre) { list = vimpelnUnits; nUnits = (int)(sizeof(vimpelnUnits) / sizeof(vimpelnUnits[0])); }
    else if (lowName.find("storken") != std::string::npos) { list = storkenUnits; nUnits = (int)(sizeof(storkenUnits) / sizeof(storkenUnits[0])); }
    int next = bigCentre ? 0 : rint(h, 0, nUnits - 1);
    bool foodCourt = lowName.find("storken") != std::string::npos;   // the small gallery has a café, not a food court
    for (int s = 0; s < strips; ++s) {
        int s0 = depth * s / strips, s1 = depth * (s + 1) / strips;
        int sd = s1 - s0;
        if (s > 0)
            for (int a = 0; a < len; ++a) lenWall(a, s0);
        int c0 = s0 + (sd - ww) / 2;
        Room walk{rect(0, c0, len, ww), RoomType::Hall};
        walk.label = "Gallerian";
        b.rooms.push_back(walk);
        for (int side = 0; side < 2; ++side) {
            int d0 = side == 0 ? s0 : c0 + ww;
            int dl = side == 0 ? c0 - s0 : s1 - c0 - ww;
            if (dl < 3) continue;
            int pos = 0;
            while (pos < len) {
                bool court = !foodCourt && side == 0 && pos > len / 3 && len - pos > 18;
                // the supermarket is the big unit at the end of a big centre
                bool anchor = bigCentre && next == 0 && !court;
                int rl = court ? std::min(18, len - pos) : anchor ? std::min(len - pos, 22) : std::min(len - pos, rint(h, 7, 12));
                if (len - pos - rl < 5) rl = len - pos;
                Room room{rect(pos, d0, rl, dl), RoomType::ShopFloor};
                if (court) {
                    room.type = RoomType::Dining;
                    room.loot = "food_court_counter";
                    room.label = "Food court";
                    foodCourt = true;
                } else {
                    const Unit& u = list[next++ % nUnits];
                    room.type = u.type;
                    room.loot = u.loot;
                    room.label = u.label;
                }
                b.rooms.push_back(room);
                if (pos + rl < len)
                    for (int i = 0; i < dl; ++i) crossWall(pos + rl, d0 + i);
                // Shop front: wall with a wide open entrance (the food court is open)
                int wallD = side == 0 ? c0 : c0 + ww;
                int open0 = rl / 2 - 1, open1 = rl / 2 + 1;
                if (!court)
                    for (int i = 0; i < rl; ++i)
                        if (i < open0 || i > open1) lenWall(pos + i, wallD);
                pos += rl;
            }
        }
        // Entrances at both ends of the walkway
        for (int k = 1; k < ww - 1; ++k) {
            exteriorDoor(0, c0 + k);
            exteriorDoor(len, c0 + k);
        }
    }
}

// Homes: the front door opens into a hall; the hall leads to a corridor
// across the house; every room opens off the hall or the corridor. Living
// room and kitchen at the front, bedrooms and the bathroom at the back.
// Small cottages: an open kitchen-living room with the bedroom and bathroom
// off it.
void WorldGen::layoutHome(Building& b, uint32_t& h) {
    const IRect& r = b.rect;
    int side = roadSide(b);                 // the front: 0 N, 1 E, 2 S, 3 W
    bool frontAlongX = side == 0 || side == 2;
    int L = frontAlongX ? r.w : r.h;        // along the front
    int D = frontAlongX ? r.h : r.w;        // from the front inwards
    // Local tiles (u along the front, v inwards from it) to world tiles
    auto tileAt = [&](int u, int v) -> std::pair<int, int> {
        switch (side) {
            case 0: return {r.x + u, r.y + v};
            case 2: return {r.x + u, r.bottom() - 1 - v};
            case 3: return {r.x + v, r.y + u};
            default: return {r.right() - 1 - v, r.y + u};
        }
    };
    auto room = [&](int u0, int v0, int du, int dv, RoomType t) {
        auto [ax, ay] = tileAt(u0, v0);
        auto [bx, by] = tileAt(u0 + du - 1, v0 + dv - 1);
        b.rooms.push_back({IRect{std::min(ax, bx), std::min(ay, by), std::abs(bx - ax) + 1, std::abs(by - ay) + 1}, t});
    };
    // A wall across the house between rows v = k-1 and k, for u in [u0, u1)
    auto across = [&](int k, int u0, int u1, const std::vector<int>& doors = {}) {
        for (int u = u0; u < u1; ++u) {
            bool d = std::find(doors.begin(), doors.end(), u) != doors.end();
            switch (side) {
                case 0: b.edges.push_back({r.x + u, r.y + k, (uint8_t)(WallN | (d ? DoorN : 0))}); break;
                case 2: b.edges.push_back({r.x + u, r.bottom() - k, (uint8_t)(WallN | (d ? DoorN : 0))}); break;
                case 3: b.edges.push_back({r.x + k, r.y + u, (uint8_t)(WallW | (d ? DoorW : 0))}); break;
                default: b.edges.push_back({r.right() - k, r.y + u, (uint8_t)(WallW | (d ? DoorW : 0))}); break;
            }
        }
    };
    // A wall running inwards between columns u = k-1 and k, for v in [v0, v1)
    auto along = [&](int k, int v0, int v1, int door = -1) {
        for (int v = v0; v < v1; ++v) {
            bool d = v == door;
            switch (side) {
                case 0: b.edges.push_back({r.x + k, r.y + v, (uint8_t)(WallW | (d ? DoorW : 0))}); break;
                case 2: b.edges.push_back({r.x + k, r.bottom() - 1 - v, (uint8_t)(WallW | (d ? DoorW : 0))}); break;
                case 3: b.edges.push_back({r.x + v, r.y + k, (uint8_t)(WallN | (d ? DoorN : 0))}); break;
                default: b.edges.push_back({r.right() - 1 - v, r.y + k, (uint8_t)(WallN | (d ? DoorN : 0))}); break;
            }
        }
    };

    if (L < 6 || D < 5) {   // a single room
        b.rooms.push_back({r, RoomType::Living});
        addExteriorDoor(b, h, side, L / 2);
        return;
    }
    if (L < 8 || D < 8) {
        // Cottage: living room (and a small kitchen) at the front, bedroom
        // and bathroom behind, both opening off the living room
        int db = 3, df = D - db;
        int bw = L >= 7 ? 3 : 2;            // bathroom width
        bool bathLeft = rnd(h) < 0.5f;
        int bathU = bathLeft ? 0 : L - bw, bedU = bathLeft ? bw : 0, bedW = L - bw;
        int lu0 = 0, lu1 = L;               // the living room's span along the front
        if (L >= 7 && df >= 3) {
            int kw = 3;
            bool kitchenLeft = rnd(h) < 0.5f;
            int ku = kitchenLeft ? 0 : L - kw;
            lu0 = kitchenLeft ? kw : 0;
            lu1 = kitchenLeft ? L : L - kw;
            along(kitchenLeft ? kw : L - kw, 0, df, df / 2);
            room(ku, 0, kw, df, RoomType::Kitchen);
        }
        room(lu0, 0, lu1 - lu0, df, RoomType::Living);
        // doors from the living room into the back rooms
        auto doorInto = [&](int u0, int w) {
            int a = std::max(u0, lu0), c = std::min(u0 + w, lu1);
            return a < c ? (a + c - 1) / 2 : u0 + w / 2;
        };
        across(df, 0, L, {doorInto(bathU, bw), doorInto(bedU, bedW)});
        along(bathLeft ? bw : L - bw, df, D);
        room(bathU, df, bw, db, RoomType::Bathroom);
        room(bedU, df, bedW, db, RoomType::Bedroom);
        addExteriorDoor(b, h, side, (lu0 + lu1) / 2);
        return;
    }

    // Front rooms | corridor (2 wide) | back rooms
    const int cw = 2;
    int df = (D - cw + 1) / 2, db = D - cw - df;
    if (db < 3) { db = 3; df = D - cw - db; }
    int vc = df;                            // the corridor is rows vc and vc+1
    // The entrance hall, 2 wide, runs from the front door to the corridor;
    // the living room takes the bigger side of it
    bool livingLeft = rnd(h) < 0.5f;
    int hu = (int)std::lround((L - 2) * (livingLeft ? 0.6f : 0.4f));
    hu = std::clamp(hu, 3, L - 5);
    int wl = hu, wr = L - hu - 2;
    along(hu, 0, vc, vc / 2);               // hall | left room
    along(hu + 2, 0, vc, vc / 2);           // hall | right room
    room(hu, 0, 2, vc, RoomType::Hall);
    room(0, vc, L, cw, RoomType::Hall);
    int livingU = livingLeft ? 0 : hu + 2, livingW = livingLeft ? wl : wr;
    int otherU = livingLeft ? hu + 2 : 0, otherW = livingLeft ? wr : wl;
    // A wide living room also opens onto the corridor
    int livingDoor = livingW >= 5 ? livingU + livingW / 2 : -1;
    // The other front room: the kitchen, plus a study in big houses
    if (otherW >= 8 && (b.type == BuildingType::Manor || b.type == BuildingType::Farmhouse || rnd(h) < 0.5f)) {
        int kw = otherW / 2;
        int ku = livingLeft ? otherU : otherU + otherW - kw;   // the kitchen next to the hall
        int su = livingLeft ? otherU + kw : otherU;
        along(livingLeft ? otherU + kw : otherU + otherW - kw, 0, vc);
        room(ku, 0, kw, vc, RoomType::Kitchen);
        room(su, 0, otherW - kw, vc, b.type == BuildingType::Manor ? RoomType::Office : RoomType::Bedroom);
        int studyDoor = su + (otherW - kw) / 2;
        if (livingLeft) { across(vc, 0, hu, {livingDoor}); across(vc, hu + 2, L, {studyDoor}); }
        else { across(vc, 0, hu, {studyDoor}); across(vc, hu + 2, L, {livingDoor}); }
    } else {
        room(otherU, 0, otherW, vc, RoomType::Kitchen);
        if (livingLeft) { across(vc, 0, hu, {livingDoor}); across(vc, hu + 2, L); }
        else { across(vc, 0, hu); across(vc, hu + 2, L, {livingDoor}); }
    }
    room(livingU, 0, livingW, vc, RoomType::Living);

    // Back rooms side by side, each with a door onto the corridor: a bathroom
    // and bedrooms (and a storeroom in long houses)
    int vb = vc + cw;
    int bathW = L >= 10 ? 3 : 2;
    int rest = L - bathW;
    bool store = rest >= 13;
    if (store) rest -= 2;
    int beds = std::max(1, rest / 4);
    std::vector<std::pair<int, RoomType>> backs;   // widths and types, front to back along u
    for (int i = 0; i < beds; ++i) backs.push_back({rest / beds + (i < rest % beds ? 1 : 0), RoomType::Bedroom});
    int bathAt = rint(h, 0, (int)backs.size());
    backs.insert(backs.begin() + bathAt, {bathW, RoomType::Bathroom});
    if (store) backs.insert(backs.begin() + (bathAt == 0 ? (int)backs.size() : 0), {2, RoomType::Storage});
    int u = 0;
    std::vector<int> doors;
    for (size_t i = 0; i < backs.size(); ++i) {
        auto [w, t] = backs[i];
        room(u, vb, w, db, t);
        doors.push_back(u + w / 2);
        if (i + 1 < backs.size()) along(u + w, vb, D);
        u += w;
    }
    across(vb, 0, L, doors);
    addExteriorDoor(b, h, side, hu + (rnd(h) < 0.5f ? 0 : 1));
}

void WorldGen::assignRooms(Building& b, uint32_t& h) {
    auto& rooms = b.rooms;
    switch (b.type) {
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
        case BuildingType::Bunker: {
            // bunk rooms, the store, a kitchen and the radio room
            int i = 0;
            static const RoomType kinds[] = {RoomType::Storage, RoomType::Bedroom, RoomType::Office, RoomType::Kitchen, RoomType::Bedroom, RoomType::Bathroom};
            for (auto& r : rooms)
                if (r.type == RoomType::Office) r.type = kinds[i++ % 6];
            break;
        }
        default: break;
    }
}

// The ground floor is level. A small building stands at the height of its
// highest corner (a plinth shows on the downhill side); a big one is dug a
// little into the slope.
float WorldGen::floorHeight(const IRect& r) const {
    if (!map_.hasElevation()) return 0.0f;
    float top = -1e9f, sum = 0;
    for (int j = 0; j <= 2; ++j)
        for (int i = 0; i <= 2; ++i) {
            float hgt = map_.elevation(r.x + r.w * 0.5f * i, r.y + r.h * 0.5f * j);
            top = std::max(top, hgt);
            sum += hgt;
        }
    float mean = sum / 9.0f;
    float f = r.area() <= 400 ? top - 0.1f : std::max(mean, top - 1.2f);
    return f + 0.12f;
}

// Which side of the building faces the nearest road (0 N, 1 E, 2 S, 3 W)
int WorldGen::roadSide(const Building& b) const {
    const IRect& r = b.rect;
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
    return best;
}

void WorldGen::addExteriorDoor(Building& b, uint32_t& h, int presetSide, int presetIndex) {
    const IRect& r = b.rect;
    int best = presetSide >= 0 ? presetSide : roadSide(b);
    // Prefer a door into a living space
    auto goodRoom = [](RoomType t) {
        return t == RoomType::Living || t == RoomType::Hall || t == RoomType::Lobby || t == RoomType::ShopFloor ||
               t == RoomType::Kitchen || t == RoomType::Nave || t == RoomType::Locker || t == RoomType::Storage ||
               t == RoomType::Dining;
    };
    std::vector<int> candidates;
    int len = (best == 0 || best == 2) ? r.w : r.h;
    if (presetIndex >= 0) candidates.push_back(std::clamp(presetIndex, 0, len - 1));
    for (int i = 1; i < len - 1 && presetIndex < 0; ++i) {
        int tx = (best == 0 || best == 2) ? r.x + i : (best == 1 ? r.right() - 1 : r.x);
        int ty = (best == 1 || best == 3) ? r.y + i : (best == 2 ? r.bottom() - 1 : r.y);
        const Room* room = b.roomAt(tx, ty);
        if (room && goodRoom(room->type)) candidates.push_back(i);
    }
    if (candidates.empty())
        for (int i = 1; i < len - 1; ++i) candidates.push_back(i);
    int i = candidates[rint(h, 0, (int)candidates.size() - 1)];
    if (candidates.size() > 2) i = candidates[candidates.size() / 2 + rint(h, -1, 1)];
    if (presetIndex >= 0) i = candidates[0];

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

    // Big buildings get a back door too (homes: their layout has none)
    if (r.w * r.h > 150 && !isHomeType(b.type)) {
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
    // Rows of shelves with a cross aisle every 10 tiles
    auto rows = [&](const Room& room, Prop p, const std::string& container, int spacing) {
        const IRect& r = room.rect;
        bool alongX = r.w >= r.h;
        if (alongX) {
            for (int y = r.y + 2; y < r.bottom() - 2; y += spacing)
                for (int x = r.x + 2; x < r.right() - 2; ++x)
                    if ((x - r.x) % 10 != 6) place(x, y, p, 2, container);
        } else {
            for (int x = r.x + 2; x < r.right() - 2; x += spacing)
                for (int y = r.y + 2; y < r.bottom() - 2; ++y)
                    if ((y - r.y) % 10 != 6) place(x, y, p, 1, container);
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
            if (const char* l = shopLoot(b.shop)) { shelfLoot = l; break; }
            static const std::vector<std::string> shops = {"gas_shelf", "wardrobe", "bookshelf", "hardware_shelf",
                                                           "grocery_shelf", "pharmacy_shelf", "dresser"};
            shelfLoot = pick(h, shops);
            break;
        }
        default: break;
    }
    // --- Stairs ------------------------------------------------------------------
    // Upstairs and in the basement the rooms are used differently
    if (b.levelOf >= 0) {
        bool firstBath = true;
        for (auto& room : b.rooms) {
            if (b.level > 0 && isHomeType(b.type) && room.type == RoomType::Kitchen) room.type = RoomType::Bedroom;
            if (b.level < 0) {
                if (room.type == RoomType::Bathroom && firstBath) { firstBath = false; continue; }   // the laundry
                if (room.type != RoomType::Hall) room.type = RoomType::Storage;
            }
        }
        // the staircases that lead here, exactly above (below) the ones downstairs
        for (const auto& st : b.stairs) {
            blocked.erase(tileKey(st.x, st.y));
            place(st.x, st.y, st.dir > 0 ? Prop::StairsUp : Prop::StairsDown, 2);
        }
    } else if (!b.isOutbuilding && b.stairs.empty()) {
        bool home = b.type == BuildingType::House || b.type == BuildingType::Farmhouse || b.type == BuildingType::Manor;
        bool corridor = b.type == BuildingType::Apartment || b.type == BuildingType::Office || b.type == BuildingType::School ||
                        b.type == BuildingType::Hospital || b.type == BuildingType::Hostel || b.type == BuildingType::TownHall ||
                        b.type == BuildingType::Police || b.type == BuildingType::FireStation;
        bool upper = b.floors >= 2 && (home || corridor);
        float basementChance = b.type == BuildingType::House ? 0.45f : b.type == BuildingType::Farmhouse ? 0.55f
                             : b.type == BuildingType::Manor || b.type == BuildingType::Apartment || b.type == BuildingType::Hospital ||
                               b.type == BuildingType::TownHall || b.type == BuildingType::Police ? 1.0f
                             : corridor ? 0.5f : b.type == BuildingType::Church ? 0.6f : 0.0f;
        bool basement = rnd(h) < basementChance;
        // where stairs can go: along the walls of the hall (or the biggest living space)
        const Room* hall = nullptr;
        for (const auto& room : b.rooms)
            if (room.type == RoomType::Hall || room.type == RoomType::Lobby) { if (!hall || room.rect.area() > hall->rect.area()) hall = &room; }
        if (!hall)
            for (const auto& room : b.rooms)
                if (room.type == RoomType::Living || room.type == RoomType::Nave || room.type == RoomType::Kitchen) { hall = &room; break; }
        if (!hall && !b.rooms.empty()) hall = &b.rooms[0];
        auto spots = [&](const Room& room) {
            std::vector<std::pair<std::pair<int, int>, uint8_t>> v;
            const IRect& r = room.rect;
            for (int x = r.x; x < r.right(); ++x) { v.push_back({{x, r.y}, 2}); v.push_back({{x, r.bottom() - 1}, 0}); }
            for (int y = r.y + 1; y < r.bottom() - 1; ++y) { v.push_back({{r.x, y}, 1}); v.push_back({{r.right() - 1, y}, 3}); }
            return v;
        };
        // the free spot nearest to `want`
        auto stairNear = [&](Vec2 want, Prop p, int dir, bool main, float awayFrom = -1, Vec2 other = {}) {
            if (!hall) return false;
            auto v = spots(*hall);
            std::sort(v.begin(), v.end(), [&](const auto& a, const auto& c) {
                return distance(Vec2{a.first.first + 0.5f, a.first.second + 0.5f}, want) < distance(Vec2{c.first.first + 0.5f, c.first.second + 0.5f}, want);
            });
            for (const auto& s : v) {
                Vec2 at{s.first.first + 0.5f, s.first.second + 0.5f};
                if (awayFrom > 0 && distance(at, other) < awayFrom) continue;
                if (place(s.first.first, s.first.second, p, s.second)) {
                    b.stairs.push_back({s.first.first, s.first.second, dir, main});
                    return true;
                }
            }
            return false;
        };
        if (hall) {
            const IRect& hr = hall->rect;
            bool alongX = hr.w >= hr.h;
            Vec2 centre = hr.centre();
            Vec2 endA = alongX ? Vec2{hr.x + 2.5f, centre.y} : Vec2{centre.x, hr.y + 2.5f};
            Vec2 endB = alongX ? Vec2{hr.right() - 2.5f, centre.y} : Vec2{centre.x, hr.bottom() - 2.5f};
            bool big = corridor && b.rect.area() >= 250 && (alongX ? hr.w : hr.h) >= 18;
            if (upper) {
                if (big) {
                    // the main staircase in the middle, smaller stairs at the ends (now and then one has given way)
                    stairNear(centre, Prop::StairsUp, 1, true);
                    for (Vec2 e : {endA, endB}) stairNear(e, rnd(h) < 0.2f ? Prop::BrokenStairs : Prop::StairsUp, 1, false, 6.0f, centre);
                } else {
                    stairNear(rnd(h) < 0.5f ? endA : centre, home && rnd(h) < 0.15f ? Prop::BrokenStairs : Prop::StairsUp, 1, !home);
                }
            }
            if (basement) {
                Vec2 first = b.stairs.empty() ? centre : Vec2{b.stairs[0].x + 0.5f, b.stairs[0].y + 0.5f};
                stairNear(big ? endB : endA, Prop::StairsDown, -1, false, 2.5f, first);
            }
        }
    }

    bool hunterHome = rnd(h) < 0.18f;   // lots of Swedes hunt

    for (const auto& room : b.rooms) {
        switch (room.type) {
            case RoomType::Kitchen:
                alongWalls(room, {{Prop::Fridge, ""}, {Prop::Stove, ""}, {Prop::Counter, ""}, {Prop::Counter, ""},
                                  {Prop::Sink, ""}, {Prop::Counter, b.type == BuildingType::SummerCottage ? "cottage_cupboard" : "pantry"}});
                if (b.type == BuildingType::Farmhouse) alongWalls(room, {{Prop::Shelf, "farm_pantry"}});   // the farm's larder
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
                if (b.type == BuildingType::SummerCottage)   // the cupboard everyone leaves things in over the winter
                    alongWalls(room, {{Prop::Shelf, "cottage_cupboard"}, {Prop::Stove, ""}});
                break;
            case RoomType::Bedroom:
                if (b.type == BuildingType::Bunker) {   // bunks and a locker each
                    alongWalls(room, {{Prop::Bed, ""}, {Prop::Bed, ""}, {Prop::Locker, "bunker_locker"}});
                    break;
                }
                alongWalls(room, {{Prop::Bed, ""}, {Prop::Wardrobe, ""}, {Prop::Dresser, ""}});
                if (hunterHome && b.type != BuildingType::Hostel) { alongWalls(room, {{Prop::GunCabinet, ""}}); hunterHome = false; }
                break;
            case RoomType::Bathroom:
                alongWalls(room, {{Prop::Toilet, ""}, {Prop::Sink, ""}, {Prop::BathroomCabinet, ""}, {Prop::Bathtub, ""}});
                if (rnd(h) < 0.6f) alongWalls(room, {{Prop::Bin, "bathroom_bin"}});
                break;
            case RoomType::Storage:
                if (b.level < 0) {   // basement storerooms
                    alongWalls(room, {{Prop::Shelf, "basement_shelf"}, {Prop::Shelf, "basement_shelf"}, {Prop::Workbench, "garage_shelf"},
                                      {Prop::Freezer, ""}});
                    break;
                }
                if (b.type == BuildingType::ParkingGarage) {
                    const IRect& r = room.rect;
                    for (int y = r.y + 2; y < r.bottom() - 1; y += 6)
                        for (int x = r.x + 2; x < r.right() - 1; x += 7) place(x, y, Prop::Pillar, 0);
                    for (int y = r.y + 4; y < r.bottom() - 2; y += 6)
                        for (int x = r.x + 3; x < r.right() - 2; x += 3)
                            if (rnd(h) < 0.3f) place(x, y, Prop::Car, (uint8_t)(rnd(h) < 0.5f ? 0 : 1));
                    break;
                }
                if (b.type == BuildingType::MilitaryContainer) {
                    alongWalls(room, {{Prop::Crate, "military_crate"}, {Prop::Crate, "military_crate"}, {Prop::Crate, "military_crate"},
                                      {Prop::Crate, "military_crate"}, {Prop::GunCabinet, "military_rack"}});
                } else if (b.type == BuildingType::Bunker) {
                    alongWalls(room, {{Prop::Shelf, "bunker_store"}, {Prop::Shelf, "bunker_store"}, {Prop::Shelf, "bunker_store"},
                                      {Prop::Locker, "bunker_locker"}, {Prop::Desk, "bunker_desk"}});
                } else if (b.type == BuildingType::Grocery || b.type == BuildingType::Pharmacy || b.type == BuildingType::Hardware ||
                    b.type == BuildingType::GasStation || b.type == BuildingType::Shop) {
                    alongWalls(room, {{Prop::Shelf, shelfLoot}, {Prop::Shelf, shelfLoot}, {Prop::Shelf, shelfLoot},
                                      {Prop::Desk, b.type == BuildingType::Pharmacy ? "pharmacy_back" : "desk"}});
                } else if (b.type == BuildingType::Warehouse && room.rect.area() > 120) {
                    rows(room, Prop::Shelf, "warehouse_shelf", 5);
                } else if (b.type == BuildingType::Barn) {
                    alongWalls(room, {{Prop::HayBale, ""}, {Prop::HayBale, ""}, {Prop::Workbench, "farm_store"}, {Prop::Woodpile, ""},
                                      {Prop::Shelf, "farm_store"}});
                } else {
                    alongWalls(room, {{Prop::Workbench, b.type == BuildingType::Shed ? "toolbox" : "garage_shelf"},
                                      {Prop::Shelf, b.type == BuildingType::Shed ? "toolbox" : "pantry"}});
                    if (b.type == BuildingType::Shed) alongWalls(room, {{Prop::Woodpile, ""}});
                    if (hunterHome) { alongWalls(room, {{Prop::GunCabinet, ""}}); hunterHome = false; }
                }
                break;
            case RoomType::ShopFloor: {
                const std::string& loot = room.loot.empty() ? shelfLoot : room.loot;
                bool clothes = loot == "clothes_rack" || loot == "shoe_shelf";
                rows(room, clothes ? Prop::Wardrobe : Prop::Shelf, loot, 3);
                std::vector<std::pair<Prop, std::string>> wall;
                if (b.type == BuildingType::Grocery || b.type == BuildingType::GasStation || loot == "grocery_shelf")
                    for (int i = 0; i < 4; ++i) wall.push_back({Prop::Cooler, "grocery_cooler"});
                wall.push_back({Prop::Checkout, "checkout"});
                alongWalls(room, wall);
                break;
            }
            case RoomType::Office:
                if (b.type == BuildingType::Bunker) {   // the radio room
                    alongWalls(room, {{Prop::Desk, "bunker_desk"}, {Prop::Desk, "bunker_desk"}, {Prop::Chair, ""}, {Prop::GunCabinet, ""}});
                    break;
                }
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
                if (rnd(h) < 0.45f) alongWalls(room, {{Prop::MedicalCabinet, "first_aid_kit"}});
                break;
            case RoomType::Nave:
                rows(room, Prop::Pew, "", 2);
                alongWalls(room, {{Prop::Altar, ""}});
                break;
            case RoomType::Dining: {
                // Tables with chairs, a counter, and bins nobody emptied
                const IRect& r = room.rect;
                bool court = room.loot == "food_court_counter";
                for (int y = r.y + 2; y < r.bottom() - 2; y += 3)
                    for (int x = r.x + 2; x < r.right() - 2; x += 3) {
                        place(x, y, Prop::Table, 0, court || rnd(h) < 0.5f ? "food_court_table" : "");
                        place(x + 1, y, Prop::Chair, 3);
                        if (rnd(h) < 0.6f) place(x - 1, y, Prop::Chair, 1);
                    }
                std::string counter = room.loot.empty() ? "cafe_counter" : room.loot;
                alongWalls(room, {{Prop::Counter, counter}, {Prop::Counter, counter}, {Prop::Fridge, court ? "restaurant_fridge" : ""},
                                  {Prop::Bin, "food_court_bin"}, {Prop::Bin, "food_court_bin"}});
                if (court) alongWalls(room, {{Prop::Counter, counter}, {Prop::Stove, ""}, {Prop::Bin, "food_court_bin"}});
                break;
            }
            case RoomType::Lobby:
                alongWalls(room, {{Prop::Bench, ""}, {Prop::Bench, ""}, {Prop::Desk, ""}, {Prop::Bin, ""}});
                break;
            case RoomType::Hall:
                if (b.type == BuildingType::Hospital) alongWalls(room, {{Prop::MedicalCabinet, ""}});
                if (isHomeType(b.type) && room.rect.area() >= 6 && rnd(h) < 0.6f) alongWalls(room, {{Prop::Shelf, "home_shelf"}});
                if (b.type == BuildingType::Mall) {
                    // benches and litter bins down the middle of the walkway
                    const IRect& r = room.rect;
                    bool ax = r.w >= r.h;
                    int len = ax ? r.w : r.h;
                    for (int a = 5; a < len - 4; a += 9) {
                        int x = ax ? r.x + a : r.x + r.w / 2, y = ax ? r.y + r.h / 2 : r.y + a;
                        place(x, y, (a / 9) % 3 == 2 ? Prop::Bin : Prop::Bench, ax ? 0 : 1, (a / 9) % 3 == 2 ? "food_court_bin" : "");
                    }
                }
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
        case BuildingType::SummerCottage:   // a falu red stuga with white corners
            b.wallColour = rnd(h) < 0.85f ? 0x8b2a1e : 0x2f4a3a; b.trimColour = 0xf4f2ea; b.roofColour = rnd(h) < 0.6f ? 0x2c2c2e : 0x7a2e22;
            b.floors = 1; b.floor = Ground::FloorWood; b.roof = RoofKind::Gable; break;
        case BuildingType::MilitaryContainer:
            b.wallColour = 0x4f5a3c; b.roofColour = 0x46503a; b.trimColour = 0x3a4230; b.floors = 1; b.roof = RoofKind::Flat;
            b.floor = Ground::FloorConcrete; break;
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
        case BuildingType::Mall:
            b.wallColour = rnd(h) < 0.5f ? 0xd8d2c4 : 0xb85c3c; b.trimColour = 0x2a5a8a; b.roofColour = 0x45474a;
            b.roof = RoofKind::Flat; b.floors = 2; b.floor = Ground::FloorTile; break;
        case BuildingType::Restaurant: {
            static const std::vector<uint32_t> w = {0xe3c565, 0xf0ede4, 0x8b2a1e, 0xd9cbb0};
            b.wallColour = pick(h, w); b.roofColour = pick(h, roofs); b.floor = Ground::FloorTile; b.floors = 1;
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
        case BuildingType::Bunker:
            b.wallColour = 0x84867f; b.roofColour = 0x55594f; b.trimColour = 0x3a3c38; b.floors = 1; b.roof = RoofKind::Flat;
            b.floor = Ground::FloorConcrete; break;
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
            // Dense Swedish spruce forest with thicker and thinner stands
            if (r < 0.10f + n * 0.09f) t.prop = pickTree(0.55f, 0.27f, 0.14f, 0.02f);
            else if (r < 0.25f) t.prop = Prop::BlueberryBush;
            else if (r < 0.29f) t.prop = Prop::LingonBush;
            else if (r < 0.297f) t.prop = Prop::Rock;
            else if (r < 0.2995f) t.prop = Prop::Boulder;
            else if (r < 0.303f) t.prop = Prop::Stump;
            break;
        case Zone::ForestDense:
            // Young, close-grown forest: spruce and birch thick together, undergrowth
            t.ground = n > 0.5f ? Ground::Moss : Ground::ForestFloor;
            if (r < 0.2f + n * 0.08f) t.prop = pickTree(0.58f, 0.12f, 0.27f, 0.01f);
            else if (r < 0.34f) t.prop = Prop::BlueberryBush;
            else if (r < 0.42f) t.prop = Prop::Shrub;
            else if (r < 0.43f) t.prop = Prop::Rock;
            break;
        case Zone::Thicket:
            // Barely passable: saplings, brush and fallen trees
            t.ground = Ground::ForestFloor;
            if (r < 0.26f) t.prop = pickTree(0.5f, 0.05f, 0.4f, 0.0f);
            else if (r < 0.62f) t.prop = Prop::Shrub;
            else if (r < 0.66f) t.prop = Prop::Stump;
            break;
        case Zone::Marsh:
            // Bog: wet moss, tussocks, reeds and pools, the odd stunted pine
            t.ground = n > 0.62f ? Ground::WaterShallow : n > 0.4f ? Ground::Moss : Ground::TallGrass;
            if (t.ground != Ground::WaterShallow) {
                if (r < 0.025f) t.prop = r2 < 0.6f ? Prop::Pine : Prop::Birch;
                else if (r < 0.14f) t.prop = Prop::Reeds;
                else if (r < 0.17f) t.prop = Prop::LingonBush;
            } else if (r < 0.2f) t.prop = Prop::Reeds;
            break;
        case Zone::Rock:
            // Bare rock: slabs and boulders, pines in the cracks
            t.ground = n > 0.55f ? Ground::Moss : Ground::Gravel;
            if (r < 0.04f) t.prop = Prop::Pine;
            else if (r < 0.12f) t.prop = Prop::Rock;
            else if (r < 0.16f) t.prop = Prop::Boulder;
            break;
        case Zone::Woodland:
            t.ground = n > 0.6f ? Ground::TallGrass : (n < 0.3f ? Ground::ForestFloor : Ground::Grass);
            if (r < 0.055f) t.prop = pickTree(0.2f, 0.15f, 0.45f, 0.15f);
            else if (r < 0.075f) t.prop = Prop::Shrub;
            else if (r < 0.08f) t.prop = Prop::RoseBush;
            else if (r < 0.10f) t.prop = Prop::BlueberryBush;
            else if (r < 0.102f) t.prop = Prop::Boulder;
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
        case Zone::Paved:     // squares and pedestrian streets
            t.ground = Ground::Cobble;
            if ((x % 16) == 0 && (y % 16) == 0) t.prop = Prop::Lamppost;
            else if (r < 0.0015f) t.prop = Prop::Bench;
            else if (r < 0.0025f) t.prop = Prop::Bin;
            break;
        case Zone::Parking: {
            t.ground = Ground::Asphalt;
            // parked cars in bays
            if ((((y % 16) + 16) % 16 == 2 || ((y % 16) + 16) % 16 == 7) && ((x % 3) + 3) % 3 == 1 && r < 0.35f) {
                t.prop = Prop::Car;
                t.variant = (uint8_t)(r2 < 0.5f ? 0 : 1);
            }
            break;
        }
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
    if (isFootpath(road.cls)) {
        // Footpaths and forest tracks: paved in town, gravel or dirt outside
        if (d < hw) {
            if (road.cls == RoadClass::Track) t.ground = (std::fabs(d - hw * 0.55f) < 0.35f) ? Ground::Dirt : Ground::Gravel;
            else t.ground = urban ? Ground::Sidewalk : (z == Zone::Forest ? Ground::Dirt : Ground::Gravel);
            if (t.prop != Prop::Lamppost) t.prop = Prop::None;
        } else if (d < hw + 1.0f && (isTree(t.prop) || t.prop == Prop::Boulder || t.prop == Prop::Rock)) {
            t.prop = Prop::None;
        }
        return;
    }
    if (road.cls == RoadClass::Service && d < hw) {
        t.ground = Ground::Asphalt;
        if (t.prop != Prop::Car) t.prop = Prop::None;
        return;
    }
    if (road.cls == RoadClass::Service) {
        if (d < hw + 1.5f && isTree(t.prop)) t.prop = Prop::None;
        return;
    }
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
        // town streets are lit too, a bit more sparsely
        else if (road.cls == RoadClass::Minor && d > hw + 1.0f && std::fmod(road.along, 34.0f) < 1.0f) t.prop = Prop::Lamppost;
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

    // Now and then a bike left by the front door (bike shops have a row of them)
    {
        bool bikeShop = b.name == "Stampens cykel" || b.name == "Allebike";
        bool home = (b.type == BuildingType::House || b.type == BuildingType::Apartment || b.type == BuildingType::Cottage ||
                     b.type == BuildingType::Farmhouse) && !b.isOutbuilding;
        int n = bikeShop ? 4 : (home && hashFloat(hash32(b.seed ^ 0xB1CE)) < 0.07f) ? 1 : 0;
        Vec2 out = b.hasPath ? (b.pathEnd - b.doorPos).normalized() : Vec2{0, 1};
        Vec2 side{-out.y, out.x};
        for (int i = 0; i < n; ++i) {
            Vec2 bp = b.doorPos + out * 1.2f + side * (1.6f + 1.1f * i);
            int bx = (int)std::floor(bp.x), by = (int)std::floor(bp.y);
            if (!inChunk(bx, by)) continue;
            Tile& t = tileAt(bx, by);
            if (t.building >= 0 || t.prop != Prop::None || !groundWalkable(t.ground)) continue;
            t.prop = Prop::Bike;
            t.variant = (uint8_t)(hash32(b.seed + i * 31) & 0xFF);
        }
    }

    // Fuel stations: a concrete forecourt with two pump islands under a canopy
    if (b.type == BuildingType::GasStation && !b.isOutbuilding) {
        Vec2 out = b.hasPath ? (b.pathEnd - b.doorPos).normalized() : Vec2{0, 1};
        if (out.lengthSq() < 0.5f) out = Vec2{0, 1};
        // snap to the nearest axis so the islands line up with the tiles
        if (std::fabs(out.x) > std::fabs(out.y)) out = Vec2{out.x > 0 ? 1.0f : -1.0f, 0};
        else out = Vec2{0, out.y > 0 ? 1.0f : -1.0f};
        Vec2 side{-out.y, out.x};
        Vec2 centre = b.doorPos + out * 6.0f;
        for (int y = (int)centre.y - 7; y <= (int)centre.y + 7; ++y)
            for (int x = (int)centre.x - 7; x <= (int)centre.x + 7; ++x) {
                if (!inChunk(x, y)) continue;
                Tile& t = tileAt(x, y);
                if (t.building >= 0 || t.ground == Ground::WaterShallow || t.ground == Ground::WaterDeep) continue;
                if (distance(Vec2{x + 0.5f, y + 0.5f}, centre) > 7.5f) continue;
                t.ground = Ground::Concrete;
                if (t.prop != Prop::Lamppost) t.prop = Prop::None;
            }
        for (float k : {-3.0f, 3.0f}) {
            Vec2 pp = centre + side * k;
            int px = (int)std::floor(pp.x), py = (int)std::floor(pp.y);
            if (!inChunk(px, py) || tileAt(px, py).building >= 0) continue;
            tileAt(px, py).prop = Prop::FuelPump;
            tileAt(px, py).variant = (uint8_t)(std::fabs(out.x) > 0.5f ? 1 : 0);   // pumps face along the forecourt
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
            if (x >= 15000) {   // the bands where the upper floors and basements are: nothing but them
                t.ground = Ground::Void;
                continue;
            }
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
            if (water > 0.25f && map_.pierAt(p.x, p.y)) { t.ground = Ground::Deck; t.prop = Prop::None; }
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
            if (!osmProps_.empty()) {
                auto it = osmProps_.find(tileKey(x, y));
                if (it != osmProps_.end() && t.ground != Ground::Asphalt && t.ground != Ground::Rail && water < 0.3f) {
                    uint32_t hh = hash2(x, y, seed_ ^ 0x7EE);
                    if (it->second == 1) t.prop = (hh & 3) == 0 ? Prop::Birch : (hh & 3) == 1 ? Prop::Oak : Prop::Maple;
                    else t.prop = Prop::Lamppost;
                }
            }
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
