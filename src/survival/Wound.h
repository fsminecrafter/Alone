#pragma once
#include <algorithm>
#include <string>

namespace alone {

enum class BodyPart { Head, Torso, LeftArm, RightArm, LeftHand, RightHand, LeftLeg, RightLeg, LeftFoot, RightFoot, Count };
enum class WoundType { Scratch, Laceration, Bite, Puncture, Burn, Count };

// What can be put over a wound. One dressing per body part.
enum class DressingKind { None, Bandaid, Bandage, Rag, Count };

const char* bodyPartName(BodyPart p);
const char* woundTypeName(WoundType t);
const char* dressingName(DressingKind k);
DressingKind dressingFromString(const std::string& s);

struct Wound {
    BodyPart part = BodyPart::Torso;
    WoundType type = WoundType::Scratch;
    float severity = 0.2f;       // 0..1, how deep/large
    float bleeding = 0;          // ml per minute (before any dressing)
    float contamination = 0.3f;  // 0..1, dirt and germs in the wound
    float infection = 0;         // 0..1
    float healed = 0;            // 0..1, removed at 1
};

// A bandaid, bandage or rag tied over a body part. It soaks up blood and
// what the wound weeps, and picks up dirt from where you walk and what you
// do. A dirty dressing stops protecting the wound and starts infecting it.
struct Dressing {
    DressingKind kind = DressingKind::None;
    float dirt = 0;              // 0 clean .. 1 filthy (soil, mud, sweat)
    float blood = 0;             // 0 .. 1 soaked through
    float hours = 0;             // how long it has been on
    bool sterile = false;        // came out of a sealed pack

    bool on() const { return kind != DressingKind::None; }
    // How dirty it looks overall, 0..1
    float soiling() const { return std::min(1.0f, dirt + 0.6f * blood); }
};

} // namespace alone
