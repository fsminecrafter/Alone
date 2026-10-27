#include "client/Renderer.h"
#include "core/Log.h"
#include "core/Noise.h"
#include "core/Paths.h"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>

namespace alone {

namespace {

constexpr float kStorey = 3.0f;
constexpr float kInnerWall = 2.7f;
constexpr float kWallT = 0.14f;
constexpr float kDeepWater = -0.35f;
constexpr float kShallowWater = -0.12f;

Color groundColour(Ground g) {
    switch (g) {
        case Ground::Grass: return hexColor(0x6e8c44);
        case Ground::ForestFloor: return hexColor(0x4f4a2c);
        case Ground::Moss: return hexColor(0x52693a);
        case Ground::Dirt: return hexColor(0x6b583e);
        case Ground::Sand: return hexColor(0xcbb98a);
        case Ground::TallGrass: return hexColor(0x8a9450);
        case Ground::FieldSoil: return hexColor(0x5c4530);
        case Ground::Stubble: return hexColor(0xaa9a5c);
        case Ground::Asphalt: return hexColor(0x47494d);
        case Ground::Sidewalk: return hexColor(0x9b9995);
        case Ground::Gravel: return hexColor(0x928775);
        case Ground::Cobble: return hexColor(0x7f7872);
        case Ground::Rail: return hexColor(0x5a5048);
        case Ground::Concrete: return hexColor(0x9a9892);
        case Ground::WaterShallow: return hexColor(0x4d7e96, 200);
        case Ground::WaterDeep: return hexColor(0x2c5674, 200);
        case Ground::FloorWood: return hexColor(0x9c7650);
        case Ground::FloorTile: return hexColor(0xc4c2bc);
        case Ground::FloorConcrete: return hexColor(0x8a8884);
        case Ground::FloorCarpet: return hexColor(0x7a5656);
        default: return MAGENTA;
    }
}

bool isWater(Ground g) { return g == Ground::WaterShallow || g == Ground::WaterDeep; }
float groundHeight(Ground g) { return g == Ground::WaterDeep ? kDeepWater : g == Ground::WaterShallow ? kShallowWater : 0.0f; }

// Leaf colour through the autumn: green, then yellow/red, then bare-ish
Color leafColour(Prop p, int doy, uint8_t variant) {
    float turn = std::clamp((doy - 250) / 40.0f, 0.0f, 1.0f);     // mid Sep .. late Oct
    float v = (variant % 32) / 32.0f;
    Color summer = hexColor(0x4f7a32);
    Color autumn;
    switch (p) {
        case Prop::Birch: summer = hexColor(0x6a9438); autumn = hexColor(0xd8b02c); break;
        case Prop::Maple: autumn = v < 0.5f ? hexColor(0xc8481c) : hexColor(0xe08a20); break;
        case Prop::Oak: autumn = hexColor(0x9a7a2c); turn *= 0.7f; break;
        case Prop::Rowan: autumn = hexColor(0xc05020); break;
        default: autumn = summer; break;
    }
    Color c = mixColor(summer, autumn, std::clamp(turn * (0.7f + 0.6f * v), 0.0f, 1.0f));
    return shade(c, 0.88f + 0.24f * v);
}

Vector3 V(float x, float y, float z) { return Vector3{x, y, z}; }

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool Renderer::init() {
    shader_ = LoadShader(Paths::data("shaders/world.vs").c_str(), Paths::data("shaders/world.fs").c_str());
    if (shader_.id == 0) { Log::error("Failed to load world shader"); return false; }
    shader_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(shader_, "matModel");
    locSunDir_ = GetShaderLocation(shader_, "sunDir");
    locSunColor_ = GetShaderLocation(shader_, "sunColor");
    locAmbient_ = GetShaderLocation(shader_, "ambientColor");
    locPlayer_ = GetShaderLocation(shader_, "playerPos");
    locViewDir_ = GetShaderLocation(shader_, "viewDir");
    locCutRadius_ = GetShaderLocation(shader_, "cutRadius");
    locCutaway_ = GetShaderLocation(shader_, "cutaway");
    locLights_ = GetShaderLocation(shader_, "lights");
    locLightColors_ = GetShaderLocation(shader_, "lightColors");
    locLightCount_ = GetShaderLocation(shader_, "lightCount");
    locTime_ = GetShaderLocation(shader_, "time");
    locFogColor_ = GetShaderLocation(shader_, "fogColor");
    locFogStart_ = GetShaderLocation(shader_, "fogStart");
    locFogEnd_ = GetShaderLocation(shader_, "fogEnd");
    locUnlit_ = GetShaderLocation(shader_, "unlit");
    mat_ = LoadMaterialDefault();
    mat_.shader = shader_;
    cube_ = GenMeshCube(1, 1, 1);
    sphere_ = GenMeshSphere(1, 6, 8);
    cylinder_ = GenMeshCylinder(1, 1, 8);
    cone_ = GenMeshCone(1, 1, 7);
    cam_.up = {0, 1, 0};
    cam_.projection = CAMERA_ORTHOGRAPHIC;
    cam_.fovy = zoom_;
    return true;
}

void Renderer::unloadChunk(ChunkGfx& g) {
    if (g.hasGround) UnloadMesh(g.ground);
    if (g.hasProps) UnloadMesh(g.props);
    g.hasGround = g.hasProps = false;
}

void Renderer::invalidateAll() {
    for (auto& [k, g] : chunks_) unloadChunk(g);
    chunks_.clear();
    for (auto& [k, b] : buildings_) {
        if (b.hasWalls) UnloadMesh(b.walls);
        if (b.hasWallsLow) UnloadMesh(b.wallsLow);
        if (b.hasRoof) UnloadMesh(b.roof);
    }
    buildings_.clear();
}

void Renderer::shutdown() {
    invalidateAll();
    UnloadMesh(cube_);
    UnloadMesh(sphere_);
    UnloadMesh(cylinder_);
    UnloadMesh(cone_);
    UnloadShader(shader_);
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void Renderer::updateCamera() {
    float d = 120.0f;
    Vector3 t{target_.x, 0, target_.y};
    cam_.target = t;
    cam_.position = {t.x + std::cos(pitch_) * std::sin(yaw_) * d, t.y + std::sin(pitch_) * d,
                     t.z + std::cos(pitch_) * std::cos(yaw_) * d};
    cam_.fovy = zoom_;
}

void Renderer::follow(Vec2 target, float dt) {
    float k = std::min(1.0f, dt * 8.0f);
    target_ = target_ + (target - target_) * k;
    float dy = yawTarget_ - yaw_;
    yaw_ += dy * std::min(1.0f, dt * 10.0f);
    updateCamera();
}

void Renderer::snapTo(Vec2 target) {
    target_ = target;
    yaw_ = yawTarget_;
    updateCamera();
}

void Renderer::rotate(int steps) { yawTarget_ += steps * PI * 0.5f; }

void Renderer::zoom(float f) { zoom_ = std::clamp(zoom_ * f, 10.0f, 90.0f); }

Vec2 Renderer::pickGround(Vector2 mouse) {
    Ray r = GetScreenToWorldRay(mouse, cam_);
    if (std::fabs(r.direction.y) < 1e-5f) return target_;
    float t = -r.position.y / r.direction.y;
    return Vec2{r.position.x + r.direction.x * t, r.position.z + r.direction.z * t};
}

Vec2 Renderer::pick(Vector2 mouse, World& world) {
    Ray r = GetScreenToWorldRay(mouse, cam_);
    Vec2 g = pickGround(mouse);
    // Prefer furniture/props the ray hits before reaching the ground
    float best = 1e9f;
    Vec2 out = g;
    int gx = (int)std::floor(g.x), gy = (int)std::floor(g.y);
    for (int y = gy - 4; y <= gy + 4; ++y)
        for (int x = gx - 4; x <= gx + 4; ++x) {
            const Tile& t = world.tile(x, y);
            if (t.prop == Prop::None) continue;
            float h = std::min(propHeight(t.prop), 2.2f);
            if (h < 0.3f) continue;
            float inset = isTree(t.prop) ? 0.3f : 0.05f;
            BoundingBox bb{{x + inset, 0, y + inset}, {x + 1 - inset, h, y + 1 - inset}};
            RayCollision c = GetRayCollisionBox(r, bb);
            if (c.hit && c.distance < best) { best = c.distance; out = Vec2{x + 0.5f, y + 0.5f}; }
        }
    return out;
}

Vector2 Renderer::toScreen(Vec2 p, float h) const {
    return GetWorldToScreen(Vector3{p.x, h, p.y}, cam_);
}

Vec2 Renderer::screenToWorldDir(float sx, float sy) const {
    // screen right and screen up projected onto the ground
    Vec2 fwd{-std::sin(yaw_), -std::cos(yaw_)};
    Vec2 right{std::cos(yaw_), -std::sin(yaw_)};
    return right * sx + fwd * (-sy);
}

// ---------------------------------------------------------------------------
// Chunk meshes
// ---------------------------------------------------------------------------

void Renderer::addRoadSurfaces(MeshBuilder& mb, World& world, int x0, int y0) {
    // Roads as smooth strips over the blocky tiles: sidewalk/shoulder first, asphalt on top
    const MapData& map = world.map();
    Color asphalt = groundColour(Ground::Asphalt), walk = groundColour(Ground::Sidewalk), gravel = groundColour(Ground::Gravel);
    auto strip = [&](Vec2 a, Vec2 b, float hw, float y, Color c) {
        Vec2 d = (b - a).normalized();
        Vec2 n{-d.y * hw, d.x * hw};
        mb.quad(V(a.x - n.x, y, a.y - n.y), V(a.x + n.x, y, a.y + n.y), V(b.x + n.x, y, b.y + n.y), V(b.x - n.x, y, b.y - n.y), c);
    };
    auto disc = [&](Vec2 c, float r, float y, Color col) {
        const int n = 10;
        for (int i = 0; i < n; ++i) {
            float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
            mb.tri(V(c.x, y, c.y), V(c.x + std::cos(a1) * r, y, c.y + std::sin(a1) * r), V(c.x + std::cos(a0) * r, y, c.y + std::sin(a0) * r), col);
        }
    };
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& s : map.roads()) {
            float pad = s.halfWidth + 3;
            float minx = std::min(s.a.x, s.b.x) - pad, maxx = std::max(s.a.x, s.b.x) + pad;
            float miny = std::min(s.a.y, s.b.y) - pad, maxy = std::max(s.a.y, s.b.y) + pad;
            if (maxx < x0 || minx > x0 + kChunkSize || maxy < y0 || miny > y0 + kChunkSize) continue;
            if ((s.b - s.a).lengthSq() < 0.01f) continue;
            Vec2 mid = (s.a + s.b) * 0.5f;
            if (pass == 0) {
                // Edge: sidewalk in town, gravel shoulder outside
                Ground g = world.tile((int)std::floor(mid.x + (s.b - s.a).normalized().y * (s.halfWidth + 1.0f)),
                                      (int)std::floor(mid.y - (s.b - s.a).normalized().x * (s.halfWidth + 1.0f))).ground;
                bool town = g == Ground::Sidewalk;
                float extra = s.cls == RoadClass::Motorway ? 1.6f : town ? 1.8f : 0.8f;
                strip(s.a, s.b, s.halfWidth + extra, 0.004f, town ? walk : gravel);
                disc(s.a, s.halfWidth + extra, 0.004f, town ? walk : gravel);
                disc(s.b, s.halfWidth + extra, 0.004f, town ? walk : gravel);
            } else {
                strip(s.a, s.b, s.halfWidth, 0.008f, asphalt);
                disc(s.a, s.halfWidth, 0.008f, asphalt);
                disc(s.b, s.halfWidth, 0.008f, asphalt);
            }
        }
}

void Renderer::addRoadMarkings(MeshBuilder& mb, const MapData& map, int x0, int y0) {
    const float y = 0.014f;
    Color white = hexColor(0xe6e4dc), yellow = hexColor(0xd8b840);
    auto inChunk = [&](Vec2 p) { return p.x >= x0 && p.y >= y0 && p.x < x0 + kChunkSize && p.y < y0 + kChunkSize; };
    auto stripe = [&](Vec2 a, Vec2 b, float w, Color c) {
        Vec2 d = (b - a).normalized();
        Vec2 n{-d.y * w * 0.5f, d.x * w * 0.5f};
        mb.quad(V(a.x - n.x, y, a.y - n.y), V(a.x + n.x, y, a.y + n.y), V(b.x + n.x, y, b.y + n.y),
                V(b.x - n.x, y, b.y - n.y), c);
    };
    for (const auto& s : map.roads()) {
        float minx = std::min(s.a.x, s.b.x), maxx = std::max(s.a.x, s.b.x);
        float miny = std::min(s.a.y, s.b.y), maxy = std::max(s.a.y, s.b.y);
        if (maxx < x0 - 12 || minx > x0 + kChunkSize + 12 || maxy < y0 - 12 || miny > y0 + kChunkSize + 12) continue;
        Vec2 d = (s.b - s.a);
        float len = d.length();
        if (len < 0.5f) continue;
        d = d * (1.0f / len);
        Vec2 n{-d.y, d.x};
        float phase = std::fmod(std::fabs(s.a.x * 0.37f + s.a.y * 0.61f), 9.0f);
        if (s.cls == RoadClass::Major || s.cls == RoadClass::Motorway) {
            float dash = s.cls == RoadClass::Major ? 3.0f : 4.5f, gap = s.cls == RoadClass::Major ? 6.0f : 9.0f;
            for (float t = phase; t + dash < len; t += dash + gap) {
                Vec2 a = s.a + d * t, b = s.a + d * (t + dash);
                if (!inChunk((a + b) * 0.5f)) continue;
                if (s.cls == RoadClass::Major) stripe(a, b, 0.15f, white);
                else {
                    stripe(a + n * 5.5f, b + n * 5.5f, 0.15f, white);
                    stripe(a - n * 5.5f, b - n * 5.5f, 0.15f, white);
                }
            }
            if (s.cls == RoadClass::Motorway) {
                // edge lines and the central barrier strip, in 2 m pieces so they clip to the chunk
                for (float t = 0; t < len; t += 2.0f) {
                    float t1 = std::min(len, t + 2.0f);
                    Vec2 a = s.a + d * t, b = s.a + d * t1;
                    if (!inChunk((a + b) * 0.5f)) continue;
                    stripe(a + n * (s.halfWidth - 0.6f), b + n * (s.halfWidth - 0.6f), 0.2f, white);
                    stripe(a - n * (s.halfWidth - 0.6f), b - n * (s.halfWidth - 0.6f), 0.2f, white);
                    stripe(a + n * 0.4f, b + n * 0.4f, 0.12f, yellow);
                    stripe(a - n * 0.4f, b - n * 0.4f, 0.12f, yellow);
                    // barrier
                    Vec2 m0 = a, m1 = b;
                    mb.quad(V(m0.x, 0.75f, m0.y), V(m1.x, 0.75f, m1.y), V(m1.x, 0.75f, m1.y), V(m0.x, 0.75f, m0.y), white);
                    Vec2 off = n * 0.05f;
                    mb.quad(V(m0.x + off.x, 0, m0.y + off.y), V(m1.x + off.x, 0, m1.y + off.y), V(m1.x + off.x, 0.75f, m1.y + off.y),
                            V(m0.x + off.x, 0.75f, m0.y + off.y), hexColor(0xb0b0b0));
                    mb.quad(V(m1.x - off.x, 0, m1.y - off.y), V(m0.x - off.x, 0, m0.y - off.y), V(m0.x - off.x, 0.75f, m0.y - off.y),
                            V(m1.x - off.x, 0.75f, m1.y - off.y), hexColor(0xa0a0a0));
                }
            }
        }
    }
    // Railway: sleepers and rails
    for (const auto& s : map.rails()) {
        float minx = std::min(s.a.x, s.b.x), maxx = std::max(s.a.x, s.b.x);
        float miny = std::min(s.a.y, s.b.y), maxy = std::max(s.a.y, s.b.y);
        if (maxx < x0 - 4 || minx > x0 + kChunkSize + 4 || maxy < y0 - 4 || miny > y0 + kChunkSize + 4) continue;
        Vec2 d = s.b - s.a;
        float len = d.length();
        d = d * (1.0f / len);
        Vec2 n{-d.y, d.x};
        for (float t = 0; t < len; t += 0.65f) {
            Vec2 c = s.a + d * t;
            if (!inChunk(c)) continue;
            Vec2 a = c - n * 1.3f, b = c + n * 1.3f;
            Vec2 w = d * 0.12f;
            mb.quad(V(a.x - w.x, 0.05f, a.y - w.y), V(a.x + w.x, 0.05f, a.y + w.y), V(b.x + w.x, 0.05f, b.y + w.y),
                    V(b.x - w.x, 0.05f, b.y - w.y), hexColor(0x4a3a2c));
        }
        for (float t = 0; t < len; t += 2.0f) {
            float t1 = std::min(len, t + 2.0f);
            Vec2 a = s.a + d * t, b = s.a + d * t1;
            if (!inChunk((a + b) * 0.5f)) continue;
            for (float side : {-0.72f, 0.72f}) {
                Vec2 o = n * side;
                Vec2 hw = n * 0.04f;
                mb.quad(V(a.x + o.x - hw.x, 0.14f, a.y + o.y - hw.y), V(a.x + o.x + hw.x, 0.14f, a.y + o.y + hw.y),
                        V(b.x + o.x + hw.x, 0.14f, b.y + o.y + hw.y), V(b.x + o.x - hw.x, 0.14f, b.y + o.y - hw.y),
                        hexColor(0x9a9aa0));
            }
        }
    }
}

void Renderer::addProp(MeshBuilder& mb, World& world, const Tile& t, int x, int y, int doy) {
    float r1 = (t.variant & 15) / 15.0f, r2 = ((t.variant >> 4) & 15) / 15.0f;
    float cx = x + 0.5f, cz = y + 0.5f;
    if (isTree(t.prop) || t.prop == Prop::Shrub || t.prop == Prop::Boulder || t.prop == Prop::Rock) {
        cx += (r1 - 0.5f) * 0.4f;
        cz += (r2 - 0.5f) * 0.4f;
    }
    float h = propHeight(t.prop);
    uint32_t seed = hash2(x, y, 99);
    // furniture faces into the room; its back is against the wall
    auto backOffset = [&](float amount) {
        int f = t.variant & 3;
        Vec2 dir = f == 0 ? Vec2{0, -1} : f == 1 ? Vec2{1, 0} : f == 2 ? Vec2{0, 1} : Vec2{-1, 0};
        return Vec2{cx - dir.x * amount, cz - dir.y * amount};
    };
    auto fbox = [&](float w, float hh, float d, Color c, float back = 0.15f, float y0 = 0) {
        Vec2 p = backOffset(back);
        int f = t.variant & 3;
        float hw = (f == 1 || f == 3) ? d * 0.5f : w * 0.5f, hd = (f == 1 || f == 3) ? w * 0.5f : d * 0.5f;
        mb.box(V(p.x - hw, y0, p.y - hd), V(p.x + hw, y0 + hh, p.y + hd), c);
    };
    switch (t.prop) {
        case Prop::Spruce: {
            float th = h * (0.75f + 0.5f * r1);
            mb.cylinder(V(cx, 0, cz), 0.16f, th * 0.3f, 5, hexColor(0x4a3626), false);
            Color g = shade(hexColor(0x1f3b25), 0.85f + 0.3f * r2);
            float rad = 1.5f + 0.5f * r2;
            mb.cone(V(cx, th * 0.12f, cz), rad, th * 0.45f, 7, g);
            mb.cone(V(cx, th * 0.38f, cz), rad * 0.78f, th * 0.4f, 7, shade(g, 1.05f));
            mb.cone(V(cx, th * 0.62f, cz), rad * 0.52f, th * 0.38f, 6, shade(g, 1.1f));
            break;
        }
        case Prop::Pine: {
            float th = h * (0.75f + 0.5f * r1);
            mb.cylinder(V(cx, 0, cz), 0.19f, th * 0.72f, 5, hexColor(0x8a5634), false);
            Color g = shade(hexColor(0x2d4a2b), 0.85f + 0.3f * r2);
            mb.blob(V(cx, th * 0.78f, cz), V(2.0f, 1.3f, 2.0f), 7, 4, g, seed, 0.35f);
            mb.blob(V(cx + 0.6f, th * 0.66f, cz - 0.4f), V(1.2f, 0.8f, 1.2f), 6, 3, shade(g, 0.9f), seed + 1, 0.3f);
            break;
        }
        case Prop::Birch: {
            float th = h * (0.75f + 0.5f * r1);
            mb.cylinder(V(cx, 0, cz), 0.13f, th * 0.6f, 5, hexColor(0xe2ded4), false);
            mb.cylinder(V(cx, th * 0.12f, cz), 0.135f, 0.15f, 5, hexColor(0x303030), false);
            mb.cylinder(V(cx, th * 0.3f, cz), 0.135f, 0.12f, 5, hexColor(0x303030), false);
            Color g = leafColour(Prop::Birch, doy, t.variant);
            float fall = std::clamp((doy - 290) / 25.0f, 0.0f, 0.7f);
            mb.blob(V(cx, th * 0.68f, cz), V(1.6f * (1 - fall * 0.5f), th * 0.3f, 1.6f * (1 - fall * 0.5f)), 7, 4, g, seed, 0.4f);
            break;
        }
        case Prop::Oak: case Prop::Maple: {
            float th = h * (0.75f + 0.5f * r1);
            mb.cylinder(V(cx, 0, cz), t.prop == Prop::Oak ? 0.3f : 0.24f, th * 0.45f, 6, hexColor(0x54402e), false);
            Color g = leafColour(t.prop, doy, t.variant);
            mb.blob(V(cx, th * 0.62f, cz), V(2.6f, th * 0.35f, 2.6f), 8, 5, g, seed, 0.35f);
            break;
        }
        case Prop::Rowan: {
            float th = h * (0.75f + 0.5f * r1);
            mb.cylinder(V(cx, 0, cz), 0.12f, th * 0.5f, 5, hexColor(0x5a4a3a), false);
            Color g = leafColour(Prop::Rowan, doy, t.variant);
            mb.blob(V(cx, th * 0.65f, cz), V(1.4f, th * 0.3f, 1.4f), 6, 4, g, seed, 0.4f);
            for (int i = 0; i < 4; ++i)
                mb.box(V(cx - 1 + i * 0.6f, th * 0.5f, cz + 0.8f - i * 0.4f), V(cx - 0.8f + i * 0.6f, th * 0.5f + 0.2f, cz + 1.0f - i * 0.4f), hexColor(0xd03020));
            break;
        }
        case Prop::Shrub:
            mb.blob(V(cx, 0.55f, cz), V(0.75f, 0.6f, 0.75f), 6, 3, shade(hexColor(0x4a6634), 0.85f + 0.3f * r1), seed, 0.4f);
            break;
        case Prop::Hedge:
            mb.box(V(x + 0.02f, 0, y + 0.02f), V(x + 0.98f, 1.4f, y + 0.98f), shade(hexColor(0x2f5a2c), 0.9f + 0.2f * r1));
            break;
        case Prop::LingonBush: case Prop::BlueberryBush: {
            bool lingon = t.prop == Prop::LingonBush;
            Color g = lingon ? hexColor(0x2e5a2a) : hexColor(0x4a6a34);
            mb.blob(V(cx, 0.08f, cz), V(0.45f, lingon ? 0.16f : 0.26f, 0.45f), 6, 3, g, seed, 0.4f);
            bool season = lingon ? (doy >= 222 && doy <= 319) : (doy >= 182 && doy <= 262);
            if (season && !world.harvested(x, y, 200)) {
                Color b = lingon ? hexColor(0xd02030) : hexColor(0x30387a);
                for (int i = 0; i < 5; ++i) {
                    float a = i * 1.3f + r1 * 3;
                    float bx = cx + std::cos(a) * 0.25f, bz = cz + std::sin(a) * 0.25f;
                    mb.box(V(bx - 0.04f, 0.18f, bz - 0.04f), V(bx + 0.04f, 0.26f, bz + 0.04f), b);
                }
            }
            break;
        }
        case Prop::RoseBush: {
            mb.blob(V(cx, 0.65f, cz), V(0.7f, 0.65f, 0.7f), 6, 3, hexColor(0x4e5a2e), seed, 0.45f);
            if (doy >= 228 && !world.harvested(x, y, 200))
                for (int i = 0; i < 6; ++i) {
                    float a = i * 1.1f + r2 * 3;
                    float bx = cx + std::cos(a) * 0.6f, bz = cz + std::sin(a) * 0.6f, by = 0.5f + (i % 3) * 0.25f;
                    mb.box(V(bx - 0.05f, by, bz - 0.05f), V(bx + 0.05f, by + 0.1f, bz + 0.05f), hexColor(0xe04818));
                }
            break;
        }
        case Prop::Rock:
            mb.blob(V(cx, 0.1f, cz), V(0.35f, 0.25f, 0.3f), 5, 3, shade(hexColor(0x85857f), 0.85f + 0.3f * r1), seed, 0.4f);
            break;
        case Prop::Boulder:
            mb.blob(V(cx, 0.5f, cz), V(1.1f, 0.95f, 0.95f), 7, 4, shade(hexColor(0x8a8c84), 0.85f + 0.25f * r1), seed, 0.35f);
            break;
        case Prop::Stump:
            mb.cylinder(V(cx, 0, cz), 0.3f, 0.4f, 6, hexColor(0x6a5038));
            break;
        case Prop::Reeds:
            for (int i = 0; i < 5; ++i) {
                float bx = x + 0.15f + 0.17f * i, bz = y + 0.2f + 0.6f * ((i * 7 + t.variant) % 5) / 5.0f;
                mb.box(V(bx, -0.2f, bz), V(bx + 0.05f, 1.2f + 0.3f * ((i + t.variant) % 3) / 2.0f, bz + 0.05f), hexColor(0x9a9a58));
            }
            break;
        case Prop::Fence: {
            Color c = hexColor(0xe6e2d8);
            bool alongZ = t.variant == 1;
            if (alongZ) {
                mb.box(V(cx - 0.05f, 0, y + 0.0f), V(cx + 0.05f, 1.0f, y + 0.1f), c);
                mb.box(V(cx - 0.03f, 0.35f, (float)y), V(cx + 0.03f, 0.45f, y + 1.0f), c);
                mb.box(V(cx - 0.03f, 0.75f, (float)y), V(cx + 0.03f, 0.85f, y + 1.0f), c);
            } else {
                mb.box(V(x + 0.0f, 0, cz - 0.05f), V(x + 0.1f, 1.0f, cz + 0.05f), c);
                mb.box(V((float)x, 0.35f, cz - 0.03f), V(x + 1.0f, 0.45f, cz + 0.03f), c);
                mb.box(V((float)x, 0.75f, cz - 0.03f), V(x + 1.0f, 0.85f, cz + 0.03f), c);
            }
            break;
        }
        case Prop::Lamppost:
            mb.cylinder(V(cx, 0, cz), 0.07f, 5.0f, 5, hexColor(0x3a3c40));
            mb.box(V(cx - 0.25f, 4.9f, cz - 0.15f), V(cx + 0.25f, 5.1f, cz + 0.15f), hexColor(0x2a2c30));
            break;
        case Prop::Bench:
            mb.box(V(x + 0.05f, 0.4f, cz - 0.2f), V(x + 0.95f, 0.47f, cz + 0.2f), hexColor(0x8a6a44));
            mb.box(V(x + 0.05f, 0.47f, cz + 0.15f), V(x + 0.95f, 0.85f, cz + 0.2f), hexColor(0x8a6a44));
            mb.box(V(x + 0.1f, 0, cz - 0.2f), V(x + 0.16f, 0.4f, cz + 0.2f), hexColor(0x303030));
            mb.box(V(x + 0.84f, 0, cz - 0.2f), V(x + 0.9f, 0.4f, cz + 0.2f), hexColor(0x303030));
            break;
        case Prop::Mailbox:
            mb.box(V(cx - 0.04f, 0, cz - 0.04f), V(cx + 0.04f, 0.9f, cz + 0.04f), hexColor(0x5a5a5a));
            mb.box(V(cx - 0.2f, 0.9f, cz - 0.15f), V(cx + 0.2f, 1.15f, cz + 0.15f), hexColor(0x2a6a9a));
            break;
        case Prop::Car: {
            static const uint32_t cols[] = {0xa02020, 0x203a6a, 0xd0d0d0, 0x303030, 0x6a7a80, 0x2a5a3a, 0xe0e0e0};
            Color c = hexColor(cols[t.variant % 7]);
            float yaw = (t.variant & 1) ? PI * 0.5f : 0.0f;
            yaw += (r2 - 0.5f) * 0.3f;
            mb.orientedBox(V(cx, 0.55f, cz), V(2.1f, 0.35f, 0.88f), yaw, c);
            mb.orientedBox(V(cx - std::cos(yaw) * 0.2f, 1.1f, cz - std::sin(yaw) * 0.2f), V(1.2f, 0.25f, 0.8f), yaw, hexColor(0x404850));
            for (int i = 0; i < 4; ++i) {
                float ox = (i < 2 ? 1.3f : -1.3f), oz = (i % 2 ? 0.8f : -0.8f);
                float wx = cx + ox * std::cos(yaw) - oz * std::sin(yaw), wz = cz + ox * std::sin(yaw) + oz * std::cos(yaw);
                mb.box(V(wx - 0.3f, 0, wz - 0.12f), V(wx + 0.3f, 0.45f, wz + 0.12f), hexColor(0x1a1a1a));
            }
            break;
        }
        case Prop::Bin:
            mb.box(V(cx - 0.3f, 0, cz - 0.35f), V(cx + 0.3f, 1.05f, cz + 0.35f), hexColor(0x2a5a32));
            break;
        case Prop::HayBale:
            mb.cylinder(V(cx, 0, cz), 0.65f, 1.2f, 8, hexColor(0xe8e8e0));
            break;
        case Prop::Woodpile:
            mb.box(V(x + 0.05f, 0, y + 0.2f), V(x + 0.95f, 0.95f, y + 0.8f), hexColor(0x8a6a48));
            mb.box(V(x + 0.03f, 0.95f, y + 0.15f), V(x + 0.97f, 1.05f, y + 0.85f), hexColor(0x3a3a3a));
            break;
        case Prop::RainBarrel:
            mb.cylinder(V(cx, 0, cz), 0.35f, 0.9f, 8, hexColor(0x2a4a32));
            break;
        case Prop::Barrier:
            mb.box(V((float)x, 0, cz - 0.1f), V(x + 1.0f, 0.8f, cz + 0.1f), hexColor(0xc0c0c0));
            break;
        // Furniture -----------------------------------------------------------
        case Prop::Fridge: fbox(0.7f, 1.8f, 0.68f, hexColor(0xeeeeea)); break;
        case Prop::Freezer: fbox(0.85f, 0.9f, 0.65f, hexColor(0xe6e6e2)); break;
        case Prop::Counter:
            fbox(1.0f, 0.86f, 0.62f, hexColor(0xd8d0c0));
            fbox(1.0f, 0.04f, 0.64f, hexColor(0x6a5a4a), 0.15f, 0.86f);
            fbox(1.0f, 0.6f, 0.35f, hexColor(0xd8d0c0), 0.3f, 1.4f);   // wall cupboard
            break;
        case Prop::Stove:
            fbox(0.6f, 0.86f, 0.62f, hexColor(0xf0f0f0));
            fbox(0.6f, 0.04f, 0.62f, hexColor(0x202020), 0.15f, 0.86f);
            break;
        case Prop::Sink:
            fbox(1.0f, 0.86f, 0.62f, hexColor(0xd8d0c0));
            fbox(1.0f, 0.05f, 0.64f, hexColor(0xb0b4b8), 0.15f, 0.86f);
            break;
        case Prop::Table:
            mb.box(V(x + 0.05f, 0.7f, y + 0.05f), V(x + 0.95f, 0.76f, y + 0.95f), hexColor(0x9a7650));
            mb.box(V(x + 0.1f, 0, y + 0.1f), V(x + 0.18f, 0.7f, y + 0.18f), hexColor(0x7a5a3a));
            mb.box(V(x + 0.82f, 0, y + 0.82f), V(x + 0.9f, 0.7f, y + 0.9f), hexColor(0x7a5a3a));
            mb.box(V(x + 0.1f, 0, y + 0.82f), V(x + 0.18f, 0.7f, y + 0.9f), hexColor(0x7a5a3a));
            mb.box(V(x + 0.82f, 0, y + 0.1f), V(x + 0.9f, 0.7f, y + 0.18f), hexColor(0x7a5a3a));
            break;
        case Prop::Chair:
            mb.box(V(cx - 0.22f, 0.42f, cz - 0.22f), V(cx + 0.22f, 0.47f, cz + 0.22f), hexColor(0x8a6a48));
            fbox(0.45f, 0.5f, 0.06f, hexColor(0x8a6a48), 0.2f, 0.47f);
            mb.box(V(cx - 0.2f, 0, cz - 0.2f), V(cx - 0.15f, 0.42f, cz - 0.15f), hexColor(0x6a4a30));
            mb.box(V(cx + 0.15f, 0, cz + 0.15f), V(cx + 0.2f, 0.42f, cz + 0.2f), hexColor(0x6a4a30));
            break;
        case Prop::Sofa: {
            static const uint32_t cols[] = {0x5a6a7a, 0x7a5a4a, 0x4a5a3a, 0x8a8a80};
            Color c = hexColor(cols[seed % 4]);
            fbox(0.96f, 0.45f, 0.85f, c, 0.05f);
            fbox(0.96f, 0.4f, 0.22f, shade(c, 0.85f), 0.35f, 0.45f);
            break;
        }
        case Prop::Bed: {
            static const uint32_t cols[] = {0xb04040, 0x4060a0, 0x60a070, 0xd0c080, 0x8080a0};
            mb.box(V(x + 0.05f, 0, y + 0.03f), V(x + 0.95f, 0.38f, y + 0.97f), hexColor(0x8a6a48));
            mb.box(V(x + 0.07f, 0.38f, y + 0.05f), V(x + 0.93f, 0.5f, y + 0.95f), hexColor(cols[seed % 5]));
            fbox(0.7f, 0.1f, 0.25f, hexColor(0xf0f0f0), 0.3f, 0.5f);
            break;
        }
        case Prop::Wardrobe: fbox(0.95f, 2.0f, 0.58f, hexColor(0x9a7a56), 0.2f); break;
        case Prop::Dresser: fbox(0.9f, 0.95f, 0.48f, hexColor(0xa88660), 0.25f); break;
        case Prop::Bookshelf: {
            fbox(0.95f, 1.9f, 0.34f, hexColor(0x7a5a3a), 0.32f);
            static const uint32_t books[] = {0xa03030, 0x30508a, 0x3a7a3a, 0xc0a040, 0x6a3a7a};
            for (int s = 0; s < 4; ++s) fbox(0.85f, 0.25f, 0.3f, hexColor(books[(seed + s) % 5]), 0.3f, 0.15f + s * 0.45f);
            break;
        }
        case Prop::Toilet:
            fbox(0.4f, 0.42f, 0.6f, hexColor(0xf4f4f4), 0.15f);
            fbox(0.42f, 0.4f, 0.18f, hexColor(0xf4f4f4), 0.38f, 0.42f);
            break;
        case Prop::Bathtub: mb.box(V(x + 0.05f, 0, y + 0.05f), V(x + 0.95f, 0.58f, y + 0.95f), hexColor(0xf2f2f2)); break;
        case Prop::BathroomCabinet: fbox(0.6f, 0.6f, 0.2f, hexColor(0xf0f0f0), 0.38f, 1.1f); fbox(0.5f, 0.85f, 0.4f, hexColor(0xe0e0e0), 0.3f); break;
        case Prop::Shelf: {
            fbox(0.95f, 1.7f, 0.6f, hexColor(0x9aa0a6), 0.18f);
            static const uint32_t goods[] = {0xc04040, 0x40a040, 0xd0b040, 0x4060c0, 0xe08030};
            for (int s = 0; s < 3; ++s) fbox(0.85f, 0.22f, 0.5f, hexColor(goods[(seed + s) % 5]), 0.18f, 0.2f + s * 0.52f);
            break;
        }
        case Prop::Cooler: fbox(0.95f, 1.9f, 0.7f, hexColor(0xcfe0ea), 0.12f); break;
        case Prop::Checkout: fbox(0.95f, 0.95f, 0.8f, hexColor(0x5a5a60)); break;
        case Prop::Desk:
            fbox(0.95f, 0.05f, 0.65f, hexColor(0xb09a78), 0.12f, 0.72f);
            fbox(0.35f, 0.72f, 0.6f, hexColor(0x9a8464), 0.12f);
            break;
        case Prop::Locker: fbox(0.9f, 1.9f, 0.5f, hexColor(0x5070a0), 0.22f); break;
        case Prop::Pew: fbox(0.98f, 0.45f, 0.45f, hexColor(0x7a5a3a)); fbox(0.98f, 0.5f, 0.08f, hexColor(0x7a5a3a), 0.35f, 0.45f); break;
        case Prop::MedicalCabinet: fbox(0.9f, 1.8f, 0.45f, hexColor(0xf0f0f0), 0.25f); break;
        case Prop::GunCabinet: fbox(0.6f, 1.7f, 0.45f, hexColor(0x2e3a30), 0.25f); break;
        case Prop::Workbench: fbox(0.98f, 0.9f, 0.7f, hexColor(0x8a6a44), 0.12f); break;
        case Prop::Altar: fbox(0.98f, 1.0f, 0.6f, hexColor(0xe8e0d0)); break;
        case Prop::HospitalBed:
            mb.box(V(x + 0.1f, 0, y + 0.05f), V(x + 0.9f, 0.6f, y + 0.95f), hexColor(0xc0c4c8));
            mb.box(V(x + 0.12f, 0.6f, y + 0.07f), V(x + 0.88f, 0.7f, y + 0.93f), hexColor(0xe8f0f4));
            break;
        default:
            if (h > 0) mb.box(V(x + 0.2f, 0, y + 0.2f), V(x + 0.8f, h, y + 0.8f), GRAY);
            break;
    }
}

void Renderer::buildChunk(World& world, Chunk& c, ChunkGfx& g) {
    unloadChunk(g);
    MeshBuilder ground, props;
    int x0 = c.cx * kChunkSize, y0 = c.cy * kChunkSize;
    int doy = world.clock().dayOfYear();
    for (int ly = 0; ly < kChunkSize; ++ly)
        for (int lx = 0; lx < kChunkSize; ++lx) {
            const Tile& t = c.at(lx, ly);
            int x = x0 + lx, y = y0 + ly;
            float h = groundHeight(t.ground);
            Color col = groundColour(t.ground);
            if (t.ground == Ground::Asphalt) col = groundColour(t.building >= 0 ? Ground::FloorConcrete : Ground::Gravel);
            float n = fbm(x * 0.15f, y * 0.15f, 3, 2);
            float jitter = 0.93f + 0.1f * ((t.variant & 7) / 7.0f) + (n - 0.5f) * 0.12f;
            if (t.ground == Ground::Cobble || t.ground == Ground::FloorTile) jitter *= ((x + y) & 1) ? 1.0f : 0.94f;
            if (t.ground == Ground::FloorWood) jitter *= (y & 1) ? 1.0f : 0.95f;
            if (t.ground == Ground::Grass && t.building < 0) col = mixColor(col, hexColor(0x8a8a48), std::clamp((doy - 260) / 60.0f, 0.0f, 0.5f));
            col = shade(col, jitter);
            ground.quad(V((float)x, h, (float)y), V((float)x, h, y + 1.0f), V(x + 1.0f, h, y + 1.0f), V(x + 1.0f, h, (float)y), col);
            // Banks where land meets water
            if (!isWater(t.ground)) {
                Color bank = hexColor(0x5a4a36);
                auto edge = [&](int nx, int ny, Vector3 a, Vector3 b) {
                    const Tile& o = (nx >= x0 && ny >= y0 && nx < x0 + kChunkSize && ny < y0 + kChunkSize)
                                        ? c.at(nx - x0, ny - y0) : world.tile(nx, ny);
                    if (!isWater(o.ground)) return;
                    float oh = groundHeight(o.ground);
                    ground.quad(a, V(a.x, oh, a.z), V(b.x, oh, b.z), b, bank);
                };
                edge(x, y - 1, V(x + 1.0f, 0, (float)y), V((float)x, 0, (float)y));
                edge(x, y + 1, V((float)x, 0, y + 1.0f), V(x + 1.0f, 0, y + 1.0f));
                edge(x - 1, y, V((float)x, 0, (float)y), V((float)x, 0, y + 1.0f));
                edge(x + 1, y, V(x + 1.0f, 0, y + 1.0f), V(x + 1.0f, 0, (float)y));
            }
            if (t.prop != Prop::None) addProp(props, world, t, x, y, doy);
        }
    addRoadSurfaces(ground, world, x0, y0);
    addRoadMarkings(props, world.map(), x0, y0);
    g.ground = ground.build();
    g.hasGround = true;
    if (!props.empty()) { g.props = props.build(); g.hasProps = true; }
    g.version = c.version;
}

// ---------------------------------------------------------------------------
// Buildings
// ---------------------------------------------------------------------------

void Renderer::buildBuilding(World& world, const Building& b, BuildingGfx& g) {
    MeshBuilder walls, roof;
    Color ext = hexColor(b.wallColour);
    Color trim = hexColor(b.trimColour);
    static const uint32_t interiors[] = {0xe8e4da, 0xdcd4c4, 0xc8d0bc, 0xe0d8c8, 0xd4dce0};
    Color inner = hexColor(interiors[b.seed % 5]);
    Color glass = hexColor(0x5a7080);
    Color plinth = hexColor(0x6a6a66);
    float top = b.floors * kStorey;
    bool wooden = b.type == BuildingType::House || b.type == BuildingType::Cottage || b.type == BuildingType::Farmhouse ||
                  b.type == BuildingType::Barn || b.type == BuildingType::Shed || b.type == BuildingType::Shop ||
                  b.type == BuildingType::Hostel || b.type == BuildingType::Manor;
    const IRect& R = b.rect;

    // One wall segment along an edge. negCol faces -z (or -x), posCol faces +z (or +x).
    MeshBuilder* cur = &walls;
    auto segment = [&](float ax, float az, bool alongX, float y0, float y1, Color negCol, Color posCol, float ext0 = 0, float ext1 = 0) {
        MeshBuilder& walls = *cur;
        if (y1 <= y0) return;
        float h = kWallT * 0.5f;
        Vector3 mn, mx;
        if (alongX) { mn = V(ax - ext0, y0, az - h); mx = V(ax + 1 + ext1, y1, az + h); }
        else { mn = V(ax - h, y0, az - ext0); mx = V(ax + h, y1, az + 1 + ext1); }
        // faces
        Color topC = shade(negCol, 0.8f);
        walls.quad(V(mn.x, mx.y, mn.z), V(mn.x, mx.y, mx.z), V(mx.x, mx.y, mx.z), V(mx.x, mx.y, mn.z), topC);
        if (alongX) {
            walls.quad(V(mn.x, mn.y, mx.z), V(mx.x, mn.y, mx.z), V(mx.x, mx.y, mx.z), V(mn.x, mx.y, mx.z), posCol);
            walls.quad(V(mx.x, mn.y, mn.z), V(mn.x, mn.y, mn.z), V(mn.x, mx.y, mn.z), V(mx.x, mx.y, mn.z), negCol);
            walls.quad(V(mx.x, mn.y, mx.z), V(mx.x, mn.y, mn.z), V(mx.x, mx.y, mn.z), V(mx.x, mx.y, mx.z), shade(negCol, 0.9f));
            walls.quad(V(mn.x, mn.y, mn.z), V(mn.x, mn.y, mx.z), V(mn.x, mx.y, mx.z), V(mn.x, mx.y, mn.z), shade(negCol, 0.9f));
        } else {
            walls.quad(V(mx.x, mn.y, mx.z), V(mx.x, mn.y, mn.z), V(mx.x, mx.y, mn.z), V(mx.x, mx.y, mx.z), posCol);
            walls.quad(V(mn.x, mn.y, mn.z), V(mn.x, mn.y, mx.z), V(mn.x, mx.y, mx.z), V(mn.x, mx.y, mn.z), negCol);
            walls.quad(V(mn.x, mn.y, mx.z), V(mx.x, mn.y, mx.z), V(mx.x, mx.y, mx.z), V(mn.x, mx.y, mx.z), shade(negCol, 0.9f));
            walls.quad(V(mx.x, mn.y, mn.z), V(mn.x, mn.y, mn.z), V(mn.x, mx.y, mn.z), V(mx.x, mx.y, mn.z), shade(negCol, 0.9f));
        }
    };

    auto buildWalls = [&](MeshBuilder& walls, float top) {
    cur = &walls;
    int edgeIndex = 0;
    for (const auto& e : b.edges) {
        for (int dir = 0; dir < 2; ++dir) {
            uint8_t wallBit = dir == 0 ? WallN : WallW;
            if (!(e.bits & wallBit)) continue;
            bool door = e.bits & (dir == 0 ? DoorN : DoorW);
            bool window = e.bits & (dir == 0 ? WindowN : WindowW);
            bool alongX = dir == 0;
            bool exterior = alongX ? (e.y == R.y || e.y == R.bottom()) : (e.x == R.x || e.x == R.right());
            // Which side is outside? For the north/west wall outside is the negative side.
            bool outsideNeg = alongX ? e.y == R.y : e.x == R.x;
            Color negCol = exterior ? (outsideNeg ? ext : inner) : inner;
            Color posCol = exterior ? (outsideNeg ? inner : ext) : inner;
            float ax = (float)e.x, az = (float)e.y;
            float height = exterior ? top : kInnerWall;
            float e0 = exterior ? kWallT * 0.5f : 0, e1 = e0;
            if (door) {
                segment(ax, az, alongX, 2.1f, height, negCol, posCol, e0, e1);
                // frame
                Color fr = exterior ? trim : shade(inner, 0.85f);
                if (alongX) {
                    walls.box(V(ax, 0, az - 0.09f), V(ax + 0.08f, 2.1f, az + 0.09f), fr);
                    walls.box(V(ax + 0.92f, 0, az - 0.09f), V(ax + 1, 2.1f, az + 0.09f), fr);
                } else {
                    walls.box(V(ax - 0.09f, 0, az), V(ax + 0.09f, 2.1f, az + 0.08f), fr);
                    walls.box(V(ax - 0.09f, 0, az + 0.92f), V(ax + 0.09f, 2.1f, az + 1), fr);
                }
            } else if (window) {
                segment(ax, az, alongX, 0, 0.9f, negCol, posCol, e0, e1);
                segment(ax, az, alongX, 2.0f, kStorey, negCol, posCol, e0, e1);
                Color g2 = glass;
                if (alongX) walls.box(V(ax + 0.08f, 0.9f, az - 0.02f), V(ax + 0.92f, 2.0f, az + 0.02f), g2);
                else walls.box(V(ax - 0.02f, 0.9f, az + 0.08f), V(ax + 0.02f, 2.0f, az + 0.92f), g2);
                // white frames (outside)
                float o = outsideNeg ? -0.08f : 0.08f;
                if (alongX) {
                    walls.box(V(ax + 0.02f, 0.85f, az + o - 0.03f), V(ax + 0.98f, 0.92f, az + o + 0.03f), trim);
                    walls.box(V(ax + 0.02f, 1.98f, az + o - 0.03f), V(ax + 0.98f, 2.06f, az + o + 0.03f), trim);
                } else {
                    walls.box(V(ax + o - 0.03f, 0.85f, az + 0.02f), V(ax + o + 0.03f, 0.92f, az + 0.98f), trim);
                    walls.box(V(ax + o - 0.03f, 1.98f, az + 0.02f), V(ax + o + 0.03f, 2.06f, az + 0.98f), trim);
                }
                if (exterior) segment(ax, az, alongX, kStorey, top, negCol, posCol, e0, e1);
            } else {
                segment(ax, az, alongX, 0, height, negCol, posCol, e0, e1);
            }
            // Upper floor windows on exterior walls
            if (exterior && top > kStorey + 0.5f && (edgeIndex % 3 == 1)) {
                for (int f = 1; f < b.floors; ++f) {
                    float y0 = f * kStorey + 0.9f, y1 = f * kStorey + 2.0f;
                    float o = outsideNeg ? -0.075f : 0.075f;
                    if (alongX) walls.box(V(ax + 0.15f, y0, az + o - 0.01f), V(ax + 0.85f, y1, az + o + 0.01f), glass);
                    else walls.box(V(ax + o - 0.01f, y0, az + 0.15f), V(ax + o + 0.01f, y1, az + 0.85f), glass);
                }
            }
            // Stone plinth outside
            if (exterior) {
                float o = outsideNeg ? -0.1f : 0.1f;
                if (alongX) walls.box(V(ax - 0.1f, 0, az + o - 0.04f), V(ax + 1.1f, 0.3f, az + o + 0.04f), plinth);
                else walls.box(V(ax + o - 0.04f, 0, az - 0.1f), V(ax + o + 0.04f, 0.3f, az + 1.1f), plinth);
            }
            ++edgeIndex;
        }
    }
    // Corner boards on wooden houses
    if (wooden) {
        float t2 = 0.12f;
        for (Vec2 c : {Vec2{(float)R.x, (float)R.y}, Vec2{(float)R.right(), (float)R.y}, Vec2{(float)R.x, (float)R.bottom()},
                       Vec2{(float)R.right(), (float)R.bottom()}})
            walls.box(V(c.x - t2, 0, c.y - t2), V(c.x + t2, top, c.y + t2), trim);
    }
    };
    MeshBuilder lowWalls;
    buildWalls(walls, top);
    buildWalls(lowWalls, kStorey);

    // Roof
    Vector3 mn = V((float)R.x, top, (float)R.y), mx = V((float)R.right(), top, (float)R.bottom());
    Color roofCol = hexColor(b.roofColour);
    float span = b.roofAlongX ? (float)R.h : (float)R.w;
    switch (b.roof) {
        case RoofKind::Gable:
            roof.gable(mn, mx, span * 0.42f, b.roofAlongX, roofCol, ext, 0.45f);
            if (b.type == BuildingType::House || b.type == BuildingType::Cottage || b.type == BuildingType::Farmhouse) {
                float chx = R.x + R.w * (b.roofAlongX ? 0.7f : 0.5f), chz = R.y + R.h * (b.roofAlongX ? 0.5f : 0.7f);
                roof.box(V(chx - 0.35f, top, chz - 0.35f), V(chx + 0.35f, top + span * 0.42f + 0.8f, chz + 0.35f), hexColor(0x5a2a22));
            }
            break;
        case RoofKind::Hip:
            roof.hip(mn, mx, span * 0.3f, roofCol, 0.4f);
            break;
        case RoofKind::Flat:
            roof.box(V(mn.x - 0.1f, top, mn.z - 0.1f), V(mx.x + 0.1f, top + 0.35f, mx.z + 0.1f), shade(roofCol, 0.9f));
            roof.box(V(mn.x + 0.4f, top + 0.35f, mn.z + 0.4f), V(mx.x - 0.4f, top + 0.4f, mx.z - 0.4f), roofCol);
            break;
    }
    if (b.type == BuildingType::Church) {
        // Tower with a spire at the west end
        float tx = (float)R.x - 2.5f, tz = R.y + R.h * 0.5f;
        walls.box(V(tx - 2.5f, 0, tz - 2.5f), V(tx + 2.5f, 20.0f, tz + 2.5f), ext);
        walls.box(V(tx - 2.6f, 15.5f, tz - 2.6f), V(tx + 2.6f, 16.0f, tz + 2.6f), trim);
        roof.cone(V(tx, 20.0f, tz), 3.4f, 13.0f, 8, hexColor(0x2a3a3a));
    }
    if (b.type == BuildingType::Station) {
        roof.box(V(R.x - 2.0f, 3.2f, R.bottom() + 0.0f), V(R.right() + 2.0f, 3.4f, R.bottom() + 4.0f), hexColor(0x5a3028));
    }
    if (!walls.empty()) { g.walls = walls.build(); g.hasWalls = true; }
    if (!lowWalls.empty()) { g.wallsLow = lowWalls.build(); g.hasWallsLow = true; }
    if (!roof.empty()) { g.roof = roof.build(); g.hasRoof = true; }
    g.day = 0;
    (void)world;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Renderer::setCutaway(bool on) {
    float v = on ? 1.0f : 0.0f;
    SetShaderValue(shader_, locCutaway_, &v, SHADER_UNIFORM_FLOAT);
}

void Renderer::setUnlit(bool on) {
    float v = on ? 1.0f : 0.0f;
    SetShaderValue(shader_, locUnlit_, &v, SHADER_UNIFORM_FLOAT);
}

void Renderer::setLighting(Game& game, float time) {
    World& w = game.world();
    const Clock& clock = w.clock();
    float day = clock.daylight();
    float elev = clock.sunElevationDeg();
    const Weather& wx = clock.weather();

    float hour = clock.hourOfDay();
    float az = (hour - 12.5f) / 12.0f * PI;   // sun moves east to west
    Vector3 sunDir = Vector3Normalize(V(-std::sin(az) * 0.8f, std::max(0.25f, std::sin(elev * DEG2RAD) + 0.3f), std::cos(az) * 0.6f - 0.4f));
    float warm = 1.0f - std::clamp((elev - 2.0f) / 18.0f, 0.0f, 1.0f);   // low sun is orange
    Vector3 sunCol = Vector3Scale(V(0.62f + 0.25f * warm, 0.58f, 0.5f - 0.18f * warm), day * (1.0f - 0.65f * wx.cloud));
    float moon = clock.moonlight() * (1.0f - day);
    Vector3 night = Vector3Add(V(0.05f, 0.06f, 0.1f), Vector3Scale(V(0.09f, 0.1f, 0.14f), moon));
    Vector3 amb = Vector3Lerp(night, V(0.46f, 0.48f, 0.52f), day);
    SetShaderValue(shader_, locSunDir_, &sunDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locSunColor_, &sunCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locAmbient_, &amb, SHADER_UNIFORM_VEC3);

    Vector3 pp = V(game.player().pos.x, 1.0f, game.player().pos.y);
    SetShaderValue(shader_, locPlayer_, &pp, SHADER_UNIFORM_VEC3);
    Vector3 view = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    SetShaderValue(shader_, locViewDir_, &view, SHADER_UNIFORM_VEC3);
    float cut = currentBuilding_ >= 0 ? 9.0f : 3.2f;
    SetShaderValue(shader_, locCutRadius_, &cut, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locTime_, &time, SHADER_UNIFORM_FLOAT);

    // Lights: campfires, flashlight, and eyes adjusting to the dark
    Vector4 lights[16];
    Vector4 cols[16];
    int n = 0;
    for (const auto& f : w.campfires()) {
        if (!f.lit || n >= 14) continue;
        if (distance(f.pos, game.player().pos) > 60) continue;
        float flicker = 0.85f + 0.15f * std::sin(time * 13.0f + f.id) * std::sin(time * 7.3f);
        lights[n] = {f.pos.x, 0.8f, f.pos.y, 11.0f * flicker};
        cols[n] = {1.0f, 0.55f, 0.22f, 1.6f};
        ++n;
    }
    const Player& p = game.player();
    if (p.flashlightOn) {
        Vec2 fwd{std::cos(p.facing), std::sin(p.facing)};
        lights[n] = {p.pos.x + fwd.x * 4.5f, 1.2f, p.pos.y + fwd.y * 4.5f, 9.0f};
        cols[n] = {0.95f, 0.92f, 0.8f, 1.3f};
        ++n;
    }
    float dark = 1.0f - day;
    if (dark > 0.3f) {
        lights[n] = {p.pos.x, 2.0f, p.pos.y, 13.0f};
        cols[n] = {0.35f, 0.4f, 0.55f, dark * 0.55f};
        ++n;
    }
    SetShaderValueV(shader_, locLights_, lights, SHADER_UNIFORM_VEC4, std::max(1, n));
    SetShaderValueV(shader_, locLightColors_, cols, SHADER_UNIFORM_VEC4, std::max(1, n));
    SetShaderValue(shader_, locLightCount_, &n, SHADER_UNIFORM_INT);

    // Sky / fog
    Color daySky = mixColor(hexColor(0x9fb4c4), hexColor(0x8a9096), wx.cloud);
    Color nightSky = hexColor(0x0a0d14);
    skyColor_ = mixColor(nightSky, daySky, day);
    Vector3 fogC = V(skyColor_.r / 255.0f, skyColor_.g / 255.0f, skyColor_.b / 255.0f);
    float fogStart = 70.0f - 40.0f * wx.rain - 55.0f * wx.fog;
    float fogEnd = 140.0f - 70.0f * wx.rain - 105.0f * wx.fog;
    SetShaderValue(shader_, locFogColor_, &fogC, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locFogStart_, &fogStart, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locFogEnd_, &fogEnd, SHADER_UNIFORM_FLOAT);
    setUnlit(false);
}

void Renderer::drawPrim(const Mesh& m, Vector3 pos, Vector3 scale, float yaw, Color c, float pitch, float roll) {
    Matrix t = MatrixScale(scale.x, scale.y, scale.z);
    if (pitch != 0) t = MatrixMultiply(t, MatrixRotateX(pitch));
    if (roll != 0) t = MatrixMultiply(t, MatrixRotateZ(roll));
    t = MatrixMultiply(t, MatrixRotateY(yaw));
    t = MatrixMultiply(t, MatrixTranslate(pos.x, pos.y, pos.z));
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = c;
    DrawMesh(m, mat_, t);
}

void Renderer::drawPlayer(Game& game, float time) {
    const Player& p = game.player();
    float yaw = -p.facing;
    Vector3 base = V(p.pos.x, 0, p.pos.y);
    auto col = [&](ClothingSlot s, uint32_t def) {
        const ItemInstance& it = p.worn[(size_t)s];
        return it.def ? hexColor(it.def->colour) : hexColor(def);
    };
    Color torso = col(ClothingSlot::Jacket, 0);
    if (!p.worn[(size_t)ClothingSlot::Jacket].def) torso = col(ClothingSlot::Sweater, 0);
    if (!p.worn[(size_t)ClothingSlot::Jacket].def && !p.worn[(size_t)ClothingSlot::Sweater].def) torso = col(ClothingSlot::Torso, 0xd8c8b0);
    Color legs = col(ClothingSlot::Legs, 0xd8b898);
    Color feet = col(ClothingSlot::Feet, 0xc8a080);
    Color skin = hexColor(0xe0b494);

    if (p.asleep) {
        drawPrim(cube_, V(base.x, 0.15f, base.z), V(1.0f, 0.25f, 0.42f), yaw, torso);
        drawPrim(sphere_, V(base.x + std::cos(p.facing) * 0.62f, 0.2f, base.z + std::sin(p.facing) * 0.62f), V(0.13f, 0.13f, 0.13f), yaw, skin);
        return;
    }
    float speed = p.running ? 11.0f : 7.0f;
    float phase = p.moving ? std::sin(time * speed) : 0.0f;
    float bob = p.moving ? std::fabs(std::sin(time * speed)) * 0.04f : 0.0f;
    Vec2 fwd{std::cos(p.facing), std::sin(p.facing)};
    Vec2 side{-fwd.y, fwd.x};
    auto at = [&](float f, float s) { return Vec2{p.pos.x + fwd.x * f + side.x * s, p.pos.y + fwd.y * f + side.y * s}; };
    float crouch = p.sneaking ? 0.82f : 1.0f;
    // legs
    for (int i = 0; i < 2; ++i) {
        float s = i ? 0.1f : -0.1f;
        Vec2 lp = at((i ? phase : -phase) * 0.16f, s);
        drawPrim(cube_, V(lp.x, 0.42f * crouch, lp.y), V(0.15f, 0.84f * crouch, 0.15f), yaw, legs);
        drawPrim(cube_, V(lp.x + fwd.x * 0.04f, 0.05f, lp.y + fwd.y * 0.04f), V(0.24f, 0.1f, 0.15f), yaw, feet);
    }
    float ty = (0.84f + 0.32f) * crouch + bob;
    drawPrim(cube_, V(p.pos.x, ty, p.pos.y), V(0.26f, 0.62f, 0.44f), yaw, torso);
    // arms
    for (int i = 0; i < 2; ++i) {
        float s = i ? 0.28f : -0.28f;
        float swing = (i ? -phase : phase) * 0.14f;
        if (i == 1 && p.swing > 0) swing = 0.3f * p.swing;
        Vec2 ap = at(swing, s);
        drawPrim(cube_, V(ap.x, ty - 0.02f, ap.y), V(0.12f, 0.56f, 0.12f), yaw, torso);
        drawPrim(cube_, V(ap.x, ty - 0.32f, ap.y), V(0.1f, 0.1f, 0.1f), yaw, skin);
    }
    drawPrim(sphere_, V(p.pos.x, ty + 0.46f, p.pos.y), V(0.14f, 0.15f, 0.14f), yaw, skin);
    if (p.worn[(size_t)ClothingSlot::Head].def)
        drawPrim(sphere_, V(p.pos.x, ty + 0.53f, p.pos.y), V(0.15f, 0.1f, 0.15f), yaw, col(ClothingSlot::Head, 0));
    else
        drawPrim(sphere_, V(p.pos.x - fwd.x * 0.02f, ty + 0.52f, p.pos.y - fwd.y * 0.02f), V(0.14f, 0.09f, 0.14f), yaw, hexColor(0x5a4030));
    if (p.worn[(size_t)ClothingSlot::Back].def) {
        Vec2 bp = at(-0.22f, 0);
        drawPrim(cube_, V(bp.x, ty + 0.05f, bp.y), V(0.2f, 0.5f, 0.36f), yaw, col(ClothingSlot::Back, 0));
    }
    if (const ItemInstance* w = game.weapon()) {
        float len = w->def->firearm ? 1.1f : std::max(0.3f, w->def->reach * 0.6f);
        float swing = p.swing;
        Vec2 hp = at(0.25f + swing * 0.3f, 0.3f);
        drawPrim(cube_, V(hp.x + fwd.x * len * 0.4f, ty - 0.2f + swing * 0.3f, hp.y + fwd.y * len * 0.4f), V(len, 0.06f, 0.06f), yaw,
                 hexColor(w->def->colour));
    }
}

void Renderer::drawAnimal(const Animal& a, float time) {
    const AnimalDef& d = *a.def;
    Color c = hexColor(d.colour);
    float s = d.size;
    float yaw = -a.heading;
    Vec2 fwd{std::cos(a.heading), std::sin(a.heading)};
    bool dead = !a.alive();
    float roll = dead ? PI * 0.5f : 0.0f;
    float speed = a.vel.length();
    float phase = std::sin(time * (4 + speed * 2) + a.id);
    if (d.id == "hare") {
        float hop = dead ? 0 : std::fabs(phase) * std::min(1.0f, speed * 0.2f) * 0.25f;
        float y = dead ? 0.1f : 0.16f + hop;
        drawPrim(sphere_, V(a.pos.x, y, a.pos.y), V(s * 0.5f, s * 0.32f, s * 0.3f), yaw, c, 0, roll);
        if (!dead) {
            drawPrim(sphere_, V(a.pos.x + fwd.x * s * 0.42f, y + s * 0.2f, a.pos.y + fwd.y * s * 0.42f), V(0.09f, 0.08f, 0.08f), yaw, c);
            for (int i = -1; i <= 1; i += 2) {
                Vec2 e{a.pos.x + fwd.x * s * 0.38f - fwd.y * 0.04f * i, a.pos.y + fwd.y * s * 0.38f + fwd.x * 0.04f * i};
                drawPrim(cube_, V(e.x, y + s * 0.42f, e.y), V(0.04f, 0.18f, 0.06f), yaw, shade(c, 0.85f));
            }
            drawPrim(sphere_, V(a.pos.x - fwd.x * s * 0.45f, y + 0.05f, a.pos.y - fwd.y * s * 0.45f), V(0.05f, 0.05f, 0.05f), yaw, hexColor(0xe8e4dc));
        }
        return;
    }
    bool boar = d.id == "wild_boar";
    bool moose = d.id == "moose";
    float legH = boar ? s * 0.32f : moose ? s * 0.5f : s * 0.48f;
    float bodyH = boar ? s * 0.42f : s * 0.32f;
    float by = dead ? bodyH * 0.5f : legH + bodyH * 0.5f;
    Vec2 sideV{-fwd.y, fwd.x};
    if (dead) {
        drawPrim(cube_, V(a.pos.x, by, a.pos.y), V(s, bodyH, s * 0.32f), yaw, c, 0, roll);
        drawPrim(cube_, V(a.pos.x + fwd.x * s * 0.55f, bodyH * 0.4f, a.pos.y + fwd.y * s * 0.55f), V(s * 0.3f, s * 0.15f, s * 0.15f), yaw, shade(c, 0.9f));
        return;
    }
    drawPrim(cube_, V(a.pos.x, by, a.pos.y), V(s, bodyH, s * (boar ? 0.36f : 0.3f)), yaw, c);
    // legs
    for (int i = 0; i < 4; ++i) {
        float f = (i < 2 ? 0.38f : -0.38f) * s;
        float sd = (i % 2 ? 0.1f : -0.1f) * s;
        float sw = (i % 2 == (i < 2 ? 0 : 1) ? phase : -phase) * std::min(1.0f, speed * 0.3f) * 0.12f * s;
        Vec2 lp{a.pos.x + fwd.x * (f + sw) + sideV.x * sd, a.pos.y + fwd.y * (f + sw) + sideV.y * sd};
        drawPrim(cube_, V(lp.x, legH * 0.5f, lp.y), V(0.07f * s + 0.03f, legH, 0.07f * s + 0.03f), yaw, shade(c, 0.8f));
    }
    // neck and head
    float headY = boar ? by : by + s * 0.35f;
    Vec2 hp{a.pos.x + fwd.x * s * 0.62f, a.pos.y + fwd.y * s * 0.62f};
    if (!boar) {
        Vec2 np{a.pos.x + fwd.x * s * 0.48f, a.pos.y + fwd.y * s * 0.48f};
        drawPrim(cube_, V(np.x, by + s * 0.2f, np.y), V(s * 0.16f, s * 0.38f, s * 0.13f), yaw, c);
    }
    drawPrim(cube_, V(hp.x, headY, hp.y), V(s * (moose ? 0.38f : 0.28f), s * 0.16f, s * 0.14f), yaw, shade(c, 0.92f));
    if (boar) {
        Vec2 sn{hp.x + fwd.x * s * 0.16f, hp.y + fwd.y * s * 0.16f};
        drawPrim(cube_, V(sn.x, headY - 0.03f, sn.y), V(0.08f, 0.1f, 0.12f), yaw, hexColor(0x2a2220));
    }
    if (moose && (a.id & 1)) {
        for (int i = -1; i <= 1; i += 2)
            drawPrim(cube_, V(hp.x - fwd.y * 0.35f * i, headY + 0.25f, hp.y + fwd.x * 0.35f * i), V(0.15f, 0.06f, 0.5f), yaw, hexColor(0xd8c8a0));
    }
    if (d.id == "roe_deer")
        drawPrim(sphere_, V(a.pos.x - fwd.x * s * 0.5f, by + 0.05f, a.pos.y - fwd.y * s * 0.5f), V(0.1f, 0.12f, 0.12f), yaw, hexColor(0xf0ece0));
}

void Renderer::drawDoors(World& world, const std::vector<int>& ids) {
    for (int id : ids) {
        const Building* b = world.building(id);
        if (!b) continue;
        Color c = hexColor(0x7a5a3c);
        for (const auto& e : b->edges) {
            for (int dir = 0; dir < 2; ++dir) {
                if (!(e.bits & (dir == 0 ? DoorN : DoorW))) continue;
                bool open = world.doorOpen(e.x, e.y, dir);
                float ax = (float)e.x, az = (float)e.y;
                if (dir == 0) {
                    if (!open) drawPrim(cube_, V(ax + 0.5f, 1.03f, az), V(0.84f, 2.05f, 0.05f), 0, c);
                    else drawPrim(cube_, V(ax + 0.1f, 1.03f, az + 0.42f), V(0.05f, 2.05f, 0.84f), 0, c);
                } else {
                    if (!open) drawPrim(cube_, V(ax, 1.03f, az + 0.5f), V(0.05f, 2.05f, 0.84f), 0, c);
                    else drawPrim(cube_, V(ax + 0.42f, 1.03f, az + 0.1f), V(0.84f, 2.05f, 0.05f), 0, c);
                }
            }
        }
    }
}

void Renderer::drawEntities(Game& game, float time) {
    World& w = game.world();
    Vec2 pp = game.player().pos;
    for (const auto& a : game.animals().animals()) {
        if (a.butchered || distance(a.pos, pp) > 90) continue;
        drawAnimal(a, time);
    }
    // Items on the ground
    for (const auto& [key, stack] : w.allGroundItems()) {
        if (stack.empty()) continue;
        int x = (int)(key >> 32), y = (int)(uint32_t)key;
        if (std::abs(x - pp.x) > 70 || std::abs(y - pp.y) > 70) continue;
        for (size_t i = 0; i < stack.size() && i < 5; ++i) {
            float ox = 0.25f + 0.22f * (i % 3), oz = 0.3f + 0.35f * (i / 3);
            drawPrim(cube_, V(x + ox, 0.07f, y + oz), V(0.22f, 0.14f, 0.16f), i * 0.6f, hexColor(stack[i].def->colour));
        }
    }
    // Campfires
    for (const auto& f : w.campfires()) {
        if (distance(f.pos, pp) > 80) continue;
        for (int i = 0; i < 7; ++i) {
            float a = i * 2 * PI / 7;
            drawPrim(sphere_, V(f.pos.x + std::cos(a) * 0.5f, 0.08f, f.pos.y + std::sin(a) * 0.5f), V(0.13f, 0.1f, 0.13f), 0, hexColor(0x707070));
        }
        if (f.fuelMinutes > 0 || f.lit) {
            drawPrim(cube_, V(f.pos.x, 0.1f, f.pos.y), V(0.7f, 0.1f, 0.12f), 0.4f, hexColor(0x5a4030));
            drawPrim(cube_, V(f.pos.x, 0.12f, f.pos.y), V(0.7f, 0.1f, 0.12f), 2.0f, hexColor(0x5a4030));
        }
        if (f.lit) {
            setUnlit(true);
            float fl = 0.8f + 0.2f * std::sin(time * 17.0f + f.id) * std::sin(time * 5.3f);
            drawPrim(cone_, V(f.pos.x, 0.1f, f.pos.y), V(0.32f, 0.85f * fl, 0.32f), time, hexColor(0xff7a18));
            drawPrim(cone_, V(f.pos.x, 0.1f, f.pos.y), V(0.18f, 1.1f * fl, 0.18f), -time * 1.3f, hexColor(0xffd040));
            setUnlit(false);
        }
    }
    for (const auto& s : w.snares()) {
        if (distance(s.pos, pp) > 60) continue;
        drawPrim(cylinder_, V(s.pos.x, 0, s.pos.y), V(0.12f, 0.15f, 0.12f), 0, hexColor(0xc0a040));
    }
    drawPlayer(game, time);
}

void Renderer::drawWeather(Game& game, float time) {
    const Weather& w = game.world().clock().weather();
    if (w.rain <= 0.01f) return;
    bool inside = currentBuilding_ >= 0;
    bool snow = w.kind == WeatherKind::Snow;
    int drops = (int)(450 * w.rain);
    Vec2 c = target_;
    for (int i = 0; i < drops; ++i) {
        uint32_t h = hash32(i * 7919 + 1);
        float x = c.x + (hashFloat(h) - 0.5f) * zoom_ * 2.2f;
        float z = c.y + (hashFloat(hash32(h)) - 0.5f) * zoom_ * 2.2f;
        float fall = snow ? 1.5f : 14.0f;
        float y = std::fmod(hashFloat(hash32(h + 5)) * 12.0f - time * fall, 12.0f);
        if (y < 0) y += 12.0f;
        if (inside && distance(Vec2{x, z}, game.player().pos) < 9) continue;
        if (snow) DrawCube(V(x, y, z), 0.06f, 0.06f, 0.06f, Fade(WHITE, 0.8f));
        else DrawLine3D(V(x, y, z), V(x - w.wind * 0.03f, y + 0.6f, z), Fade(hexColor(0xb0c4d0), 0.55f));
    }
}

void Renderer::draw(Game& game, float time, Vec2 hover, bool showHover) {
    World& world = game.world();
    playerPos_ = game.player().pos;
    currentBuilding_ = world.tile((int)std::floor(playerPos_.x), (int)std::floor(playerPos_.y)).building;
    setLighting(game, time);
    ClearBackground(skyColor_);

    BeginMode3D(cam_);
    // Visible area on the ground
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    Vec2 corners[4] = {pickGround({0, 0}), pickGround({sw, 0}), pickGround({0, sh}), pickGround({sw, sh})};
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (auto& c : corners) { minx = std::min(minx, c.x); maxx = std::max(maxx, c.x); miny = std::min(miny, c.y); maxy = std::max(maxy, c.y); }
    minx -= 20; miny -= 20; maxx += 20; maxy += 25;
    int cx0 = floorDiv((int)minx, kChunkSize), cx1 = floorDiv((int)maxx, kChunkSize);
    int cy0 = floorDiv((int)miny, kChunkSize), cy1 = floorDiv((int)maxy, kChunkSize);
    cx1 = std::min(cx1, cx0 + 14);
    cy1 = std::min(cy1, cy0 + 14);

    // Build missing / outdated chunk meshes, nearest first, a few per frame
    struct Need { int cx, cy; float d; };
    std::vector<Need> needs;
    std::vector<std::pair<ChunkGfx*, Chunk*>> visible;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) {
            int64_t key = tileKey(cx, cy);
            auto it = chunks_.find(key);
            Chunk* c = world.chunkIfLoaded(cx, cy);
            if (it == chunks_.end() || !c || it->second.version != c->version) {
                float d = distance(Vec2{(cx + 0.5f) * kChunkSize, (cy + 0.5f) * kChunkSize}, playerPos_);
                needs.push_back({cx, cy, d});
            }
            if (it != chunks_.end() && it->second.hasGround) visible.push_back({&it->second, c});
        }
    std::sort(needs.begin(), needs.end(), [](const Need& a, const Need& b) { return a.d < b.d; });
    double budgetStart = GetTime();
    for (const auto& n : needs) {
        if (GetTime() - budgetStart > 0.012) break;
        Chunk& c = world.chunk(n.cx, n.cy);
        ChunkGfx& g = chunks_[tileKey(n.cx, n.cy)];
        buildChunk(world, c, g);
    }
    // Free far-away chunk meshes
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        int cx = (int)(it->first >> 32), cy = (int)(uint32_t)it->first;
        if (cx < cx0 - 6 || cx > cx1 + 6 || cy < cy0 - 6 || cy > cy1 + 6) { unloadChunk(it->second); it = chunks_.erase(it); }
        else ++it;
    }

