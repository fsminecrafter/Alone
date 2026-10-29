#include "world/MapData.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Noise.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace alone {

namespace {

bool fileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return (bool)f;
}

float halfWidthFor(RoadClass c) {
    switch (c) {
        case RoadClass::Minor: return 3.0f;
        case RoadClass::Major: return 4.5f;
        case RoadClass::Motorway: return 11.0f;
        case RoadClass::Railway: return 2.6f;
        case RoadClass::Path: return 0.9f;
        case RoadClass::Service: return 2.2f;
        case RoadClass::Track: return 1.4f;
        default: return 0;
    }
}

// Parse a line that may contain "quoted strings" and numbers.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        if (isspace((unsigned char)line[i])) { ++i; continue; }
        if (line[i] == '"') {
            size_t end = line.find('"', i + 1);
            if (end == std::string::npos) end = line.size();
            out.push_back(line.substr(i + 1, end - i - 1));
            i = end + 1;
        } else {
            size_t end = i;
            while (end < line.size() && !isspace((unsigned char)line[end])) ++end;
            out.push_back(line.substr(i, end - i));
            i = end;
        }
    }
    return out;
}

} // namespace

bool MapData::load(const std::string& dir) {
    Config cfg;
    if (!cfg.load(dir + "/map.ini")) return false;
    const Config::Section* m = cfg.find("map");
    if (!m) { Log::error("map.ini has no [map] section"); return false; }
    name_ = m->str("name", "Unknown");
    mpp_ = m->num("metres_per_pixel", 1.0f);
    latitude_ = m->num("latitude", 58.0f);
    auto sp = split(m->str("spawn", "0 0"), ' ');

    // --- terrain.pgm ---------------------------------------------------------
    std::string osmTerrain = m->str("terrain_osm", "");
    tmpp_ = mpp_;
    std::string terrainFile = m->str("terrain", "terrain.pgm");
    if (!osmTerrain.empty() && fileExists(dir + "/" + osmTerrain)) {
        terrainFile = osmTerrain;
        tmpp_ = m->num("terrain_osm_metres_per_pixel", 2.0f);
    }
    std::ifstream f(dir + "/" + terrainFile, std::ios::binary);
    if (!f) { Log::error("Missing terrain for map %s", name_.c_str()); return false; }
    std::string magic;
    int maxv = 0;
    f >> magic >> width_ >> height_ >> maxv;
    f.get();
    if (magic != "P5" || width_ <= 0 || height_ <= 0) { Log::error("Bad terrain.pgm"); return false; }
    zones_.resize((size_t)width_ * height_);
    f.read((char*)zones_.data(), zones_.size());

    // --- elevation.pgm: 16-bit heights on a coarse grid ---------------------------------------
    std::string elevFile = m->str("elevation", "");
    if (!elevFile.empty() && fileExists(dir + "/" + elevFile)) {
        std::ifstream ef(dir + "/" + elevFile, std::ios::binary);
        std::string em;
        int emax = 0;
        ef >> em >> ew_ >> eh_ >> emax;
        ef.get();
        float unit = m->num("elevation_units", 0.01f);
        empp_ = m->num("elevation_metres_per_pixel", 8.0f);
        if (em == "P5" && ew_ > 1 && eh_ > 1 && emax > 255) {
            std::vector<unsigned char> raw((size_t)ew_ * eh_ * 2);
            ef.read((char*)raw.data(), (std::streamsize)raw.size());
            elev_.resize((size_t)ew_ * eh_);
            elevMin_ = 1e9f;
            elevMax_ = -1e9f;
            for (size_t i = 0; i < elev_.size(); ++i) {
                elev_[i] = ((raw[i * 2] << 8) | raw[i * 2 + 1]) * unit;   // big-endian, as PGM says
                elevMin_ = std::min(elevMin_, elev_[i]);
                elevMax_ = std::max(elevMax_, elev_[i]);
            }
        } else {
            Log::warn("Bad %s: no elevation", elevFile.c_str());
            ew_ = eh_ = 0;
        }
    }

    if (sp.size() >= 2) spawn_ = Vec2{(float)atof(sp[0].c_str()) * mpp_, (float)atof(sp[1].c_str()) * mpp_};
    else spawn_ = Vec2{widthM() / 2, heightM() / 2};

    gridW_ = (int)std::ceil(widthM() / kGridCell) + 1;
    gridH_ = (int)std::ceil(heightM() / kGridCell) + 1;

    // --- roads ------------------------------------------------------------------
    std::string roadsM = m->str("roads_metres", "");
    bool osm = !roadsM.empty() && fileExists(dir + "/" + roadsM);
    if (osm) {
        std::string text;
        readTextFile(dir + "/" + roadsM, text);
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            int cls;
            std::string nm;
            ls >> cls >> nm;
            int nameIdx = -1;
            if (nm != "-") {
                for (auto& ch : nm) if (ch == '_') ch = ' ';
                for (int i = 0; i < (int)roadNames_.size(); ++i)
                    if (roadNames_[i] == nm) { nameIdx = i; break; }
                if (nameIdx < 0) { nameIdx = (int)roadNames_.size(); roadNames_.push_back(nm); }
            }
            std::vector<Vec2> pts;
            float x, y;
            while (ls >> x >> y) pts.push_back(Vec2{x, y});
            RoadClass rc = (RoadClass)std::clamp(cls, 1, 7);
            if (rc == RoadClass::Railway) continue;
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                RoadSegment sg{pts[i], pts[i + 1], rc, halfWidthFor(rc)};
                sg.name = nameIdx;
                roads_.push_back(sg);
            }
        }
    } else {
        std::string text;
        if (readTextFile(dir + "/" + m->str("roads", "roads.txt"), text)) {
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream ls(line);
                int cls;
                ls >> cls;
                std::vector<Vec2> pts;
                float x, y;
                while (ls >> x >> y) pts.push_back(Vec2{(x + 0.5f) * mpp_, (y + 0.5f) * mpp_});
                RoadClass rc = (RoadClass)std::clamp(cls, 1, 3);
                for (size_t i = 0; i + 1 < pts.size(); ++i)
                    roads_.push_back({pts[i], pts[i + 1], rc, halfWidthFor(rc)});
            }
        }
    }

    int digitisedRoads = (int)roads_.size();

    // --- features.txt ----------------------------------------------------------
    {
        std::string text;
        if (readTextFile(dir + "/" + m->str("features", "features.txt"), text)) {
            std::istringstream in(text);
            std::string line;
            int lineNo = 0;
            while (std::getline(in, line)) {
                ++lineNo;
                auto t = tokenize(line);
                if (t.empty() || t[0][0] == '#') continue;
                auto num = [&](size_t i) { return i < t.size() ? (float)atof(t[i].c_str()) : 0.0f; };
                // The OSM import supersedes the hand-traced streets, squares and old-town blocks
                if (osm && (t[0] == "road" || t[0] == "dense" || t[0] == "plaza" || t[0] == "railway" || t[0] == "landmark"))
                    continue;
                if (t[0] == "railway") {
                    std::vector<Vec2> pts;
                    for (size_t i = 1; i + 1 < t.size(); i += 2) pts.push_back(Vec2{num(i) * mpp_, num(i + 1) * mpp_});
                    for (size_t i = 0; i + 1 < pts.size(); ++i)
                        rails_.push_back({pts[i], pts[i + 1], RoadClass::Railway, halfWidthFor(RoadClass::Railway)});
                } else if (t[0] == "road" && t.size() >= 6) {
                    RoadClass rc = (RoadClass)std::clamp(atoi(t[1].c_str()), 1, 3);
                    std::vector<Vec2> pts;
                    for (size_t i = 2; i + 1 < t.size(); i += 2) pts.push_back(Vec2{num(i) * mpp_, num(i + 1) * mpp_});
                    for (size_t i = 0; i + 1 < pts.size(); ++i)
                        roads_.push_back({pts[i], pts[i + 1], rc, halfWidthFor(rc)});
                } else if (t[0] == "plaza" && t.size() >= 6) {
                    plazas_.push_back({t[1], Vec2{num(2) * mpp_, num(3) * mpp_}, Vec2{num(4), num(5)}});
                } else if (t[0] == "label" && t.size() >= 4) {
                    labels_.push_back({t[1], Vec2{num(2) * mpp_, num(3) * mpp_}, t.size() >= 5 ? num(4) : 0.0f});
                } else if (t[0] == "dense" && t.size() >= 5) {
                    dense_.push_back({t[1], Vec2{num(2) * mpp_, num(3) * mpp_}, num(4) * mpp_});
                } else if (t[0] == "district" && t.size() >= 5) {
                    districts_.push_back({t[1], Vec2{num(2) * mpp_, num(3) * mpp_}, num(4) * mpp_});
                } else if (t[0] == "water" && t.size() >= 4) {
                    waterNames_.push_back({t[1], Vec2{num(2) * mpp_, num(3) * mpp_}});
                } else if ((t[0] == "landmark" || t[0] == "place") && t.size() >= 5) {
                    // place: like a landmark, but kept on top of the OSM data too
                    // (something OpenStreetMap doesn't have, like a hidden shelter)
                    Landmark l;
                    l.kind = t[1];
                    l.name = t[2];
                    l.centre = Vec2{num(3) * mpp_, num(4) * mpp_};
                    l.size = t.size() >= 7 ? Vec2{num(5), num(6)} : Vec2{20, 14};
                    landmarks_.push_back(l);
                } else {
                    Log::warn("features.txt:%d: unknown entry '%s'", lineNo, t[0].c_str());
                }
            }
        }
    }

    // --- osm_features.txt (metres) ----------------------------------------------
    if (osm) {
        std::string text;
        if (readTextFile(dir + "/" + m->str("osm_features", "osm_features.txt"), text)) {
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                auto t = tokenize(line);
                if (t.empty() || t[0][0] == '#') continue;
                auto num = [&](size_t i) { return i < t.size() ? (float)atof(t[i].c_str()) : 0.0f; };
                if (t[0] == "coverage" && t.size() >= 5) {
                    coverage_.push_back({num(1), num(2), num(3), num(4)});
                } else if (t[0] == "railway") {
                    std::vector<Vec2> pts;
                    for (size_t i = 1; i + 1 < t.size(); i += 2) pts.push_back(Vec2{num(i), num(i + 1)});
                    for (size_t i = 0; i + 1 < pts.size(); ++i)
                        rails_.push_back({pts[i], pts[i + 1], RoadClass::Railway, halfWidthFor(RoadClass::Railway)});
                } else if (t[0] == "district" && t.size() >= 5) {
                    bool dup = false;
                    for (const auto& d : districts_) if (d.name == t[1]) dup = true;
                    if (!dup) districts_.push_back({t[1], Vec2{num(2), num(3)}, num(4)});
                } else if (t[0] == "tree" && t.size() >= 3) {
                    osmTrees_.push_back(Vec2{num(1), num(2)});
                } else if (t[0] == "lamp" && t.size() >= 3) {
                    osmLamps_.push_back(Vec2{num(1), num(2)});
                } else if (t[0] == "building" && t.size() >= 7) {
                    OsmBuilding b;
                    b.kind = t[1];
                    b.levels = atoi(t[2].c_str());
                    b.centre = Vec2{num(3), num(4)};
                    b.w = atoi(t[5].c_str());
                    b.h = atoi(t[6].c_str());
                    b.name = t.size() >= 8 ? t[7] : "";
                    for (size_t i = 8; i < t.size(); ++i) {
                        size_t eq = t[i].find('=');
                        if (eq == std::string::npos) continue;
                        std::string k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
                        if (k == "colour") b.colour = (uint32_t)std::strtoul(v.c_str(), nullptr, 16) | 0x1000000u;
                        else if (k == "roof_colour") b.roofColour = (uint32_t)std::strtoul(v.c_str(), nullptr, 16) | 0x1000000u;
                        else if (k == "roof") b.roof = v == "gable" ? 0 : v == "flat" ? 1 : v == "hip" ? 2 : -1;
                    }
                    osmBuildings_.push_back(b);
                } else if ((t[0] == "pier" || t[0] == "pier_area") && t.size() >= 5) {
                    Pier pr;
                    pr.area = t[0] == "pier_area";
                    for (size_t i = 1; i + 1 < t.size(); i += 2) pr.pts.push_back(Vec2{num(i), num(i + 1)});
                    piers_.push_back(pr);
                }
            }
        }
    }

    // Inside hand-drawn old-town areas the traced streets replace the digitised ones
    if (!dense_.empty()) {
        std::vector<RoadSegment> kept;
        for (size_t i = 0; i < roads_.size(); ++i) {
            const auto& r = roads_[i];
            bool replaced = false;
            if ((int)i < digitisedRoads && r.cls != RoadClass::Motorway)
                for (const auto& d : dense_)
                    if (distance((r.a + r.b) * 0.5f, d.centre) < d.radius * 0.9f) replaced = true;
            if (!replaced) kept.push_back(r);
        }
        roads_.swap(kept);
    }
    index(roads_, roadGrid_);
    index(rails_, railGrid_);
    Log::info("Map %s: %.1f x %.1f km, %d road segments, %d landmarks, %d OSM buildings", name_.c_str(),
              widthM() / 1000, heightM() / 1000, (int)roads_.size(), (int)landmarks_.size(),
              (int)osmBuildings_.size());
    return true;
}

