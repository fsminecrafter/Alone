#pragma once
#include "raylib.h"
#include "raymath.h"
#include <array>

namespace alone {

// A small human skeleton. Bones form a hierarchy; each frame a pose (local
// joint rotations) is computed procedurally from what the character is doing,
// blended between activities, and forward kinematics gives world transforms
// that the renderer hangs body parts on.
enum Bone {
    BPelvis, BSpine, BChest, BNeck, BHead,
    BShoulderL, BUpperArmL, BForearmL, BHandL,
    BShoulderR, BUpperArmR, BForearmR, BHandR,
    BThighL, BShinL, BFootL,
    BThighR, BShinR, BFootR,
    BoneCount
};

struct Pose {
    std::array<Vector3, BoneCount> rot{};   // local Euler rotation (x roll, y yaw, z pitch forward)
    Vector3 rootOffset{0, 0, 0};            // pelvis offset from its rest position
    float rootPitch = 0;                    // whole-body lean / lying down (about the right axis)
    float rootRoll = 0;                     // whole-body roll (about the forward axis)
};

// What drives the animation this frame.
struct AnimInput {
    float speed = 0;          // m/s over the ground
    float distance = 0;       // total distance walked (drives the step cycle)
    bool sneaking = false;
    bool asleep = false;
    bool dead = false;
    float swing = 0;          // 1 -> 0 after a melee attack
    bool aiming = false;      // holding a rifle up
    bool working = false;     // searching, butchering, building...
    bool eating = false;
    float time = 0;
    // What is in the right hand: 0 nothing, 1 something to chop/club with,
    // 2 a knife (stabs), 3 a rifle
    int weapon = 0;
    // The body: injuries and how you feel change how you move
    float limpL = 0, limpR = 0;       // 0..1 hurt left/right leg or foot
    float armHurtL = 0, armHurtR = 0; // 0..1 hurt arm or hand (held close, guarded)
    float hunch = 0;                  // pain, weakness: bent over
    float shiver = 0;                 // cold
    float nausea = 0;                 // a hand on the stomach
    float winded = 0;                 // out of breath: hands on the knees when stopped
    int ride = 0;                     // 1 on a bike (pedalling with distance), 2 sitting in a car
};

class CharacterRig {
public:
    CharacterRig();
    void update(const AnimInput& in, float dt);   // blends towards the target pose
    float aimWeight() const { return wAim_; }

    // World transform of a bone (origin at the joint, +y along the bone)
    const Matrix& bone(int b) const { return world_[b]; }
    Vector3 joint(int b) const { return {world_[b].m12, world_[b].m13, world_[b].m14}; }
    // Where the bone ends (the next joint)
    Vector3 tip(int b) const;
    float length(int b) const { return length_[b]; }
    // Place the whole figure: feet on the ground at pos, facing angle (radians, world xz)
    void place(Vector3 pos, float facing);

    const Pose& pose() const { return pose_; }

private:
    Pose computeTarget(const AnimInput& in) const;
    void solve();

    int parent_[BoneCount];
    Vector3 offset_[BoneCount];   // joint position in the parent's frame
    Vector3 dir_[BoneCount];      // rest direction of the bone (unit)
    float length_[BoneCount];
    Pose pose_;
    Matrix world_[BoneCount];
    Matrix rootM_ = MatrixIdentity();
    // smoothed activity weights
    float wMove_ = 0, wRun_ = 0, wSneak_ = 0, wAim_ = 0, wWork_ = 0, wEat_ = 0, wLie_ = 0;
    float wLimpL_ = 0, wLimpR_ = 0, wArmL_ = 0, wArmR_ = 0, wHunch_ = 0, wShiver_ = 0, wNausea_ = 0, wWinded_ = 0;
    float wHold_ = 0;   // holding a weapon at the ready
    float wRide_ = 0;   // on a bike or in a car seat
};

} // namespace alone
