#include "world/MapData.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Noise.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace alone {

namespace {

float halfWidthFor(RoadClass c) {
    switch (c) {
        case RoadClass::Minor: return 3.0f;
        case RoadClass::Major: return 4.5f;
        case RoadClass::Motorway: return 11.0f;
        case RoadClass::Railway: return 2.6f;
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
    std::ifstream f(dir + "/" + m->str("terrain", "terrain.pgm"), std::ios::binary);
    if (!f) { Log::error("Missing terrain for map %s", name_.c_str()); return false; }
    std::string magic;
    int maxv = 0;
    f >> magic >> width_ >> height_ >> maxv;
    f.get();
    if (magic != "P5" || width_ <= 0 || height_ <= 0) { Log::error("Bad terrain.pgm"); return false; }
    zones_.resize((size_t)width_ * height_);
    f.read((char*)zones_.data(), zones_.size());

    if (sp.size() >= 2) spawn_ = Vec2{(float)atof(sp[0].c_str()) * mpp_, (float)atof(sp[1].c_str()) * mpp_};
    else spawn_ = Vec2{widthM() / 2, heightM() / 2};

    gridW_ = (int)std::ceil(widthM() / kGridCell) + 1;
    gridH_ = (int)std::ceil(heightM() / kGridCell) + 1;

    // --- roads.txt -----------------------------------------------------------
    {
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
                } else if (t[0] == "landmark" && t.size() >= 5) {
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
    Log::info("Map %s: %.1f x %.1f km, %d road segments, %d landmarks", name_.c_str(), widthM() / 1000,
              heightM() / 1000, (int)roads_.size(), (int)landmarks_.size());
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
            best.along = t * (s.b - s.a).length() + std::fabs(s.a.x * 0.37f + s.a.y * 0.61f);
        }
    }
    return best;
}

RoadHit MapData::nearestRoad(float x, float y, bool rail) const {
    return rail ? query(rails_, railGrid_, x, y) : query(roads_, roadGrid_, x, y);
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
    float fx = x / mpp_ - 0.5f, fy = y / mpp_ - 0.5f;
    // wobble the shoreline a bit
    fx += (fbm(x * 0.05f, y * 0.05f, 77, 3) - 0.5f) * 0.9f;
    fy += (fbm(x * 0.05f + 31.7f, y * 0.05f, 78, 3) - 0.5f) * 0.9f;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float tx = fx - ix, ty = fy - iy;
    float a = waterAtPixel(ix, iy), b = waterAtPixel(ix + 1, iy);
    float c = waterAtPixel(ix, iy + 1), d = waterAtPixel(ix + 1, iy + 1);
    return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

Zone MapData::zoneAt(float x, float y) const {
    float w = waterCoverage(x, y);
    if (w >= 0.5f) return Zone::Water;
    float fx = x / mpp_ + (fbm(x * 0.03f, y * 0.03f, 11, 3) - 0.5f) * 1.6f;
    float fy = y / mpp_ + (fbm(x * 0.03f, y * 0.03f + 91.3f, 12, 3) - 0.5f) * 1.6f;
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
