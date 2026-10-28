#pragma once
#include "client/CharacterRig.h"
#include "raylib.h"
#include "survival/Wound.h"
#include <cstdint>
#include <vector>

namespace alone {

// What a character wears, as colours. Alpha 0 means "not worn".
struct CharacterLook {
    Color skin{226, 184, 150, 255};
    Color hair{74, 54, 38, 255};
    Color top{214, 204, 186, 255};      // shirt or sweater
    Color jacket{0, 0, 0, 0};
    Color pants{60, 74, 104, 255};
    Color shoes{110, 80, 56, 255};
    Color gloves{0, 0, 0, 0};
    Color hat{0, 0, 0, 0};
    bool shortSleeves = false;          // only a t-shirt on top: arms show
    bool barefoot = false;
    float blood[(int)BodyPart::Count]{};   // blood soaked into the clothes, 0..1
};

// One smooth, seamless body mesh hung on the CharacterRig skeleton. It is
// built once in the rest pose with every vertex weighted to one or two bones;
// each frame the vertices follow their bones (linear blend skinning on the
// CPU, a couple of thousand vertices) and are uploaded to the GPU. Joints
// bend instead of showing separate blocks.
class CharacterModel {
public:
    bool init();
    void shutdown();
    // Pose the mesh like the rig and colour it for this look
    void skin(const CharacterRig& rig, const CharacterLook& look);
    const Mesh& mesh() const { return mesh_; }
    bool ready() const { return ready_; }

    enum Region : uint8_t {
        RHead, RNose, REar, REye, RBrow, RLip, RHair, RNeck,
        RTorso, RUpperArm, RForearm, RHand, RLeg, RShoe, RSole, Count
    };

private:
    struct Vtx {
        Vector3 pos{}, nrm{};
        uint8_t b0 = 0, b1 = 0;
        float w0 = 1;            // weight of b0 (b1 gets the rest)
        uint8_t region = 0;
        uint8_t part = 0;        // BodyPart, for blood
        float t = 0;             // position along the limb (sleeve length etc.)
    };
    struct Ring {
        Vector3 c;               // centre
        Vector3 u, v;            // axes of the cross-section
        float ru, rv;            // radii along u and v
        int b0, b1;
        float w0;
        float t;
        float squareness = 1.0f; // 1 = ellipse, smaller = boxier
    };
    // Adds a lofted tube through the rings (optionally capped at the ends).
    // keep(i, angle) can drop quads (hairlines).
    void tube(const std::vector<Ring>& rings, int segments, Region region, BodyPart part, bool capStart, bool capEnd,
              bool (*keep)(int ring, int count, float angle) = nullptr);
    void ellipsoid(Vector3 c, Vector3 r, int bone, Region region, BodyPart part, int rings = 8, int segments = 10,
                   bool (*keep)(int ring, int count, float angle) = nullptr);
    void build();
    void computeNormals();

    std::vector<Vtx> rest_;
    std::vector<unsigned short> idx_;
    Matrix invBind_[BoneCount];
    Mesh mesh_{};
    bool ready_ = false;
};

} // namespace alone
