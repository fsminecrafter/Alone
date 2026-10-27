#include "client/CharacterRig.h"
#include <algorithm>
#include <cmath>

namespace alone {

namespace {

Pose lerpPose(const Pose& a, const Pose& b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    Pose p;
    for (int i = 0; i < BoneCount; ++i) p.rot[i] = Vector3Lerp(a.rot[i], b.rot[i], t);
    p.rootOffset = Vector3Lerp(a.rootOffset, b.rootOffset, t);
    p.rootPitch = a.rootPitch + (b.rootPitch - a.rootPitch) * t;
    p.rootRoll = a.rootRoll + (b.rootRoll - a.rootRoll) * t;
    return p;
}

void lerpBones(Pose& p, const Pose& b, float t, std::initializer_list<int> bones) {
    for (int i : bones) p.rot[i] = Vector3Lerp(p.rot[i], b.rot[i], t);
}

float approach(float v, float target, float rate, float dt) {
    float k = 1.0f - std::exp(-rate * dt);
    return v + (target - v) * k;
}

} // namespace

CharacterRig::CharacterRig() {
    auto def = [&](int b, int parent, Vector3 offset, Vector3 dir, float len) {
        parent_[b] = parent;
        offset_[b] = offset;
        dir_[b] = dir;
        length_[b] = len;
    };
    const Vector3 up{0, 1, 0}, down{0, -1, 0}, fwd{1, 0, 0};
    def(BPelvis, -1, {0, 0.95f, 0}, up, 0.1f);
    def(BSpine, BPelvis, {0, 0.1f, 0}, up, 0.22f);
    def(BChest, BSpine, {0, 0.22f, 0}, up, 0.24f);
    def(BNeck, BChest, {0, 0.24f, 0}, up, 0.08f);
    def(BHead, BNeck, {0, 0.08f, 0}, up, 0.22f);
    def(BShoulderL, BChest, {0, 0.2f, -0.15f}, down, 0.0f);
    def(BUpperArmL, BShoulderL, {0, 0, -0.04f}, down, 0.29f);
    def(BForearmL, BUpperArmL, {0, -0.29f, 0}, down, 0.26f);
    def(BHandL, BForearmL, {0, -0.26f, 0}, down, 0.09f);
    def(BShoulderR, BChest, {0, 0.2f, 0.15f}, down, 0.0f);
    def(BUpperArmR, BShoulderR, {0, 0, 0.04f}, down, 0.29f);
    def(BForearmR, BUpperArmR, {0, -0.29f, 0}, down, 0.26f);
    def(BHandR, BForearmR, {0, -0.26f, 0}, down, 0.09f);
    def(BThighL, BPelvis, {0, 0, -0.095f}, down, 0.44f);
    def(BShinL, BThighL, {0, -0.44f, 0}, down, 0.43f);
    def(BFootL, BShinL, {0, -0.43f, 0}, fwd, 0.19f);
    def(BThighR, BPelvis, {0, 0, 0.095f}, down, 0.44f);
    def(BShinR, BThighR, {0, -0.44f, 0}, down, 0.43f);
    def(BFootR, BShinR, {0, -0.43f, 0}, fwd, 0.19f);
    for (auto& m : world_) m = MatrixIdentity();
    solve();
}

Vector3 CharacterRig::tip(int b) const {
    return Vector3Transform(Vector3Scale(dir_[b], length_[b]), world_[b]);
}

void CharacterRig::place(Vector3 pos, float facing) {
    rootM_ = MatrixMultiply(MatrixRotateY(-facing), MatrixTranslate(pos.x, pos.y, pos.z));
    solve();
}

Pose CharacterRig::computeTarget(const AnimInput& in) const {
    const float t = in.time;
    // --- idle: breathing, a little weight shift, looking around ---------------
    Pose idle;
    float breath = std::sin(t * 1.7f);
    idle.rot[BChest] = {0, 0, 0.025f * breath};
    idle.rot[BSpine] = {0.02f * std::sin(t * 0.4f), 0, -0.02f};
    idle.rot[BHead] = {0, 0.3f * std::sin(t * 0.31f) * std::sin(t * 0.13f), 0.05f};
    idle.rot[BUpperArmL] = {0.09f, 0, 0.03f * breath};
    idle.rot[BUpperArmR] = {-0.09f, 0, 0.03f * breath};
    idle.rot[BForearmL] = {0, 0, 0.12f};
    idle.rot[BForearmR] = {0, 0, 0.12f};
    idle.rot[BThighL] = {0.03f, 0, 0};
    idle.rot[BThighR] = {-0.03f, 0, 0};
    idle.rootOffset = {0, -0.005f * breath, 0.012f * std::sin(t * 0.4f)};

    // --- walk / run: the step phase follows distance so feet don't slide -----
    auto gait = [&](float stride, float thighAmp, float kneeAmp, float armAmp, float elbow, float lean, float bob) {
        Pose p = idle;
        float ph = in.distance / stride * 2.0f * PI;
        float sL = std::sin(ph), sR = std::sin(ph + PI);
        float cL = std::cos(ph), cR = std::cos(ph + PI);
        p.rot[BThighL] = {0.02f, 0, thighAmp * sL + lean * 0.3f};
        p.rot[BThighR] = {-0.02f, 0, thighAmp * sR + lean * 0.3f};
        p.rot[BShinL] = {0, 0, -(0.08f + kneeAmp * std::max(0.0f, cL))};
        p.rot[BShinR] = {0, 0, -(0.08f + kneeAmp * std::max(0.0f, cR))};
        p.rot[BFootL] = {0, 0, 0.25f * sL - 0.1f};
        p.rot[BFootR] = {0, 0, 0.25f * sR - 0.1f};
        p.rot[BUpperArmL] = {0.1f, 0, armAmp * sR};
        p.rot[BUpperArmR] = {-0.1f, 0, armAmp * sL};
        p.rot[BForearmL] = {0, 0, elbow + 0.2f * std::max(0.0f, sR)};
        p.rot[BForearmR] = {0, 0, elbow + 0.2f * std::max(0.0f, sL)};
        p.rot[BPelvis] = {0.04f * sL, 0.12f * sL, 0};
        p.rot[BSpine] = {0, -0.06f * sL, -lean};
        p.rot[BChest] = {0, -0.12f * sL, -lean * 0.3f};
        p.rot[BHead] = {0, 0.1f * sL, lean * 0.8f};
        p.rootOffset = {0, -bob + bob * std::fabs(std::cos(ph)) * 1.6f, 0};
        return p;
    };
    Pose walk = gait(1.35f, 0.42f, 0.7f, 0.32f, 0.25f, 0.05f, 0.025f);
    Pose run = gait(2.5f, 0.75f, 1.35f, 0.75f, 1.35f, 0.28f, 0.06f);

    // --- sneaking: crouched, short careful steps ----------------------------
    Pose sneak;
    {
        float ph = in.distance / 0.8f * 2.0f * PI;
        float m = std::min(1.0f, in.speed);
        sneak.rootOffset = {-0.05f, -0.26f, 0};
        sneak.rot[BThighL] = {0.05f, 0, 0.85f + 0.3f * m * std::sin(ph)};
        sneak.rot[BThighR] = {-0.05f, 0, 0.85f + 0.3f * m * std::sin(ph + PI)};
        sneak.rot[BShinL] = {0, 0, -1.35f - 0.25f * m * std::max(0.0f, std::cos(ph))};
        sneak.rot[BShinR] = {0, 0, -1.35f - 0.25f * m * std::max(0.0f, std::cos(ph + PI))};
        sneak.rot[BFootL] = {0, 0, 0.45f};
        sneak.rot[BFootR] = {0, 0, 0.45f};
        sneak.rot[BSpine] = {0, 0, -0.45f};
        sneak.rot[BChest] = {0, 0, -0.15f};
        sneak.rot[BHead] = {0, 0.2f * std::sin(t * 0.7f), 0.5f};
        sneak.rot[BUpperArmL] = {0.25f, 0, 0.55f};
        sneak.rot[BUpperArmR] = {-0.25f, 0, 0.55f};
        sneak.rot[BForearmL] = {0, 0, 0.9f};
        sneak.rot[BForearmR] = {0, 0, 0.9f};
    }

    // --- working (searching, butchering): crouched, hands busy --------------
    Pose work = idle;
    {
        float h = std::sin(t * 6.0f);
        work.rootOffset = {-0.04f, -0.2f, 0};
        work.rot[BThighL] = {0.1f, 0, 0.75f};
        work.rot[BThighR] = {-0.1f, 0, 0.55f};
        work.rot[BShinL] = {0, 0, -1.05f};
        work.rot[BShinR] = {0, 0, -0.85f};
        work.rot[BFootL] = {0, 0, 0.3f};
        work.rot[BFootR] = {0, 0, 0.3f};
        work.rot[BSpine] = {0, 0, -0.55f};
        work.rot[BChest] = {0, 0.1f * std::sin(t * 1.3f), -0.2f};
        work.rot[BHead] = {0, 0, -0.1f};
        work.rot[BUpperArmL] = {-0.15f, 0, 0.9f + 0.15f * h};
        work.rot[BUpperArmR] = {0.15f, 0, 0.9f - 0.15f * h};
        work.rot[BForearmL] = {0, 0, 0.6f};
        work.rot[BForearmR] = {0, 0, 0.6f};
    }

    // --- arm actions -------------------------------------------------------
    Pose aim = idle;
    aim.rot[BUpperArmL] = {-0.45f, 0, 1.45f};
    aim.rot[BForearmL] = {0, 0, 0.15f};
    aim.rot[BUpperArmR] = {0.1f, 0, 1.05f};
    aim.rot[BForearmR] = {0, 0, 1.0f};
    aim.rot[BChest] = {0, -0.25f, 0};
    aim.rot[BHead] = {0, 0.2f, 0.05f};

    Pose eat = idle;
    eat.rot[BUpperArmR] = {0.35f, 0, 0.45f + 0.12f * std::sin(t * 3.0f)};
    eat.rot[BForearmR] = {0, 0, 2.2f};
    eat.rot[BUpperArmL] = {-0.2f, 0, 0.35f};
    eat.rot[BForearmL] = {0, 0, 1.4f};
    eat.rot[BHead] = {0, 0, -0.12f + 0.05f * std::sin(t * 6.0f)};

    // --- lying down / dead ---------------------------------------------------
    Pose lie;
    lie.rootPitch = PI * 0.5f;
    lie.rootOffset = {-0.05f, -0.82f, 0};
    lie.rot[BHead] = {0.4f, 0, 0.2f};
    lie.rot[BUpperArmL] = {in.dead ? 0.9f : 0.15f, 0, 0};
    lie.rot[BUpperArmR] = {in.dead ? -0.5f : -0.15f, 0, in.dead ? 0.6f : 0};
    lie.rot[BForearmL] = {0, 0, in.dead ? 0.3f : 0.4f};
    lie.rot[BForearmR] = {0, 0, 0.4f};
    lie.rot[BThighL] = {in.dead ? 0.25f : 0.05f, 0, 0.25f};
    lie.rot[BShinL] = {0, 0, -0.5f};
    lie.rot[BThighR] = {-0.05f, 0, 0.05f};
    lie.rot[BShinR] = {0, 0, -0.1f};
    lie.rot[BFootL] = {0, 0, -0.5f};
    lie.rot[BFootR] = {0, 0, -0.5f};

    // --- blend ---------------------------------------------------------------
    Pose p = lerpPose(idle, walk, wMove_ * (1.0f - wRun_));
    p = lerpPose(p, run, wMove_ * wRun_);
    p = lerpPose(p, sneak, wSneak_);
    p = lerpPose(p, work, wWork_);
    lerpBones(p, aim, wAim_, {BUpperArmL, BForearmL, BUpperArmR, BForearmR, BChest, BHead});
    lerpBones(p, eat, wEat_, {BUpperArmL, BForearmL, BUpperArmR, BForearmR, BHead});

    // Melee swing: wind up over the shoulder, then strike down and across
    if (in.swing > 0.01f) {
        float k = 1.0f - in.swing;            // 0 .. 1 through the attack
        float raise, elbow, twist;
        if (k < 0.35f) {
            float u = k / 0.35f;
            raise = 2.7f * u; elbow = 1.3f * u; twist = -0.45f * u;
        } else if (k < 0.6f) {
            float u = (k - 0.35f) / 0.25f;
            raise = 2.7f - 2.2f * u; elbow = 1.3f - 1.1f * u; twist = -0.45f + 0.8f * u;
        } else {
            float u = (k - 0.6f) / 0.4f;
            raise = 0.5f * (1.0f - u); elbow = 0.2f + 0.1f * u; twist = 0.35f * (1.0f - u);
        }
        p.rot[BUpperArmR] = {-0.25f, 0, raise};
        p.rot[BForearmR] = {0, 0, elbow};
        p.rot[BChest].y += twist;
        p.rot[BSpine].z -= 0.15f * std::sin(k * PI);
    }
    p = lerpPose(p, lie, wLie_);
    return p;
}

void CharacterRig::update(const AnimInput& in, float dt) {
    wMove_ = approach(wMove_, std::clamp(in.speed / 1.2f, 0.0f, 1.0f), 10.0f, dt);
    wRun_ = approach(wRun_, in.speed > 3.4f ? 1.0f : 0.0f, 6.0f, dt);
    wSneak_ = approach(wSneak_, in.sneaking ? 1.0f : 0.0f, 6.0f, dt);
    wAim_ = approach(wAim_, in.aiming ? 1.0f : 0.0f, 10.0f, dt);
    wWork_ = approach(wWork_, in.working ? 1.0f : 0.0f, 5.0f, dt);
    wEat_ = approach(wEat_, in.eating ? 1.0f : 0.0f, 6.0f, dt);
    wLie_ = approach(wLie_, (in.asleep || in.dead) ? 1.0f : 0.0f, in.dead ? 3.0f : 2.5f, dt);
    pose_ = computeTarget(in);
    solve();
}

void CharacterRig::solve() {
    for (int b = 0; b < BoneCount; ++b) {
        Vector3 off = offset_[b];
        Matrix local = MatrixRotateXYZ(pose_.rot[b]);
        if (b == BPelvis) {
            off = Vector3Add(off, pose_.rootOffset);
            // Lying down pivots the whole body
            local = MatrixMultiply(local, MatrixRotateZ(pose_.rootPitch));
            local = MatrixMultiply(local, MatrixRotateX(pose_.rootRoll));
        }
        local = MatrixMultiply(local, MatrixTranslate(off.x, off.y, off.z));
        const Matrix& parent = parent_[b] < 0 ? rootM_ : world_[parent_[b]];
        world_[b] = MatrixMultiply(local, parent);
    }
}

} // namespace alone
