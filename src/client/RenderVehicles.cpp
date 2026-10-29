// Cars, bikes and other people.
//
// The car is lofted from cross-sections along its length (a rounded box for
// the body), with a glass cabin on top, wheel arches, pillars, lights,
// bumpers and Swedish number plates. Four body styles: estate, hatchback,
// saloon and van. The same builder makes the parked cars in the chunk meshes
// and the meshes for cars you've moved.
#include "client/RenderCommon.h"
#include "client/Renderer.h"
#include "core/Rng.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>

namespace alone {
using namespace rc;

namespace {

// Builds faces in the vehicle's own frame (x forward, y up, z to the side) and
// hands the corners through P into the mesh. Each face is turned to face
// `out`, so winding never has to be thought about.
struct Shaper {
    MeshBuilder& mb;
    const VehicleTransform& P;
    void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 out, Color col) {
        Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        if (Vector3DotProduct(n, out) < 0) std::swap(b, d);
        mb.quad(P(a), P(b), P(c), P(d), col);
    }
    void tri(Vector3 a, Vector3 b, Vector3 c, Vector3 out, Color col) {
        Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        if (Vector3DotProduct(n, out) < 0) std::swap(b, c);
        mb.tri(P(a), P(b), P(c), col);
    }
    void box(Vector3 mn, Vector3 mx, Color col) {
        float x0 = mn.x, y0 = mn.y, z0 = mn.z, x1 = mx.x, y1 = mx.y, z1 = mx.z;
        quad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, {1, 0, 0}, col);
        quad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {-1, 0, 0}, col);
        quad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {0, 1, 0}, col);
        quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {0, 0, 1}, col);
        quad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, {0, 0, -1}, col);
    }
    // A half disc on the side of the body (wheel arch), facing out along z
    void arch(float x, float y, float z, float r, Color col) {
        float sz = z > 0 ? 1.0f : -1.0f;
        int n = 10;
        for (int i = 0; i < n; ++i) {
            float a0 = kPi * i / n, a1 = kPi * (i + 1) / n;
            tri({x, y, z}, {x + std::cos(a0) * r, y + std::sin(a0) * r, z}, {x + std::cos(a1) * r, y + std::sin(a1) * r, z}, {0, 0, sz}, col);
        }
    }
    void tube(Vector3 a, Vector3 b, float r, int sides, Color col) { mb.tube(P(a), P(b), r, r, sides, col); }
};

struct CarStyle {
    float front, tail;            // bumper ends
    float wheelF, wheelR, wheelRad;
    float bottom;                 // sill height
    float wsx, belt;              // windscreen base
    float rfx, roofY;             // roof front
    float rrx;                    // roof rear
    float rgx, beltRear;          // rear glass base
    float halfW;
    bool rails;                   // roof rails (estates)
    int doors;                    // 2 or 4 per... side seams
};

const CarStyle kStyles[] = {
    // estate (the classic Swedish Volvo)
    {2.25f, -2.3f, 1.4f, -1.38f, 0.32f, 0.28f, 0.92f, 0.96f, 0.1f, 1.47f, -1.95f, -2.2f, 1.0f, 0.88f, true, 4},
    // hatchback
    {2.0f, -1.95f, 1.26f, -1.24f, 0.31f, 0.27f, 0.78f, 0.94f, 0.0f, 1.45f, -1.3f, -1.9f, 1.0f, 0.85f, false, 2},
    // saloon
    {2.3f, -2.35f, 1.42f, -1.42f, 0.32f, 0.27f, 0.86f, 0.95f, 0.05f, 1.42f, -0.95f, -1.62f, 0.98f, 0.88f, false, 4},
    // van / SUV, higher
    {2.25f, -2.25f, 1.4f, -1.4f, 0.37f, 0.38f, 1.28f, 1.04f, 0.62f, 1.86f, -2.08f, -2.2f, 1.06f, 0.92f, true, 2},
};

