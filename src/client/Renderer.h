#pragma once
#include "client/MeshBuilder.h"
#include "game/Game.h"
#include "raylib.h"
#include <unordered_map>
#include <unordered_set>

namespace alone {

// Draws the world in 3D with an isometric (orthographic) camera.
class Renderer {
public:
    bool init();
    void shutdown();

    // Camera control
    void follow(Vec2 target, float dt);
    void snapTo(Vec2 target);
    void rotate(int steps);                 // 90 degree steps
    void zoom(float factor);
    float yaw() const { return yaw_; }
    Camera3D& camera() { return cam_; }

    // Converts a screen point to the tile/point the player means
    Vec2 pickGround(Vector2 mouse);
    Vec2 pick(Vector2 mouse, World& world);
    Vector2 toScreen(Vec2 p, float height = 0) const;
    // Camera-relative movement: screen up = away from the camera
    Vec2 screenToWorldDir(float sx, float sy) const;

    void draw(Game& game, float time, Vec2 hover, bool showHover);
    void invalidateAll();

private:
    struct ChunkGfx {
        uint32_t version = 0;
        Mesh ground{}, props{};
        bool hasGround = false, hasProps = false;
    };
    struct BuildingGfx {
        Mesh walls{}, wallsLow{}, roof{};
        bool hasWalls = false, hasWallsLow = false, hasRoof = false;
        int day = -1;
    };

    void buildChunk(World& world, Chunk& c, ChunkGfx& g);
    void buildBuilding(World& world, const Building& b, BuildingGfx& g);
    void addProp(MeshBuilder& mb, World& world, const Tile& t, int x, int y, int doy);
    void addRoadMarkings(MeshBuilder& mb, const MapData& map, int x0, int y0);
    void addRoadSurfaces(MeshBuilder& mb, World& world, int x0, int y0);
    void setLighting(Game& game, float time);
    void drawEntities(Game& game, float time);
    void drawPlayer(Game& game, float time);
    void drawAnimal(const Animal& a, float time);
    void drawDoors(World& world, const std::vector<int>& buildings);
    void drawPrim(const Mesh& m, Vector3 pos, Vector3 scale, float yaw, Color c, float pitch = 0, float roll = 0);
    void drawWeather(Game& game, float time);
    void unloadChunk(ChunkGfx& g);
    void setCutaway(bool on);
    void setUnlit(bool on);
    void updateCamera();

    Camera3D cam_{};
    Vec2 target_;
    float yaw_ = 0.785f;      // radians
    float yawTarget_ = 0.785f;
    float zoom_ = 30;         // metres of world visible vertically
    float pitch_ = 0.62f;     // ~35 degrees

    Shader shader_{};
    Material mat_{};
    int locSunDir_, locSunColor_, locAmbient_, locPlayer_, locViewDir_, locCutRadius_, locCutaway_;
    int locLights_, locLightColors_, locLightCount_, locTime_, locFogColor_, locFogStart_, locFogEnd_, locUnlit_;
    Mesh cube_{}, sphere_{}, cylinder_{}, cone_{};
    std::unordered_map<int64_t, ChunkGfx> chunks_;
    std::unordered_map<int, BuildingGfx> buildings_;
    Color skyColor_{};
    Vec2 playerPos_;
    int currentBuilding_ = -1;
};

} // namespace alone