void MapData::index(const std::vector<RoadSegment>& segs, std::vector<std::vector<int>>& grid) {
    grid.assign((size_t)gridW_ * gridH_, {});
    for (int i = 0; i < (int)segs.size(); ++i) {
        const auto& s = segs[i];
        float pad = s.halfWidth + 4.0f;
        int x0 = std::max(0, (int)((std::min(s.a.x, s.b.x) - pad) / kGridCell));
        int x1 = std::min(gridW_ - 1, (int)((std::max(s.a.x, s.b.x) + pad) / kGridCell));
        int y0 = std::max(0, (int)((std::min(s.a.y, s.b.y) - pad) / kGridCell));
        int y1 = std::min(gridH_ - 1, (int)((std::max(s.a.y, s.b.y) + pad) / kGridCell));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) grid[(size_t)y * gridW_ + x].push_back(i);
    }
}

RoadHit MapData::query(const std::vector<RoadSegment>& segs, const std::vector<std::vector<int>>& grid,
                       float x, float y) const {
    RoadHit best;
    int gx = (int)(x / kGridCell), gy = (int)(y / kGridCell);
    if (gx < 0 || gy < 0 || gx >= gridW_ || gy >= gridH_) return best;
    // Prefer the "most important" road where they overlap (motorway over minor)
    float bestScore = 1e9f;
    for (int i : grid[(size_t)gy * gridW_ + gx]) {
        const auto& s = segs[i];
        float t;
        float d = distToSegment(Vec2{x, y}, s.a, s.b, &t);
        float score = d - s.halfWidth;   // negative = on the road
        if (score < bestScore) {
            bestScore = score;
            best.cls = s.cls;
            best.distance = d;
            best.halfWidth = s.halfWidth;
            best.dir = (s.b - s.a).normalized();
            best.name = s.name;
            best.along = t * (s.b - s.a).length() + std::fabs(s.a.x * 0.37f + s.a.y * 0.61f);
        }
    }
    return best;
}

