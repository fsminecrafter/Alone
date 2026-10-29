#pragma once
#include "client/CharacterModel.h"
#include "client/CharacterRig.h"
#include "client/MeshBuilder.h"
#include "game/Game.h"
#include "raylib.h"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace alone {

// What the mouse is over, drawn as an isometric tile cursor.
// Vehicle builders (RenderVehicles.cpp): local frame x forward, y up, z to the side.
using VehicleTransform = std::function<Vector3(Vector3)>;
void addCarModel(MeshBuilder& mb, const VehicleTransform& P, uint32_t look, bool smashed, float dust);
void addBikeModel(MeshBuilder& mb, const VehicleTransform& P, uint32_t look, float lean);

struct HoverInfo {
    Vec2 point;
    bool show = false;
    Color colour = WHITE;
    // Build mode: a ghost of the piece about to be built
    int buildKind = -1;          // BuildKind, -1 = not building
    int bx = 0, by = 0, bdir = 0;
    bool buildOk = false;
    // Holding Ctrl: an aiming reticle on the ground and a line from you to it
    bool aiming = false;
    Vec2 from;
    float range = 1.4f;
    bool ranged = false;
};

// Graphics settings the renderer understands (chosen in Settings > Graphics).
struct GraphicsOptions {
    int shadows = 2;          // 0 off, 1 low (1024), 2 high (2048), 3 ultra (4096)
    int maxLights = 32;       // 8, 16 or 32 point lights per frame
    bool weather = true;      // rain and snow particles
    bool visionCone = true;   // darken what the character can't see
    float brightness = 1.0f;  // 0.7 .. 1.4
    int renderScale = 0;      // 3D view resolution in percent (100, 75, 50); 0 = automatic
};

// Draws the world in 3D with an isometric (orthographic) camera, a sun shadow
// map, many coloured lights, and procedural surface materials.
class Renderer {
public:
    bool init();
    void shutdown();

    // Camera control
    void follow(Vec2 target, float dt);
    void snapTo(Vec2 target);
    void rotate(int steps);                 // 90 degree steps
    void faceTowards(Vec2 dir);             // turn the camera to look against this direction
    void zoom(float factor);
    void setZoom(float z) { zoom_ = z; updateCamera(); }
    float yaw() const { return yaw_; }
    Camera3D& camera() { return cam_; }

    // Converts a screen point to the tile/point the player means
    Vec2 pickGround(Vector2 mouse);
    Vec2 pick(Vector2 mouse, World& world);
    Vector2 toScreen(Vec2 p, float height = 0) const;
    // Camera-relative movement: screen up = away from the camera
    Vec2 screenToWorldDir(float sx, float sy) const;

    void draw(Game& game, float time, float dt, const HoverInfo& hover);
    void setOptions(const GraphicsOptions& o);
    // Per-frame timings (ms, summed) for --bench
    struct Profile { double rig = 0, prep = 0, shadow = 0, scene = 0, rest = 0; int frames = 0; double verts = 0, sunVerts = 0, chunks = 0, sunChunks = 0, buildingVerts = 0, sunBuildingVerts = 0; };
    Profile& profile() { return prof_; }
    bool finishEachPhase = false;   // bench: wait for the GPU after each phase
    const GraphicsOptions& options() const { return opts_; }
    float renderScale() const { return scale_; }   // what the 3D view is drawn at now
    void invalidateAll();