const uint32_t kPaint[] = {0xe8e6e0, 0xb4b8bc, 0x1c1d20, 0x1e3352, 0x8a1c1c, 0x2c4a36, 0xc8b88e, 0x5a6670, 0x6a2a3a, 0xd8c040, 0x3a3a3c, 0x8aa0b0};

} // namespace

void addCarModel(MeshBuilder& mb, const VehicleTransform& P, uint32_t look, bool smashed, float dust) {
    const CarStyle& st = kStyles[(look >> 4) % 4];
    Color paint = hexColor(kPaint[look % 12]);
    paint = mixColor(paint, Color{120, 110, 90, 255}, std::clamp(dust, 0.0f, 0.5f));   // grime of weeks standing
    Color dark = hexColor(0x18191b), trim = hexColor(0x2a2b2e), glass = hexColor(0x223038), chrome = hexColor(0xa8acb0);
    Color tyre = hexColor(0x141414), rim = hexColor(0x9a9ea2);
    Shaper S{mb, P};
    float w = st.halfW;

    // --- lower body: lofted rounded box ------------------------------------
    struct Station { float x, y0, y1, w; };
    float hood = st.belt - 0.08f;
    std::vector<Station> sts = {
        {st.front, st.bottom + 0.08f, hood - 0.26f, w - 0.07f},
        {st.front - 0.16f, st.bottom, hood - 0.1f, w - 0.015f},
        {st.front - 0.6f, st.bottom, hood - 0.02f, w},
        {st.wsx, st.bottom, st.belt, w},
        {st.rgx + 0.1f, st.bottom, st.beltRear, w},
        {st.tail + 0.14f, st.bottom, st.beltRear - 0.04f, w - 0.015f},
        {st.tail, st.bottom + 0.08f, st.beltRear - 0.2f, w - 0.07f},
    };
    auto ring = [&](const Station& s, int k) -> Vector3 {
        float r = 0.07f;
        switch (k) {
            case 0: return {s.x, s.y0, s.w - 0.05f};
            case 1: return {s.x, s.y0 + r, s.w};
            case 2: return {s.x, s.y1 - r, s.w};
            case 3: return {s.x, s.y1, s.w - r};
            case 4: return {s.x, s.y1, -(s.w - r)};
            case 5: return {s.x, s.y1 - r, -s.w};
            case 6: return {s.x, s.y0 + r, -s.w};
            default: return {s.x, s.y0, -(s.w - 0.05f)};
        }
    };
    mb.material(MMetal);
    for (size_t i = 0; i + 1 < sts.size(); ++i)
        for (int k = 0; k < 8; ++k) {
            Vector3 a = ring(sts[i], k), b = ring(sts[i + 1], k), c = ring(sts[i + 1], (k + 1) % 8), d = ring(sts[i], (k + 1) % 8);
            Vector3 mid = Vector3Scale(Vector3Add(Vector3Add(a, b), Vector3Add(c, d)), 0.25f);
            Vector3 axis{mid.x, (sts[i].y0 + sts[i].y1) * 0.5f, 0};
            Color col = k == 7 ? dark : paint;   // underside
            S.quad(a, b, c, d, Vector3Subtract(mid, axis), col);
        }
    for (int end = 0; end < 2; ++end) {
        const Station& s = end == 0 ? sts.front() : sts.back();
        Vector3 c{s.x, (s.y0 + s.y1) * 0.5f, 0};
        for (int k = 0; k < 8; ++k) S.tri(c, ring(s, k), ring(s, (k + 1) % 8), {end == 0 ? 1.0f : -1.0f, 0, 0}, paint);
    }

    // --- wheels in their arches ----------------------------------------------
    for (float wx : {st.wheelF, st.wheelR})
        for (float side : {-1.0f, 1.0f}) {
            mb.material(MPlain);
            S.arch(wx, st.wheelRad - 0.01f, side * (w + 0.006f), st.wheelRad + 0.1f, dark);
            S.tube({wx, st.wheelRad, side * (w - 0.2f)}, {wx, st.wheelRad, side * (w - 0.01f)}, st.wheelRad, 14, tyre);
            mb.material(MMetal);
            S.tube({wx, st.wheelRad, side * (w - 0.012f)}, {wx, st.wheelRad, side * (w + 0.0f)}, st.wheelRad * 0.62f, 10, rim);
            S.tube({wx, st.wheelRad, side * (w)}, {wx, st.wheelRad, side * (w + 0.008f)}, st.wheelRad * 0.2f, 6, chrome);
        }

    // --- the cabin: glass with pillars and a painted roof -------------------
    float wb = w - 0.05f, wt = w - 0.2f;
    Vector3 A{st.wsx, st.belt, wb}, B{st.rfx, st.roofY, wt}, C{st.rrx, st.roofY - 0.02f, wt}, D{st.rgx, st.beltRear, wb};
    auto mz = [](Vector3 v) { return Vector3{v.x, v.y, -v.z}; };
    mb.material(MGlass);
    S.quad(A, mz(A), mz(B), B, {1, 0.9f, 0}, glass);                        // windscreen
    S.quad(C, mz(C), mz(D), D, {-1, 0.9f, 0}, glass);                       // rear window
    for (float side : {1.0f, -1.0f}) {
        auto sd = [&](Vector3 v) { return Vector3{v.x, v.y, v.z * side}; };
        Color g = (smashed && side < 0) ? hexColor(0x0a0a0a) : glass;
        S.quad(sd(A), sd(B), sd(C), sd(D), {0, 0.25f, side}, g);
    }
    mb.material(MMetal);
    S.quad(B, mz(B), mz(C), C, {0, 1, 0}, paint);                           // roof
    // pillars and the window frame, a hair outside the glass
    for (float side : {1.0f, -1.0f}) {
        auto o = [&](Vector3 v, float dx) { return Vector3{v.x + dx, v.y, (v.z + 0.012f) * side}; };
        Vector3 out{0, 0.25f, side};
        S.quad(o(A, 0), o(B, 0), o(B, -0.11f), o(A, -0.14f), out, paint);       // A pillar
        float bx = (st.wsx + st.rrx) * 0.5f - 0.1f;
        float tb = (bx - st.rgx) / (st.wsx - st.rgx);
        float topT = (bx - st.rrx) / (st.rfx - st.rrx);
        Vector3 bb{bx, st.beltRear + (st.belt - st.beltRear) * tb, wb}, bt{bx, st.roofY - 0.01f, wt};
        (void)topT;
        S.quad(o(bb, -0.05f), o(bb, 0.05f), o(bt, 0.04f), o(bt, -0.04f), out, trim);   // B pillar
        if (st.doors == 4 && st.rrx < -1.5f) {
            float cx = st.rrx + 0.45f;
            Vector3 cb{cx, st.beltRear + 0.0f, wb}, ct{cx, st.roofY - 0.02f, wt};
            S.quad(o(cb, -0.04f), o(cb, 0.04f), o(ct, 0.04f), o(ct, -0.04f), out, trim);   // C pillar (estate)
        }
        S.quad(o(D, 0.16f), o(D, 0), o(C, 0), o(C, 0.12f), out, paint);       // rearmost pillar
        // dark rubber along the bottom of the windows
        S.quad(o(Vector3{A.x, A.y + 0.02f, A.z}, 0), o(Vector3{D.x, D.y + 0.02f, D.z}, 0), o(D, 0), o(A, 0), out, trim);
    }
    if (st.rails)
        for (float side : {1.0f, -1.0f})
            S.box({st.rrx + 0.1f, st.roofY, side * wt * 0.82f - 0.025f}, {st.rfx - 0.15f, st.roofY + 0.05f, side * wt * 0.82f + 0.025f}, trim);

    // --- details --------------------------------------------------------------
    mb.material(MPlain);
    float bw = w - 0.03f;
    S.box({st.front - 0.04f, st.bottom + 0.02f, -bw}, {st.front + 0.07f, st.bottom + 0.2f, bw}, trim);        // bumpers
    S.box({st.tail - 0.07f, st.bottom + 0.04f, -bw}, {st.tail + 0.04f, st.bottom + 0.22f, bw}, trim);
    S.box({st.front - 0.01f, st.bottom + 0.22f, -0.42f}, {st.front + 0.02f, hood - 0.3f, 0.42f}, dark);      // grille
    Color lamp = hexColor(0xf2eedc), red = hexColor(0xa81810), amber = hexColor(0xd88a20);
    for (float side : {1.0f, -1.0f}) {
        float z0 = side * 0.5f, z1 = side * (w - 0.08f);
        S.box({st.front - 0.08f, hood - 0.32f, std::min(z0, z1)}, {st.front + 0.01f, hood - 0.22f, std::max(z0, z1)}, lamp);
        S.box({st.front - 0.05f, st.bottom + 0.1f, std::min(side * 0.62f, side * 0.76f)}, {st.front + 0.08f, st.bottom + 0.15f, std::max(side * 0.62f, side * 0.76f)}, amber);
        float r0 = side * 0.52f, r1 = side * (w - 0.05f);
        S.box({st.tail - 0.02f, st.beltRear - 0.36f, std::min(r0, r1)}, {st.tail + 0.06f, st.beltRear - 0.14f, std::max(r0, r1)}, red);
        // mirrors
        mb.material(MMetal);
        S.box({st.wsx - 0.12f, st.belt + 0.02f, side > 0 ? w - 0.02f : -(w + 0.13f)}, {st.wsx - 0.02f, st.belt + 0.12f, side > 0 ? w + 0.13f : -(w - 0.02f)}, paint);
        mb.material(MPlain);
        // door handles and seams
        for (float hx : {st.wsx - 0.75f, st.doors == 4 ? st.wsx - 1.75f : -100.0f}) {
            if (hx < st.tail + 0.4f) continue;
            float z = side * (w + 0.004f);
            S.box({hx - 0.08f, st.belt - 0.12f, std::min(z, z + side * 0.015f)}, {hx + 0.08f, st.belt - 0.09f, std::max(z, z + side * 0.015f)}, trim);
        }
        std::vector<float> seams = {st.wsx - 0.06f, st.wsx - 1.05f};
        if (st.doors == 4) seams.push_back(st.wsx - 2.0f);
        for (float sx : seams) {
            if (sx < st.tail + 0.3f) continue;
            float z = side * (w + 0.003f);
            S.quad({sx - 0.006f, st.bottom + 0.06f, z}, {sx + 0.006f, st.bottom + 0.06f, z}, {sx + 0.006f, st.belt - 0.01f, z},
                   {sx - 0.006f, st.belt - 0.01f, z}, {0, 0, side}, dark);
        }
        // a dark strip along the sills
        float z = side * (w + 0.002f);
        S.quad({st.front - 0.3f, st.bottom + 0.0f, z}, {st.tail + 0.3f, st.bottom, z}, {st.tail + 0.3f, st.bottom + 0.09f, z},
               {st.front - 0.3f, st.bottom + 0.09f, z}, {0, 0, side}, trim);
    }
    // Swedish number plates: white with a blue strip on the left
    for (int end = 0; end < 2; ++end) {
        float x = end == 0 ? st.front + 0.075f : st.tail - 0.075f;
        float dir = end == 0 ? 1.0f : -1.0f;
        float y0 = end == 0 ? st.bottom + 0.05f : st.beltRear - 0.42f;
        S.box({x - 0.005f, y0, -0.26f}, {x + 0.005f, y0 + 0.11f, 0.26f}, hexColor(0xf4f4f0));
        float zb = dir > 0 ? -0.26f : 0.2f;
        S.box({x - 0.006f + dir * 0.002f, y0, zb}, {x + 0.006f + dir * 0.002f, y0 + 0.11f, zb + 0.06f}, hexColor(0x1a3a9a));
        S.box({x - 0.006f + dir * 0.002f, y0 + 0.035f, -0.14f}, {x + 0.006f + dir * 0.002f, y0 + 0.075f, dir > 0 ? 0.2f : 0.15f}, hexColor(0x202020));
    }
}