    Matrix identity = MatrixIdentity();
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    setCutaway(false);
    std::vector<int> buildingIds;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) {
            auto it = chunks_.find(tileKey(cx, cy));
            if (it == chunks_.end() || !it->second.hasGround) continue;
            DrawMesh(it->second.ground, mat_, identity);
            if (Chunk* c = world.chunkIfLoaded(cx, cy))
                for (int id : c->buildings) buildingIds.push_back(id);
        }
    std::sort(buildingIds.begin(), buildingIds.end());
    buildingIds.erase(std::unique(buildingIds.begin(), buildingIds.end()), buildingIds.end());

    if (showHover) {
        int hx = (int)std::floor(hover.x), hy = (int)std::floor(hover.y);
        DrawCubeWires(V(hx + 0.5f, 0.02f, hy + 0.5f), 1.0f, 0.04f, 1.0f, Fade(WHITE, 0.7f));
    }

    setCutaway(true);
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) {
            auto it = chunks_.find(tileKey(cx, cy));
            if (it != chunks_.end() && it->second.hasProps) DrawMesh(it->second.props, mat_, identity);
        }

    // Buildings (a few new meshes per frame)
    int built = 0;
    for (int id : buildingIds) {
        auto it = buildings_.find(id);
        if (it == buildings_.end()) {
            if (built >= 6) continue;
            const Building* b = world.building(id);
            if (!b) continue;
            BuildingGfx g;
            buildBuilding(world, *b, g);
            it = buildings_.emplace(id, g).first;
            ++built;
        }
        mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        if (id == currentBuilding_ && it->second.hasWallsLow) DrawMesh(it->second.wallsLow, mat_, identity);
        else if (it->second.hasWalls) DrawMesh(it->second.walls, mat_, identity);
        if (it->second.hasRoof && id != currentBuilding_) DrawMesh(it->second.roof, mat_, identity);
    }
    if (buildings_.size() > 900) {
        for (auto it = buildings_.begin(); it != buildings_.end();) {
            if (!std::binary_search(buildingIds.begin(), buildingIds.end(), it->first)) {
                if (it->second.hasWalls) UnloadMesh(it->second.walls);
                if (it->second.hasWallsLow) UnloadMesh(it->second.wallsLow);
                if (it->second.hasRoof) UnloadMesh(it->second.roof);
                it = buildings_.erase(it);
            } else ++it;
        }
    }
    drawDoors(world, buildingIds);

    setCutaway(false);
    drawEntities(game, time);
    drawWeather(game, time);
    EndMode3D();
}

} // namespace alone
