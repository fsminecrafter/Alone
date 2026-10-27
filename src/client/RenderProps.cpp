// Ground, roads and everything that stands on a tile: trees, bushes, fences,
// lamps, cars and furniture. Built once per chunk into a static mesh.
#include "client/RenderCommon.h"
#include "client/Renderer.h"
#include "core/Noise.h"
#include "core/Rng.h"
#include "raymath.h"

namespace alone {

using namespace rc;

namespace {

float rnd(uint32_t& h) {
    h = hash32(h + 0x9E3779B9u);
    return hashFloat(h);
}

// A deciduous tree: trunk, a few forking branches, leaf clusters that thin out in autumn.
void deciduous(MeshBuilder& mb, Vector3 base, float h, float trunkR, Color bark, int barkMat, Color leaf, float density,
               float spread, uint32_t seed) {
    uint32_t r = seed;
    Vector3 lean = V((rnd(r) - 0.5f) * 0.4f, 0, (rnd(r) - 0.5f) * 0.4f);
    Vector3 top = V(base.x + lean.x, base.y + h * 0.62f, base.z + lean.z);
    mb.material(barkMat);
    mb.tube(base, top, trunkR, trunkR * 0.55f, 6, bark);
    mb.tube(V(base.x, base.y - 0.1f, base.z), V(base.x, base.y + 0.35f, base.z), trunkR * 1.35f, trunkR, 6, bark, false);  // root flare
    int n = 4 + (int)(rnd(r) * 2);
    std::vector<Vector3> tips;
    for (int i = 0; i < n; ++i) {
        float t = 0.42f + 0.5f * (i / (float)n);
        Vector3 start = Vector3Lerp(base, top, t);
        float az = i * 2.4f + rnd(r) * 0.8f;
        float el = 0.55f + rnd(r) * 0.45f;
        float len = h * (0.28f + 0.14f * rnd(r)) * spread;
        Vector3 dir = V(std::cos(az) * std::cos(el), std::sin(el), std::sin(az) * std::cos(el));
        Vector3 end = Vector3Add(start, Vector3Scale(dir, len));
        mb.tube(start, end, trunkR * 0.42f, 0.03f, 5, bark, false);
        // a fork near the end
        Vector3 mid = Vector3Lerp(start, end, 0.6f);
        float az2 = az + (rnd(r) - 0.5f) * 1.6f;
        Vector3 dir2 = V(std::cos(az2) * 0.7f, 0.7f, std::sin(az2) * 0.7f);
        Vector3 end2 = Vector3Add(mid, Vector3Scale(dir2, len * 0.5f));
        mb.tube(mid, end2, 0.05f, 0.015f, 4, bark, false);
        tips.push_back(end);
        tips.push_back(end2);
    }
    mb.tube(top, V(top.x, top.y + h * 0.3f, top.z), trunkR * 0.5f, 0.02f, 5, bark, false);
    tips.push_back(V(top.x, top.y + h * 0.28f, top.z));
    if (density > 0.04f) {
        mb.material(MLeaves);
        float s = std::sqrt(density);
        for (size_t i = 0; i < tips.size(); ++i) {
            if (density < 0.6f && hashFloat(hash32(seed + (uint32_t)i)) > density + 0.2f) continue;
            float rad = (0.75f + 0.5f * rnd(r)) * spread * s * (i % 2 ? 0.75f : 1.0f);
            mb.blob(tips[i], V(rad * 1.25f, rad, rad * 1.25f), 6, 3, shade(leaf, 0.9f + 0.2f * rnd(r)), seed + (uint32_t)i, 0.45f);
        }
        if (density > 0.3f)
            mb.blob(V(top.x, top.y + h * 0.08f, top.z), V(1.5f * spread * s, h * 0.22f * s, 1.5f * spread * s), 7, 4, leaf, seed + 99, 0.4f);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Roads
// ---------------------------------------------------------------------------

void Renderer::addRoadSurfaces(MeshBuilder& mb, World& world, int x0, int y0) {
    const MapData& map = world.map();
    Color asphalt = groundColour(Ground::Asphalt), walk = groundColour(Ground::Sidewalk), gravel = groundColour(Ground::Gravel);
    Color curb = hexColor(0xb0aea8);
    auto strip = [&](Vec2 a, Vec2 b, float hw, float y, Color c) {
        Vec2 d = (b - a).normalized();
        Vec2 n{-d.y * hw, d.x * hw};
        mb.quad(V(a.x - n.x, y, a.y - n.y), V(a.x + n.x, y, a.y + n.y), V(b.x + n.x, y, b.y + n.y), V(b.x - n.x, y, b.y - n.y), c);
    };
    auto disc = [&](Vec2 c, float r, float y, Color col) {
        const int n = 12;
        for (int i = 0; i < n; ++i) {
            float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
            mb.tri(V(c.x, y, c.y), V(c.x + std::cos(a1) * r, y, c.y + std::sin(a1) * r), V(c.x + std::cos(a0) * r, y, c.y + std::sin(a0) * r), col);
        }
    };
    for (int pass = 0; pass < 3; ++pass)
        for (const auto& s : map.roads()) {
            float pad = s.halfWidth + 3;
            float minx = std::min(s.a.x, s.b.x) - pad, maxx = std::max(s.a.x, s.b.x) + pad;
            float miny = std::min(s.a.y, s.b.y) - pad, maxy = std::max(s.a.y, s.b.y) + pad;
            if (maxx < x0 || minx > x0 + kChunkSize || maxy < y0 || miny > y0 + kChunkSize) continue;
            if ((s.b - s.a).lengthSq() < 0.01f) continue;
            Vec2 mid = (s.a + s.b) * 0.5f;
            Vec2 dn = (s.b - s.a).normalized();
            Ground g = world.tile((int)std::floor(mid.x + dn.y * (s.halfWidth + 1.0f)), (int)std::floor(mid.y - dn.x * (s.halfWidth + 1.0f))).ground;
            bool town = g == Ground::Sidewalk;
            float extra = s.cls == RoadClass::Motorway ? 1.6f : town ? 1.8f : 0.8f;
            if (pass == 0) {
                mb.material(town ? MPaving : MDirt);
                strip(s.a, s.b, s.halfWidth + extra, 0.004f, town ? walk : gravel);
                disc(s.a, s.halfWidth + extra, 0.004f, town ? walk : gravel);
                disc(s.b, s.halfWidth + extra, 0.004f, town ? walk : gravel);
            } else if (pass == 1 && town) {
                mb.material(MConcrete);
                strip(s.a, s.b, s.halfWidth + 0.18f, 0.006f, curb);   // kerb stones
                disc(s.a, s.halfWidth + 0.18f, 0.006f, curb);
                disc(s.b, s.halfWidth + 0.18f, 0.006f, curb);
            } else if (pass == 2) {
                mb.material(MAsphalt);
                strip(s.a, s.b, s.halfWidth, 0.008f, asphalt);
                disc(s.a, s.halfWidth, 0.008f, asphalt);
                disc(s.b, s.halfWidth, 0.008f, asphalt);
            }
        }
    mb.material(MPlain);
}

void Renderer::addRoadMarkings(MeshBuilder& mb, const MapData& map, int x0, int y0) {
    const float y = 0.014f;
    Color white = hexColor(0xd8d6ce), yellow = hexColor(0xd0b040);
    mb.material(MPlain);
    auto inChunk = [&](Vec2 p) { return p.x >= x0 && p.y >= y0 && p.x < x0 + kChunkSize && p.y < y0 + kChunkSize; };
    auto stripe = [&](Vec2 a, Vec2 b, float w, Color c) {
        Vec2 d = (b - a).normalized();
        Vec2 n{-d.y * w * 0.5f, d.x * w * 0.5f};
        mb.quad(V(a.x - n.x, y, a.y - n.y), V(a.x + n.x, y, a.y + n.y), V(b.x + n.x, y, b.y + n.y), V(b.x - n.x, y, b.y - n.y), c);
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
                for (float t = 0; t < len; t += 2.0f) {
                    float t1 = std::min(len, t + 2.0f);
                    Vec2 a = s.a + d * t, b = s.a + d * t1;
                    if (!inChunk((a + b) * 0.5f)) continue;
                    stripe(a + n * (s.halfWidth - 0.6f), b + n * (s.halfWidth - 0.6f), 0.2f, white);
                    stripe(a - n * (s.halfWidth - 0.6f), b - n * (s.halfWidth - 0.6f), 0.2f, white);
                    stripe(a + n * 0.5f, b + n * 0.5f, 0.12f, yellow);
                    stripe(a - n * 0.5f, b - n * 0.5f, 0.12f, yellow);
                    // central crash barrier on posts
                    mb.material(MMetal);
                    Vec2 o = n * 0.06f;
                    mb.quad(V(a.x + o.x, 0.45f, a.y + o.y), V(b.x + o.x, 0.45f, b.y + o.y), V(b.x + o.x, 0.75f, b.y + o.y), V(a.x + o.x, 0.75f, a.y + o.y), hexColor(0xb4b6b8));
                    mb.quad(V(b.x - o.x, 0.45f, b.y - o.y), V(a.x - o.x, 0.45f, a.y - o.y), V(a.x - o.x, 0.75f, a.y - o.y), V(b.x - o.x, 0.75f, b.y - o.y), hexColor(0xb4b6b8));
                    mb.box(V(a.x - 0.05f, 0, a.y - 0.05f), V(a.x + 0.05f, 0.75f, a.y + 0.05f), hexColor(0x707274));
                    mb.material(MPlain);
                }
            }
        }
        // Zebra crossings where town streets meet (near segment starts)
        if (s.cls == RoadClass::Minor && len > 30) {
            Vec2 c = s.a + d * 8.0f;
            if (inChunk(c) && (hash2((int)s.a.x, (int)s.a.y, 5) & 3) == 0)
                for (float k = -s.halfWidth + 0.4f; k < s.halfWidth - 0.3f; k += 0.9f) {
                    Vec2 p0 = c + n * k;
                    stripe(p0 - d * 1.2f, p0 + d * 1.2f, 0.45f, white);
                }
        }
    }
    // Railway: ballast, sleepers and rails
    for (const auto& s : map.rails()) {
        float minx = std::min(s.a.x, s.b.x), maxx = std::max(s.a.x, s.b.x);
        float miny = std::min(s.a.y, s.b.y), maxy = std::max(s.a.y, s.b.y);
        if (maxx < x0 - 4 || minx > x0 + kChunkSize + 4 || maxy < y0 - 4 || miny > y0 + kChunkSize + 4) continue;
        Vec2 d = s.b - s.a;
        float len = d.length();
        d = d * (1.0f / len);
        Vec2 n{-d.y, d.x};
        mb.material(MBark);
        for (float t = 0; t < len; t += 0.65f) {
            Vec2 c = s.a + d * t;
            if (!inChunk(c)) continue;
            Vec2 a = c - n * 1.3f, b = c + n * 1.3f;
            Vec2 w = d * 0.12f;
            mb.box(V(std::min({a.x - w.x, a.x + w.x, b.x - w.x, b.x + w.x}), 0.0f, std::min({a.y - w.y, a.y + w.y, b.y - w.y, b.y + w.y})),
                   V(std::max({a.x - w.x, a.x + w.x, b.x - w.x, b.x + w.x}), 0.1f, std::max({a.y - w.y, a.y + w.y, b.y - w.y, b.y + w.y})),
                   hexColor(0x4a3a2c));
        }
        mb.material(MMetal);
        for (float t = 0; t < len; t += 2.0f) {
            float t1 = std::min(len, t + 2.0f);
            Vec2 a = s.a + d * t, b = s.a + d * t1;
            if (!inChunk((a + b) * 0.5f)) continue;
            for (float side : {-0.72f, 0.72f}) {
                Vec2 o = n * side;
                mb.tube(V(a.x + o.x, 0.14f, a.y + o.y), V(b.x + o.x, 0.14f, b.y + o.y), 0.045f, 0.045f, 4, hexColor(0x8a8a90), false);
            }
        }
        mb.material(MPlain);
    }
}

void Renderer::addPowerLines(MeshBuilder& mb, World& world, int x0, int y0) {
    // Wooden utility poles along roads outside the town centre, wires sagging between them
    const MapData& map = world.map();
    const float spacing = 38.0f;
    for (const auto& s : map.roads()) {
        if (s.cls != RoadClass::Major && s.cls != RoadClass::Minor) continue;
        Vec2 d = s.b - s.a;
        float len = d.length();
        if (len < spacing) continue;
        float minx = std::min(s.a.x, s.b.x) - 40, maxx = std::max(s.a.x, s.b.x) + 40;
        float miny = std::min(s.a.y, s.b.y) - 40, maxy = std::max(s.a.y, s.b.y) + 40;
        if (maxx < x0 || minx > x0 + kChunkSize || maxy < y0 || miny > y0 + kChunkSize) continue;
        if ((hash2((int)s.a.x, (int)s.a.y, 77) % 3) != 0) continue;     // only some roads have lines
        d = d * (1.0f / len);
        Vec2 n{-d.y, d.x};
        float off = s.halfWidth + 3.2f;
        for (float t = spacing * 0.5f; t < len; t += spacing) {
            Vec2 p = s.a + d * t + n * off;
            if (p.x < x0 || p.y < y0 || p.x >= x0 + kChunkSize || p.y >= y0 + kChunkSize) continue;
            if (map.denseAreaAt(p)) continue;
            const Tile& tile = world.tile((int)std::floor(p.x), (int)std::floor(p.y));
            if (tile.building >= 0 || isWater(tile.ground)) continue;
            Color wood = hexColor(0x5a4632);
            mb.material(MBark);
            mb.tube(V(p.x, 0, p.y), V(p.x, 8.0f, p.y), 0.14f, 0.11f, 6, wood);
            Vec2 a0 = p - d * 0.9f, a1 = p + d * 0.9f;
            mb.tube(V(a0.x, 7.6f, a0.y), V(a1.x, 7.6f, a1.y), 0.06f, 0.06f, 4, wood);
            mb.material(MMetal);
            for (float k : {-0.7f, 0.0f, 0.7f}) {
                Vec2 ip = p + d * k;
                mb.tube(V(ip.x, 7.66f, ip.y), V(ip.x, 7.82f, ip.y), 0.035f, 0.025f, 4, hexColor(0x5a6a5a));
            }
            // Wires to the next pole
            if (t + spacing < len) {
                Vec2 q = s.a + d * (t + spacing) + n * off;
                for (float k : {-0.7f, 0.0f, 0.7f}) {
                    Vec2 pa = p + d * k, pb = q + d * k;
                    Vector3 prev = V(pa.x, 7.8f, pa.y);
                    for (int i = 1; i <= 6; ++i) {
                        float u = i / 6.0f;
                        Vec2 pp = pa + (pb - pa) * u;
                        float sag = 0.7f * 4.0f * u * (1 - u);
                        Vector3 cur = V(pp.x, 7.8f - sag, pp.y);
                        mb.tube(prev, cur, 0.012f, 0.012f, 3, hexColor(0x202020), false);
                        prev = cur;
                    }
                }
            }
        }
    }
    mb.material(MPlain);
}

// ---------------------------------------------------------------------------
// Props
// ---------------------------------------------------------------------------

void Renderer::addProp(MeshBuilder& mb, World& world, const Tile& t, int x, int y, int doy, ChunkGfx& g) {
    float r1 = (t.variant & 15) / 15.0f, r2 = ((t.variant >> 4) & 15) / 15.0f;
    float cx = x + 0.5f, cz = y + 0.5f;
    if (isTree(t.prop) || t.prop == Prop::Shrub || t.prop == Prop::Boulder || t.prop == Prop::Rock) {
        cx += (r1 - 0.5f) * 0.4f;
        cz += (r2 - 0.5f) * 0.4f;
    }
    float h = propHeight(t.prop);
    uint32_t seed = hash2(x, y, 99);

    // Furniture helper: local frame u (across, -0.5..0.5), v (up), w (from wall 0 into room 1)
    int f = t.variant & 3;
    Vec2 dirIn = f == 0 ? Vec2{0, -1} : f == 1 ? Vec2{1, 0} : f == 2 ? Vec2{0, 1} : Vec2{-1, 0};
    Vec2 across{-dirIn.y, dirIn.x};
    auto L = [&](float u, float v, float w) {
        return V(cx + across.x * u + dirIn.x * (w - 0.5f), v, cz + across.y * u + dirIn.y * (w - 0.5f));
    };
    auto fb = [&](float u0, float u1, float v0, float v1, float w0, float w1, Color c, int mat = MPlain) {
        Vector3 a = L(u0, v0, w0), b = L(u1, v1, w1);
        mb.material(mat);
        mb.box(V(std::min(a.x, b.x), v0, std::min(a.z, b.z)), V(std::max(a.x, b.x), v1, std::max(a.z, b.z)), c);
    };

    mb.material(MPlain);
    switch (t.prop) {
        case Prop::Spruce: {
            float th = h * (0.75f + 0.5f * r1);
            mb.material(MBark);
            mb.tube(V(cx, 0, cz), V(cx, th * 0.95f, cz), 0.2f, 0.04f, 5, hexColor(0x4a3626), false);
            mb.material(MLeaves);
            Color gcol = shade(hexColor(0x1d3a24), 0.85f + 0.3f * r2);
            float rad = 1.7f + 0.5f * r2;
            int layers = 6;
            for (int i = 0; i < layers; ++i) {
                float u = i / (float)layers;
                float y0 = th * (0.1f + 0.78f * u);
                float rr = rad * (1.0f - u * 0.85f);
                mb.cone(V(cx, y0, cz), rr, th * 0.24f, 8, shade(gcol, 0.92f + 0.12f * u));
            }
            break;
        }
        case Prop::Pine: {
            float th = h * (0.75f + 0.5f * r1);
            mb.material(MBark);
            mb.tube(V(cx, 0, cz), V(cx, th * 0.55f, cz), 0.22f, 0.16f, 6, hexColor(0x6a4a34));
            mb.tube(V(cx, th * 0.55f, cz), V(cx + 0.2f, th * 0.86f, cz), 0.16f, 0.06f, 5, hexColor(0xb06a3c));
            uint32_t s = seed;
            for (int i = 0; i < 3; ++i) {
                float az = i * 2.1f + rnd(s);
                Vector3 a = V(cx, th * (0.68f + 0.08f * i), cz);
                Vector3 b = V(cx + std::cos(az) * 1.4f, th * (0.74f + 0.08f * i), cz + std::sin(az) * 1.4f);
                mb.tube(a, b, 0.08f, 0.03f, 4, hexColor(0xa06038), false);
                mb.material(MLeaves);
                mb.blob(b, V(1.3f, 0.7f, 1.3f), 7, 3, shade(hexColor(0x2b4628), 0.9f + 0.2f * rnd(s)), s, 0.4f);
                mb.material(MBark);
            }
            mb.material(MLeaves);
            mb.blob(V(cx + 0.2f, th * 0.9f, cz), V(1.8f, 0.9f, 1.8f), 8, 4, shade(hexColor(0x2d4a2b), 0.9f + 0.2f * r2), seed, 0.35f);
            break;
        }
        case Prop::Birch: {
            float th = h * (0.75f + 0.5f * r1);
            deciduous(mb, V(cx, 0, cz), th, 0.14f, hexColor(0xe6e2d8), MBirch, leafColour(Prop::Birch, doy, t.variant),
                      leafDensity(Prop::Birch, doy), 0.75f, seed);
            break;
        }
        case Prop::Oak:
            deciduous(mb, V(cx, 0, cz), h * (0.8f + 0.4f * r1), 0.32f, hexColor(0x54402e), MBark,
                      leafColour(Prop::Oak, doy, t.variant), leafDensity(Prop::Oak, doy), 1.15f, seed);
            break;
        case Prop::Maple:
            deciduous(mb, V(cx, 0, cz), h * (0.8f + 0.4f * r1), 0.25f, hexColor(0x5a4636), MBark,
                      leafColour(Prop::Maple, doy, t.variant), leafDensity(Prop::Maple, doy), 1.0f, seed);
            break;
        case Prop::Rowan: {
            float th = h * (0.75f + 0.5f * r1);
            deciduous(mb, V(cx, 0, cz), th, 0.11f, hexColor(0x5a4a3a), MBark, leafColour(Prop::Rowan, doy, t.variant),
                      leafDensity(Prop::Rowan, doy), 0.7f, seed);
            if (doy > 220 && doy < 330) {   // red berry clusters stay after the leaves fall
                uint32_t s = seed + 3;
                mb.material(MPlain);
                for (int i = 0; i < 6; ++i) {
                    Vector3 p = V(cx + (rnd(s) - 0.5f) * 2.2f, th * (0.55f + 0.3f * rnd(s)), cz + (rnd(s) - 0.5f) * 2.2f);
                    mb.blob(p, V(0.12f, 0.1f, 0.12f), 4, 2, hexColor(0xd02818));
                }
            }
            break;
        }
        case Prop::Shrub: {
            mb.material(MLeaves);
            Color c = shade(hexColor(0x4a6230), 0.85f + 0.3f * r1);
            mb.blob(V(cx, 0.5f, cz), V(0.7f, 0.55f, 0.7f), 6, 3, c, seed, 0.45f);
            mb.blob(V(cx + 0.3f, 0.35f, cz - 0.25f), V(0.45f, 0.38f, 0.45f), 5, 3, shade(c, 1.1f), seed + 1, 0.4f);
            break;
        }
        case Prop::Hedge: {
            mb.material(MLeaves);
            Color c = shade(hexColor(0x2d5229), 0.9f + 0.2f * r1);
            mb.box(V(x + 0.08f, 0, y + 0.08f), V(x + 0.92f, 1.25f, y + 0.92f), c);
            mb.blob(V(cx, 1.25f, cz), V(0.5f, 0.2f, 0.5f), 6, 2, shade(c, 1.08f), seed, 0.3f);
            break;
        }
        case Prop::LingonBush: case Prop::BlueberryBush: {
            bool lingon = t.prop == Prop::LingonBush;
            Color c = lingon ? hexColor(0x2e5226) : hexColor(0x5a6a30);
            if (!lingon && doy > 255) c = mixColor(c, hexColor(0x9a3a20), std::clamp((doy - 255) / 30.0f, 0.0f, 0.8f));  // blueberry leaves redden
            mb.material(MLeaves);
            mb.blob(V(cx - 0.15f, 0.06f, cz), V(0.32f, lingon ? 0.14f : 0.22f, 0.3f), 6, 3, c, seed, 0.5f);
            mb.blob(V(cx + 0.2f, 0.05f, cz + 0.15f), V(0.26f, lingon ? 0.12f : 0.18f, 0.26f), 5, 3, shade(c, 1.1f), seed + 1, 0.5f);
            bool season = lingon ? (doy >= 222 && doy <= 319) : (doy >= 182 && doy <= 262);
            if (season && !world.harvested(x, y, 200)) {
                mb.material(MPlain);
                Color b = lingon ? hexColor(0xd01828) : hexColor(0x2a3070);
                for (int i = 0; i < 7; ++i) {
                    float a = i * 1.3f + r1 * 3;
                    float bx = cx + std::cos(a) * 0.25f, bz = cz + std::sin(a) * 0.25f;
                    mb.blob(V(bx, 0.17f, bz), V(0.04f, 0.04f, 0.04f), 4, 2, b);
                }
            }
            break;
        }
        case Prop::RoseBush: {
            mb.material(MLeaves);
            Color c = mixColor(hexColor(0x4e5a2e), hexColor(0x7a5a2a), std::clamp((doy - 270) / 40.0f, 0.0f, 1.0f));
            mb.blob(V(cx, 0.65f, cz), V(0.7f, 0.62f, 0.7f), 6, 3, c, seed, 0.5f);
            if (doy >= 228 && !world.harvested(x, y, 200)) {
                mb.material(MPlain);
                for (int i = 0; i < 9; ++i) {
                    float a = i * 1.1f + r2 * 3;
                    float bx = cx + std::cos(a) * 0.62f, bz = cz + std::sin(a) * 0.62f, by = 0.45f + (i % 3) * 0.25f;
                    mb.blob(V(bx, by, bz), V(0.06f, 0.08f, 0.06f), 4, 2, hexColor(0xe04010));
                }
            }
            break;
        }
        case Prop::Rock:
            mb.material(MConcrete);
            mb.blob(V(cx, 0.08f, cz), V(0.35f, 0.24f, 0.3f), 6, 3, shade(hexColor(0x85857f), 0.85f + 0.3f * r1), seed, 0.45f);
            break;
        case Prop::Boulder:
            mb.material(MConcrete);
            mb.blob(V(cx, 0.45f, cz), V(1.1f, 0.95f, 0.95f), 8, 5, shade(hexColor(0x8a8c84), 0.85f + 0.25f * r1), seed, 0.35f);
            mb.material(MLeaves);   // lichen/moss cap
            mb.blob(V(cx + 0.1f, 1.2f, cz), V(0.55f, 0.18f, 0.5f), 6, 2, hexColor(0x6a7a40), seed + 2, 0.4f);
            break;
        case Prop::Stump:
            mb.material(MBark);
            mb.tube(V(cx, 0, cz), V(cx, 0.4f, cz), 0.32f, 0.28f, 7, hexColor(0x6a5038));
            mb.material(MFloorWood);
            mb.cylinder(V(cx, 0.39f, cz), 0.27f, 0.02f, 7, hexColor(0xc0a070));
            break;
        case Prop::Reeds:
            mb.material(MLeaves);
            for (int i = 0; i < 7; ++i) {
                float bx = x + 0.1f + 0.13f * i, bz = y + 0.2f + 0.6f * ((i * 7 + t.variant) % 5) / 5.0f;
                float hh = 1.1f + 0.4f * ((i + t.variant) % 3) / 2.0f;
                mb.spike(V(bx, -0.2f, bz), V(bx + 0.08f, hh, bz + 0.03f), 0.03f, 3, hexColor(0xa09a58));
                if (i % 2) mb.tube(V(bx + 0.07f, hh - 0.3f, bz + 0.03f), V(bx + 0.08f, hh - 0.05f, bz + 0.03f), 0.04f, 0.04f, 4, hexColor(0x5a4030));
            }
            break;
        case Prop::Fence: {
            Color c = hexColor(0xe2ded2);
            bool alongZ = t.variant == 1;
            mb.material(MSiding, 1);
            for (int i = 0; i < 6; ++i) {
                float o = 0.08f + i * 0.17f;
                if (alongZ) mb.box(V(cx - 0.02f, 0, y + o - 0.04f), V(cx + 0.02f, 0.95f, y + o + 0.04f), c);
                else mb.box(V(x + o - 0.04f, 0, cz - 0.02f), V(x + o + 0.04f, 0.95f, cz + 0.02f), c);
            }
            if (alongZ) {
                mb.box(V(cx - 0.05f, 0.25f, (float)y), V(cx - 0.02f, 0.33f, y + 1.0f), c);
                mb.box(V(cx - 0.05f, 0.7f, (float)y), V(cx - 0.02f, 0.78f, y + 1.0f), c);
            } else {
                mb.box(V((float)x, 0.25f, cz - 0.05f), V(x + 1.0f, 0.33f, cz - 0.02f), c);
                mb.box(V((float)x, 0.7f, cz - 0.05f), V(x + 1.0f, 0.78f, cz - 0.02f), c);
            }
            break;
        }
        case Prop::Lamppost: {
            mb.material(MMetal);
            Color pole = hexColor(0x3c4240);
            mb.tube(V(cx, 0, cz), V(cx, 5.4f, cz), 0.08f, 0.055f, 6, pole);
            Vec2 arm = across * 0.9f;
            mb.tube(V(cx, 5.3f, cz), V(cx + arm.x * 0.6f, 5.55f, cz + arm.y * 0.6f), 0.04f, 0.04f, 4, pole, false);
            mb.tube(V(cx + arm.x * 0.6f, 5.55f, cz + arm.y * 0.6f), V(cx + arm.x, 5.5f, cz + arm.y), 0.04f, 0.04f, 4, pole);
            mb.box(V(cx + arm.x - 0.25f, 5.35f, cz + arm.y - 0.18f), V(cx + arm.x + 0.25f, 5.52f, cz + arm.y + 0.18f), hexColor(0x2a2c2e));
            mb.material(MLamp);
            mb.box(V(cx + arm.x - 0.2f, 5.32f, cz + arm.y - 0.14f), V(cx + arm.x + 0.2f, 5.36f, cz + arm.y + 0.14f), hexColor(0xd8d0b0));
            g.lamps.push_back(V(cx + arm.x, 5.2f, cz + arm.y));
            break;
        }
        case Prop::Bench: {
            mb.material(MFloorWood);
            Color wood = hexColor(0x8a6a44);
            mb.box(V(x + 0.05f, 0.42f, cz - 0.22f), V(x + 0.95f, 0.47f, cz + 0.2f), wood);
            mb.box(V(x + 0.05f, 0.55f, cz + 0.17f), V(x + 0.95f, 0.88f, cz + 0.22f), wood);
            mb.material(MMetal);
            for (float bx : {0.1f, 0.84f}) {
                mb.box(V(x + bx, 0, cz - 0.2f), V(x + bx + 0.06f, 0.42f, cz - 0.15f), hexColor(0x2a2a2a));
                mb.box(V(x + bx, 0, cz + 0.12f), V(x + bx + 0.06f, 0.88f, cz + 0.18f), hexColor(0x2a2a2a));
            }
            break;
        }
        case Prop::Mailbox:
            mb.material(MBark);
            mb.box(V(cx - 0.04f, 0, cz - 0.04f), V(cx + 0.04f, 0.95f, cz + 0.04f), hexColor(0x6a5a48));
            mb.material(MMetal);
            mb.box(V(cx - 0.22f, 0.95f, cz - 0.14f), V(cx + 0.22f, 1.15f, cz + 0.14f), hexColor(0x2a5a7a));
            mb.blob(V(cx, 1.15f, cz), V(0.22f, 0.07f, 0.14f), 6, 2, hexColor(0x2a5a7a));
            break;
        case Prop::Car: {
            static const uint32_t cols[] = {0x8a2020, 0x203a5a, 0xc8c8c4, 0x2a2a2c, 0x5a6a70, 0x2a4a34, 0xd8d6d0, 0x6a4a2a};
            Color body = hexColor(cols[t.variant % 8]);
            Color dark = hexColor(0x1a1c1e), glass = hexColor(0x2a3640);
            float yaw = (t.variant & 1) ? PI * 0.5f : 0.0f;
            yaw += (r2 - 0.5f) * 0.25f;
            float cs = std::cos(yaw), sn = std::sin(yaw);
            auto P = [&](float lx, float ly, float lz) { return V(cx + lx * cs - lz * sn, ly, cz + lx * sn + lz * cs); };
            // lower body
            mb.material(MMetal);
            mb.orientedBox(V(cx, 0.6f, cz), V(2.15f, 0.32f, 0.86f), yaw, body);
            // cabin: tapered with sloped screens
            float hx0 = -1.15f, hx1 = 0.85f, top = 1.45f, inset = 0.12f;
            Vector3 b0 = P(hx0 - 0.25f, 0.92f, -0.82f), b1 = P(hx1 + 0.35f, 0.92f, -0.82f), b2 = P(hx1 + 0.35f, 0.92f, 0.82f), b3 = P(hx0 - 0.25f, 0.92f, 0.82f);
            Vector3 t0 = P(hx0, top, -0.82f + inset), t1 = P(hx1 - 0.2f, top, -0.82f + inset), t2 = P(hx1 - 0.2f, top, 0.82f - inset), t3 = P(hx0, top, 0.82f - inset);
            mb.material(MGlass);
            mb.quad(b1, b2, t2, t1, glass);   // windscreen
            mb.quad(b3, b0, t0, t3, glass);   // rear window
            mb.quad(b0, b1, t1, t0, glass);   // side
            mb.quad(b2, b3, t3, t2, glass);   // side
            mb.material(MMetal);
            mb.quad(t0, t1, t2, t3, body);    // roof
            // wheels
            for (int i = 0; i < 4; ++i) {
                float lx = i < 2 ? 1.35f : -1.35f, lz = i % 2 ? 0.8f : -0.8f;
                Vector3 a = P(lx, 0.33f, lz - 0.12f), b = P(lx, 0.33f, lz + 0.12f);
                mb.tube(a, b, 0.33f, 0.33f, 10, dark);
                Vector3 hub = P(lx, 0.33f, lz + (lz > 0 ? 0.125f : -0.125f));
                mb.blob(hub, V(0.12f, 0.12f, 0.12f), 6, 2, hexColor(0x8a8a8c));
            }
            // lights
            mb.material(MPlain);
            for (float lz : {-0.6f, 0.6f}) {
                mb.blob(P(2.15f, 0.72f, lz), V(0.05f, 0.08f, 0.14f), 5, 2, hexColor(0xe8e4d0));
                mb.blob(P(-2.15f, 0.75f, lz), V(0.05f, 0.08f, 0.14f), 5, 2, hexColor(0xa01810));
            }
            break;
        }
        case Prop::Bin:
            mb.material(MMetal);
            mb.box(V(cx - 0.3f, 0.05f, cz - 0.35f), V(cx + 0.3f, 1.0f, cz + 0.35f), hexColor(0x2a4a30));
            mb.box(V(cx - 0.33f, 1.0f, cz - 0.38f), V(cx + 0.33f, 1.07f, cz + 0.38f), hexColor(0x223e28));
            mb.tube(V(cx - 0.3f, 0.12f, cz + 0.36f), V(cx + 0.3f, 0.12f, cz + 0.36f), 0.1f, 0.1f, 6, hexColor(0x1a1a1a));
            break;
        case Prop::HayBale:
            mb.material(MPlain);
            mb.tube(V(cx, 0.65f, cz - 0.6f), V(cx, 0.65f, cz + 0.6f), 0.65f, 0.65f, 12, hexColor(0xe8e8e2));
            break;
        case Prop::Woodpile: {
            mb.material(MBark);
            for (int row = 0; row < 4; ++row)
                for (int i = 0; i < 5 - (row & 1); ++i) {
                    float bx = x + 0.12f + i * 0.18f + (row & 1) * 0.09f;
                    float by = 0.1f + row * 0.17f;
                    mb.tube(V(bx, by, y + 0.15f), V(bx, by, y + 0.85f), 0.085f, 0.085f, 6, shade(hexColor(0x8a6a48), 0.85f + 0.3f * hashFloat(hash32(seed + row * 7 + i))));
                }
            mb.material(MMetal);
            mb.box(V(x + 0.02f, 0.8f, y + 0.1f), V(x + 0.98f, 0.86f, y + 0.9f), hexColor(0x3a3a3c));
            break;
        }
        case Prop::RainBarrel:
            mb.material(MMetal);
            mb.tube(V(cx, 0, cz), V(cx, 0.9f, cz), 0.35f, 0.38f, 10, hexColor(0x2a4a32));
            break;
        case Prop::Barrier:
            mb.material(MMetal);
            mb.box(V((float)x, 0.45f, cz - 0.05f), V(x + 1.0f, 0.75f, cz + 0.05f), hexColor(0xc0c0c0));
            break;

        // Furniture ---------------------------------------------------------
        case Prop::Fridge:
            fb(-0.35f, 0.35f, 0, 1.8f, 0.04f, 0.7f, hexColor(0xeeeeea), MMetal);
            fb(-0.33f, 0.33f, 1.18f, 1.2f, 0.7f, 0.71f, hexColor(0xa0a0a0));
            fb(0.24f, 0.28f, 0.6f, 1.6f, 0.7f, 0.74f, hexColor(0xb0b0b0), MMetal);
            break;
        case Prop::Freezer:
            fb(-0.42f, 0.42f, 0, 0.88f, 0.04f, 0.68f, hexColor(0xe6e6e2), MMetal);
            fb(-0.3f, 0.3f, 0.78f, 0.82f, 0.68f, 0.71f, hexColor(0xa0a0a0));
            break;
        case Prop::Counter:
            fb(-0.5f, 0.5f, 0.08f, 0.86f, 0.04f, 0.62f, hexColor(0xd8d0c0), MPlaster);
            fb(-0.5f, 0.5f, 0, 0.08f, 0.04f, 0.58f, hexColor(0x3a3634));
            fb(-0.51f, 0.51f, 0.86f, 0.9f, 0.02f, 0.65f, hexColor(0x5a524a), MConcrete);
            fb(-0.01f, 0.01f, 0.12f, 0.82f, 0.62f, 0.63f, hexColor(0xa09a90));
            fb(-0.5f, 0.5f, 1.45f, 2.1f, 0.02f, 0.36f, hexColor(0xd8d0c0), MPlaster);   // wall cupboard
            break;
        case Prop::Stove:
            fb(-0.3f, 0.3f, 0, 0.88f, 0.04f, 0.62f, hexColor(0xf0f0ec), MMetal);
            fb(-0.3f, 0.3f, 0.88f, 0.9f, 0.04f, 0.62f, hexColor(0x1a1a1a));
            fb(-0.22f, 0.22f, 0.25f, 0.65f, 0.62f, 0.63f, hexColor(0x202428), MGlass);
            mb.material(MMetal);
            for (float u : {-0.14f, 0.14f})
                for (float w : {0.2f, 0.45f}) {
                    Vector3 c = L(u, 0.905f, w);
                    mb.cylinder(c, 0.08f, 0.01f, 8, hexColor(0x404040));
                }
            break;
        case Prop::Sink:
            fb(-0.5f, 0.5f, 0, 0.86f, 0.04f, 0.62f, hexColor(0xd8d0c0), MPlaster);
            fb(-0.51f, 0.51f, 0.86f, 0.9f, 0.02f, 0.65f, hexColor(0xb0b4b8), MMetal);
            fb(-0.3f, 0.3f, 0.82f, 0.905f, 0.12f, 0.5f, hexColor(0x6a6e72), MMetal);
            mb.material(MMetal);
            mb.tube(L(0, 0.9f, 0.08f), L(0, 1.15f, 0.08f), 0.02f, 0.02f, 4, hexColor(0xc0c0c4), false);
            mb.tube(L(0, 1.15f, 0.08f), L(0, 1.12f, 0.25f), 0.02f, 0.02f, 4, hexColor(0xc0c0c4));
            break;
        case Prop::Table: {
            Color wood = hexColor(0x9a7650);
            mb.material(MFloorWood);
            mb.box(V(x + 0.04f, 0.72f, y + 0.04f), V(x + 0.96f, 0.77f, y + 0.96f), wood);
            for (float lx : {0.1f, 0.84f})
                for (float lz : {0.1f, 0.84f}) mb.box(V(x + lx, 0, y + lz), V(x + lx + 0.06f, 0.72f, y + lz + 0.06f), shade(wood, 0.8f));
            if (seed & 1) {   // something left on the table
                mb.material(MPlain);
                mb.cylinder(V(cx + 0.15f, 0.77f, cz - 0.1f), 0.1f, 0.02f, 8, hexColor(0xf0f0f0));
                mb.tube(V(cx - 0.2f, 0.77f, cz + 0.15f), V(cx - 0.2f, 0.88f, cz + 0.15f), 0.04f, 0.04f, 6, hexColor(0x3a5a8a));
            }
            break;
        }
        case Prop::Chair: {
            Color wood = hexColor(0x8a6a48);
            fb(-0.22f, 0.22f, 0.42f, 0.47f, 0.25f, 0.7f, wood, MFloorWood);
            fb(-0.22f, 0.22f, 0.47f, 0.95f, 0.25f, 0.3f, wood, MFloorWood);
            for (float u : {-0.2f, 0.16f})
                for (float w : {0.26f, 0.64f}) fb(u, u + 0.04f, 0, 0.42f, w, w + 0.04f, shade(wood, 0.8f));
            break;
        }
        case Prop::Sofa: {
            static const uint32_t cols[] = {0x5a6a7a, 0x7a5a4a, 0x4a5a3a, 0x8a8a80, 0x6a3a3a};
            Color c = hexColor(cols[seed % 5]);
            fb(-0.49f, 0.49f, 0.08f, 0.42f, 0.05f, 0.88f, c, MCarpet);
            fb(-0.49f, 0.49f, 0.42f, 0.85f, 0.05f, 0.25f, shade(c, 0.9f), MCarpet);
            fb(-0.49f, -0.38f, 0.42f, 0.62f, 0.05f, 0.88f, shade(c, 0.9f), MCarpet);
            fb(0.38f, 0.49f, 0.42f, 0.62f, 0.05f, 0.88f, shade(c, 0.9f), MCarpet);
            fb(-0.37f, 0.37f, 0.42f, 0.52f, 0.25f, 0.85f, shade(c, 1.1f), MCarpet);
            fb(-0.45f, 0.45f, 0, 0.08f, 0.1f, 0.84f, hexColor(0x2a2420));
            break;
        }
        case Prop::Bed: {
            static const uint32_t cols[] = {0xa04040, 0x4060a0, 0x60a070, 0xd0c080, 0x8080a0, 0xe0e0e0};
            Color wood = hexColor(0x8a6a48);
            mb.material(MFloorWood);
            mb.box(V(x + 0.04f, 0.12f, y + 0.03f), V(x + 0.96f, 0.36f, y + 0.97f), wood);
            fb(-0.47f, 0.47f, 0, 0.95f, 0.02f, 0.08f, wood, MFloorWood);   // headboard
            mb.material(MCarpet);
            mb.box(V(x + 0.07f, 0.36f, y + 0.06f), V(x + 0.93f, 0.48f, y + 0.94f), hexColor(0xf0ece6));
            fb(-0.44f, 0.44f, 0.48f, 0.53f, 0.35f, 0.95f, hexColor(cols[seed % 6]), MCarpet);
            fb(-0.3f, 0.3f, 0.48f, 0.6f, 0.1f, 0.3f, hexColor(0xf4f4f4), MCarpet);
            break;
        }
        case Prop::Wardrobe: {
            Color wood = hexColor(0x9a7a56);
            fb(-0.47f, 0.47f, 0, 2.0f, 0.03f, 0.6f, wood, MFloorWood);
            fb(-0.005f, 0.005f, 0.05f, 1.95f, 0.6f, 0.61f, shade(wood, 0.6f));
            fb(-0.08f, -0.05f, 0.9f, 1.2f, 0.6f, 0.63f, hexColor(0xc0b090), MMetal);
            fb(0.05f, 0.08f, 0.9f, 1.2f, 0.6f, 0.63f, hexColor(0xc0b090), MMetal);
            break;
        }
        case Prop::Dresser: {
            Color wood = hexColor(0xa88660);
            fb(-0.45f, 0.45f, 0, 0.95f, 0.03f, 0.48f, wood, MFloorWood);
            for (int i = 0; i < 3; ++i) {
                float v = 0.12f + i * 0.28f;
                fb(-0.42f, 0.42f, v + 0.22f, v + 0.24f, 0.48f, 0.49f, shade(wood, 0.6f));
                fb(-0.05f, 0.05f, v + 0.1f, v + 0.13f, 0.48f, 0.51f, hexColor(0xc0b090), MMetal);
            }
            break;
        }
        case Prop::Bookshelf: {
            Color wood = hexColor(0x6a4a30);
            fb(-0.47f, 0.47f, 0, 1.92f, 0.03f, 0.36f, wood, MFloorWood);
            static const uint32_t books[] = {0xa03030, 0x30508a, 0x3a7a3a, 0xc0a040, 0x6a3a7a, 0xd8d0c0, 0x2a2a2a};
            uint32_t s = seed;
            for (int sh = 0; sh < 4; ++sh) {
                float v = 0.08f + sh * 0.46f;
                float u = -0.42f;
                while (u < 0.38f) {
                    float w = 0.04f + 0.05f * rnd(s);
                    float bh = 0.22f + 0.14f * rnd(s);
                    fb(u, u + w, v, v + bh, 0.08f, 0.37f, hexColor(books[(int)(rnd(s) * 7) % 7]));
                    u += w + 0.006f;
                }
            }
            break;
        }
        case Prop::Toilet:
            mb.material(MPlain);
            mb.tube(L(0, 0, 0.35f), L(0, 0.4f, 0.35f), 0.17f, 0.2f, 10, hexColor(0xf4f4f4));
            fb(-0.2f, 0.2f, 0.4f, 0.78f, 0.04f, 0.22f, hexColor(0xf4f4f4));
            fb(-0.22f, 0.22f, 0.4f, 0.43f, 0.15f, 0.58f, hexColor(0xe8e8e8));
            break;
        case Prop::Bathtub:
            mb.material(MPlain);
            mb.box(V(x + 0.04f, 0, y + 0.04f), V(x + 0.96f, 0.58f, y + 0.96f), hexColor(0xf2f2f2));
            mb.box(V(x + 0.12f, 0.3f, y + 0.12f), V(x + 0.88f, 0.585f, y + 0.88f), hexColor(0xd8dcde));
            break;
        case Prop::BathroomCabinet:
            fb(-0.3f, 0.3f, 1.1f, 1.7f, 0.02f, 0.2f, hexColor(0xf0f0f0), MPlaster);
            fb(-0.25f, 0.25f, 1.15f, 1.65f, 0.2f, 0.21f, hexColor(0x8aa0b0), MGlass);
            fb(-0.28f, 0.28f, 0, 0.82f, 0.03f, 0.45f, hexColor(0xe0e0e0), MPlaster);
            fb(-0.29f, 0.29f, 0.82f, 0.86f, 0.02f, 0.47f, hexColor(0xf8f8f8));
            break;
        case Prop::Shelf: {
            Color metal = hexColor(0x8a9096);
            fb(-0.48f, -0.44f, 0, 1.75f, 0.03f, 0.62f, metal, MMetal);
            fb(0.44f, 0.48f, 0, 1.75f, 0.03f, 0.62f, metal, MMetal);
            fb(-0.48f, 0.48f, 0, 1.75f, 0.03f, 0.06f, shade(metal, 0.85f), MMetal);
            static const uint32_t goods[] = {0xc04040, 0x40a040, 0xd0b040, 0x4060c0, 0xe08030, 0xe8e0d0, 0x8a5a3a};
            uint32_t s = seed;
            for (int sh = 0; sh < 4; ++sh) {
                float v = 0.1f + sh * 0.42f;
                fb(-0.46f, 0.46f, v - 0.03f, v, 0.03f, 0.62f, metal, MMetal);
                float u = -0.42f;
                while (u < 0.36f) {
                    float w = 0.08f + 0.1f * rnd(s);
                    if (rnd(s) < 0.25f) { u += w; continue; }   // gaps where things were taken
                    fb(u, u + w, v, v + 0.12f + 0.15f * rnd(s), 0.1f, 0.55f, hexColor(goods[(int)(rnd(s) * 7) % 7]));
                    u += w + 0.01f;
                }
            }
            break;
        }
        case Prop::Cooler:
            fb(-0.48f, 0.48f, 0, 1.95f, 0.03f, 0.72f, hexColor(0xd0d8de), MMetal);
            fb(-0.44f, 0.44f, 0.15f, 1.8f, 0.72f, 0.73f, hexColor(0x5a7a8a), MGlass);
            break;
        case Prop::Checkout:
            fb(-0.48f, 0.48f, 0, 0.92f, 0.08f, 0.85f, hexColor(0x5a5a60), MMetal);
            fb(-0.48f, 0.48f, 0.92f, 0.94f, 0.3f, 0.85f, hexColor(0x1a1a1a));
            fb(-0.15f, 0.15f, 0.94f, 1.2f, 0.1f, 0.3f, hexColor(0x2a2a2e), MMetal);
            break;
        case Prop::Desk:
            fb(-0.48f, 0.48f, 0.72f, 0.77f, 0.04f, 0.68f, hexColor(0xb09a78), MFloorWood);
            fb(-0.46f, -0.12f, 0, 0.72f, 0.06f, 0.66f, hexColor(0x9a8464), MFloorWood);
            fb(0.42f, 0.46f, 0, 0.72f, 0.06f, 0.66f, hexColor(0x9a8464));
            if (seed & 1) fb(-0.2f, 0.2f, 0.77f, 1.05f, 0.1f, 0.14f, hexColor(0x1a1a1e), MMetal);   // monitor
            break;
        case Prop::Locker:
            fb(-0.46f, 0.46f, 0, 1.9f, 0.03f, 0.5f, hexColor(0x4a6a9a), MMetal);
            for (float u : {-0.15f, 0.15f}) fb(u - 0.005f, u + 0.005f, 0.05f, 1.85f, 0.5f, 0.51f, hexColor(0x2a3a5a));
            break;
        case Prop::Pew:
            fb(-0.49f, 0.49f, 0.42f, 0.48f, 0.15f, 0.6f, hexColor(0x7a5a3a), MFloorWood);
            fb(-0.49f, 0.49f, 0.48f, 1.0f, 0.1f, 0.16f, hexColor(0x7a5a3a), MFloorWood);
            fb(-0.49f, -0.45f, 0, 0.9f, 0.1f, 0.6f, hexColor(0x6a4a2a), MFloorWood);
            fb(0.45f, 0.49f, 0, 0.9f, 0.1f, 0.6f, hexColor(0x6a4a2a), MFloorWood);
            break;
        case Prop::MedicalCabinet:
            fb(-0.45f, 0.45f, 0, 1.8f, 0.03f, 0.45f, hexColor(0xf0f0f0), MMetal);
            fb(-0.1f, 0.1f, 1.35f, 1.42f, 0.45f, 0.46f, hexColor(0xc02020));
            fb(-0.035f, 0.035f, 1.28f, 1.49f, 0.45f, 0.46f, hexColor(0xc02020));
            break;
        case Prop::GunCabinet:
            fb(-0.3f, 0.3f, 0, 1.7f, 0.03f, 0.45f, hexColor(0x2e3a30), MMetal);
            fb(0.18f, 0.22f, 0.8f, 1.0f, 0.45f, 0.48f, hexColor(0x909090), MMetal);
            break;
        case Prop::Workbench:
            fb(-0.49f, 0.49f, 0.85f, 0.92f, 0.04f, 0.72f, hexColor(0x8a6a44), MFloorWood);
            fb(-0.46f, -0.4f, 0, 0.85f, 0.06f, 0.7f, hexColor(0x6a5034));
            fb(0.4f, 0.46f, 0, 0.85f, 0.06f, 0.7f, hexColor(0x6a5034));
            fb(-0.45f, 0.45f, 1.1f, 1.9f, 0.02f, 0.05f, hexColor(0xa08a64), MFloorWood);    // tool board
            fb(-0.3f, -0.25f, 1.3f, 1.7f, 0.05f, 0.1f, hexColor(0x5a5a5e), MMetal);
            fb(0.1f, 0.3f, 1.5f, 1.56f, 0.05f, 0.1f, hexColor(0xb04020));
            break;
        case Prop::Altar:
            fb(-0.49f, 0.49f, 0, 1.0f, 0.1f, 0.7f, hexColor(0xe8e0d0), MPlaster);
            fb(-0.49f, 0.49f, 0.6f, 0.95f, 0.08f, 0.72f, hexColor(0xc0a040), MCarpet);
            mb.material(MPlain);
            for (float u : {-0.3f, 0.3f}) mb.tube(L(u, 1.0f, 0.4f), L(u, 1.35f, 0.4f), 0.03f, 0.03f, 6, hexColor(0xf4f0e0));
            break;
        case Prop::HospitalBed:
            mb.material(MMetal);
            mb.box(V(x + 0.1f, 0.25f, y + 0.05f), V(x + 0.9f, 0.6f, y + 0.95f), hexColor(0xc0c4c8));
            mb.material(MCarpet);
            mb.box(V(x + 0.12f, 0.6f, y + 0.07f), V(x + 0.88f, 0.72f, y + 0.93f), hexColor(0xe8f0f4));
            mb.material(MMetal);
            for (float lx : {0.12f, 0.82f})
                for (float lz : {0.07f, 0.87f}) mb.box(V(x + lx, 0, y + lz), V(x + lx + 0.05f, 0.25f, y + lz + 0.05f), hexColor(0x8a8e92));
            break;
        default:
            if (h > 0) mb.box(V(x + 0.2f, 0, y + 0.2f), V(x + 0.8f, h, y + 0.8f), GRAY);
            break;
    }
    mb.material(MPlain);
}

// ---------------------------------------------------------------------------
// Chunk mesh
// ---------------------------------------------------------------------------

void Renderer::buildChunk(World& world, Chunk& c, ChunkGfx& g) {
    unloadChunk(g);
    g.lamps.clear();
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
            if (t.ground == Ground::Grass && t.building < 0)
                col = mixColor(col, hexColor(0x8a8648), std::clamp((doy - 260) / 60.0f, 0.0f, 0.55f));   // grass yellows
            float n = fbm(x * 0.09f, y * 0.09f, 3, 2);
            col = shade(col, 0.94f + (n - 0.5f) * 0.18f + 0.04f * ((t.variant & 7) / 7.0f));
            ground.material(groundMaterial(t.ground == Ground::Asphalt ? Ground::Gravel : t.ground));
            ground.quad(V((float)x, h, (float)y), V((float)x, h, y + 1.0f), V(x + 1.0f, h, y + 1.0f), V(x + 1.0f, h, (float)y), col);
            // Banks where land meets water
            if (!isWater(t.ground)) {
                Color bank = hexColor(0x5a4a36);
                ground.material(MDirt);
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
            // Tufts of grass
            if ((t.ground == Ground::TallGrass || t.ground == Ground::Stubble || (t.ground == Ground::Grass && t.building < 0 && (t.variant & 7) == 0)) &&
                t.prop == Prop::None) {
                uint32_t s = hash2(x, y, 31);
                Color gc = t.ground == Ground::Stubble ? hexColor(0xb8a868) : mixColor(hexColor(0x7a8a48), hexColor(0xa89a58), std::clamp((doy - 250) / 50.0f, 0.0f, 1.0f));
                props.material(MLeaves);
                int blades = t.ground == Ground::TallGrass ? 3 : 2;
                for (int i = 0; i < blades; ++i) {
                    float bx = x + 0.15f + 0.7f * rnd(s), bz = y + 0.15f + 0.7f * rnd(s);
                    float bh = (t.ground == Ground::TallGrass ? 0.45f : 0.22f) * (0.7f + 0.6f * rnd(s));
                    props.spike(V(bx, 0, bz), V(bx + (rnd(s) - 0.5f) * 0.25f, bh, bz + (rnd(s) - 0.5f) * 0.25f), 0.06f, 3, shade(gc, 0.85f + 0.3f * rnd(s)));
                }
            }
            if (t.prop != Prop::None) addProp(props, world, t, x, y, doy, g);
        }
    addRoadSurfaces(ground, world, x0, y0);
    addRoadMarkings(props, world.map(), x0, y0);
    addPowerLines(props, world, x0, y0);
    g.ground = ground.build();
    g.hasGround = true;
    if (!props.empty()) { g.props = props.build(); g.hasProps = true; }
    g.version = c.version;
}

} // namespace alone
