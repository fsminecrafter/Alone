#include "client/Renderer.h"
#include "client/RenderCommon.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Rng.h"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>

namespace alone {

using namespace rc;

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

namespace {

RenderTexture2D loadShadowMap(int size) {
    RenderTexture2D t{};
    t.id = rlLoadFramebuffer();
    t.texture.width = size;
    t.texture.height = size;
    if (t.id > 0) {
        rlEnableFramebuffer(t.id);
        t.depth.id = rlLoadTextureDepth(size, size, false);
        t.depth.width = size;
        t.depth.height = size;
        t.depth.format = 19;
        t.depth.mipmaps = 1;
        rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
        if (!rlFramebufferComplete(t.id)) Log::warn("Shadow map framebuffer incomplete");
        rlDisableFramebuffer();
    }
    return t;
}

Mesh unitMesh(int kind) {
    MeshBuilder mb;
    mb.material(MPlain);
    Color w = WHITE;
    switch (kind) {
        case 0: mb.box(V(-0.5f, -0.5f, -0.5f), V(0.5f, 0.5f, 0.5f), w, true); break;          // cube
        case 1: mb.blob(V(0, 0, 0), V(1, 1, 1), 12, 8, w); break;                              // sphere
        case 2: mb.tube(V(0, 0, 0), V(0, 1, 0), 1, 1, 10, w); break;                           // cylinder
        default: mb.cone(V(0, 0, 0), 1, 1, 9, w); break;                                       // cone
    }
    return mb.build();
}

} // namespace

bool Renderer::init() {
    shader_ = LoadShader(Paths::data("shaders/world.vs").c_str(), Paths::data("shaders/world.fs").c_str());
    if (shader_.id == 0 || shader_.id == rlGetShaderIdDefault()) { Log::error("Failed to load world shader"); return false; }
    depthShader_ = LoadShader(Paths::data("shaders/depth.vs").c_str(), Paths::data("shaders/depth.fs").c_str());
    shader_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(shader_, "matModel");
    auto loc = [&](const char* n) { return GetShaderLocation(shader_, n); };
    locSunDir_ = loc("sunDir"); locSunColor_ = loc("sunColor"); locAmbient_ = loc("ambientColor"); locSky_ = loc("skyColor");
    locPlayer_ = loc("playerPos"); locViewDir_ = loc("viewDir"); locCutRadius_ = loc("cutRadius"); locCutaway_ = loc("cutaway");
    locLights_ = loc("lights"); locLightColors_ = loc("lightColors"); locLightCount_ = loc("lightCount"); locTime_ = loc("time");
    locFogColor_ = loc("fogColor"); locFogStart_ = loc("fogStart"); locFogEnd_ = loc("fogEnd"); locUnlit_ = loc("unlit");
    locSpotPos_ = loc("spotPos"); locSpotDir_ = loc("spotDir"); locSpotOn_ = loc("spotOn"); locSnow_ = loc("snow"); locWet_ = loc("wet");
    locEmissive_ = loc("emissive"); locLampGlow_ = loc("lampGlow"); locVision_ = loc("visionCone"); locFacing_ = loc("facing");
    locLightVP_ = loc("lightVP"); locShadowMap_ = loc("shadowMap"); locShadowOn_ = loc("shadowOn"); locShadowTexel_ = loc("shadowTexel");
    mat_ = LoadMaterialDefault();
    mat_.shader = shader_;
    depthMat_ = LoadMaterialDefault();
    depthMat_.shader = depthShader_;
    active_ = &mat_;
    cube_ = unitMesh(0);
    sphere_ = unitMesh(1);
    cylinder_ = unitMesh(2);
    cone_ = unitMesh(3);
    shadowMap_ = loadShadowMap(shadowSize_);
    shadowsReady_ = shadowMap_.id > 0 && depthShader_.id != 0;
    cam_.up = {0, 1, 0};
    cam_.projection = CAMERA_ORTHOGRAPHIC;
    cam_.fovy = zoom_;
    return true;
}

void Renderer::unloadChunk(ChunkGfx& g) {
    if (g.hasGround) UnloadMesh(g.ground);
    if (g.hasProps) UnloadMesh(g.props);
    g.hasGround = g.hasProps = false;
}

void Renderer::invalidateAll() {
    for (auto& [k, g] : chunks_) unloadChunk(g);
    chunks_.clear();
    for (auto& [k, b] : buildings_) {
        if (b.hasWalls) UnloadMesh(b.walls);
        if (b.hasWallsLow) UnloadMesh(b.wallsLow);
        if (b.hasRoof) UnloadMesh(b.roof);
    }
    buildings_.clear();
    rigInit_ = false;
}

void Renderer::shutdown() {
    invalidateAll();
    UnloadMesh(cube_);
    UnloadMesh(sphere_);
    UnloadMesh(cylinder_);
    UnloadMesh(cone_);
    if (shadowMap_.id) {
        rlUnloadTexture(shadowMap_.depth.id);
        rlUnloadFramebuffer(shadowMap_.id);
    }
    UnloadShader(depthShader_);
    UnloadShader(shader_);
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void Renderer::updateCamera() {
    float d = 120.0f;
    Vector3 t{target_.x, 0, target_.y};
    cam_.target = t;
    cam_.position = {t.x + std::cos(pitch_) * std::sin(yaw_) * d, t.y + std::sin(pitch_) * d,
                     t.z + std::cos(pitch_) * std::cos(yaw_) * d};
    cam_.fovy = zoom_;
}

void Renderer::follow(Vec2 target, float dt) {
    float k = std::min(1.0f, dt * 8.0f);
    target_ = target_ + (target - target_) * k;
    yaw_ += (yawTarget_ - yaw_) * std::min(1.0f, dt * 10.0f);
    updateCamera();
}

void Renderer::snapTo(Vec2 target) {
    target_ = target;
    yaw_ = yawTarget_;
    updateCamera();
}

void Renderer::rotate(int steps) { yawTarget_ += steps * PI * 0.5f; }

void Renderer::zoom(float f) { zoom_ = std::clamp(zoom_ * f, 10.0f, 90.0f); }

Vec2 Renderer::pickGround(Vector2 mouse) {
    Ray r = GetScreenToWorldRay(mouse, cam_);
    if (std::fabs(r.direction.y) < 1e-5f) return target_;
    float t = -r.position.y / r.direction.y;
    return Vec2{r.position.x + r.direction.x * t, r.position.z + r.direction.z * t};
}

Vec2 Renderer::pick(Vector2 mouse, World& world) {
    Ray r = GetScreenToWorldRay(mouse, cam_);
    Vec2 g = pickGround(mouse);
    float best = 1e9f;
    Vec2 out = g;
    int gx = (int)std::floor(g.x), gy = (int)std::floor(g.y);
    for (int y = gy - 4; y <= gy + 4; ++y)
        for (int x = gx - 4; x <= gx + 4; ++x) {
            const Tile& t = world.tile(x, y);
            if (t.prop == Prop::None) continue;
            float h = std::min(propHeight(t.prop), 2.2f);
            if (h < 0.3f) continue;
            float inset = isTree(t.prop) ? 0.3f : 0.05f;
            BoundingBox bb{{x + inset, 0, y + inset}, {x + 1 - inset, h, y + 1 - inset}};
            RayCollision c = GetRayCollisionBox(r, bb);
            if (c.hit && c.distance < best) { best = c.distance; out = Vec2{x + 0.5f, y + 0.5f}; }
        }
    return out;
}

Vector2 Renderer::toScreen(Vec2 p, float h) const {
    return GetWorldToScreen(Vector3{p.x, h, p.y}, cam_);
}

Vec2 Renderer::screenToWorldDir(float sx, float sy) const {
    Vec2 fwd{-std::sin(yaw_), -std::cos(yaw_)};
    Vec2 right{std::cos(yaw_), -std::sin(yaw_)};
    return right * sx + fwd * (-sy);
}

// ---------------------------------------------------------------------------
// Uniform helpers
// ---------------------------------------------------------------------------

void Renderer::setCutaway(bool on) {
    float v = on ? 1.0f : 0.0f;
    SetShaderValue(shader_, locCutaway_, &v, SHADER_UNIFORM_FLOAT);
}

void Renderer::setUnlit(bool on) {
    float v = on ? 1.0f : 0.0f;
    SetShaderValue(shader_, locUnlit_, &v, SHADER_UNIFORM_FLOAT);
}

void Renderer::setEmissive(Vector3 e) {
    SetShaderValue(shader_, locEmissive_, &e, SHADER_UNIFORM_VEC3);
}

// ---------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------

static Vector3 sunDirection(const Clock& clock) {
    float hour = clock.hourOfDay();
    float elev = clock.sunElevationDeg();
    float az = (hour - 12.5f) / 12.0f * PI;
    return Vector3Normalize(V(-std::sin(az) * 0.8f, std::max(0.35f, std::sin(elev * DEG2RAD) + 0.3f), std::cos(az) * 0.6f - 0.4f));
}

void Renderer::setLighting(Game& game, float time, const std::vector<LightSrc>& lightList) {
    World& w = game.world();
    const Clock& clock = w.clock();
    float day = clock.daylight();
    float elev = clock.sunElevationDeg();
    const Weather& wx = clock.weather();
    dark_ = 1.0f - day;

    Vector3 sunDir = sunDirection(clock);
    float warm = 1.0f - std::clamp((elev - 2.0f) / 18.0f, 0.0f, 1.0f);
    float moon = clock.moonlight() * dark_;
    Vector3 sunCol = Vector3Scale(V(1.0f + 0.25f * warm, 0.93f, 0.8f - 0.2f * warm), day * (1.0f - 0.55f * wx.cloud));
    sunCol = Vector3Add(sunCol, Vector3Scale(V(0.25f, 0.3f, 0.45f), moon * 0.5f));
    Vector3 night = Vector3Add(V(0.05f, 0.06f, 0.1f), Vector3Scale(V(0.06f, 0.07f, 0.1f), moon));
    Vector3 amb = Vector3Lerp(night, V(0.5f, 0.52f, 0.57f), day);
    Vector3 sky = Vector3Lerp(V(0.05f, 0.07f, 0.14f), V(0.55f, 0.62f, 0.7f), day);
    SetShaderValue(shader_, locSunDir_, &sunDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locSunColor_, &sunCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locAmbient_, &amb, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locSky_, &sky, SHADER_UNIFORM_VEC3);

    const Player& p = game.player();
    Vector3 pp = V(p.pos.x, 1.0f, p.pos.y);
    SetShaderValue(shader_, locPlayer_, &pp, SHADER_UNIFORM_VEC3);
    Vector3 view = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    SetShaderValue(shader_, locViewDir_, &view, SHADER_UNIFORM_VEC3);
    float cut = currentBuilding_ >= 0 ? 9.0f : 3.2f;
    SetShaderValue(shader_, locCutRadius_, &cut, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locTime_, &time, SHADER_UNIFORM_FLOAT);
    float snow = w.snowCover(), wet = w.groundWetness();
    SetShaderValue(shader_, locSnow_, &snow, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locWet_, &wet, SHADER_UNIFORM_FLOAT);
    float vision = game.dead() || p.asleep ? 0.0f : 1.0f;
    SetShaderValue(shader_, locVision_, &vision, SHADER_UNIFORM_FLOAT);
    Vector2 facing{std::cos(p.facing), std::sin(p.facing)};
    SetShaderValue(shader_, locFacing_, &facing, SHADER_UNIFORM_VEC2);

    float spotOn = p.flashlightOn ? 1.0f : 0.0f;
    Vector3 spotPos = V(p.pos.x, 1.3f, p.pos.y);
    Vector3 spotDir = Vector3Normalize(V(std::cos(p.facing), -0.35f, std::sin(p.facing)));
    SetShaderValue(shader_, locSpotOn_, &spotOn, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locSpotPos_, &spotPos, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locSpotDir_, &spotDir, SHADER_UNIFORM_VEC3);

    Vector4 lights[32];
    Vector4 cols[32];
    int n = 0;
    for (const auto& l : lightList) {
        if (n >= 32) break;
        float inten = l.intensity;
        if (l.colour.z > l.colour.x + 0.2f) inten *= 0.6f + 0.4f * std::fabs(std::sin(time * 7.0f + l.pos.x) * std::sin(time * 3.1f));
        lights[n] = {l.pos.x, l.pos.y, l.pos.z, l.radius};
        cols[n] = {l.colour.x, l.colour.y, l.colour.z, inten};
        ++n;
    }
    if (n == 0) { lights[0] = {0, 0, 0, 1}; cols[0] = {0, 0, 0, 0}; }
    SetShaderValueV(shader_, locLights_, lights, SHADER_UNIFORM_VEC4, std::max(1, n));
    SetShaderValueV(shader_, locLightColors_, cols, SHADER_UNIFORM_VEC4, std::max(1, n));
    SetShaderValue(shader_, locLightCount_, &n, SHADER_UNIFORM_INT);

    Color daySky = mixColor(hexColor(0x9fb4c4), hexColor(0x8a9096), wx.cloud);
    skyColor_ = mixColor(hexColor(0x0a0d14), daySky, day);
    Vector3 fogC = V(skyColor_.r / 255.0f, skyColor_.g / 255.0f, skyColor_.b / 255.0f);
    float fogStart = 70.0f - 40.0f * wx.rain - 55.0f * wx.fog;
    float fogEnd = 140.0f - 70.0f * wx.rain - 105.0f * wx.fog;
    SetShaderValue(shader_, locFogColor_, &fogC, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locFogStart_, &fogStart, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locFogEnd_, &fogEnd, SHADER_UNIFORM_FLOAT);

    power_ = w.powerOn();
    float glow = power_ ? std::clamp((dark_ - 0.3f) * 2.0f, 0.0f, 1.0f) : 0.0f;
    lampGlow_ = Vector3Scale(V(1.0f, 0.75f, 0.42f), glow * 1.4f);
    windowGlow_ = Vector3Scale(V(0.85f, 0.62f, 0.32f), glow);
    SetShaderValue(shader_, locLampGlow_, &lampGlow_, SHADER_UNIFORM_VEC3);
    setEmissive(V(0, 0, 0));

    float sOn = (shadowsReady_ && (day > 0.05f || moon > 0.3f)) ? 1.0f : 0.0f;
    float texel = 1.0f / shadowSize_;
    SetShaderValue(shader_, locShadowOn_, &sOn, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locShadowTexel_, &texel, SHADER_UNIFORM_FLOAT);
    SetShaderValueMatrix(shader_, locLightVP_, lightVP_);
    if (shadowsReady_) {
        int slot = 10;
        rlActiveTextureSlot(slot);
        rlEnableTexture(shadowMap_.depth.id);
        rlActiveTextureSlot(0);
        SetShaderValue(shader_, locShadowMap_, &slot, SHADER_UNIFORM_INT);
    }
    setUnlit(false);
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

void Renderer::drawScene(Game& game, const std::vector<int>& buildingIds, float time, bool shadowPass) {
    World& world = game.world();
    Matrix identity = MatrixIdentity();
    Material& m = shadowPass ? depthMat_ : mat_;
    active_ = &m;
    m.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (!shadowPass) {
        setCutaway(false);
        for (auto& [key, g] : chunks_)
            if (g.hasGround) DrawMesh(g.ground, m, identity);
        setCutaway(true);
    }
    for (auto& [key, g] : chunks_)
        if (g.hasProps) DrawMesh(g.props, m, identity);
    for (int id : buildingIds) {
        auto it = buildings_.find(id);
        if (it == buildings_.end()) continue;
        if (!shadowPass) setEmissive(power_ && world.buildingLightsOn(id) ? windowGlow_ : V(0, 0, 0));
        m.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        bool inside = id == currentBuilding_;
        if (inside && it->second.hasWallsLow && !shadowPass) DrawMesh(it->second.wallsLow, m, identity);
        else if (it->second.hasWalls) DrawMesh(it->second.walls, m, identity);
        if (it->second.hasRoof && !(inside && !shadowPass)) DrawMesh(it->second.roof, m, identity);
    }
    if (!shadowPass) setEmissive(V(0, 0, 0));
    drawDoors(world, buildingIds);
    if (!shadowPass) setCutaway(false);
    drawEntities(game, time, shadowPass);
    active_ = &mat_;
}

void Renderer::renderShadows(Game& game, const std::vector<int>& buildingIds, float time) {
    if (!shadowsReady_) return;
    Vector3 sunDir = sunDirection(game.world().clock());
    Camera3D lc{};
    lc.target = V(target_.x, 0, target_.y);
    lc.position = Vector3Add(lc.target, Vector3Scale(sunDir, 150.0f));
    lc.up = {0, 1, 0};
    lc.projection = CAMERA_ORTHOGRAPHIC;
    lc.fovy = zoom_ * 2.4f + 20.0f;

    BeginTextureMode(shadowMap_);
    rlClearColor(255, 255, 255, 255);
    rlClearScreenBuffers();
    rlDisableBackfaceCulling();
    BeginMode3D(lc);
    Matrix view = rlGetMatrixModelview();
    Matrix proj = rlGetMatrixProjection();
    drawScene(game, buildingIds, time, true);
    EndMode3D();
    rlEnableBackfaceCulling();
    EndTextureMode();
    lightVP_ = MatrixMultiply(view, proj);
}

void Renderer::draw(Game& game, float time, float dt, const HoverInfo& hover) {
    World& world = game.world();
    playerPos_ = game.player().pos;
    currentBuilding_ = world.tile((int)std::floor(playerPos_.x), (int)std::floor(playerPos_.y)).building;
    updatePlayerRig(game, time, dt);

    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    Vec2 corners[4] = {pickGround({0, 0}), pickGround({sw, 0}), pickGround({0, sh}), pickGround({sw, sh})};
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (auto& c : corners) { minx = std::min(minx, c.x); maxx = std::max(maxx, c.x); miny = std::min(miny, c.y); maxy = std::max(maxy, c.y); }
    minx -= 24; miny -= 24; maxx += 24; maxy += 28;
    int cx0 = floorDiv((int)minx, kChunkSize), cx1 = floorDiv((int)maxx, kChunkSize);
    int cy0 = floorDiv((int)miny, kChunkSize), cy1 = floorDiv((int)maxy, kChunkSize);
    cx1 = std::min(cx1, cx0 + 14);
    cy1 = std::min(cy1, cy0 + 14);

    struct Need { int cx, cy; float d; };
    std::vector<Need> needs;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) {
            auto it = chunks_.find(tileKey(cx, cy));
            Chunk* c = world.chunkIfLoaded(cx, cy);
            if (it == chunks_.end() || !c || it->second.version != c->version) {
                float d = distance(Vec2{(cx + 0.5f) * kChunkSize, (cy + 0.5f) * kChunkSize}, playerPos_);
                needs.push_back({cx, cy, d});
            }
        }
    std::sort(needs.begin(), needs.end(), [](const Need& a, const Need& b) { return a.d < b.d; });
    double budgetStart = GetTime();
    for (const auto& n : needs) {
        if (GetTime() - budgetStart > 0.014) break;
        Chunk& c = world.chunk(n.cx, n.cy);
        buildChunk(world, c, chunks_[tileKey(n.cx, n.cy)]);
    }
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        int cx = (int)(it->first >> 32), cy = (int)(uint32_t)it->first;
        if (cx < cx0 - 2 || cx > cx1 + 2 || cy < cy0 - 2 || cy > cy1 + 2) { unloadChunk(it->second); it = chunks_.erase(it); }
        else ++it;
    }

    std::vector<int> buildingIds;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx)
            if (Chunk* c = world.chunkIfLoaded(cx, cy))
                for (int id : c->buildings) buildingIds.push_back(id);
    std::sort(buildingIds.begin(), buildingIds.end());
    buildingIds.erase(std::unique(buildingIds.begin(), buildingIds.end()), buildingIds.end());
    int built = 0;
    for (int id : buildingIds) {
        if (buildings_.count(id) || built >= 8) continue;
        const Building* b = world.building(id);
        if (!b) continue;
        BuildingGfx g;
        buildBuilding(world, *b, g);
        buildings_.emplace(id, std::move(g));
        ++built;
    }
    if (buildings_.size() > 900) {
        for (auto it = buildings_.begin(); it != buildings_.end();) {
            if (!std::binary_search(buildingIds.begin(), buildingIds.end(), it->first)) {
                if (it->second.hasWalls) UnloadMesh(it->second.walls);
                if (it->second.hasWallsLow) UnloadMesh(it->second.wallsLow);
                if (it->second.hasRoof) UnloadMesh(it->second.roof);
                it = buildings_.erase(it);
            } else ++it;
        }
    }