void addBikeModel(MeshBuilder& mb, const VehicleTransform& P, uint32_t look, float lean) {
    static const uint32_t frames[] = {0x1a1a1c, 0xb02020, 0x2a4a8a, 0x2a6a3a, 0xd8d8d0, 0x6a6a70, 0xc89020, 0x4a2a5a};
    Color frame = hexColor(frames[look % 8]), dark = hexColor(0x161616), metal = hexColor(0x9a9ea4), saddle = hexColor(0x2a2220);
    float cl = std::cos(lean), sl = std::sin(lean);
    VehicleTransform L = [&](Vector3 v) { return P(Vector3{v.x, v.y * cl - v.z * sl, v.z * cl + v.y * sl}); };   // leaning on its stand
    Shaper S{mb, L};
    float r = 0.34f, fx = 0.53f, rx = -0.5f;
    mb.material(MPlain);
    // wheels: a tyre ring, a few spokes, the hub
    for (float wx : {fx, rx}) {
        int n = 16;
        for (int i = 0; i < n; ++i) {
            float a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
            S.tube({wx + std::cos(a0) * r, r + std::sin(a0) * r, 0}, {wx + std::cos(a1) * r, r + std::sin(a1) * r, 0}, 0.026f, 5, dark);
        }
        for (int i = 0; i < 4; ++i) {
            float a = kPi * i / 4;
            S.tube({wx + std::cos(a) * r * 0.92f, r + std::sin(a) * r * 0.92f, 0}, {wx - std::cos(a) * r * 0.92f, r - std::sin(a) * r * 0.92f, 0}, 0.004f, 3, metal);
        }
        S.tube({wx, r, -0.04f}, {wx, r, 0.04f}, 0.03f, 6, metal);
    }
    // frame
    mb.material(MMetal);
    Vector3 bb{-0.02f, 0.3f, 0}, seat{-0.14f, 0.8f, 0}, headT{0.4f, 0.84f, 0}, headB{0.44f, 0.7f, 0}, rh{rx, r, 0}, fh{fx, r, 0};
    float t = 0.022f;
    S.tube(rh, bb, t, 6, frame);
    S.tube(bb, seat, t, 6, frame);
    S.tube(seat, headT, t, 6, frame);
    S.tube(bb, headB, t * 1.2f, 6, frame);
    S.tube({-0.12f, 0.74f, 0}, rh, t * 0.8f, 6, frame);
    S.tube(headB, headT, t * 1.3f, 6, frame);
    S.tube({0.44f, 0.72f, 0.04f}, {fx, r, 0.04f}, t * 0.7f, 5, metal);
    S.tube({0.44f, 0.72f, -0.04f}, {fx, r, -0.04f}, t * 0.7f, 5, metal);
    // handlebars and saddle
    S.tube(headT, {0.38f, 1.0f, 0}, 0.016f, 5, metal);
    S.tube({0.36f, 1.0f, -0.27f}, {0.36f, 1.0f, 0.27f}, 0.014f, 6, metal);
    mb.material(MPlain);
    for (float z : {-0.27f, 0.27f}) S.tube({0.36f, 1.0f, z}, {0.36f, 1.0f, z * 0.82f}, 0.02f, 6, dark);
    S.tube(seat, {-0.16f, 0.88f, 0}, 0.012f, 5, metal);
    S.box({-0.29f, 0.88f, -0.07f}, {-0.03f, 0.93f, 0.07f}, saddle);
    // crank and pedals
    S.tube({-0.02f, 0.3f, -0.1f}, {-0.02f, 0.3f, 0.1f}, 0.012f, 5, metal);
    S.box({0.06f, 0.2f, 0.1f}, {0.12f, 0.22f, 0.17f}, dark);
    S.box({-0.16f, 0.38f, -0.17f}, {-0.1f, 0.4f, -0.1f}, dark);
    // a mudguard and a rack: it's a Swedish town bike
    mb.material(MMetal);
    for (int i = 0; i < 6; ++i) {
        float a0 = kPi * (0.15f + 0.12f * i), a1 = kPi * (0.15f + 0.12f * (i + 1));
        S.tube({rx + std::cos(a0) * (r + 0.05f), r + std::sin(a0) * (r + 0.05f), 0}, {rx + std::cos(a1) * (r + 0.05f), r + std::sin(a1) * (r + 0.05f), 0}, 0.03f, 4, frame);
    }
    S.box({-0.75f, 0.68f, -0.09f}, {-0.3f, 0.7f, 0.09f}, metal);
}