RoadHit MapData::nearestRoad(float x, float y, bool rail) const {
    return rail ? query(rails_, railGrid_, x, y) : query(roads_, roadGrid_, x, y);
}

RoadHit MapData::nearestStreet(float x, float y) const {
    RoadHit best;
    int gx = (int)(x / kGridCell), gy = (int)(y / kGridCell);
    if (gx < 0 || gy < 0 || gx >= gridW_ || gy >= gridH_) return best;
    float bestScore = 1e9f;
    for (int i : roadGrid_[(size_t)gy * gridW_ + gx]) {
        const auto& s = roads_[i];
        if (isFootpath(s.cls)) continue;
        float t;
        float d = distToSegment(Vec2{x, y}, s.a, s.b, &t);
        if (d - s.halfWidth < bestScore) {
            bestScore = d - s.halfWidth;
            best.cls = s.cls;
            best.distance = d;
            best.halfWidth = s.halfWidth;
            best.dir = (s.b - s.a).normalized();
            best.name = s.name;
        }
    }
    return best;
}

bool MapData::pierAt(float x, float y) const {
    Vec2 p{x, y};
    for (const auto& pr : piers_) {
        if (pr.area) {
            // point in polygon
            bool in = false;
            for (size_t i = 0, j = pr.pts.size() - 1; i < pr.pts.size(); j = i++) {
                const Vec2 &a = pr.pts[i], &b = pr.pts[j];
                if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
            }
            if (in) return true;
        } else {
            for (size_t i = 0; i + 1 < pr.pts.size(); ++i)
                if (distToSegment(p, pr.pts[i], pr.pts[i + 1], nullptr) < 1.3f) return true;
        }
    }
    return false;
}