    // Lights, nearest to the view centre first
    std::vector<LightSrc> lights;
    float day = world.clock().daylight();
    bool night = day < 0.6f;
    for (const auto& f : world.campfires()) {
        if (!f.lit) continue;
        float flicker = 0.85f + 0.15f * std::sin(time * 13.0f + f.id) * std::sin(time * 7.3f);
        lights.push_back({V(f.pos.x, 0.8f, f.pos.y), 12.0f * flicker, V(1.0f, 0.52f, 0.2f), 1.7f});
    }
    if (world.powerOn() && night) {
        for (auto& [key, g] : chunks_)
            for (const auto& l : g.lamps) lights.push_back({l, 14.0f, V(1.0f, 0.62f, 0.3f), 1.25f});
        for (int id : buildingIds) {
            if (!world.buildingLightsOn(id)) continue;
            auto it = buildings_.find(id);
            if (it == buildings_.end()) continue;
            for (const auto& l : it->second.lights) lights.push_back(l);
        }
    }
    const Player& p = game.player();
    if (p.flashlightOn) lights.push_back({V(p.pos.x + std::cos(p.facing) * 0.6f, 1.2f, p.pos.y + std::sin(p.facing) * 0.6f), 3.0f, V(1, 0.95f, 0.85f), 0.6f});
    Vector3 ctr = V(target_.x, 0, target_.y);
    std::sort(lights.begin(), lights.end(), [&](const LightSrc& a, const LightSrc& b) {
        return Vector3DistanceSqr(a.pos, ctr) < Vector3DistanceSqr(b.pos, ctr);
    });
    if (1.0f - day > 0.3f)
        lights.insert(lights.begin(), LightSrc{V(p.pos.x, 2.0f, p.pos.y), 12.0f, V(0.32f, 0.38f, 0.55f), (1.0f - day) * 0.5f});

