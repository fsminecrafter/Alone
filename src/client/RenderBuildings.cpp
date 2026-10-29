// Building meshes: walls with real openings, window frames, porches, roofs
// with fascia boards, gutters and chimneys, plus the lights inside.
#include "client/RenderCommon.h"
#include "client/Renderer.h"
#include "core/Rng.h"
#include "core/Math.h"
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
    // Everything is built with its floor at 0 and lifted to the floor's
    // height; the ground under it is in local heights too
    const MapData& map = world.map();
    float fy = b.floorY;
    auto groundBelow = [&](float x, float z) { return map.elevation(x, z) - fy; };
    Color ext = hexColor(b.wallColour);
    Color trim = hexColor(b.trimColour);
    static const uint32_t interiors[] = {0xe8e4da, 0xdcd4c4, 0xc8d0bc, 0xe0d8c8, 0xd4dce0, 0xe8dcc8};
    Color inner = hexColor(interiors[b.seed % 6]);
    Color glass = hexColor(0x3c4e5a);
    Color plinth = hexColor(0x6a6a66);
    float top = b.floors * kStorey;
    bool wooden = isHome(b.type) || b.type == BuildingType::Barn || b.type == BuildingType::Shed || b.type == BuildingType::Shop ||
                  b.type == BuildingType::Hostel || b.oldTown;
    if (b.type == BuildingType::Mall || b.type == BuildingType::ParkingGarage) wooden = false;
    int extMat = wooden ? MSiding : isBrick(b.wallColour) ? MBrick : MPlaster;
    if (b.type == BuildingType::Mall && b.sign == "Vimpeln") extMat = MCladding;
    float extParam = (b.type == BuildingType::Barn || b.type == BuildingType::Shed) ? 1.0f : 0.0f;
    const IRect& R = b.rect;

    // A wall slab along a tile edge: the face towards the outside gets the
    // exterior material, the other face plaster.
    MeshBuilder* cur = &walls;
    auto segment = [&](float ax, float az, bool alongX, float y0, float y1, bool exterior, bool outsideNeg, float ext0 = 0, float ext1 = 0) {
        MeshBuilder& w = *cur;
        if (y1 <= y0) return;
        if (alongX) w.piece(ax + 0.5f, az, y0);
        else w.piece(ax, az + 0.5f, y0);
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
    auto window = [&](MeshBuilder& w, float ax, float az, bool alongX, float y0, float y1, bool outsideNeg, bool frame, bool broken = false) {
        if (alongX) w.piece(ax + 0.5f, az, y0 - 0.08f, true);
        else w.piece(ax, az + 0.5f, y0 - 0.08f, true);
        w.material(MGlass);
        if (!broken) {
            if (alongX) w.box(V(ax + 0.08f, y0, az - 0.02f), V(ax + 0.92f, y1, az + 0.02f), glass);
            else w.box(V(ax - 0.02f, y0, az + 0.08f), V(ax + 0.02f, y1, az + 0.92f), glass);
        } else {
            // smashed: a few jagged shards left in the corners of the frame
            const float sh[][4] = {{0.12f, 0.0f, 0.26f, 0.22f}, {0.7f, 0.0f, 0.88f, 0.14f}, {0.12f, 0.8f, 0.22f, 1.0f}, {0.78f, 0.7f, 0.88f, 1.0f}};
            for (const auto& q : sh) {
                float v0 = y0 + (y1 - y0) * q[1], v1 = y0 + (y1 - y0) * q[3];
                if (alongX) w.box(V(ax + q[0], v0, az - 0.015f), V(ax + q[2], v1, az + 0.015f), glass);
                else w.box(V(ax - 0.015f, v0, az + q[0]), V(ax + 0.015f, v1, az + q[2]), glass);
            }
        }
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
                float pcx = alongX ? ax + 0.5f : ax, pcz = alongX ? az : az + 0.5f;   // centre of this piece of wall
                if (door) {
                    segment(ax, az, alongX, 2.1f, height, exterior, outsideNeg, e0, e1);
                    Color fr = exterior ? trim : shade(inner, 0.8f);
                    w.material(MPlain);
                    w.piece(pcx, pcz, 0, true);
                    if (alongX) {
                        w.box(V(ax, 0, az - 0.09f), V(ax + 0.08f, 2.18f, az + 0.09f), fr);
                        w.box(V(ax + 0.92f, 0, az - 0.09f), V(ax + 1, 2.18f, az + 0.09f), fr);
                        w.piece(pcx, pcz, 2.1f, true);
                        w.box(V(ax, 2.1f, az - 0.09f), V(ax + 1, 2.18f, az + 0.09f), fr);
                    } else {
                        w.box(V(ax - 0.09f, 0, az), V(ax + 0.09f, 2.18f, az + 0.08f), fr);
                        w.box(V(ax - 0.09f, 0, az + 0.92f), V(ax + 0.09f, 2.18f, az + 1), fr);
                        w.piece(pcx, pcz, 2.1f, true);
                        w.box(V(ax - 0.09f, 2.1f, az), V(ax + 0.09f, 2.18f, az + 1), fr);
                    }
                    w.piece(pcx, pcz, 0, true);
                    if (exterior) {   // stone step outside, down to the ground
                        float o0 = outsideNeg ? -0.45f : 0.07f, o1 = outsideNeg ? -0.07f : 0.45f;
                        float sb = std::min(0.0f, groundBelow(pcx + (alongX ? 0 : (o0 + o1) * 0.5f), pcz + (alongX ? (o0 + o1) * 0.5f : 0)) - 0.1f);
                        w.material(MConcrete);
                        if (alongX) w.box(V(ax - 0.1f, sb, az + o0), V(ax + 1.1f, 0.15f, az + o1), plinth);
                        else w.box(V(ax + o0, sb, az - 0.1f), V(ax + o1, 0.15f, az + 1.1f), plinth);
                    }
                } else if (win) {
                    segment(ax, az, alongX, 0, 0.9f, exterior, outsideNeg, e0, e1);
                    segment(ax, az, alongX, 2.0f, kStorey, exterior, outsideNeg, e0, e1);
                    window(w, ax, az, alongX, 0.9f, 2.0f, outsideNeg, exterior, world.windowBroken(e.x, e.y, dir));
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
                    // the plinth, reaching down to the ground where it falls away
                    // (a foundation wall on the downhill side)
                    float o = outsideNeg ? -0.1f : 0.1f;
                    float gb = std::min(groundBelow(alongX ? ax : ax + o, alongX ? az + o : az),
                                        groundBelow(alongX ? ax + 1 : ax + o, alongX ? az + o : az + 1));
                    float pb = std::min(0.0f, gb - 0.3f);
                    w.piece(pcx, pcz, pb, true);
                    w.material(MConcrete);
                    if (alongX) w.box(V(ax - 0.1f, pb, az + o - 0.04f), V(ax + 1.1f, 0.35f, az + o + 0.04f), plinth);
                    else w.box(V(ax + o - 0.04f, pb, az - 0.1f), V(ax + o + 0.04f, 0.35f, az + 1.1f), plinth);
                    if (pb < -0.35f) {   // a proper foundation wall under the wall
                        float h2 = kWallT * 0.5f + 0.02f;
                        if (alongX) w.box(V(ax - h2, pb, az - h2), V(ax + 1 + h2, 0.0f, az + h2), shade(plinth, 0.92f));
                        else w.box(V(ax - h2, pb, az - h2), V(ax + h2, 0.0f, az + 1 + h2), shade(plinth, 0.92f));
                    }
                }
                ++edgeIndex;
            }
        }
        if (wooden) {
            w.material(MPlain);
            float t2 = 0.12f;
            for (Vec2 c : {Vec2{(float)R.x, (float)R.y}, Vec2{(float)R.right(), (float)R.y}, Vec2{(float)R.x, (float)R.bottom()},
                           Vec2{(float)R.right(), (float)R.bottom()}}) {
                w.piece(c.x, c.y, 0, true);
                w.box(V(c.x - t2, 0, c.y - t2), V(c.x + t2, topH, c.y + t2), trim);
            }
        }
        w.noPiece();
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
    // The ground floor alone, drawn when you're inside or behind the building.
    // Its walls know their pieces so the GPU can cut those in front of you.
    MeshBuilder lowWalls;
    lowWalls.trackPieces();
    walls.setLift(fy);
    lowWalls.setLift(fy);
    roof.setLift(fy);
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
        float db = std::min(0.0f, std::min(groundBelow(a.x, a.y), groundBelow(c.x, c.y)) - 0.1f);   // down to the ground
        walls.material(MFloorWood);
        walls.box(V(std::min(a.x, c.x), db, std::min(a.y, c.y)), V(std::max(a.x, c.x), 0.22f, std::max(a.y, c.y)), deck);
        lowWalls.material(MFloorWood);
        lowWalls.box(V(std::min(a.x, c.x), db, std::min(a.y, c.y)), V(std::max(a.x, c.x), 0.22f, std::max(a.y, c.y)), deck);
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
        g.lights.push_back({V(wallP.x + side.x * 0.8f + n.x * 0.3f, fy + 2.2f, wallP.y + side.y * 0.8f + n.y * 0.3f), 5.0f, V(1.0f, 0.78f, 0.5f), 0.9f});
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


    // --- The street side: which face has the main door --------------------------------
    struct Facade { Vec2 f0, along, nrm; float len, door; };
    auto facadeOf = [&](Vec2 doorPos) {
        Vec2 out = doorPos - R.centre();
        Facade f;
        if (std::fabs(out.x) * R.h > std::fabs(out.y) * R.w) {
            f.f0 = out.x > 0 ? Vec2{(float)R.right(), (float)R.y} : Vec2{(float)R.x, (float)R.y};
            f.along = {0, 1}; f.nrm = {out.x > 0 ? 1.0f : -1.0f, 0}; f.len = (float)R.h;
        } else {
            f.f0 = out.y > 0 ? Vec2{(float)R.x, (float)R.bottom()} : Vec2{(float)R.x, (float)R.y};
            f.along = {1, 0}; f.nrm = {0, out.y > 0 ? 1.0f : -1.0f}; f.len = (float)R.w;
        }
        f.door = std::clamp(dot(doorPos - f.f0, f.along), 0.0f, f.len);
        return f;
    };
    auto at = [](const Facade& f, float t, float out, float y) {
        Vec2 p = f.f0 + f.along * t + f.nrm * out;
        return V(p.x, y, p.y);
    };
    // A box aligned to a facade: t along it, o out from it, y up
    auto fbox = [&](MeshBuilder& m, const Facade& f, float t0, float t1, float o0, float o1, float y0, float y1, Color c) {
        Vector3 a = at(f, t0, o0, y0), bb = at(f, t1, o1, y1);
        m.box(V(std::min(a.x, bb.x), y0, std::min(a.z, bb.z)), V(std::max(a.x, bb.x), y1, std::max(a.z, bb.z)), c);
    };
    Facade front = facadeOf(b.doorPos);

    // --- Shop fronts: display windows and a striped awning ---------------------------------
    if (b.shopFront && b.type != BuildingType::Mall) {
        static const uint32_t awnings[] = {0x2e6a3e, 0x2a5a8a, 0x3a3a3c, 0xa83a2a, 0x1e1e20, 0x6a2a3a};
        Color stripe = hexColor(awnings[b.seed % 6]), white = hexColor(0xeeeae0);
        bool plainAwning = (b.seed >> 4) % 3 == 0;
        for (MeshBuilder* m : {&walls, &lowWalls}) {
            // a piece per window pane: centre on the wall line, its lowest point
            auto pane = [&](float t0, float t1, float base) {
                Vector3 c = at(front, (t0 + t1) * 0.5f, 0.0f, 0.0f);
                m->piece(c.x, c.z, base, true);
            };
            m->material(MGlass);
            for (int seg = 0; seg < 2; ++seg) {
                float t0 = seg == 0 ? 0.35f : front.door + 0.75f, t1 = seg == 0 ? front.door - 0.75f : front.len - 0.35f;
                if (t1 - t0 < 0.8f) continue;
                for (float u = t0; u < t1 - 0.01f; u += 1.0f) {   // glass in 1 m panes, cut like the wall behind
                    pane(u, std::min(t1, u + 1.0f), 0.4f);
                    fbox(*m, front, u, std::min(t1, u + 1.0f), 0.05f, 0.09f, 0.4f, 2.35f, hexColor(0x30424e));
                }
                m->material(MMetal);
                pane(t0, t1, 2.35f);
                fbox(*m, front, t0 - 0.05f, t1 + 0.05f, 0.04f, 0.11f, 2.35f, 2.45f, hexColor(0x2c2c2e));
                pane(t0, t1, 0.3f);
                fbox(*m, front, t0 - 0.05f, t1 + 0.05f, 0.04f, 0.13f, 0.3f, 0.4f, hexColor(0x2c2c2e));
                for (float t = t0; t <= t1 + 0.01f; t += std::max(0.8f, (t1 - t0) / std::max(1.0f, std::round((t1 - t0) / 1.6f)))) {
                    pane(t - 0.5f, t + 0.5f, 0.4f);
                    fbox(*m, front, t - 0.03f, t + 0.03f, 0.04f, 0.11f, 0.4f, 2.35f, hexColor(0x2c2c2e));
                }
                m->material(MGlass);
            }
            // The awning slopes out over the pavement
            m->material(MCarpet);
            float a0 = 0.25f, a1 = front.len - 0.25f, sw = 0.42f;
            for (float t = a0; t < a1 - 0.01f; t += sw) {
                float t1 = std::min(a1, t + sw);
                pane(t, t1, 2.3f);
                Color c = plainAwning ? stripe : (((int)((t - a0) / sw)) % 2 == 0 ? stripe : white);
                Vector3 p0 = at(front, t, 0.1f, 2.95f), p1 = at(front, t1, 0.1f, 2.95f);
                Vector3 p2 = at(front, t1, 1.15f, 2.5f), p3 = at(front, t, 1.15f, 2.5f);
                m->quad(p0, p1, p2, p3, c);
                m->quad(p3, p2, p1, p0, shade(c, 0.7f));
                m->quad(p3, p2, at(front, t1, 1.15f, 2.3f), at(front, t, 1.15f, 2.3f), shade(c, 0.92f));   // valance
                m->quad(at(front, t, 1.15f, 2.3f), at(front, t1, 1.15f, 2.3f), p2, p3, shade(c, 0.8f));
            }
            m->material(MPlain);
            m->noPiece();
        }
        if (!b.sign.empty()) {
            SignGfx sg;
            sg.text = b.sign;
            Vector3 c = at(front, front.door, 0.13f, 3.3f);
            sg.centre = c;
            sg.right = V(front.along.x, 0, front.along.y);
            if (cross(front.along, front.nrm) < 0) sg.right = Vector3Negate(sg.right);   // read from outside
            sg.height = 0.62f;
            sg.fg = hexColor(0xf4f0e4);
            sg.bg = hexColor(0x1e1e20);
            g.signs.push_back(sg);
        }
    } else if (!b.sign.empty() && b.type != BuildingType::Mall && b.type != BuildingType::ParkingGarage) {
        SignGfx sg;
        sg.text = b.sign;
        sg.centre = at(front, front.door, 0.12f, std::min(top - 0.5f, 2.7f));
        sg.right = V(front.along.x, 0, front.along.y);
        if (cross(front.along, front.nrm) < 0) sg.right = Vector3Negate(sg.right);
        sg.height = 0.6f;
        sg.fg = b.type == BuildingType::Pharmacy ? hexColor(0x2aa05a) : hexColor(0xf4f0e4);
        sg.bg = b.type == BuildingType::Pharmacy ? hexColor(0xf4f4f0) : hexColor(0x26282a);
        g.signs.push_back(sg);
    }

    // --- Balconies on blocks of flats --------------------------------------------------------
    if (b.type == BuildingType::Apartment && b.floors >= 3 && !b.oldTown) {
        bool alongX = R.w >= R.h;
        int count = 0;
        for (int sideN = 0; sideN < 2; ++sideN) {
            Facade f;
            if (alongX) { f.f0 = {(float)R.x, sideN ? (float)R.bottom() : (float)R.y}; f.along = {1, 0}; f.nrm = {0, sideN ? 1.0f : -1.0f}; f.len = (float)R.w; }
            else { f.f0 = {sideN ? (float)R.right() : (float)R.x, (float)R.y}; f.along = {0, 1}; f.nrm = {sideN ? 1.0f : -1.0f, 0}; f.len = (float)R.h; }
            Color slab = hexColor(0xd8d6d0), rail = hexColor(0x8fa6b2), metal = hexColor(0x3a3c3e);
            for (int fl = 1; fl < b.floors && count < 600; ++fl)
                for (float t = 2.0f; t + 2.4f < f.len - 1.0f && count < 600; t += 4.5f, ++count) {
                    float y = fl * kStorey;
                    walls.material(MConcrete);
                    fbox(walls, f, t, t + 2.4f, 0.0f, 1.3f, y - 0.16f, y, slab);
                    walls.material(MGlass);
                    fbox(walls, f, t, t + 2.4f, 1.24f, 1.3f, y, y + 1.0f, rail);
                    fbox(walls, f, t, t + 0.06f, 0.0f, 1.3f, y, y + 1.0f, rail);
                    fbox(walls, f, t + 2.34f, t + 2.4f, 0.0f, 1.3f, y, y + 1.0f, rail);
                    walls.material(MMetal);
                    fbox(walls, f, t, t + 2.4f, 1.22f, 1.32f, y + 1.0f, y + 1.06f, metal);
                }
        }
    }

    // --- Shopping centres: roof lights over the walkways, Vimpeln's glass tower --------------
    if (b.type == BuildingType::Mall) {
        roof.material(MGlass);
        for (const auto& room : b.rooms) {
            if (room.type != RoomType::Hall) continue;
            const IRect& h = room.rect;
            roof.box(V(h.x + 0.6f, top + 0.42f, h.y + 0.6f), V(h.right() - 0.6f, top + 0.95f, h.bottom() - 0.6f), hexColor(0x5a7686));
            roof.material(MMetal);
            for (int x = h.x + 2; x < h.right() - 1; x += 3)
                if (h.w > h.h) roof.box(V(x - 0.04f, top + 0.42f, h.y + 0.55f), V(x + 0.04f, top + 0.98f, h.bottom() - 0.55f), hexColor(0x8a8e92));
            for (int y = h.y + 2; y < h.bottom() - 1; y += 3)
                if (h.h >= h.w) roof.box(V(h.x + 0.55f, top + 0.42f, y - 0.04f), V(h.right() - 0.55f, top + 0.98f, y + 0.04f), hexColor(0x8a8e92));
            roof.material(MGlass);
        }
        roof.material(MPlain);
        // Shop signs over the shop openings, seen from the walkway
        for (const auto& room : b.rooms) {
            if (room.label.empty() || room.type == RoomType::Hall) continue;
            for (const auto& hall : b.rooms) {
                if (hall.type != RoomType::Hall) continue;
                const IRect& a = room.rect;
                const IRect& hr = hall.rect;
                SignGfx sg;
                sg.text = room.label;
                sg.height = 0.5f;
                sg.fg = hexColor(0xf6f4ee);
                sg.bg = room.type == RoomType::Dining ? hexColor(0x8a3a2a) : hexColor(0x26282c);
                sg.interior = true;
                bool overlapX = a.x < hr.right() && hr.x < a.right(), overlapY = a.y < hr.bottom() && hr.y < a.bottom();
                if (overlapX && a.bottom() == hr.y) { sg.centre = V(a.x + a.w * 0.5f, 2.55f, (float)a.bottom() + 0.08f); sg.right = V(1, 0, 0); }
                else if (overlapX && a.y == hr.bottom()) { sg.centre = V(a.x + a.w * 0.5f, 2.55f, (float)a.y - 0.08f); sg.right = V(-1, 0, 0); }
                else if (overlapY && a.right() == hr.x) { sg.centre = V((float)a.right() + 0.08f, 2.55f, a.y + a.h * 0.5f); sg.right = V(0, 0, -1); }
                else if (overlapY && a.x == hr.right()) { sg.centre = V((float)a.x - 0.08f, 2.55f, a.y + a.h * 0.5f); sg.right = V(0, 0, 1); }
                else continue;
                g.signs.push_back(sg);
                break;
            }
        }
        if (b.sign == "Vimpeln") {
            // A glass entrance hall rising above the roof, the name in orange on top
            float t0 = front.door - 3.5f, t1 = front.door + 3.5f, th = top + 4.0f;
            roof.material(MGlass);
            fbox(roof, front, t0, t1, -0.3f, -5.0f, 0.0f, th - 0.8f, hexColor(0x4e6c7c));
            roof.material(MCladding);
            fbox(roof, front, t0 - 0.3f, t1 + 0.3f, 0.6f, -5.3f, th - 0.8f, th, hexColor(0xc6c8ca));   // the canopy band
            roof.material(MMetal);
            for (float t = t0; t <= t1 + 0.01f; t += 1.4f) fbox(roof, front, t - 0.05f, t + 0.05f, -0.26f, -0.36f, 0.0f, th - 0.8f, hexColor(0x6a6e72));
            for (float y = 1.6f; y < th - 1.0f; y += 1.6f) fbox(roof, front, t0, t1, -0.26f, -0.36f, y - 0.04f, y + 0.04f, hexColor(0x6a6e72));
            roof.material(MPlain);
            SignGfx sg;
            sg.text = "Vimpeln";
            sg.centre = at(front, front.door, 0.66f, th + 1.0f);
            sg.right = V(front.along.x, 0, front.along.y);
            if (cross(front.along, front.nrm) < 0) sg.right = Vector3Negate(sg.right);
            sg.height = 1.6f;
            sg.fg = hexColor(0xf07a1e);
            g.signs.push_back(sg);
            // Corten planters by the doors
            walls.material(MMetal);
            for (float s : {-1.0f, 1.0f})
                fbox(walls, front, front.door + s * 2.2f - 0.6f, front.door + s * 2.2f + 0.6f, 0.4f, 1.6f, 0.0f, 0.55f, hexColor(0x8a4a24));
            walls.material(MPlain);
        }
        if (b.sign == "Storken") {
            SignGfx sg;
            sg.text = "Storken";
            sg.centre = at(front, front.door, 0.14f, kStorey + 1.3f);
            sg.right = V(front.along.x, 0, front.along.y);
            if (cross(front.along, front.nrm) < 0) sg.right = Vector3Negate(sg.right);
            sg.height = 0.9f;
            sg.fg = hexColor(0xf6f2ea);
            sg.dot = true;
            g.signs.push_back(sg);
            // Tall windows over the entrance
            walls.material(MGlass);
            fbox(walls, front, front.door - 2.5f, front.door + 2.5f, 0.05f, 0.09f, 0.2f, 2.4f, hexColor(0x34464e));
            walls.material(MPlain);
        }
    }

    // --- Car park: open decks on pillars, a spiral stair in the corner -----------------------
    if (b.type == BuildingType::ParkingGarage) {
        Color conc = hexColor(0xb4b4ae), rail = hexColor(0x5a5c5e);
        float deckH = 3.0f;
        for (int f = 1; f <= b.floors; ++f) {
            float y = f * deckH;
            roof.material(MConcrete);
            roof.box(V((float)R.x, y - 0.3f, (float)R.y), V((float)R.right(), y, (float)R.bottom()), conc);
            // parapets with a gap of daylight under them
            roof.box(V((float)R.x, y, (float)R.y), V((float)R.right(), y + 0.95f, R.y + 0.18f), shade(conc, 0.95f));
            roof.box(V((float)R.x, y, R.bottom() - 0.18f), V((float)R.right(), y + 0.95f, (float)R.bottom()), shade(conc, 0.95f));
            roof.box(V((float)R.x, y, (float)R.y), V(R.x + 0.18f, y + 0.95f, (float)R.bottom()), shade(conc, 0.9f));
            roof.box(V(R.right() - 0.18f, y, (float)R.y), V((float)R.right(), y + 0.95f, (float)R.bottom()), shade(conc, 0.9f));
            roof.material(MPlain);
            // painted bays on the top deck
            if (f == b.floors)
                for (float x = R.x + 2.0f; x < R.right() - 1.0f; x += 2.6f)
                    roof.box(V(x, y + 0.002f, R.y + 1.0f), V(x + 0.1f, y + 0.012f, R.y + 5.5f), hexColor(0xd8d8d0));
        }
        // the spiral stair: a post, steps winding round it and a rail
        Vector3 c = V(R.x - 1.6f, 0, R.y + 1.6f);
        float hTot = b.floors * deckH + 1.0f;
        roof.material(MMetal);
        roof.tube(c, V(c.x, hTot, c.z), 0.1f, 0.1f, 8, rail);
        Vector3 prev = V(c.x + 1.4f, 1.0f, c.z);
        for (float a = 0, y = 0; y < hTot - 1.0f; a += 0.45f, y += 0.2f) {
            Vector3 p0 = V(c.x + std::cos(a) * 0.15f, y, c.z + std::sin(a) * 0.15f);
            Vector3 p1 = V(c.x + std::cos(a) * 1.35f, y, c.z + std::sin(a) * 1.35f);
            roof.tube(p0, p1, 0.12f, 0.12f, 4, hexColor(0x6a6c6e), false);
            Vector3 r = V(c.x + std::cos(a) * 1.4f, y + 1.0f, c.z + std::sin(a) * 1.4f);
            roof.tube(prev, r, 0.03f, 0.03f, 4, rail, false);
            prev = r;
        }
        roof.material(MPlain);
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
        if (tv) g.lights.push_back({V(c.x, fy + 1.0f, c.y), 6.0f, V(0.45f, 0.6f, 1.0f), 0.9f});
        else g.lights.push_back({V(c.x, fy + 2.3f, c.y), rad, V(1.0f, 0.82f, 0.55f), 1.0f});
        ++count;
    }

    // Furniture: drawn only when the building is open to view, in 16 m cells
    // so a big shop only draws the part you can see
    {
        const int cell = 16;
        int cw = (R.w + cell - 1) / cell, ch = (R.h + cell - 1) / cell;
        std::vector<MeshBuilder> cells((size_t)(cw * ch));
        for (auto& mb : cells) mb.setLift(fy);
        ChunkGfx scratch;
        int doy = world.clock().dayOfYear();
        if (b.type != BuildingType::ParkingGarage)
            for (int y = R.y; y < R.bottom(); ++y)
                for (int x = R.x; x < R.right(); ++x) {
                    const Tile& t = world.tile(x, y);
                    if (t.building == b.id && t.prop != Prop::None)
                        addProp(cells[(size_t)(((y - R.y) / cell) * cw + (x - R.x) / cell)], world, t, x, y, doy, scratch);
                }
        for (auto& mb : cells) addPart(g.interior, mb);   // (each lifted to the floor below)
    }
    g.stamp = buildingStamp(world, b);
    float tall = top + 12.0f + (b.type == BuildingType::Church ? 26.0f : 0.0f) + (b.type == BuildingType::ParkingGarage ? 16.0f : 0.0f);
    g.boxMin = V(R.x - 4.0f, fy - 6.0f, R.y - 4.0f);
    g.boxMax = V(R.right() + 4.0f, fy + tall, R.bottom() + 4.0f);
    for (auto& sg : g.signs) sg.centre.y += fy;

    if (!walls.empty()) { g.walls = walls.build(); g.hasWalls = true; }
    if (!lowWalls.empty()) { g.wallsLow = lowWalls.build(); g.hasWallsLow = true; }
    if (!roof.empty()) { g.roof = roof.build(); g.hasRoof = true; }
    (void)world;
}

} // namespace alone
