#pragma once
#include "client/CharacterRig.h"
#include "client/MeshBuilder.h"
#include "game/Game.h"
#include "raylib.h"
#include <unordered_map>
#include <vector>

namespace alone {

// What the mouse is over, drawn as an isometric tile cursor.
struct HoverInfo {
    Vec2 point;
    bool show = false;
    Color colour = WHITE;
};

// Graphics settings the renderer understands (chosen in Settings > Graphics).
struct GraphicsOptions {
    int shadows = 2;          // 0 off, 1 low (1024), 2 high (2048), 3 ultra (4096)
    int maxLights = 32;       // 8, 16 or 32 point lights per frame
    bool weather = true;      // rain and snow particles
    bool visionCone = true;   // darken what the character can't see
    float brightness = 1.0f;  // 0.7 .. 1.4
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
    const GraphicsOptions& options() const { return opts_; }
    void invalidateAll();

    // Renders the player character alone on a small stage (for the animation showcase)
    void drawShowcase(const AnimInput& anim, float facing, float dt, const Player& look, const ItemDef* weapon);

private:
    struct LightSrc { Vector3 pos; float radius; Vector3 colour; float intensity; };
    struct ChunkGfx {
        uint32_t version = 0;
        Mesh ground{}, props{};
        bool hasGround = false, hasProps = false;
        std::vector<Vector3> lamps;     // street lamp heads
    };
    struct BuildingGfx {
        Mesh walls{}, wallsLow{}, roof{};
        bool hasWalls = false, hasWallsLow = false, hasRoof = false;
        std::vector<LightSrc> lights;   // room lights, TVs, the door lamp
    };

    // world content (RenderProps.cpp / RenderBuildings.cpp)
    void buildChunk(World& world, Chunk& c, ChunkGfx& g);
    void buildBuilding(World& world, const Building& b, BuildingGfx& g);
    void addProp(MeshBuilder& mb, World& world, const Tile& t, int x, int y, int doy, ChunkGfx& g);
    void addRoadMarkings(MeshBuilder& mb, const MapData& map, int x0, int y0);
    void addRoadSurfaces(MeshBuilder& mb, World& world, int x0, int y0);
    void addPowerLines(MeshBuilder& mb, World& world, int x0, int y0);

    // frame (Renderer.cpp)
    void setLighting(Game& game, float time, const std::vector<LightSrc>& lights);
    void renderShadows(Game& game, const std::vector<int>& buildings, float time);
    void drawScene(Game& game, const std::vector<int>& buildings, float time, bool shadowPass);

    // entities (RenderEntities.cpp)
    void drawEntities(Game& game, float time, bool shadowPass);
    void drawCharacter(const CharacterRig& rig, const Player& look, const ItemDef* weapon);
    void drawAnimal(const Animal& a, float time);
    void drawDoors(World& world, const std::vector<int>& buildings);
    void drawWeather(Game& game, float time);
    void drawCursor(Game& game, const HoverInfo& hover, float time);
    void updatePlayerRig(Game& game, float time, float dt);

    // primitives
    void drawPrim(const Mesh& m, Vector3 pos, Vector3 scale, float yaw, Color c, float pitch = 0, float roll = 0);
    void drawTube(Vector3 a, Vector3 b, float r0, Color c);
    void drawBlob(Vector3 centre, Vector3 radii, Color c, float yaw = 0);
    void drawOnBone(const Mesh& m, const Matrix& bone, Vector3 localCentre, Vector3 scale, Color c);
    void unloadChunk(ChunkGfx& g);
    void setCutaway(bool on);
    void setUnlit(bool on);
    void setEmissive(Vector3 e);
    void updateCamera();

    Camera3D cam_{};
    Vec2 target_;
    float yaw_ = 0.785f;      // radians
    float yawTarget_ = 0.785f;
    float zoom_ = 30;         // metres of world visible vertically
    float pitch_ = 0.62f;     // ~35 degrees

    Shader shader_{}, depthShader_{};
    Material mat_{}, depthMat_{};
    Material* active_ = nullptr;     // the material drawPrim uses (depth during the shadow pass)
    RenderTexture2D shadowMap_{};
    int shadowSize_ = 2048;
    Matrix lightVP_ = MatrixIdentity();
    bool shadowsReady_ = false;
    int locSunDir_, locSunColor_, locAmbient_, locSky_, locPlayer_, locViewDir_, locCutRadius_, locCutaway_;
    int locLights_, locLightColors_, locLightCount_, locTime_, locFogColor_, locFogStart_, locFogEnd_, locUnlit_;
    int locSpotPos_, locSpotDir_, locSpotOn_, locSnow_, locWet_, locEmissive_, locLampGlow_, locVision_, locFacing_;
    int locLightVP_, locShadowMap_, locShadowOn_, locShadowTexel_, locBrightness_;
    GraphicsOptions opts_;
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
    Vec2 lastPlayerPos_;
    float playerDistance_ = 0, playerSpeed_ = 0;
    bool rigInit_ = false;
};

} // namespace alone