    renderShadows(game, buildingIds, time);
    setLighting(game, time, lights);
    ClearBackground(skyColor_);
    BeginMode3D(cam_);
    drawScene(game, buildingIds, time, false);
    rlDisableBackfaceCulling();
    drawCursor(game, hover, time);
    rlEnableBackfaceCulling();
    drawWeather(game, time);
    EndMode3D();
}

// ---------------------------------------------------------------------------
// Showcase: the character alone on a small stage
// ---------------------------------------------------------------------------

void Renderer::drawShowcase(const AnimInput& anim, float facing, float dt, const Player& look, const ItemDef* weapon) {
    static Mesh stage{};
    static bool stageReady = false;
    if (!stageReady) {
        MeshBuilder mb;
        mb.material(MGrass);
        const int n = 48;
        for (int i = 0; i < n; ++i) {
            float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
            mb.tri(V(0, 0, 0), V(std::cos(a1) * 3, 0, std::sin(a1) * 3), V(std::cos(a0) * 3, 0, std::sin(a0) * 3), hexColor(0x6a7a40));
        }
        stage = mb.build();
        stageReady = true;
    }
    playerRig_.update(anim, dt);
    playerRig_.place(V(0, 0, 0), facing);

    Vector3 sunDir = Vector3Normalize(V(-0.5f, 0.8f, 0.4f));
    Vector3 sunCol = V(0.9f, 0.82f, 0.7f), amb = V(0.38f, 0.4f, 0.46f), sky = V(0.55f, 0.62f, 0.7f);
    SetShaderValue(shader_, locSunDir_, &sunDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locSunColor_, &sunCol, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locAmbient_, &amb, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, locSky_, &sky, SHADER_UNIFORM_VEC3);
    float zero = 0, far = 1000;
    int nl = 0;
    for (int l : {locCutaway_, locSpotOn_, locSnow_, locWet_, locVision_, locShadowOn_, locUnlit_})
        SetShaderValue(shader_, l, &zero, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locLightCount_, &nl, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, locFogStart_, &far, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locFogEnd_, &far, SHADER_UNIFORM_FLOAT);
    Vector3 pp = V(0, 1, 0);
    SetShaderValue(shader_, locPlayer_, &pp, SHADER_UNIFORM_VEC3);
    setEmissive(V(0, 0, 0));

    Camera3D c{};
    c.target = V(0, 0.85f, 0);
    c.position = V(5.5f, 4.0f, 5.5f);
    c.up = {0, 1, 0};
    c.fovy = 3.0f;
    c.projection = CAMERA_ORTHOGRAPHIC;
    BeginMode3D(c);
    active_ = &mat_;
    mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    DrawMesh(stage, mat_, MatrixIdentity());
    drawCharacter(playerRig_, look, weapon);
    EndMode3D();
}

} // namespace alone