bool MapData::osmCovered(float x, float y) const {
    for (const auto& c : coverage_)
        if (x >= c[0] && y >= c[1] && x <= c[2] && y <= c[3]) return true;
    return false;
}

std::string MapData::streetAt(Vec2 p) const {
    int gx = (int)(p.x / kGridCell), gy = (int)(p.y / kGridCell);
    if (gx < 0 || gy < 0 || gx >= gridW_ || gy >= gridH_) return "";
    float best = 25.0f;
    int name = -1;
    for (int i : roadGrid_[(size_t)gy * gridW_ + gx]) {
        const auto& s = roads_[i];
        if (s.name < 0) continue;
        float d = distToSegment(p, s.a, s.b, nullptr) - s.halfWidth;
        if (d < best) { best = d; name = s.name; }
    }
    return name >= 0 ? roadNames_[name] : "";
}

float MapData::elevAt(int ix, int iy) const {
    ix = std::clamp(ix, 0, ew_ - 1);
    iy = std::clamp(iy, 0, eh_ - 1);
    return elev_[(size_t)iy * ew_ + ix];
}

float MapData::elevation(float x, float y) const {
    if (elev_.empty()) return 0.0f;
    // Catmull-Rom through the 4x4 samples around the point: smooth slopes
    float fx = x / empp_, fy = y / empp_;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float tx = fx - ix, ty = fy - iy;
    auto w = [](float t, float out[4]) {
        float t2 = t * t, t3 = t2 * t;
        out[0] = (-t3 + 2 * t2 - t) * 0.5f;
        out[1] = (3 * t3 - 5 * t2 + 2) * 0.5f;
        out[2] = (-3 * t3 + 4 * t2 + t) * 0.5f;
        out[3] = (t3 - t2) * 0.5f;
    };
    float wx[4], wy[4];
    w(tx, wx);
    w(ty, wy);
    float h = 0;
    for (int j = 0; j < 4; ++j) {
        float row = 0;
        for (int i = 0; i < 4; ++i) row += wx[i] * elevAt(ix - 1 + i, iy - 1 + j);
        h += wy[j] * row;
    }
    return h;
}

