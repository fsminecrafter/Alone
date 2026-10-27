#include "client/MeshBuilder.h"
#include "core/Rng.h"
#include "raymath.h"
#include <cmath>
#include <cstring>

namespace alone {

Color hexColor(uint32_t rgb, unsigned char a) {
    return Color{(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb, a};
}

Color shade(Color c, float f) {
    auto ch = [&](unsigned char v) { return (unsigned char)Clamp(v * f, 0, 255); };
    return Color{ch(c.r), ch(c.g), ch(c.b), c.a};
}

Color mixColor(Color a, Color b, float t) {
    return Color{(unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                 (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t)};
}

void MeshBuilder::tri(Vector3 a, Vector3 b, Vector3 c, Color col) {
    Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
    if (Vector3LengthSqr(n) < 1e-12f) return;   // degenerate
    for (Vector3 v : {a, b, c}) {
        pos_.insert(pos_.end(), {v.x, v.y, v.z});
        nrm_.insert(nrm_.end(), {n.x, n.y, n.z});
        col_.insert(col_.end(), {col.r, col.g, col.b, col.a});
        uv_.insert(uv_.end(), {mat_, param_});
    }
}

void MeshBuilder::quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col) {
    tri(a, b, c, col);
    tri(a, c, d, col);
}

void MeshBuilder::box(Vector3 mn, Vector3 mx, Color col, bool bottom) {
    Color top = col, side1 = col, side2 = col;
    // top
    quad({mn.x, mx.y, mn.z}, {mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z}, {mx.x, mx.y, mn.z}, top);
    // +z
    quad({mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}, side1);
    // -z
    quad({mx.x, mn.y, mn.z}, {mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, side1);
    // +x
    quad({mx.x, mn.y, mx.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, side2);
    // -x
    quad({mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}, side2);
    if (bottom) quad({mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}, shade(col, 0.7f));
}

void MeshBuilder::boxFaces(Vector3 mn, Vector3 mx, Color sides, Color top) {
    quad({mn.x, mx.y, mn.z}, {mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z}, {mx.x, mx.y, mn.z}, top);
    quad({mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}, sides);
    quad({mx.x, mn.y, mn.z}, {mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, sides);
    quad({mx.x, mn.y, mx.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, sides);
    quad({mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}, sides);
}

void MeshBuilder::orientedBox(Vector3 c, Vector3 h, float yaw, Color col) {
    float cs = std::cos(yaw), sn = std::sin(yaw);
    auto P = [&](float x, float y, float z) {
        return Vector3{c.x + x * cs - z * sn, c.y + y, c.z + x * sn + z * cs};
    };
    Vector3 v[8] = {P(-h.x, -h.y, -h.z), P(h.x, -h.y, -h.z), P(h.x, -h.y, h.z), P(-h.x, -h.y, h.z),
                    P(-h.x, h.y, -h.z),  P(h.x, h.y, -h.z),  P(h.x, h.y, h.z),  P(-h.x, h.y, h.z)};
    quad(v[4], v[7], v[6], v[5], col);
    quad(v[3], v[2], v[6], v[7], col);
    quad(v[1], v[0], v[4], v[5], col);
    quad(v[2], v[1], v[5], v[6], col);
    quad(v[0], v[3], v[7], v[4], col);
}

void MeshBuilder::cylinder(Vector3 b, float r, float h, int n, Color col, bool caps) {
    for (int i = 0; i < n; ++i) {
        float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
        Vector3 p0{b.x + std::cos(a0) * r, b.y, b.z + std::sin(a0) * r};
        Vector3 p1{b.x + std::cos(a1) * r, b.y, b.z + std::sin(a1) * r};
        Vector3 q0{p0.x, b.y + h, p0.z}, q1{p1.x, b.y + h, p1.z};
        quad(p0, q0, q1, p1, col);
        if (caps) tri(Vector3{b.x, b.y + h, b.z}, q1, q0, col);
    }
}

void MeshBuilder::cone(Vector3 b, float r, float h, int n, Color col) {
    Vector3 tip{b.x, b.y + h, b.z};
    for (int i = 0; i < n; ++i) {
        float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
        Vector3 p0{b.x + std::cos(a0) * r, b.y, b.z + std::sin(a0) * r};
        Vector3 p1{b.x + std::cos(a1) * r, b.y, b.z + std::sin(a1) * r};
        tri(p0, tip, p1, col);
        tri(p0, p1, b, col);
    }
}

void MeshBuilder::blob(Vector3 c, Vector3 r, int slices, int rings, Color col, uint32_t seed, float wobble) {
    auto vert = [&](int i, int j) {
        float th = PI * j / rings, ph = 2 * PI * i / slices;
        float w = 1.0f;
        if (wobble > 0 && j > 0 && j < rings)
            w += (hashFloat(hash3(i % slices, j, 7, seed)) - 0.5f) * wobble;
        return Vector3{c.x + r.x * std::sin(th) * std::cos(ph) * w, c.y + r.y * std::cos(th) * w,
                       c.z + r.z * std::sin(th) * std::sin(ph) * w};
    };
    for (int j = 0; j < rings; ++j)
        for (int i = 0; i < slices; ++i) {
            Vector3 a = vert(i, j), b = vert(i + 1, j), cc = vert(i + 1, j + 1), d = vert(i, j + 1);
            Color k = col;
            if (j == 0) tri(a, cc, d, k);
            else if (j == rings - 1) tri(a, b, d, k);
            else quad(a, b, cc, d, k);
        }
}

void MeshBuilder::gable(Vector3 mn, Vector3 mx, float rh, bool alongX, Color roof, Color gc, float o) {
    Color r2 = roof;
    if (alongX) {
        float zc = (mn.z + mx.z) * 0.5f;
        Vector3 r0{mn.x - o, mx.y + rh, zc}, r1{mx.x + o, mx.y + rh, zc};
        // slopes
        quad({mn.x - o, mx.y - o * 0.5f, mx.z + o}, {mx.x + o, mx.y - o * 0.5f, mx.z + o}, r1, r0, roof);
        quad({mx.x + o, mx.y - o * 0.5f, mn.z - o}, {mn.x - o, mx.y - o * 0.5f, mn.z - o}, r0, r1, r2);
        // gable ends
        tri({mn.x, mx.y, mx.z}, {mn.x, mx.y + rh, zc}, {mn.x, mx.y, mn.z}, gc);
        tri({mx.x, mx.y, mn.z}, {mx.x, mx.y + rh, zc}, {mx.x, mx.y, mx.z}, gc);
    } else {
        float xc = (mn.x + mx.x) * 0.5f;
        Vector3 r0{xc, mx.y + rh, mn.z - o}, r1{xc, mx.y + rh, mx.z + o};
        quad({mx.x + o, mx.y - o * 0.5f, mx.z + o}, {mx.x + o, mx.y - o * 0.5f, mn.z - o}, r0, r1, roof);
        quad({mn.x - o, mx.y - o * 0.5f, mn.z - o}, {mn.x - o, mx.y - o * 0.5f, mx.z + o}, r1, r0, r2);
        tri({mn.x, mx.y, mn.z}, {xc, mx.y + rh, mn.z}, {mx.x, mx.y, mn.z}, gc);
        tri({mx.x, mx.y, mx.z}, {xc, mx.y + rh, mx.z}, {mn.x, mx.y, mx.z}, gc);
    }
}

void MeshBuilder::hip(Vector3 mn, Vector3 mx, float h, Color roof, float o) {
    float y0 = mx.y, y1 = mx.y + h;
    float x0 = mn.x - o, x1 = mx.x + o, z0 = mn.z - o, z1 = mx.z + o;
    float inset = std::min(x1 - x0, z1 - z0) * 0.5f;
    Vector3 a{x0, y0, z0}, b{x1, y0, z0}, c{x1, y0, z1}, d{x0, y0, z1};
    Vector3 ra, rb;
    if (x1 - x0 >= z1 - z0) { ra = {x0 + inset, y1, (z0 + z1) / 2}; rb = {x1 - inset, y1, (z0 + z1) / 2}; }
    else { ra = {(x0 + x1) / 2, y1, z0 + inset}; rb = {(x0 + x1) / 2, y1, z1 - inset}; }
    if (x1 - x0 >= z1 - z0) {
        quad(d, c, rb, ra, roof);                    // south
        quad(b, a, ra, rb, roof);      // north
        tri(a, d, ra, roof);            // west
        tri(c, b, rb, roof);            // east
    } else {
        quad(c, b, ra, rb, roof);
        quad(a, d, rb, ra, roof);
        tri(b, a, ra, roof);
        tri(d, c, rb, roof);
    }
}

void MeshBuilder::tube(Vector3 a, Vector3 b, float r0, float r1, int n, Color col, bool capEnd) {
    Vector3 axis = Vector3Subtract(b, a);
    float len = Vector3Length(axis);
    if (len < 1e-4f) return;
    Vector3 d = Vector3Scale(axis, 1.0f / len);
    Vector3 helper = std::fabs(d.y) < 0.95f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 u = Vector3Normalize(Vector3CrossProduct(d, helper));
    Vector3 v = Vector3CrossProduct(u, d);
    auto ring = [&](Vector3 c, float r, float ang) {
        return Vector3Add(c, Vector3Add(Vector3Scale(u, std::cos(ang) * r), Vector3Scale(v, std::sin(ang) * r)));
    };
    for (int i = 0; i < n; ++i) {
        float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
        Vector3 p0 = ring(a, r0, a0), p1 = ring(a, r0, a1), q0 = ring(b, r1, a0), q1 = ring(b, r1, a1);
        quad(p0, q0, q1, p1, col);
        if (capEnd && r1 > 0.001f) tri(b, q1, q0, col);
    }
}

void MeshBuilder::spike(Vector3 a, Vector3 b, float r, int n, Color col) {
    tube(a, b, r, 0.0f, n, col, false);
}

Mesh MeshBuilder::build() const {
    Mesh m{};
    m.vertexCount = (int)(pos_.size() / 3);
    m.triangleCount = m.vertexCount / 3;
    m.vertices = (float*)MemAlloc((unsigned)(pos_.size() * sizeof(float)));
    m.normals = (float*)MemAlloc((unsigned)(nrm_.size() * sizeof(float)));
    m.colors = (unsigned char*)MemAlloc((unsigned)col_.size());
    m.texcoords = (float*)MemAlloc((unsigned)(uv_.size() * sizeof(float)));
    std::memcpy(m.texcoords, uv_.data(), uv_.size() * sizeof(float));
    std::memcpy(m.vertices, pos_.data(), pos_.size() * sizeof(float));
    std::memcpy(m.normals, nrm_.data(), nrm_.size() * sizeof(float));
    std::memcpy(m.colors, col_.data(), col_.size());
    UploadMesh(&m, false);
    // Free CPU copies, the GPU has them
    MemFree(m.vertices); m.vertices = nullptr;
    MemFree(m.normals); m.normals = nullptr;
    MemFree(m.colors); m.colors = nullptr;
    MemFree(m.texcoords); m.texcoords = nullptr;
    return m;
}

} // namespace alone
