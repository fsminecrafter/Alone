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

float lerp(float a, float b, float t) { return a + (b - a) * t; }

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
    // A hurt leg makes a limp: you hurry off it (the step phase is warped),
    // keep it stiff, take a shorter step with it and dip as it takes your weight.
    auto gait = [&](float stride, float thighAmp, float kneeAmp, float armAmp, float elbow, float lean, float bob) {
        Pose p = idle;
        float ph = in.distance / stride * 2.0f * PI;
        float warp = 0.55f * (wLimpL_ - wLimpR_);
        float phw = ph + warp * std::sin(ph);
        float sL = std::sin(phw), sR = std::sin(phw + PI);
        float cL = std::cos(phw), cR = std::cos(phw + PI);
        float ampL = thighAmp * (1.0f - 0.35f * wLimpL_), ampR = thighAmp * (1.0f - 0.35f * wLimpR_);
        float kneeL = kneeAmp * (1.0f - 0.6f * wLimpL_), kneeR = kneeAmp * (1.0f - 0.6f * wLimpR_);
        // legs reach further forward than back
        auto swingOf = [&](float sn, float amp) { return sn > 0 ? amp * sn : amp * 0.55f * sn; };
        p.rot[BThighL] = {0.02f, 0, swingOf(sL, ampL) + lean * 0.3f};
        p.rot[BThighR] = {-0.02f, 0, swingOf(sR, ampR) + lean * 0.3f};
        p.rot[BShinL] = {0, 0, -(0.08f + kneeL * std::max(0.0f, cL))};
        p.rot[BShinR] = {0, 0, -(0.08f + kneeR * std::max(0.0f, cR))};
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
        // the bad leg taking the weight (its thigh moving back)
        float loadL = wLimpL_ * std::max(0.0f, -cL), loadR = wLimpR_ * std::max(0.0f, -cR);
        p.rootOffset.y -= 0.04f * (loadL + loadR);
        p.rootRoll += 0.12f * (loadR - loadL);
        p.rot[BHead].x -= 0.08f * (loadR - loadL);
        return p;
    };
    Pose walk = gait(1.35f, 0.42f, 0.7f, 0.32f, 0.25f, 0.05f, 0.025f);
    Pose run = gait(2.3f, 0.68f, 1.45f, 0.7f, 1.35f, 0.24f, 0.05f);

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
    aim.rot[BUpperArmR] = {0.3f, 0, 0.75f};    // right hand up at the cheek
    aim.rot[BForearmR] = {0, 0, 1.6f};
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
    float still = (1.0f - wMove_) * (1.0f - wWork_) * (1.0f - wSneak_);

    // --- how you feel shows ------------------------------------------------------
    // Bent over with pain or weakness
    if (wHunch_ > 0.01f) {
        float h = wHunch_;
        p.rot[BSpine].z -= 0.34f * h;
        p.rot[BChest].z -= 0.16f * h;
        p.rot[BNeck].z += 0.08f * h;
        p.rot[BHead].z += 0.18f * h;
        p.rot[BThighL].z += 0.1f * h; p.rot[BThighR].z += 0.1f * h;
        p.rot[BShinL].z -= 0.2f * h; p.rot[BShinR].z -= 0.2f * h;
        p.rootOffset.y -= 0.025f * h;
    }
    // Out of breath: hands on the knees
    if (wWinded_ * still > 0.01f) {
        Pose w = p;
        w.rot[BSpine].z = -0.62f; w.rot[BChest].z = -0.22f; w.rot[BHead].z = 0.5f; w.rot[BNeck].z = 0.15f;
        w.rot[BThighL] = {0.06f, 0, 0.42f}; w.rot[BThighR] = {-0.06f, 0, 0.42f};
        w.rot[BShinL] = {0, 0, -0.5f}; w.rot[BShinR] = {0, 0, -0.5f};
        w.rot[BFootL] = {0, 0, 0.08f}; w.rot[BFootR] = {0, 0, 0.08f};
        w.rot[BUpperArmL] = {-0.05f, 0, 0.62f}; w.rot[BUpperArmR] = {0.05f, 0, 0.62f};
        w.rot[BForearmL] = {0, 0, 0.12f}; w.rot[BForearmR] = {0, 0, 0.12f};
        float pant = std::sin(t * 9.0f);
        w.rot[BChest].z += 0.04f * pant;
        w.rootOffset = {-0.04f, -0.075f, 0};
        p = lerpPose(p, w, wWinded_ * still);
    }
    // A hand pressed to the stomach
    if (wNausea_ * still > 0.01f) {
        Pose n = p;
        n.rot[BUpperArmL] = {-0.12f, -0.95f, 0.32f};   // turned in so the hand reaches the belly
        n.rot[BForearmL] = {0, 0, 1.6f};
        n.rot[BHandL] = {0, 0, 0.3f};
        n.rot[BSpine].z -= 0.1f;
        n.rot[BHead].z -= 0.06f;
        lerpBones(p, n, wNausea_ * still, {BUpperArmL, BForearmL, BHandL, BSpine, BHead});
    }
    // Cold: arms wrapped around yourself, shivering
    if (wShiver_ > 0.01f) {
        float hug = wShiver_ * still;
        if (hug > 0.01f) {
            Pose c = p;
            c.rot[BUpperArmL] = {-0.15f, -1.05f, 0.55f};  c.rot[BForearmL] = {0, 0, 1.9f};
            c.rot[BUpperArmR] = {0.15f, 1.05f, 0.62f};    c.rot[BForearmR] = {0, 0, 1.85f};
            c.rot[BChest].z -= 0.08f;
            c.rot[BHead].z += 0.06f;
            lerpBones(p, c, hug, {BUpperArmL, BForearmL, BUpperArmR, BForearmR, BChest, BHead});
        }
        float tremble = 0.025f * wShiver_;
        p.rot[BChest].x += tremble * std::sin(t * 131.0f);
        p.rot[BHead].x += tremble * 0.8f * std::sin(t * 117.0f + 1.3f);
        p.rot[BUpperArmL].z += tremble * std::sin(t * 109.0f + 2.1f);
        p.rot[BUpperArmR].z += tremble * std::sin(t * 123.0f + 0.4f);
        p.rootOffset.y += 0.004f * wShiver_ * std::sin(t * 97.0f);
    }
    // A hurt arm is held close and guarded, and barely swings
    for (int side = 0; side < 2; ++side) {
        float w = side ? wArmR_ : wArmL_;
        if (side == 1) w *= 1.0f - wHold_ * 0.7f;   // still holds what it carries
        if (w < 0.01f) continue;
        int ua = side ? BUpperArmR : BUpperArmL, fa = side ? BForearmR : BForearmL, hd = side ? BHandR : BHandL;
        float sg = side ? 1.0f : -1.0f;   // roll towards the body
        Pose g = p;
        g.rot[ua] = {0.15f * sg, 0.7f * sg, 0.3f};
        g.rot[fa] = {0, 0, 1.5f};
        g.rot[hd] = {0, 0, 0.15f};
        lerpBones(p, g, w * (1.0f - wWork_ * 0.6f), {ua, fa, hd});
    }

    // --- what's in the hand ------------------------------------------------------
    // The grip: a weapon sticks out of the fist along the hand's forward axis,
    // so bending the wrist points it. At rest it points forward and down.
    if (wHold_ > 0.01f && in.weapon != 0) {
        Pose hold = p;
        if (in.weapon == 3) {   // rifle at the low ready, the left hand on the fore-end
            hold.rot[BUpperArmR] = {-0.1f, 0, 0.22f};
            hold.rot[BForearmR] = {0, 0, 0.75f};
            hold.rot[BHandR] = {0, 0, -0.55f};
            hold.rot[BUpperArmL] = {-0.42f, 0, 0.6f};
            hold.rot[BForearmL] = {0, 0, 1.05f};
        } else {
            hold.rot[BHandR] = {0, 0, in.weapon == 2 ? -0.35f : -0.75f};
            hold.rot[BForearmR].z += 0.12f;
        }
        lerpBones(p, hold, wHold_ * (1.0f - wAim_) * (1.0f - wWork_ * 0.8f),
                  {BUpperArmR, BForearmR, BHandR, BUpperArmL, BForearmL});
    }
    lerpBones(p, aim, wAim_, {BUpperArmL, BForearmL, BUpperArmR, BForearmR, BChest, BHead});
    lerpBones(p, eat, wEat_, {BUpperArmL, BForearmL, BUpperArmR, BForearmR, BHead});

    // --- attacks ------------------------------------------------------------------
    if (in.swing > 0.01f && in.weapon != 3) {
        float k = 1.0f - in.swing;            // 0 .. 1 through the attack
        float raise, elbow, wrist, twist, bend;
        float rest0 = p.rot[BUpperArmR].z, elbow0 = p.rot[BForearmR].z, wrist0 = p.rot[BHandR].z;
        if (in.weapon == 1) {
            // Chop: wind up over the shoulder (the head of the axe behind you,
            // its edge facing up), then bring it down so the edge leads.
            if (k < 0.35f) {
                float u = k / 0.35f;
                raise = lerp(rest0, 2.7f, u); elbow = lerp(elbow0, 1.3f, u); wrist = lerp(wrist0, 0.05f, u);
                twist = -0.45f * u; bend = 0.08f * u;
            } else if (k < 0.6f) {
                float u = (k - 0.35f) / 0.25f;
                raise = lerp(2.7f, 0.45f, u); elbow = lerp(1.3f, 0.2f, u); wrist = lerp(0.05f, -0.95f, u);
                twist = -0.45f + 0.8f * u; bend = lerp(0.08f, -0.3f, u);
            } else {
                float u = (k - 0.6f) / 0.4f;
                raise = lerp(0.45f, rest0, u); elbow = lerp(0.2f, elbow0, u); wrist = lerp(-0.95f, wrist0, u);
                twist = 0.35f * (1.0f - u); bend = -0.3f * (1.0f - u);
            }
        } else {
            // Stab or punch: draw back, then thrust straight out
            if (k < 0.3f) {
                float u = k / 0.3f;
                raise = lerp(rest0, 0.55f, u); elbow = lerp(elbow0, 1.75f, u); wrist = lerp(wrist0, -0.2f, u);
                twist = -0.35f * u; bend = 0;
            } else if (k < 0.55f) {
                float u = (k - 0.3f) / 0.25f;
                raise = lerp(0.55f, 1.45f, u); elbow = lerp(1.75f, 0.08f, u); wrist = lerp(-0.2f, -1.4f, u);
                twist = -0.35f + 0.75f * u; bend = -0.12f * u;
            } else {
                float u = (k - 0.55f) / 0.45f;
                raise = lerp(1.45f, rest0, u); elbow = lerp(0.08f, elbow0, u); wrist = lerp(-1.4f, wrist0, u);
                twist = 0.4f * (1.0f - u); bend = -0.12f * (1.0f - u);
            }
        }
        p.rot[BUpperArmR] = {-0.15f, 0, raise};
        p.rot[BForearmR] = {0, 0, elbow};
        p.rot[BHandR] = {0, 0, wrist};
        p.rot[BChest].y += twist;
        p.rot[BSpine].z += bend;
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
    wHold_ = approach(wHold_, in.weapon != 0 ? 1.0f : 0.0f, 6.0f, dt);
    // the body's state changes slowly
    wLimpL_ = approach(wLimpL_, std::clamp(in.limpL, 0.0f, 1.0f), 2.0f, dt);
    wLimpR_ = approach(wLimpR_, std::clamp(in.limpR, 0.0f, 1.0f), 2.0f, dt);
    wArmL_ = approach(wArmL_, std::clamp(in.armHurtL, 0.0f, 1.0f), 2.0f, dt);
    wArmR_ = approach(wArmR_, std::clamp(in.armHurtR, 0.0f, 1.0f), 2.0f, dt);
    wHunch_ = approach(wHunch_, std::clamp(in.hunch, 0.0f, 1.0f), 1.5f, dt);
    wShiver_ = approach(wShiver_, std::clamp(in.shiver, 0.0f, 1.0f), 1.5f, dt);
    wNausea_ = approach(wNausea_, std::clamp(in.nausea, 0.0f, 1.0f), 1.5f, dt);
    wWinded_ = approach(wWinded_, std::clamp(in.winded, 0.0f, 1.0f), 3.0f, dt);
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