float MapData::elevationLinear(float x, float y) const {
    if (elev_.empty()) return 0.0f;
    float fx = x / empp_, fy = y / empp_;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float tx = fx - ix, ty = fy - iy;
    float a = elevAt(ix, iy) + (elevAt(ix + 1, iy) - elevAt(ix, iy)) * tx;
    float b = elevAt(ix, iy + 1) + (elevAt(ix + 1, iy + 1) - elevAt(ix, iy + 1)) * tx;
    return a + (b - a) * ty;
}

float MapData::waterLevel(float x, float y) const {
    if (elev_.empty()) return 0.0f;
    // the lowest of the four samples around: the water's own level, even
    // right by the shore where the land samples are higher
    int ix = (int)std::floor(x / empp_), iy = (int)std::floor(y / empp_);
    return std::min(std::min(elevAt(ix, iy), elevAt(ix + 1, iy)), std::min(elevAt(ix, iy + 1), elevAt(ix + 1, iy + 1)));
}

Zone MapData::zoneAtPixel(int px, int py) const {
    if (px < 0 || py < 0 || px >= width_ || py >= height_) return Zone::Forest;
    return (Zone)zones_[(size_t)py * width_ + px];
}

float MapData::waterAtPixel(int px, int py) const {
    if (px < 0 || py < 0 || px >= width_ || py >= height_) return 0;
    return zones_[(size_t)py * width_ + px] == (uint8_t)Zone::Water ? 1.0f : 0.0f;
}

