#pragma once
#include "raylib.h"
#include <cstdint>
#include <vector>

namespace alone {

class MapData;

// Surface materials, matching the ids in data/shaders/world.fs
enum Mat : int {
    MPlain = 0, MGrass, MForest, MDirt, MAsphalt, MPaving, MCobble, MSiding, MBrick, MPlaster,
    MRoofTiles, MShingles, MFloorWood, MFloorTile, MCarpet, MBark, MBirch, MLeaves, MGlass, MWater,
    MConcrete, MMetal, MLamp, MCladding,
};

// Collects coloured triangles on the CPU and turns them into a raylib Mesh.
class MeshBuilder {
public:
    void clear() { pos_.clear(); nrm_.clear(); col_.clear(); uv_.clear(); tan_.clear(); mn_ = {1e30f, 1e30f, 1e30f}; mx_ = {-1e30f, -1e30f, -1e30f}; }
    // Material for the following geometry (param: pattern orientation etc.)
    void material(int id, float param = 0) { mat_ = (float)id; param_ = param; }
    int currentMaterial() const { return (int)mat_; }
    bool empty() const { return pos_.empty(); }
    size_t vertexCount() const { return pos_.size() / 3; }
    // Corners of the box around everything added (for culling)
    Vector3 boundsMin() const { return mn_; }
    Vector3 boundsMax() const { return mx_; }

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

    // Standing on the terrain. lift: added to the height of what follows
    // (a tree, a building: level, at the height of its base). drape: each
    // point follows the ground under it (roads, markings) on top of lift.
    void setLift(float dy) { lift_ = dy; }
    float lift() const { return lift_; }
    void drape(const MapData* map) { drape_ = map; }
    // The shader darkens things near the ground (vertex alpha). By default
    // from each point's height before lifting; fixedAo: as if at this height
    // (the ground itself); noAo: never (unit meshes moved around by matrices).
    void fixedAo(float localY) { aoMode_ = 1; aoFixed_ = localY; }
    void autoAo() { aoMode_ = 0; }
    void noAo() { aoMode_ = 2; }

    // Walls the vertex shader can cut down when they stand between the player
    // and the camera (see world.vs). After trackPieces(), every vertex records
    // the wall piece it belongs to: its centre (x, z), its lowest point, and
    // whether it's a fitting (frames, posts: they sink just under the cut).
    // Geometry outside any piece is never cut.
    void trackPieces() { pieces_ = true; }
    void piece(float x, float z, float base, bool fitting = false) { piece_[0] = x; piece_[1] = z; piece_[2] = base; piece_[3] = fitting ? 1.0f : 0.0f; }
    void noPiece() { piece_[0] = piece_[1] = piece_[2] = 0; piece_[3] = 2; }

    Mesh build() const;   // uploads to the GPU

private:
    std::vector<float> pos_, nrm_, uv_, tan_;
    bool pieces_ = false;
    float piece_[4] = {0, 0, 0, 2};
    Vector3 mn_{1e30f, 1e30f, 1e30f}, mx_{-1e30f, -1e30f, -1e30f};
    float lift_ = 0, aoFixed_ = 0;
    int aoMode_ = 0;
    const MapData* drape_ = nullptr;
    float mat_ = 0, param_ = 0;
    std::vector<unsigned char> col_;
};

Color hexColor(uint32_t rgb, unsigned char a = 255);
Color shade(Color c, float f);
Color mixColor(Color a, Color b, float t);

} // namespace alone