    // Renders the player character alone on a small stage (for the animation showcase)
    void drawShowcase(const AnimInput& anim, float facing, float dt, const Player& look, const ItemDef* weapon);

private:
    struct LightSrc { Vector3 pos; float radius; Vector3 colour; float intensity; };
    // A piece of a mesh with the box around it: big meshes are cut into
    // cells so the camera (or the sun) only draws the cells it can see
    struct Part { Mesh mesh{}; Vector3 mn{}, mx{}; };
    static void addPart(std::vector<Part>& parts, const MeshBuilder& mb);
    static void unloadParts(std::vector<Part>& parts);
    struct ChunkGfx {
        uint32_t version = 0;
        Mesh ground{};
        bool hasGround = false;
        Vector3 mn{}, mx{};             // box around the whole chunk
        std::vector<Part> props, detail;   // in 16 m cells
        std::vector<Vector3> lamps;     // street lamp heads
    };
    // Lettering on a building: a textured quad drawn after the scene
    struct SignGfx {
        std::string text;
        Vector3 centre;          // middle of the sign
        Vector3 right;           // unit vector along the letters
        float height = 0.5f;     // letter band height (m)
        Color fg = WHITE, bg = {0, 0, 0, 0};
        bool interior = false;   // only shown when you're inside
        bool lit = true;         // glows at night while there's power
        bool dot = false;        // Storken's yellow circle
    };
    struct BuildingGfx {
        Mesh walls{}, wallsLow{}, roof{};
        bool hasWalls = false, hasWallsLow = false, hasRoof = false;
        std::vector<Part> interior;     // furniture, in 16 m cells
        std::vector<LightSrc> lights;   // room lights, TVs, the door lamp
        std::vector<SignGfx> signs;
        uint64_t stamp = 0;             // versions of the chunks it stands on (furniture changes)
        Vector3 boxMin{}, boxMax{};     // bounds, for culling
    };
    // What is drawn this frame: only what the camera (or the sun) can see,
    // nearest first so hidden pixels are rejected before they're shaded
    struct DrawItem { ChunkGfx* chunk = nullptr; int building = -1; float depth = 0; };
    std::vector<DrawItem> viewChunks_, viewBuildings_;
    static bool inView(const Camera3D& c, float aspect, Vector3 mn, Vector3 mx, float* depth = nullptr);
    // The camera of the pass being drawn (the view, or the sun for shadows)
    Camera3D passCam_{};
    float passAspect_ = 1.0f, aspect_ = 1.0f;
    bool partVisible(const Part& p) const { return inView(passCam_, passAspect_, p.mn, p.mx); }
    bool nearSightLine(Vector3 mn, Vector3 mx) const;   // could stand between you and the camera
    uint64_t buildingStamp(World& world, const Building& b);
    void unloadBuilding(BuildingGfx& g);
    struct SignTex { Texture2D tex{}; float aspect = 1; };
    std::unordered_map<std::string, SignTex> signTex_;
    const SignTex& signTexture(const std::string& text, Color fg, Color bg, bool dot);
    void drawSigns(Game& game, const std::vector<int>& buildings);

    // world content (RenderProps.cpp / RenderBuildings.cpp)
    void buildChunk(World& world, Chunk& c, ChunkGfx& g);
    void buildBuilding(World& world, const Building& b, BuildingGfx& g);
    void addProp(MeshBuilder& mb, World& world, const Tile& t, int x, int y, int doy, ChunkGfx& g);
    void addRoadMarkings(MeshBuilder& mb, const MapData& map, int x0, int y0);
    void addRoadSurfaces(MeshBuilder& mb, World& world, int x0, int y0);
    void addPowerLines(MeshBuilder& mb, World& world, int x0, int y0);
    void addBuilt(MeshBuilder& mb, World& world, int x0, int y0);
    void drawTent(const Tent& t, bool inside, Color fabric);
    const Mesh& tentMesh(const Tent& t, Color fabric);   // built once per shape, size, door side and colour
    void drawStove(const Campfire& f, float time, bool shadowPass);
    void drawBuildGhost(const HoverInfo& hover, float time);

    // frame (Renderer.cpp)
    void setLighting(Game& game, float time, const std::vector<LightSrc>& lights);
    void renderShadows(Game& game, float time);
    // Everything that stays put: trees, props, buildings, doors
    void drawStatic(Game& game, const std::vector<DrawItem>& chunks, const std::vector<DrawItem>& buildings, bool shadowPass);
    void drawScene(Game& game, float time);
    bool occludes(World& world, int buildingId);   // stands between the player and the camera

    // entities (RenderEntities.cpp)
    void drawEntities(Game& game, float time, bool shadowPass);
    void drawCharacter(const CharacterRig& rig, const CharacterModel& model, const std::array<ItemInstance, 9>& worn, const Body* body,
                       const ItemDef* weapon);
    // vehicles and other people (RenderVehicles.cpp)
    const Mesh& vehicleMesh(const Vehicle& v);
    void drawVehicles(Game& game, bool shadowPass);
    void updateNpcRigs(Game& game, float time, float dt);
    void drawSurvivors(Game& game);
    std::unordered_map<int, Mesh> vehicleMeshes_;
    std::unordered_map<uint64_t, Mesh> tentMeshes_;
    struct NpcGfx {
        CharacterRig rig;
        std::unique_ptr<CharacterModel> model;
        Vec2 lastPos;
        float dist = 0, speed = 0, y = 0;
        bool init = false;
        const ItemDef* weapon = nullptr;
    };
    std::unordered_map<int, NpcGfx> npcs_;
    void drawAnimal(const Animal& a, float time);
    void drawDoors(World& world, const std::vector<int>& buildings, bool mainPass);
    void drawWeather(Game& game, float time);
    void drawCursor(Game& game, const HoverInfo& hover, float time);
    void updatePlayerRig(Game& game, float time, float dt);
    void drawBlood(float time);
    static CharacterLook lookOf(const std::array<ItemInstance, 9>& worn, const Body* body);