// ---------------------------------------------------------------------------
// Moving vehicles
// ---------------------------------------------------------------------------

const Mesh& Renderer::vehicleMesh(const Vehicle& v) {
    int key = (int)v.kind * 1000000 + (v.variant & 0xFFFF) * 4 + (v.windowSmashed ? 1 : 0) + (v.damage >= 1 ? 2 : 0);
    auto it = vehicleMeshes_.find(key);
    if (it != vehicleMeshes_.end()) return it->second;
    MeshBuilder mb;
    mb.noAo();
    VehicleTransform id = [](Vector3 p) { return p; };
    if (v.kind == VehicleKind::Car) addCarModel(mb, id, (uint32_t)v.variant, v.windowSmashed, v.damage >= 1 ? 0.5f : 0.15f);
    else addBikeModel(mb, id, (uint32_t)v.variant, 0);
    Mesh m = mb.build();
    return vehicleMeshes_.emplace(key, m).first->second;
}

void Renderer::drawVehicles(Game& game, bool shadowPass) {
    (void)shadowPass;
    World& w = game.world();
    Vec2 pp = game.player().pos;
    for (const auto& v : game.vehicles()) {
        if (distance(v.pos, pp) > 100) continue;
        float y = w.standHeight(v.pos);
        // a bike nobody rides leans on its stand
        bool ridden = v.id == game.player().vehicle;
        for (const auto& s : game.survivors()) ridden |= s.ride == v.id;
        float lean = (v.kind == VehicleKind::Bike && !ridden) ? 0.14f : 0.0f;
        Matrix t = MatrixRotateX(lean);
        // wheels follow the ground: tilt the body with the slope it stands on
        Vec2 f = v.forward();
        float half = v.length() * 0.45f;
        float pitch = std::atan2(w.standHeight(v.pos + f * half) - w.standHeight(v.pos - f * half), 2 * half);
        t = MatrixMultiply(t, MatrixRotateZ(pitch));
        t = MatrixMultiply(t, MatrixRotateY(-v.heading));
        t = MatrixMultiply(t, MatrixTranslate(v.pos.x, y, v.pos.y));
        active_->maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        DrawMesh(vehicleMesh(v), *active_, t);
    }
}

