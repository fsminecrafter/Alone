#include "client/CharacterModel.h"
#include "client/MeshBuilder.h"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <cstring>

namespace alone {

namespace {

const Vector3 kX{1, 0, 0}, kY{0, 1, 0}, kZ{0, 0, 1};

float smooth(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Two axes perpendicular to a direction, for the cross-sections of a tube
void ringFrame(Vector3 dir, Vector3& u, Vector3& v) {
    Vector3 ref = std::fabs(dir.y) < 0.9f ? kY : kX;
    u = Vector3Normalize(Vector3CrossProduct(ref, dir));
    v = Vector3Normalize(Vector3CrossProduct(dir, u));
}

// Hairline: lower at the back of the head than over the forehead
bool keepHair(int ring, int count, float angle) {
    float t = -0.98f + 1.96f * (ring + 0.5f) / (count - 1);
    float a = std::fabs(std::atan2(std::sin(angle), std::cos(angle)));   // 0 = front
    float line = 0.42f + (-0.52f - 0.42f) * smooth(0.75f, 2.55f, a);
    return t > line;
}

} // namespace

void CharacterModel::tube(const std::vector<Ring>& rings, int seg, Region region, BodyPart part, bool capStart,
                          bool capEnd, bool (*keep)(int, int, float)) {
    int base = (int)rest_.size();
    int n = (int)rings.size();
    for (const Ring& r : rings) {
        for (int k = 0; k < seg; ++k) {
            float a = 2.0f * PI * k / seg;
            float ca = std::cos(a), sa = std::sin(a);
            float e = r.squareness;
            float cx = std::copysign(std::pow(std::fabs(ca), e), ca), sx = std::copysign(std::pow(std::fabs(sa), e), sa);
            Vtx v;
            v.pos = Vector3Add(r.c, Vector3Add(Vector3Scale(r.u, r.ru * cx), Vector3Scale(r.v, r.rv * sx)));
            v.b0 = (uint8_t)r.b0;
            v.b1 = (uint8_t)r.b1;
            v.w0 = r.w0;
            v.region = (uint8_t)region;
            v.part = (uint8_t)part;
            v.t = r.t;
            rest_.push_back(v);
        }
    }
    // Triangles wound so they face away from `inside`
    auto tri = [&](int a, int b, int c, Vector3 inside) {
        Vector3 pa = rest_[a].pos, pb = rest_[b].pos, pc = rest_[c].pos;
        Vector3 nrm = Vector3CrossProduct(Vector3Subtract(pb, pa), Vector3Subtract(pc, pa));
        Vector3 g = Vector3Scale(Vector3Add(pa, Vector3Add(pb, pc)), 1.0f / 3.0f);
        if (Vector3DotProduct(nrm, Vector3Subtract(g, inside)) < 0) std::swap(b, c);
        idx_.push_back((unsigned short)a);
        idx_.push_back((unsigned short)b);
        idx_.push_back((unsigned short)c);
    };
    for (int i = 0; i + 1 < n; ++i) {
        Vector3 mid = Vector3Scale(Vector3Add(rings[i].c, rings[i + 1].c), 0.5f);
        for (int k = 0; k < seg; ++k) {
            if (keep && !keep(i, n, 2.0f * PI * (k + 0.5f) / seg)) continue;
            int a = base + i * seg + k, b = base + i * seg + (k + 1) % seg;
            int c = base + (i + 1) * seg + (k + 1) % seg, d = base + (i + 1) * seg + k;
            tri(a, b, c, mid);
            tri(a, c, d, mid);
        }
    }
    auto cap = [&](int ringIndex, int insideIndex) {
        const Ring& r = rings[ringIndex];
        Vtx v;
        v.pos = r.c;
        v.b0 = (uint8_t)r.b0; v.b1 = (uint8_t)r.b1; v.w0 = r.w0;
        v.region = (uint8_t)region; v.part = (uint8_t)part; v.t = r.t;
        int centre = (int)rest_.size();
        rest_.push_back(v);
        Vector3 inside = rings[insideIndex].c;
        for (int k = 0; k < seg; ++k)
            tri(centre, base + ringIndex * seg + k, base + ringIndex * seg + (k + 1) % seg, inside);
    };
    if (capStart && n > 1) cap(0, 1);
    if (capEnd && n > 1) cap(n - 1, n - 2);
}

void CharacterModel::ellipsoid(Vector3 c, Vector3 r, int bone, Region region, BodyPart part, int count, int seg,
                               bool (*keep)(int, int, float)) {
    std::vector<Ring> rings;
    for (int i = 0; i < count; ++i) {
        float t = -0.98f + 1.96f * i / (count - 1);
        float k = std::sqrt(std::max(0.0f, 1.0f - t * t));
        rings.push_back({Vector3Add(c, {0, r.y * t, 0}), kX, kZ, r.x * k, r.z * k, bone, bone, 1.0f, t});
    }
    tube(rings, seg, region, part, true, true, keep);
}

void CharacterModel::build() {
    rest_.clear();
    idx_.clear();
    CharacterRig rest;
    rest.place({0, 0, 0}, 0);
    for (int b = 0; b < BoneCount; ++b) invBind_[b] = MatrixInvert(rest.bone(b));
    auto J = [&](int b) { return rest.joint(b); };

    // --- torso: hips, waist, chest and shoulders in one piece ------------------
    {
        struct TR { float y, ru, rv, cx; int b0, b1; float w0; };
        const TR tr[] = {
            {0.835f, 0.072f, 0.100f, -0.004f, BPelvis, BPelvis, 1.0f},
            {0.870f, 0.100f, 0.158f, -0.010f, BPelvis, BPelvis, 1.0f},
            {0.930f, 0.112f, 0.176f, -0.013f, BPelvis, BPelvis, 1.0f},
            {0.995f, 0.106f, 0.168f, -0.006f, BPelvis, BSpine, 0.7f},
            {1.060f, 0.100f, 0.156f, 0.000f, BSpine, BPelvis, 0.75f},
            {1.125f, 0.102f, 0.158f, 0.006f, BSpine, BSpine, 1.0f},
            {1.195f, 0.110f, 0.170f, 0.010f, BSpine, BChest, 0.6f},
            {1.265f, 0.118f, 0.182f, 0.012f, BChest, BSpine, 0.75f},
            {1.335f, 0.121f, 0.194f, 0.012f, BChest, BChest, 1.0f},
            {1.395f, 0.116f, 0.203f, 0.007f, BChest, BChest, 1.0f},
            {1.440f, 0.104f, 0.202f, 0.000f, BChest, BChest, 1.0f},
            {1.478f, 0.084f, 0.176f, -0.004f, BChest, BChest, 1.0f},
            {1.505f, 0.062f, 0.098f, -0.004f, BChest, BNeck, 0.7f},
        };
        std::vector<Ring> rings;
        for (const auto& r : tr) {
            float ru = r.ru, rv = r.rv;
            if (shape_.female) {
                // narrower shoulders and waist, wider hips
                rv *= r.y > 1.38f ? 0.9f : r.y > 1.03f && r.y < 1.2f ? 0.88f : r.y < 1.0f ? 1.07f : 0.95f;
                if (r.y > 1.24f && r.y < 1.36f) ru *= 1.04f;
            }
            rings.push_back({{r.cx, r.y, 0}, kX, kZ, ru, rv, r.b0, r.b1, r.w0, r.y, 0.8f});
        }
        tube(rings, 22, RTorso, BodyPart::Torso, true, false);
        if (shape_.female)
            for (float s : {-1.0f, 1.0f})
                ellipsoid({0.088f, 1.305f, 0.056f * s}, {0.040f, 0.046f, 0.046f}, BChest, RTorso, BodyPart::Torso, 6, 10);
    }
    // --- neck ---------------------------------------------------------------------
    {
        std::vector<Ring> rings = {
            {{0.004f, 1.455f, 0}, kX, kZ, 0.058f, 0.060f, BChest, BChest, 1.0f, 0},
            {{0.006f, 1.520f, 0}, kX, kZ, 0.053f, 0.055f, BNeck, BChest, 0.7f, 0},
            {{0.008f, 1.575f, 0}, kX, kZ, 0.050f, 0.052f, BNeck, BHead, 0.5f, 0},
            {{0.010f, 1.625f, 0}, kX, kZ, 0.048f, 0.050f, BHead, BHead, 1.0f, 0},
        };
        tube(rings, 12, RNeck, BodyPart::Head, false, false);
    }
    // --- head: an egg, narrower at the jaw, the back of the skull rounder ----------
    {
        const float ts[] = {-0.965f, -0.86f, -0.68f, -0.46f, -0.22f, 0.04f, 0.30f, 0.54f, 0.74f, 0.88f, 0.965f, 0.995f};
        std::vector<Ring> rings;
        for (float t : ts) {
            float k = std::sqrt(std::max(0.0f, 1.0f - t * t));
            float jaw = smooth(-0.1f, -0.9f, t);
            float back = smooth(0.15f, 0.8f, t);
            float rx = 0.100f * k * (1.0f - 0.08f * jaw);
            float rz = 0.088f * k * (1.0f - 0.26f * jaw);
            float cx = 0.012f + 0.014f * jaw - 0.012f * back;
            rings.push_back({{cx, 1.700f + 0.122f * t, 0}, kX, kZ, rx, rz, BHead, BHead, 1.0f, t, 0.95f});
        }
        tube(rings, 20, RHead, BodyPart::Head, true, true);
        ellipsoid({0.108f, 1.684f, 0}, {0.020f, 0.027f, 0.015f}, BHead, RNose, BodyPart::Head, 5, 8);
        for (float s : {-1.0f, 1.0f}) {
            ellipsoid({0.002f, 1.694f, 0.089f * s}, {0.020f, 0.030f, 0.011f}, BHead, REar, BodyPart::Head, 5, 8);
            ellipsoid({0.0925f, 1.713f, 0.034f * s}, {0.011f, 0.0095f, 0.013f}, BHead, REye, BodyPart::Head, 4, 8);
            ellipsoid({0.0975f, 1.735f, 0.036f * s}, {0.008f, 0.0045f, 0.020f}, BHead, RBrow, BodyPart::Head, 4, 8);
        }
        ellipsoid({0.1015f, 1.645f, 0}, {0.006f, 0.0045f, 0.024f}, BHead, RLip, BodyPart::Head, 4, 8);
        // hair (or a hat) over the skull
        ellipsoid({0.002f, 1.712f, 0}, {0.108f, 0.129f, 0.096f}, BHead, RHair, BodyPart::Head, 12, 22, keepHair);
        // and below it, whatever the hairstyle adds
        switch (shape_.hair) {
            case 1:   // long: down the back to the shoulders
                ellipsoid({-0.050f, 1.590f, 0}, {0.062f, 0.150f, 0.098f}, BHead, RHairLong, BodyPart::Head, 8, 14);
                break;
            case 2:   // ponytail
                ellipsoid({-0.112f, 1.700f, 0}, {0.030f, 0.030f, 0.030f}, BHead, RHairLong, BodyPart::Head, 5, 8);
                ellipsoid({-0.128f, 1.610f, 0}, {0.028f, 0.085f, 0.026f}, BHead, RHairLong, BodyPart::Head, 6, 8);
                break;
            case 3:   // bun
                ellipsoid({-0.092f, 1.790f, 0}, {0.046f, 0.040f, 0.046f}, BHead, RHairLong, BodyPart::Head, 6, 10);
                break;
            default: break;
        }
    }
    // --- arms and hands ---------------------------------------------------------------
    for (int side = 0; side < 2; ++side) {
        int ua = side ? BUpperArmR : BUpperArmL, fa = side ? BForearmR : BForearmL, hd = side ? BHandR : BHandL;
        BodyPart armPart = side ? BodyPart::RightArm : BodyPart::LeftArm;
        BodyPart handPart = side ? BodyPart::RightHand : BodyPart::LeftHand;
        float sg = side ? 1.0f : -1.0f;
        Vector3 A = J(ua);
        struct AR { float d, rx, rz; int b0, b1; float w0; };
        const AR upper[] = {
            {-0.045f, 0.044f, 0.042f, ua, BChest, 0.55f},
            {-0.012f, 0.056f, 0.055f, ua, BChest, 0.8f},
            {0.030f, 0.058f, 0.057f, ua, ua, 1.0f},
            {0.100f, 0.054f, 0.052f, ua, ua, 1.0f},
            {0.180f, 0.050f, 0.048f, ua, ua, 1.0f},
            {0.250f, 0.046f, 0.045f, ua, fa, 0.85f},
            {0.290f, 0.045f, 0.044f, ua, fa, 0.5f},
        };
        const AR lower[] = {
            {0.290f, 0.045f, 0.044f, ua, fa, 0.5f},
            {0.330f, 0.047f, 0.046f, fa, ua, 0.85f},
            {0.400f, 0.046f, 0.043f, fa, fa, 1.0f},
            {0.470f, 0.040f, 0.036f, fa, fa, 1.0f},
            {0.530f, 0.034f, 0.030f, fa, hd, 0.85f},
            {0.555f, 0.032f, 0.029f, fa, hd, 0.6f},
        };
        std::vector<Ring> r1, r2;
        for (const auto& r : upper) r1.push_back({Vector3Add(A, {0, -r.d, 0.004f * sg}), kX, kZ, r.rx, r.rz, r.b0, r.b1, r.w0, r.d});
        for (const auto& r : lower) r2.push_back({Vector3Add(A, {0, -r.d, 0.004f * sg}), kX, kZ, r.rx, r.rz, r.b0, r.b1, r.w0, r.d});
        tube(r1, 12, RUpperArm, armPart, true, false);
        tube(r2, 12, RForearm, armPart, false, false);
        // Hand: broad from front to back, thin from side to side (palm faces the thigh)
        Vector3 W = J(hd);
        struct HR { float d, rx, rz; };
        const HR hr[] = {{0.0f, 0.030f, 0.024f}, {0.030f, 0.042f, 0.022f}, {0.065f, 0.046f, 0.020f}, {0.100f, 0.045f, 0.018f},
                         {0.130f, 0.040f, 0.015f}, {0.155f, 0.031f, 0.012f}, {0.172f, 0.018f, 0.008f}};
        std::vector<Ring> h;
        for (const auto& r : hr)
            h.push_back({Vector3Add(W, {0.004f, -r.d, 0}), kX, kZ, r.rx, r.rz, r.d < 0.01f ? hd : hd, r.d < 0.01f ? fa : hd,
                         r.d < 0.01f ? 0.7f : 1.0f, r.d, 0.85f});
        tube(h, 10, RHand, handPart, false, true);
        // Thumb along the front edge of the hand
        Vector3 t0 = Vector3Add(W, {0.034f, -0.032f, -0.004f * sg}), t1 = Vector3Add(W, {0.056f, -0.086f, -0.010f * sg});
        Vector3 dir = Vector3Normalize(Vector3Subtract(t1, t0)), u, v;
        ringFrame(dir, u, v);
        std::vector<Ring> th;
        for (int i = 0; i < 4; ++i) {
            float f = i / 3.0f;
            th.push_back({Vector3Lerp(t0, t1, f), u, v, 0.0135f - 0.004f * f, 0.012f - 0.004f * f, hd, hd, 1.0f, 0.2f});
        }
        tube(th, 8, RHand, handPart, false, true);
    }
    // --- legs and shoes ----------------------------------------------------------------
    for (int side = 0; side < 2; ++side) {
        int th = side ? BThighR : BThighL, sh = side ? BShinR : BShinL, ft = side ? BFootR : BFootL;
        BodyPart legPart = side ? BodyPart::RightLeg : BodyPart::LeftLeg;
        BodyPart footPart = side ? BodyPart::RightFoot : BodyPart::LeftFoot;
        float sg = side ? 1.0f : -1.0f;
        Vector3 Hp = J(th);
        struct LR { float d, r, cx; int b0, b1; float w0; };
        const LR lr[] = {
            {-0.075f, 0.084f, 0.000f, th, BPelvis, 0.55f},
            {-0.020f, 0.092f, 0.002f, th, BPelvis, 0.85f},
            {0.060f, 0.091f, 0.004f, th, th, 1.0f},
            {0.160f, 0.085f, 0.004f, th, th, 1.0f},
            {0.260f, 0.077f, 0.003f, th, th, 1.0f},
            {0.360f, 0.066f, 0.002f, th, th, 1.0f},
            {0.420f, 0.060f, 0.004f, th, sh, 0.75f},
            {0.450f, 0.058f, 0.006f, th, sh, 0.5f},
            {0.490f, 0.058f, 0.000f, sh, th, 0.8f},
            {0.560f, 0.061f, -0.010f, sh, sh, 1.0f},
            {0.640f, 0.057f, -0.008f, sh, sh, 1.0f},
            {0.740f, 0.047f, -0.002f, sh, sh, 1.0f},
            {0.820f, 0.043f, 0.000f, sh, sh, 1.0f},
            {0.865f, 0.046f, 0.002f, sh, ft, 0.8f},
        };
        std::vector<Ring> rings;
        for (const auto& r : lr)
            rings.push_back({Vector3Add(Hp, {r.cx, -r.d, 0.006f * sg}), kX, kZ, r.r, r.r * 0.96f, r.b0, r.b1, r.w0, r.d});
        tube(rings, 14, RLeg, legPart, true, false);
        // Shoe: lofted from heel to toe, flat sole
        Vector3 F = J(ft);
        struct SR { float x, bottom, top, hz; };
        const SR sr[] = {{-0.078f, -0.080f, 0.020f, 0.030f}, {-0.064f, -0.080f, 0.046f, 0.040f}, {-0.025f, -0.080f, 0.052f, 0.045f},
                         {0.025f, -0.080f, 0.036f, 0.048f},  {0.080f, -0.080f, 0.014f, 0.049f},  {0.130f, -0.080f, 0.000f, 0.046f},
                         {0.168f, -0.080f, -0.016f, 0.039f}, {0.192f, -0.080f, -0.032f, 0.027f}};
        std::vector<Ring> shoe;
        for (const auto& r : sr)
            shoe.push_back({Vector3Add(F, {r.x, (r.bottom + r.top) * 0.5f, 0.004f * sg}), kY, kZ, (r.top - r.bottom) * 0.5f, r.hz, ft, ft,
                            1.0f, r.x, 0.55f});
        size_t first = rest_.size();
        tube(shoe, 14, RShoe, footPart, true, true);
        for (size_t i = first; i < rest_.size(); ++i)
            if (rest_[i].pos.y < F.y - 0.067f) rest_[i].region = RSole;
    }
}

void CharacterModel::computeNormals() {
    for (auto& v : rest_) v.nrm = {0, 0, 0};
    for (size_t i = 0; i + 2 < idx_.size(); i += 3) {
        Vtx &a = rest_[idx_[i]], &b = rest_[idx_[i + 1]], &c = rest_[idx_[i + 2]];
        Vector3 n = Vector3CrossProduct(Vector3Subtract(b.pos, a.pos), Vector3Subtract(c.pos, a.pos));
        a.nrm = Vector3Add(a.nrm, n);
        b.nrm = Vector3Add(b.nrm, n);
        c.nrm = Vector3Add(c.nrm, n);
    }
    for (auto& v : rest_) v.nrm = Vector3Length(v.nrm) > 1e-9f ? Vector3Normalize(v.nrm) : kY;
}

bool CharacterModel::init(const BodyShape& shape) {
    shape_ = shape;
    build();
    computeNormals();
    int n = (int)rest_.size();
    mesh_ = Mesh{};
    mesh_.vertexCount = n;
    mesh_.triangleCount = (int)idx_.size() / 3;
    mesh_.vertices = (float*)MemAlloc((unsigned)(n * 3 * sizeof(float)));
    mesh_.normals = (float*)MemAlloc((unsigned)(n * 3 * sizeof(float)));
    mesh_.colors = (unsigned char*)MemAlloc((unsigned)(n * 4));
    mesh_.texcoords = (float*)MemAlloc((unsigned)(n * 2 * sizeof(float)));   // material 0: plain
    mesh_.indices = (unsigned short*)MemAlloc((unsigned)(idx_.size() * sizeof(unsigned short)));
    std::memcpy(mesh_.indices, idx_.data(), idx_.size() * sizeof(unsigned short));
    for (int i = 0; i < n; ++i) {
        std::memcpy(mesh_.vertices + i * 3, &rest_[i].pos, sizeof(Vector3));
        std::memcpy(mesh_.normals + i * 3, &rest_[i].nrm, sizeof(Vector3));
        mesh_.colors[i * 4 + 0] = mesh_.colors[i * 4 + 1] = mesh_.colors[i * 4 + 2] = 200;
        mesh_.colors[i * 4 + 3] = 255;
    }
    UploadMesh(&mesh_, true);
    ready_ = mesh_.vaoId > 0 || mesh_.vboId != nullptr;
    return ready_;
}

void CharacterModel::shutdown() {
    if (ready_) UnloadMesh(mesh_);
    mesh_ = Mesh{};
    ready_ = false;
}

void CharacterModel::skin(const CharacterRig& rig, const CharacterLook& look) {
    if (!ready_) return;
    Matrix S[BoneCount];
    for (int b = 0; b < BoneCount; ++b) S[b] = MatrixMultiply(invBind_[b], rig.bone(b));
    auto rot = [](const Matrix& m, Vector3 n) {
        return Vector3{m.m0 * n.x + m.m4 * n.y + m.m8 * n.z, m.m1 * n.x + m.m5 * n.y + m.m9 * n.z, m.m2 * n.x + m.m6 * n.y + m.m10 * n.z};
    };
    bool jacket = look.jacket.a > 0;
    Color topCol = jacket ? look.jacket : look.top;
    Color belt{56, 42, 32, 255}, sole{42, 38, 36, 255};
    for (int i = 0; i < (int)rest_.size(); ++i) {
        const Vtx& v = rest_[i];
        const Matrix& A = S[v.b0];
        Vector3 p = Vector3Transform(v.pos, A), n = rot(A, v.nrm);
        if (v.w0 < 0.999f) {
            const Matrix& B = S[v.b1];
            p = Vector3Lerp(Vector3Transform(v.pos, B), p, v.w0);
            n = Vector3Lerp(rot(B, v.nrm), n, v.w0);
        }
        n = Vector3Normalize(n);
        std::memcpy(mesh_.vertices + i * 3, &p, sizeof(Vector3));
        std::memcpy(mesh_.normals + i * 3, &n, sizeof(Vector3));

        Color c = look.skin;
        bool cloth = false;
        switch (v.region) {
            case RNose: c = mixColor(look.skin, Color{200, 120, 110, 255}, 0.15f); break;
            case REar: c = shade(look.skin, 0.95f); break;
            case REye: c = Color{34, 30, 30, 255}; break;
            case RBrow: c = shade(look.hair, 0.8f); break;
            case RLip: c = mixColor(look.skin, Color{150, 70, 66, 255}, 0.55f); break;
            case RHair:
                c = look.hat.a > 0 ? look.hat : shape_.hair == 4 ? mixColor(look.skin, look.hair, 0.3f) : look.hair;
                cloth = look.hat.a > 0;
                break;
            case RHairLong:
                // a bun under a hat looks like the hat's bobble
                c = shape_.hair == 3 && look.hat.a > 0 ? look.hat : shade(look.hair, 0.94f);
                break;
            case RTorso: {
                float y = v.pos.y;
                cloth = true;
                if (jacket && y > 0.875f) c = look.jacket;
                else if (y > 1.005f) c = look.top;
                else if (y > 0.972f) c = belt;
                else c = look.pants;
                // the zip of an open jacket, a shadow line down the front
                if (jacket && y > 0.9f && y < 1.47f && v.pos.x > 0.07f && std::fabs(v.pos.z) < 0.012f) c = shade(c, 0.62f);
                break;
            }
            case RUpperArm:
                cloth = !(look.shortSleeves && !jacket && v.t > 0.135f);
                c = cloth ? topCol : look.skin;
                break;
            case RForearm:
                cloth = !(look.shortSleeves && !jacket);
                c = cloth ? topCol : look.skin;
                if (cloth && v.t > 0.53f) c = shade(c, 0.86f);   // cuff
                break;
            case RHand: if (look.gloves.a > 0) { c = look.gloves; cloth = true; } break;
            case RLeg: c = look.pants; cloth = true; break;
            case RShoe: c = look.barefoot ? look.skin : look.shoes; break;
            case RSole: c = look.barefoot ? shade(look.skin, 0.85f) : sole; break;
            default: break;
        }
        float blood = look.blood[v.part];
        if (blood > 0.01f && v.region != REye) c = mixColor(c, Color{112, 18, 16, 255}, std::min(0.75f, blood * (cloth ? 0.7f : 0.5f)));
        mesh_.colors[i * 4 + 0] = c.r;
        mesh_.colors[i * 4 + 1] = c.g;
        mesh_.colors[i * 4 + 2] = c.b;
        mesh_.colors[i * 4 + 3] = 255;
    }
    int n = (int)rest_.size();
    UpdateMeshBuffer(mesh_, 0, mesh_.vertices, n * 3 * (int)sizeof(float), 0);
    UpdateMeshBuffer(mesh_, 2, mesh_.normals, n * 3 * (int)sizeof(float), 0);
    UpdateMeshBuffer(mesh_, 3, mesh_.colors, n * 4, 0);
}

} // namespace alone