    // primitives
    void drawPrim(const Mesh& m, Vector3 pos, Vector3 scale, float yaw, Color c, float pitch = 0, float roll = 0);
    void drawTube(Vector3 a, Vector3 b, float r0, Color c);
    void drawBlob(Vector3 centre, Vector3 radii, Color c, float yaw = 0);
    void drawOnBone(const Mesh& m, const Matrix& bone, Vector3 localCentre, Vector3 scale, Color c);
    void unloadChunk(ChunkGfx& g);
    void setUnlit(bool on);
    void setWallCut(int mode);
    bool wallCutAt(Vec2 centre, int mode) const;   // the vertex shader's test, on the CPU
    void drawSilhouette();
    struct CutBuilding { int id, mode; };
    std::vector<CutBuilding> cutBuildings_;          // drawn cut this frame (doors follow them)
    void setEmissive(Vector3 e);
    void updateCamera();

    Camera3D cam_{};
    Vec2 target_;
    float yaw_ = 0.785f;      // radians
    float yawTarget_ = 0.785f;
    float zoom_ = 30;         // metres of world visible vertically
    float pitch_ = 0.62f;     // ~35 degrees

    Shader shader_{}, depthShader_{}, silhouetteShader_{};
    Shader prog_[2]{};               // world shader: plain, and with the see-through cutaway
    Material mat_{}, matCut_{}, depthMat_{}, silhouetteMat_{};
    Material* active_ = nullptr;     // the material drawPrim uses (depth during the shadow pass)
    // Sun shadows. Trees and buildings are drawn into a cached map that is
    // only redrawn (spread over a few frames, into the back map) when you've
    // walked a fair way or the sun has moved; each frame that map is copied
    // and the moving things (you, animals, fires) are added on top.
    RenderTexture2D shadowFront_{}, shadowBack_{}, shadowMap_{};
    int shadowSize_ = 2048;
    Matrix lightVP_ = MatrixIdentity();
    bool shadowsReady_ = false;
    struct ShadowView { Camera3D cam{}; Matrix vp = MatrixIdentity(); Vec2 centre; Vector3 sun{}; float zoom = 0; bool valid = false; };
    ShadowView shadowFrontView_;
    struct ShadowBuild {
        bool active = false;
        int step = 0, steps = 4;
        ShadowView view;
        std::vector<int64_t> chunks;    // chunk keys
        std::vector<int> buildings;
    } shadowBuild_;
    bool shadowDirty_ = true;           // something in the cached map changed
    Camera3D lightCamera(Vec2 centre, Vector3 sunDir) const;
    void shadowStep(Game& game);
    enum Uni {
        USunDir, USunColor, UAmbient, USky, UPlayer, UViewDir, UCutRadius, UCutaway, ULights, ULightColors,
        ULightCount, UTime, UFogColor, UFogStart, UFogEnd, UUnlit, USpotPos, USpotDir, USpotOn, USnow, UWet, UEmissive,
        ULampGlow, UVision, UFacing, ULightVP, UShadowMap, UShadowOn, UShadowTexel, UBrightness, UWallCut,
        USheltered, URainNow, UWallFloor, UCount
    };
    int loc_[2][UCount]{};
    void uni(Uni u, const void* value, int type, int count = 1);
    void uniMatrix(Uni u, const Matrix& m);
    Vector3 emissive_{-1, -1, -1};   // last values sent
    float sheltered_ = -1;
    void setSheltered(bool on);
    GraphicsOptions opts_;
    Profile prof_;
    // Render resolution: the 3D view can be drawn smaller and scaled up to
    // the window (the interface stays sharp). Automatic mode lowers it while
    // the frame rate is low and raises it again when there's room.
    RenderTexture2D sceneRT_{};
    float scale_ = 1.0f, frameAvg_ = 1.0f / 60.0f, scaleTimer_ = 0, scaleHold_ = 0;
    void updateRenderScale(float dt);
    void reloadShadowMap();
    Mesh cube_{}, sphere_{}, cylinder_{}, cone_{};
    std::unordered_map<int64_t, ChunkGfx> chunks_;
    std::unordered_map<int, BuildingGfx> buildings_;
    Color skyColor_{};
    Vec2 playerPos_;
    int currentBuilding_ = -1;
    Vector3 lampGlow_{0, 0, 0}, windowGlow_{0, 0, 0};
    float dark_ = 0;
    bool power_ = true;

    // player animation
    CharacterRig playerRig_;
    CharacterModel playerModel_;
    struct BloodDrop { Vec2 pos; float y = 0; float size = 0.08f; float age = 0; float amount = 1; bool indoors = false; };
    std::vector<BloodDrop> bloodDrops_;
    float bloodTimer_ = 0;
    Vec2 lastPlayerPos_;
    float playerDistance_ = 0, playerSpeed_ = 0;
    bool rigInit_ = false;
    float playerY_ = 0;              // height the character stands at (eased over steps)
    float liftY_ = 0;                // added to the height of primitives: the ground of what is drawn
    World* world_ = nullptr;         // (for picking the ground under the mouse)
    float targetY_ = 0;              // height the camera looks at
    bool snapY_ = true;
};

} // namespace alone
