#pragma once
#include <string>

namespace alone {

enum class BodyPart { Head, Torso, LeftArm, RightArm, LeftHand, RightHand, LeftLeg, RightLeg, LeftFoot, RightFoot, Count };
enum class WoundType { Scratch, Laceration, Bite, Puncture, Burn, Count };

const char* bodyPartName(BodyPart p);
const char* woundTypeName(WoundType t);

struct Wound {
    BodyPart part = BodyPart::Torso;
    WoundType type = WoundType::Scratch;
    float severity = 0.2f;       // 0..1, how deep/large
    float bleeding = 0;          // ml per minute
    bool bandaged = false;
    bool disinfected = false;
    float bandageDirt = 0;       // 0..1, a dirty bandage stops helping
    float contamination = 0.3f;  // 0..1, chance the wound gets infected
    float infection = 0;         // 0..1
    float healed = 0;            // 0..1, removed at 1
};

} // namespace alone