// ---------------------------------------------------------------------------
// Other people
// ---------------------------------------------------------------------------

void Renderer::updateNpcRigs(Game& game, float time, float dt) {
    World& w = game.world();
    Vec2 pp = game.player().pos;
    for (const auto& s : game.survivors()) {
        float d = distance(s.pos, pp);
        if (d > 90) continue;
        NpcGfx& g = npcs_[s.id];
        if (!g.init) {
            g.model = std::make_unique<CharacterModel>();
            if (!g.model->init(BodyShape{s.def->gender == Gender::Woman, s.def->hairStyle})) continue;
            g.lastPos = s.pos;
            g.init = true;
        }
        float moved = distance(s.pos, g.lastPos);
        if (moved > 5) moved = 0;
        g.lastPos = s.pos;
        g.dist += moved;
        float inst = dt > 0 ? moved / dt : 0;
        g.speed += (inst - g.speed) * std::min(1.0f, dt * 10.0f);
        AnimInput in;
        in.speed = g.speed;
        in.distance = g.dist;
        in.dead = !s.alive();
        in.asleep = !s.alive() || s.asleep() || s.dozing;
        in.swing = s.swing;
        in.time = time + s.id * 3.7f;
        const ItemInstance* wpn = s.weapon();
        in.weapon = 0;
        if (wpn && s.alive()) {
            const ItemDef* wd = wpn->def;
            in.weapon = wd->firearm ? 3 : (wd->hasTag("knife") && !wd->hasTag("axe")) || wd->reach < 0.75f ? 2 : 1;
            if (s.state != SurvivorState::Hunt && !s.joined && s.def->disposition != Disposition::Hostile && s.def->disposition != Disposition::Raider)
                in.weapon = 0;   // friendly people don't walk around with a weapon out
        }
        in.aiming = in.weapon == 3 && s.lastAttack < 1.5f;
        if (in.weapon == 3) in.swing = 0;
        in.hunch = s.alive() ? std::clamp(1.0f - s.hp / s.def->hp - 0.3f, 0.0f, 1.0f) : 0;
        // in a car seat or on a bike
        Vec2 at = s.pos;
        float facing = s.facing;
        const Vehicle* rv = nullptr;
        if (s.ride >= 0)
            for (const auto& v : game.vehicles())
                if (v.id == s.ride) rv = &v;
        if (rv) {
            in.ride = rv->kind == VehicleKind::Car ? 2 : 1;
            in.speed = 0;
            in.weapon = 0;
            in.swing = 0;
            in.aiming = false;
            Vec2 f = rv->forward(), side{-f.y, f.x};
            facing = rv->heading;
            if (rv->kind == VehicleKind::Bike) at = rv->pos - f * 0.16f;
            else {
                // 1 front passenger; 2-4 the back row; 5-6 the third row of a van
                int k = s.seat;
                float back = k <= 1 ? 0.3f : k <= 4 ? 1.2f : 2.0f;
                float across = k <= 1 ? 0.36f : k == 2 || k == 5 ? -0.4f : k == 3 || k == 6 ? 0.4f : 0.0f;
                at = rv->pos - f * back + side * across;
            }
        }
        g.rig.update(in, dt);
        float stand = w.standHeight(at);
        if (s.asleep()) {   // in bed, on the sofa
            Prop under = w.tile((int)std::floor(s.pos.x), (int)std::floor(s.pos.y)).prop;
            if (under == Prop::Bed || under == Prop::HospitalBed) stand += 0.42f;
            else if (under == Prop::Sofa) stand += 0.38f;
        }
        if (std::fabs(stand - g.y) > 3 || moved == 0) g.y = stand;
        else g.y += (stand - g.y) * std::min(1.0f, dt * 14.0f);
        g.rig.place(V(at.x, g.y, at.y), facing);
        CharacterLook L = lookOf(s.worn, nullptr);
        L.skin = hexColor(s.def->skin);
        L.hair = hexColor(s.def->hair);
        g.model->skin(g.rig, L);
        g.weapon = in.weapon != 0 && wpn ? wpn->def : nullptr;
    }
}

void Renderer::drawSurvivors(Game& game) {
    Vec2 pp = game.player().pos;
    for (const auto& s : game.survivors()) {
        if (distance(s.pos, pp) > 90) continue;
        auto it = npcs_.find(s.id);
        if (it == npcs_.end() || !it->second.init) continue;
        drawCharacter(it->second.rig, *it->second.model, s.worn, nullptr, it->second.weapon);
    }
}

} // namespace alone
