#pragma once
#include "raylib.h"
#include <cstdint>
#include <vector>

namespace alone {

// Surface materials, matching the ids in data/shaders/world.fs
enum Mat : int {
    MPlain = 0, MGrass, MForest, MDirt, MAsphalt, MPaving, MCobble, MSiding, MBrick, MPlaster,
    MRoofTiles, MShingles, MFloorWood, MFloorTile, MCarpet, MBark, MBirch, MLeaves, MGlass, MWater,
    MConcrete, MMetal, MLamp,
};

// Collects coloured triangles on the CPU and turns them into a raylib Mesh.
class MeshBuilder {
public:
    void clear() { pos_.clear(); nrm_.clear(); col_.clear(); uv_.clear(); }
    // Material for the following geometry (param: pattern orientation etc.)
    void material(int id, float param = 0) { mat_ = (float)id; param_ = param; }
    int currentMaterial() const { return (int)mat_; }
    bool empty() const { return pos_.empty(); }
    size_t vertexCount() const { return pos_.size() / 3; }

    void tri(Vector3 a, Vector3 b, Vector3 c, Color col);
    void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col);   // a-b-c-d counter-clockwise
    // Axis-aligned box from min corner to max corner
    void box(Vector3 mn, Vector3 mx, Color col, bool bottom = false);
    void boxFaces(Vector3 mn, Vector3 mx, Color sides, Color top);
    // Box rotated around Y by `yaw` radians about its centre
    void orientedBox(Vector3 centre, Vector3 half, float yaw, Color col);
    void cylinder(Vector3 base, float radius, float height, int sides, Color col, bool caps = true);
    void cone(Vector3 base, float radius, float height, int sides, Color col);
    // Cylinder between two points, radius tapering from r0 to r1
    void tube(Vector3 a, Vector3 b, float r0, float r1, int sides, Color col, bool capEnd = true);
    // Cone between two points (tip at b)
    void spike(Vector3 a, Vector3 b, float r, int sides, Color col);
    void blob(Vector3 centre, Vector3 radii, int slices, int rings, Color col, uint32_t seed = 0, float wobble = 0.0f);
    // Gable roof prism over the rectangle, ridge along X or Z
    void gable(Vector3 mn, Vector3 mx, float ridgeHeight, bool alongX, Color roof, Color gableCol, float overhang);
    void hip(Vector3 mn, Vector3 mx, float height, Color roof, float overhang);

    Mesh build() const;   // uploads to the GPU

private:
    std::vector<float> pos_, nrm_, uv_;
    float mat_ = 0, param_ = 0;
    std::vector<unsigned char> col_;
};

Color hexColor(uint32_t rgb, unsigned char a = 255);
Color shade(Color c, float f);
Color mixColor(Color a, Color b, float t);

} // namespace alone
