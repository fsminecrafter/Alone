// Building meshes: walls with real openings, window frames, porches, roofs
// with fascia boards, gutters and chimneys, plus the lights inside.
#include "client/RenderCommon.h"
#include "client/Renderer.h"
#include "core/Rng.h"
#include "raymath.h"

namespace alone {

using namespace rc;

namespace {

bool isBrick(uint32_t c) {
    return c == 0xb85c3c || c == 0xc9a88a || c == 0xb85c3c || c == 0xa0523a;
}

bool isHome(BuildingType t) {
    return t == BuildingType::House || t == BuildingType::Cottage || t == BuildingType::Farmhouse || t == BuildingType::Manor;
}

} // namespace

void Renderer::buildBuilding(World& world, const Building& b, BuildingGfx& g) {
    MeshBuilder walls, roof;
    Color ext = hexColor(b.wallColour);
    Color trim = hexColor(b.trimColour);
    static const uint32_t interiors[] = {0xe8e4da, 0xdcd4c4, 0xc8d0bc, 0xe0d8c8, 0xd4dce0, 0xe8dcc8};
    Color inner = hexColor(interiors[b.seed % 6]);
    Color glass = hexColor(0x3c4e5a);
    Color plinth = hexColor(0x6a6a66);
    float top = b.floors * kStorey;
    bool wooden = isHome(b.type) || b.type == BuildingType::Barn || b.type == BuildingType::Shed || b.type == BuildingType::Shop ||
                  b.type == BuildingType::Hostel;
    int extMat = wooden ? MSiding : isBrick(b.wallColour) ? MBrick : MPlaster;
    float extParam = (b.type == BuildingType::Barn || b.type == BuildingType::Shed) ? 1.0f : 0.0f;
    const IRect& R = b.rect;

    // A wall slab along a tile edge: the face towards the outside gets the
    // exterior material, the other face plaster.
    MeshBuilder* cur = &walls;
    auto segment = [&](float ax, float az, bool alongX, float y0, float y1, bool exterior, bool outsideNeg, float ext0 = 0, float ext1 = 0) {
        MeshBuilder& w = *cur;
        if (y1 <= y0) return;
        float h = kWallT * 0.5f;
        Vector3 mn, mx;
        if (alongX) { mn = V(ax - ext0, y0, az - h); mx = V(ax + 1 + ext1, y1, az + h); }
        else { mn = V(ax - h, y0, az - ext0); mx = V(ax + h, y1, az + 1 + ext1); }
        auto face = [&](bool outside) {
            if (exterior && outside) w.material(extMat, extParam);
            else w.material(MPlaster);
            return exterior && outside ? ext : inner;
        };
        bool negOut = exterior && outsideNeg, posOut = exterior && !outsideNeg;
        w.material(MPlain);
        w.quad(V(mn.x, mx.y, mn.z), V(mn.x, mx.y, mx.z), V(mx.x, mx.y, mx.z), V(mx.x, mx.y, mn.z), shade(inner, 0.85f));
        if (alongX) {
            Color cp = face(posOut);
            w.quad(V(mn.x, mn.y, mx.z), V(mx.x, mn.y, mx.z), V(mx.x, mx.y, mx.z), V(mn.x, mx.y, mx.z), cp);
            Color cn = face(negOut);
            w.quad(V(mx.x, mn.y, mn.z), V(mn.x, mn.y, mn.z), V(mn.x, mx.y, mn.z), V(mx.x, mx.y, mn.z), cn);
            face(exterior);
            w.quad(V(mx.x, mn.y, mx.z), V(mx.x, mn.y, mn.z), V(mx.x, mx.y, mn.z), V(mx.x, mx.y, mx.z), exterior ? ext : inner);
            w.quad(V(mn.x, mn.y, mn.z), V(mn.x, mn.y, mx.z), V(mn.x, mx.y, mx.z), V(mn.x, mx.y, mn.z), exterior ? ext : inner);
        } else {
            Color cp = face(posOut);
            w.quad(V(mx.x, mn.y, mx.z), V(mx.x, mn.y, mn.z), V(mx.x, mx.y, mn.z), V(mx.x, mx.y, mx.z), cp);
            Color cn = face(negOut);
            w.quad(V(mn.x, mn.y, mn.z), V(mn.x, mn.y, mx.z), V(mn.x, mx.y, mx.z), V(mn.x, mx.y, mn.z), cn);
            face(exterior);
            w.quad(V(mn.x, mn.y, mx.z), V(mx.x, mn.y, mx.z), V(mx.x, mx.y, mx.z), V(mn.x, mx.y, mx.z), exterior ? ext : inner);
            w.quad(V(mx.x, mn.y, mn.z), V(mn.x, mn.y, mn.z), V(mn.x, mx.y, mn.z), V(mx.x, mx.y, mn.z), exterior ? ext : inner);
        }
        w.material(MPlain);
    };

    // A framed window in an edge: glass, frame, cross bars and a sill
    auto window = [&](MeshBuilder& w, float ax, float az, bool alongX, float y0, float y1, bool outsideNeg, bool frame) {
        w.material(MGlass);
        if (alongX) w.box(V(ax + 0.08f, y0, az - 0.02f), V(ax + 0.92f, y1, az + 0.02f), glass);
        else w.box(V(ax - 0.02f, y0, az + 0.08f), V(ax + 0.02f, y1, az + 0.92f), glass);
        if (!frame) return;
        w.material(MPlain);
        float o = outsideNeg ? -0.075f : 0.075f;
        float d0 = o - 0.03f, d1 = o + 0.03f;
        auto bar = [&](float u0, float u1, float v0, float v1) {
            if (alongX) w.box(V(ax + u0, v0, az + std::min(d0, d1)), V(ax + u1, v1, az + std::max(d0, d1)), trim);
            else w.box(V(ax + std::min(d0, d1), v0, az + u0), V(ax + std::max(d0, d1), v1, az + u1), trim);
        };
        bar(0.04f, 0.96f, y0 - 0.05f, y0 + 0.02f);      // bottom
        bar(0.04f, 0.96f, y1 - 0.02f, y1 + 0.06f);      // top
        bar(0.04f, 0.12f, y0, y1);                       // sides
        bar(0.88f, 0.96f, y0, y1);
        bar(0.48f, 0.52f, y0, y1);                       // mullion
        bar(0.1f, 0.9f, (y0 + y1) * 0.5f - 0.02f, (y0 + y1) * 0.5f + 0.02f);
        // sill
        float s0 = outsideNeg ? -0.18f : 0.02f, s1 = outsideNeg ? -0.02f : 0.18f;
        if (alongX) w.box(V(ax + 0.02f, y0 - 0.08f, az + s0), V(ax + 0.98f, y0 - 0.03f, az + s1), shade(trim, 0.9f));
        else w.box(V(ax + s0, y0 - 0.08f, az + 0.02f), V(ax + s1, y0 - 0.03f, az + 0.98f), shade(trim, 0.9f));
    };

    auto buildWalls = [&](MeshBuilder& w, float topH) {
        cur = &w;
        int edgeIndex = 0;
        for (const auto& e : b.edges) {
            for (int dir = 0; dir < 2; ++dir) {
                uint8_t wallBit = dir == 0 ? WallN : WallW;
                if (!(e.bits & wallBit)) continue;
                bool door = e.bits & (dir == 0 ? DoorN : DoorW);
                bool win = e.bits & (dir == 0 ? WindowN : WindowW);
                bool alongX = dir == 0;
                bool exterior = alongX ? (e.y == R.y || e.y == R.bottom()) : (e.x == R.x || e.x == R.right());
                bool outsideNeg = alongX ? e.y == R.y : e.x == R.x;
                float ax = (float)e.x, az = (float)e.y;
                float height = exterior ? topH : kInnerWall;
                float e0 = exterior ? kWallT * 0.5f : 0, e1 = e0;
                if (door) {
                    segment(ax, az, alongX, 2.1f, height, exterior, outsideNeg, e0, e1);
                    Color fr = exterior ? trim : shade(inner, 0.8f);
                    w.material(MPlain);
                    if (alongX) {
                        w.box(V(ax, 0, az - 0.09f), V(ax + 0.08f, 2.18f, az + 0.09f), fr);
                        w.box(V(ax + 0.92f, 0, az - 0.09f), V(ax + 1, 2.18f, az + 0.09f), fr);
                        w.box(V(ax, 2.1f, az - 0.09f), V(ax + 1, 2.18f, az + 0.09f), fr);
                    } else {
                        w.box(V(ax - 0.09f, 0, az), V(ax + 0.09f, 2.18f, az + 0.08f), fr);
                        w.box(V(ax - 0.09f, 0, az + 0.92f), V(ax + 0.09f, 2.18f, az + 1), fr);
                        w.box(V(ax - 0.09f, 2.1f, az), V(ax + 0.09f, 2.18f, az + 1), fr);
                    }
                    if (exterior) {   // stone step outside
                        float o0 = outsideNeg ? -0.45f : 0.07f, o1 = outsideNeg ? -0.07f : 0.45f;
                        w.material(MConcrete);
                        if (alongX) w.box(V(ax - 0.1f, 0, az + o0), V(ax + 1.1f, 0.15f, az + o1), plinth);
                        else w.box(V(ax + o0, 0, az - 0.1f), V(ax + o1, 0.15f, az + 1.1f), plinth);
                    }
                } else if (win) {
                    segment(ax, az, alongX, 0, 0.9f, exterior, outsideNeg, e0, e1);
                    segment(ax, az, alongX, 2.0f, kStorey, exterior, outsideNeg, e0, e1);
                    window(w, ax, az, alongX, 0.9f, 2.0f, outsideNeg, exterior);
                    if (exterior) segment(ax, az, alongX, kStorey, topH, exterior, outsideNeg, e0, e1);
                } else {
                    segment(ax, az, alongX, 0, height, exterior, outsideNeg, e0, e1);
                }
                if (exterior && topH > kStorey + 0.5f && (edgeIndex % 3 == 1)) {
                    for (int f = 1; f < b.floors; ++f) {
                        float y0 = f * kStorey + 0.9f, y1 = f * kStorey + 2.0f;
                        float o = outsideNeg ? -0.08f : 0.08f;
                        float ox = alongX ? ax : ax + o, oz = alongX ? az + o : az;
                        window(w, ox, oz, alongX, y0, y1, outsideNeg, true);
                    }
                }
                if (exterior) {
                    float o = outsideNeg ? -0.1f : 0.1f;
                    w.material(MConcrete);
                    if (alongX) w.box(V(ax - 0.1f, 0, az + o - 0.04f), V(ax + 1.1f, 0.35f, az + o + 0.04f), plinth);
                    else w.box(V(ax + o - 0.04f, 0, az - 0.1f), V(ax + o + 0.04f, 0.35f, az + 1.1f), plinth);
                }
                ++edgeIndex;
            }
        }
        if (wooden) {
            w.material(MPlain);
            float t2 = 0.12f;
            for (Vec2 c : {Vec2{(float)R.x, (float)R.y}, Vec2{(float)R.right(), (float)R.y}, Vec2{(float)R.x, (float)R.bottom()},
                           Vec2{(float)R.right(), (float)R.bottom()}})
                w.box(V(c.x - t2, 0, c.y - t2), V(c.x + t2, topH, c.y + t2), trim);
        }
        // Rugs in living rooms
        for (const auto& room : b.rooms) {
            if (room.type != RoomType::Living || room.rect.w < 4 || room.rect.h < 4) continue;
            static const uint32_t rugs[] = {0x8a3a32, 0x3a4a6a, 0x6a5a3a, 0x4a6a4a};
            Vec2 c = room.rect.centre();
            w.material(MCarpet);
            w.box(V(c.x - 1.3f, 0.0f, c.y - 0.9f), V(c.x + 1.3f, 0.012f, c.y + 0.9f), hexColor(rugs[b.seed % 4]));
        }
        w.material(MPlain);
    };
    MeshBuilder lowWalls;
    buildWalls(walls, top);
    buildWalls(lowWalls, kStorey);

    // Porch over the front door of homes
    if (isHome(b.type) && b.hasPath) {
        Vec2 door = b.doorPos;
        Vec2 out = (b.doorPos - b.rect.centre());
        Vec2 n = std::fabs(out.x) > std::fabs(out.y) ? Vec2{out.x > 0 ? 1.0f : -1.0f, 0} : Vec2{0, out.y > 0 ? 1.0f : -1.0f};
        Vec2 side{-n.y, n.x};
        Vec2 wallP = door - n * 0.5f;            // point on the wall
        float depth = 1.8f, halfW = 1.4f;
        Vec2 a = wallP + side * halfW, c = wallP - side * halfW + n * depth;
        Color deck = hexColor(0x8a6a48);
        walls.material(MFloorWood);
        walls.box(V(std::min(a.x, c.x), 0, std::min(a.y, c.y)), V(std::max(a.x, c.x), 0.22f, std::max(a.y, c.y)), deck);
        lowWalls.material(MFloorWood);
        lowWalls.box(V(std::min(a.x, c.x), 0, std::min(a.y, c.y)), V(std::max(a.x, c.x), 0.22f, std::max(a.y, c.y)), deck);
        for (float s : {-1.0f, 1.0f}) {
            Vec2 post = wallP + side * (halfW - 0.15f) * s + n * (depth - 0.15f);
            walls.material(MPlain);
            walls.box(V(post.x - 0.07f, 0.22f, post.y - 0.07f), V(post.x + 0.07f, 2.5f, post.y + 0.07f), trim);
            // railing
            Vec2 rail0 = wallP + side * (halfW - 0.1f) * s, rail1 = post;
            walls.tube(V(rail0.x, 0.95f, rail0.y), V(rail1.x, 0.95f, rail1.y), 0.035f, 0.035f, 4, trim);
        }
        // little sloped porch roof
        Vector3 r0 = V(wallP.x + side.x * (halfW + 0.2f), 2.85f, wallP.y + side.y * (halfW + 0.2f));
        Vector3 r1 = V(wallP.x - side.x * (halfW + 0.2f), 2.85f, wallP.y - side.y * (halfW + 0.2f));
        Vector3 r2 = V(r1.x + n.x * (depth + 0.3f), 2.5f, r1.z + n.y * (depth + 0.3f));
        Vector3 r3 = V(r0.x + n.x * (depth + 0.3f), 2.5f, r0.z + n.y * (depth + 0.3f));
        roof.material(MShingles, std::fabs(n.y) > 0.5f ? 0.0f : 1.0f);
        Color rc = hexColor(b.roofColour);
        roof.quad(r0, r1, r2, r3, rc);
        roof.quad(r3, r2, r1, r0, shade(rc, 0.7f));
        // door lamp
        g.lights.push_back({V(wallP.x + side.x * 0.8f + n.x * 0.3f, 2.2f, wallP.y + side.y * 0.8f + n.y * 0.3f), 5.0f, V(1.0f, 0.78f, 0.5f), 0.9f});
    }

    // Roof
    Vector3 mn = V((float)R.x, top, (float)R.y), mx = V((float)R.right(), top, (float)R.bottom());
    Color roofCol = hexColor(b.roofColour);
    bool tiles = roofCol.r > roofCol.g + 25;
    float span = b.roofAlongX ? (float)R.h : (float)R.w;
    float rise = span * 0.42f;
    switch (b.roof) {
        case RoofKind::Gable: {
            roof.material(tiles ? MRoofTiles : MShingles, b.roofAlongX ? 1.0f : 0.0f);
            roof.gable(mn, mx, rise, b.roofAlongX, roofCol, ext, 0.45f);
            // gable ends carry the wall material
            // fascia boards and gutters along the eaves
            Color dark = hexColor(0x2a2a2c);
            roof.material(MMetal);
            if (b.roofAlongX) {
                for (float z : {mn.z - 0.45f, mx.z + 0.45f})
                    roof.tube(V(mn.x - 0.5f, top - 0.27f, z), V(mx.x + 0.5f, top - 0.27f, z), 0.07f, 0.07f, 5, dark);
            } else {
                for (float x : {mn.x - 0.45f, mx.x + 0.45f})
                    roof.tube(V(x, top - 0.27f, mn.z - 0.5f), V(x, top - 0.27f, mx.z + 0.5f), 0.07f, 0.07f, 5, dark);
            }
            if (isHome(b.type) || b.type == BuildingType::Shop) {
                float chx = R.x + R.w * (b.roofAlongX ? 0.7f : 0.5f), chz = R.y + R.h * (b.roofAlongX ? 0.5f : 0.7f);
                roof.material(MBrick);
                roof.box(V(chx - 0.35f, top, chz - 0.35f), V(chx + 0.35f, top + rise + 0.8f, chz + 0.35f), hexColor(0x7a3a2a));
                roof.material(MMetal);
                roof.box(V(chx - 0.45f, top + rise + 0.8f, chz - 0.45f), V(chx + 0.45f, top + rise + 0.9f, chz + 0.45f), dark);
            }
            break;
        }
        case RoofKind::Hip:
            roof.material(tiles ? MRoofTiles : MShingles, (R.w >= R.h) ? 1.0f : 0.0f);
            roof.hip(mn, mx, span * 0.3f, roofCol, 0.4f);
            break;
        case RoofKind::Flat:
            roof.material(MConcrete);
            roof.box(V(mn.x - 0.1f, top, mn.z - 0.1f), V(mx.x + 0.1f, top + 0.4f, mx.z + 0.1f), shade(roofCol, 0.9f));
            roof.material(MDirt);
            roof.box(V(mn.x + 0.3f, top + 0.4f, mn.z + 0.3f), V(mx.x - 0.3f, top + 0.42f, mx.z - 0.3f), roofCol);
            // vents / machinery on big flat roofs
            if (R.w * R.h > 200) {
                roof.material(MMetal);
                uint32_t s = b.seed;
                for (int i = 0; i < 3; ++i) {
                    s = hash32(s + 1);
                    float vx = R.x + 2 + hashFloat(s) * (R.w - 4), vz = R.y + 2 + hashFloat(hash32(s)) * (R.h - 4);
                    roof.box(V(vx - 0.8f, top + 0.4f, vz - 0.6f), V(vx + 0.8f, top + 1.3f, vz + 0.6f), hexColor(0x9a9ea2));
                }
            }
            break;
    }
    if (b.type == BuildingType::Church) {
        float tx = (float)R.x - 2.5f, tz = R.y + R.h * 0.5f;
        walls.material(MPlaster);
        walls.box(V(tx - 2.5f, 0, tz - 2.5f), V(tx + 2.5f, 20.0f, tz + 2.5f), ext);
        walls.material(MPlain);
        walls.box(V(tx - 2.6f, 15.5f, tz - 2.6f), V(tx + 2.6f, 16.0f, tz + 2.6f), trim);
        walls.material(MGlass);
        for (int s = 0; s < 4; ++s) {   // belfry openings
            float a = s * PI * 0.5f;
            walls.box(V(tx + std::cos(a) * 2.52f - 0.4f, 17.0f, tz + std::sin(a) * 2.52f - 0.4f),
                      V(tx + std::cos(a) * 2.52f + 0.4f, 19.0f, tz + std::sin(a) * 2.52f + 0.4f), hexColor(0x202020));
        }
        roof.material(MShingles);
        roof.cone(V(tx, 20.0f, tz), 3.4f, 13.0f, 8, hexColor(0x2a3a3a));
        roof.material(MMetal);
        roof.tube(V(tx, 33.0f, tz), V(tx, 35.0f, tz), 0.08f, 0.04f, 4, hexColor(0xc0a040));
    }
    if (b.type == BuildingType::Station) {
        roof.material(MShingles);
        roof.box(V(R.x - 2.0f, 3.2f, R.bottom() + 0.0f), V(R.right() + 2.0f, 3.4f, R.bottom() + 4.0f), hexColor(0x5a3028));
    }

    // Lights left on inside: the biggest rooms, maybe a TV
    std::vector<const Room*> rooms;
    for (const auto& r : b.rooms) rooms.push_back(&r);
    std::sort(rooms.begin(), rooms.end(), [](const Room* a, const Room* c) { return a->rect.area() > c->rect.area(); });
    int count = 0;
    for (const Room* r : rooms) {
        if (count >= 4) break;
        if (r->type == RoomType::Storage || r->type == RoomType::Bathroom) continue;
        Vec2 c = r->rect.centre();
        float rad = std::min(9.0f, 3.5f + std::sqrt((float)r->rect.area()) * 0.6f);
        bool tv = r->type == RoomType::Living && (b.seed & 6) == 2;
        if (tv) g.lights.push_back({V(c.x, 1.0f, c.y), 6.0f, V(0.45f, 0.6f, 1.0f), 0.9f});
        else g.lights.push_back({V(c.x, 2.3f, c.y), rad, V(1.0f, 0.82f, 0.55f), 1.0f});
        ++count;
    }

    if (!walls.empty()) { g.walls = walls.build(); g.hasWalls = true; }
    if (!lowWalls.empty()) { g.wallsLow = lowWalls.build(); g.hasWallsLow = true; }
    if (!roof.empty()) { g.roof = roof.build(); g.hasRoof = true; }
    (void)world;
}

} // namespace alone