float MapData::waterCoverage(float x, float y) const {
    float fx = x / tmpp_ - 0.5f, fy = y / tmpp_ - 0.5f;
    // wobble the shoreline a bit
    float wob = 6.2f / tmpp_;   // about 6 m of shoreline wobble
    fx += (fbm(x * 0.05f, y * 0.05f, 77, 3) - 0.5f) * wob;
    fy += (fbm(x * 0.05f + 31.7f, y * 0.05f, 78, 3) - 0.5f) * wob;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float tx = fx - ix, ty = fy - iy;
    float a = waterAtPixel(ix, iy), b = waterAtPixel(ix + 1, iy);
    float c = waterAtPixel(ix, iy + 1), d = waterAtPixel(ix + 1, iy + 1);
    return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

Zone MapData::zoneAt(float x, float y) const {
    float w = waterCoverage(x, y);
    if (w >= 0.5f) return Zone::Water;
    // wobble zone borders by roughly 5 m, whatever the terrain resolution
    float wb = 5.5f / tmpp_ * 2.0f;
    float fx = x / tmpp_ + (fbm(x * 0.03f, y * 0.03f, 11, 3) - 0.5f) * wb;
    float fy = y / tmpp_ + (fbm(x * 0.03f, y * 0.03f + 91.3f, 12, 3) - 0.5f) * wb;
    Zone z = zoneAtPixel((int)std::floor(fx), (int)std::floor(fy));
    if (z == Zone::Water) {
        // warped into a water pixel but the smooth shoreline says land: use a neighbour
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                Zone n = zoneAtPixel((int)std::floor(fx) + dx, (int)std::floor(fy) + dy);
                if (n != Zone::Water) return n;
            }
        return Zone::Meadow;
    }
    return z;
}

const DenseArea* MapData::denseAreaAt(Vec2 p) const {
    for (const auto& d : dense_)
        if (distance(p, d.centre) < d.radius) return &d;
    return nullptr;
}

const Plaza* MapData::plazaAt(Vec2 p) const {
    for (const auto& pl : plazas_)
        if (std::fabs(p.x - pl.centre.x) < pl.size.x / 2 && std::fabs(p.y - pl.centre.y) < pl.size.y / 2) return &pl;
    return nullptr;
}

std::string MapData::districtAt(Vec2 p) const {
    const District* best = nullptr;
    float bestScore = 1e9f;
    for (const auto& d : districts_) {
        float score = distance(p, d.centre) / d.radius;
        if (score < bestScore) { bestScore = score; best = &d; }
    }
    if (!best || bestScore > 2.5f) return name_;
    return best->name;
}

} // namespace alone
