#include "client/Renderer.h"
#include "client/RenderCommon.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Rng.h"
#include "client/Ui.h"
#include "raymath.h"
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#endif
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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
#ifdef __EMSCRIPTEN__
        // WebGL 2 wants a sized depth format
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        t.depth.id = tex;
#else
        t.depth.id = rlLoadTextureDepth(size, size, false);
#endif
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
    mb.noAo();   // moved about by a matrix: its own heights mean nothing
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

// The shaders are written for desktop OpenGL 3.3. WebGL 2 (OpenGL ES 3.0)
// takes the same code with a different header and explicit precision.
// `defines` go right after the #version line (e.g. "#define CUTAWAY\n").
static Shader loadShaderPair(const std::string& vsPath, const std::string& fsPath, const char* defines = "") {
    auto prepare = [&](const std::string& path) {
        char* text = LoadFileText(path.c_str());
        if (!text) return std::string();
        std::string src = text;
        UnloadFileText(text);
        size_t v = src.find("#version 330");
        if (v == std::string::npos) return src;
#ifdef __EMSCRIPTEN__
        src.replace(v, 12, std::string("#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n") + defines);
#else
        src.insert(v + 12, std::string("\n") + defines);
#endif
        return src;
    };
    std::string vs = prepare(vsPath), fs = prepare(fsPath);
    return LoadShaderFromMemory(vs.c_str(), fs.c_str());
}

namespace {
const char* const kUniformNames[] = {
    "sunDir", "sunColor", "ambientColor", "skyColor", "playerPos", "viewDir", "cutRadius", "cutaway", "lights", "lightColors",
    "lightCount", "time", "fogColor", "fogStart", "fogEnd", "unlit", "spotPos", "spotDir", "spotOn", "snow", "wet", "emissive",
    "lampGlow", "visionCone", "facing", "lightVP", "shadowMap", "shadowOn", "shadowTexel", "brightness", "wallCut",
    "sheltered", "rainNow", "wallFloor"};
static_assert(sizeof(kUniformNames) / sizeof(kUniformNames[0]) == 34, "one name per Renderer::Uni");
}

// Uniforms go to both world programs, so either can draw at any time
void Renderer::uni(Uni u, const void* value, int type, int count) {
    for (int k = 0; k < 2; ++k)
        if (prog_[k].id) SetShaderValueV(prog_[k], loc_[k][u], value, type, count);
}

void Renderer::uniMatrix(Uni u, const Matrix& m) {
    for (int k = 0; k < 2; ++k)
        if (prog_[k].id) SetShaderValueMatrix(prog_[k], loc_[k][u], m);
}

#ifndef __EMSCRIPTEN__
extern "C" const unsigned char* glGetString(unsigned int name);
#endif

bool Renderer::init() {
    {
        const char* gpu = (const char*)glGetString(0x1F01);   // GL_RENDERER
        Log::info("GPU: %s", gpu ? gpu : "unknown");
        if (gpu && (std::strstr(gpu, "llvmpipe") || std::strstr(gpu, "softpipe") || std::strstr(gpu, "SwiftShader")))
            Log::warn("Drawing in software (no GPU driver in use): the game will be slow");
    }
    // Two builds of the world shader: the plain one, and one that can cut a
    // see-through hole round the player. A shader that may discard pixels
    // turns off the GPU's early depth test, so only the few meshes between
    // the player and the camera use the second one.
    prog_[0] = loadShaderPair(Paths::data("shaders/world.vs"), Paths::data("shaders/world.fs"));
    prog_[1] = loadShaderPair(Paths::data("shaders/world.vs"), Paths::data("shaders/world.fs"), "#define CUTAWAY\n");
    shader_ = prog_[0];
    if (shader_.id == 0 || shader_.id == rlGetShaderIdDefault()) { Log::error("Failed to load world shader"); return false; }
    if (prog_[1].id == 0 || prog_[1].id == rlGetShaderIdDefault()) { Log::warn("No cutaway shader: using the plain one"); prog_[1] = Shader{}; }
    depthShader_ = loadShaderPair(Paths::data("shaders/depth.vs"), Paths::data("shaders/depth.fs"));
    silhouetteShader_ = loadShaderPair(Paths::data("shaders/depth.vs"), Paths::data("shaders/silhouette.fs"));
    for (int k = 0; k < 2; ++k) {
        if (!prog_[k].id) continue;
        prog_[k].locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(prog_[k], "matModel");
        for (int u = 0; u < UCount; ++u) loc_[k][u] = GetShaderLocation(prog_[k], kUniformNames[u]);
    }
    mat_ = LoadMaterialDefault();
    mat_.shader = shader_;
    matCut_ = LoadMaterialDefault();
    matCut_.shader = prog_[1].id ? prog_[1] : shader_;
    depthMat_ = LoadMaterialDefault();
    depthMat_.shader = depthShader_;
    silhouetteMat_ = LoadMaterialDefault();
    silhouetteMat_.shader = silhouetteShader_;
    active_ = &mat_;
    cube_ = unitMesh(0);
    sphere_ = unitMesh(1);
    cylinder_ = unitMesh(2);
    cone_ = unitMesh(3);
    playerModel_.init();
    reloadShadowMap();
    setOptions(opts_);
    cam_.up = {0, 1, 0};
    cam_.projection = CAMERA_ORTHOGRAPHIC;
    cam_.fovy = zoom_;
    return true;
}

void Renderer::reloadShadowMap() {
    for (RenderTexture2D* t : {&shadowFront_, &shadowBack_, &shadowMap_}) {
        if (!t->id) continue;
        rlUnloadTexture(t->depth.id);
        rlUnloadFramebuffer(t->id);
        *t = RenderTexture2D{};
    }
    shadowsReady_ = false;
    shadowFrontView_.valid = false;
    shadowBuild_.active = false;
    if (opts_.shadows <= 0) return;
    shadowSize_ = opts_.shadows == 1 ? 1024 : opts_.shadows == 2 ? 2048 : 4096;
    shadowFront_ = loadShadowMap(shadowSize_);
    shadowBack_ = loadShadowMap(shadowSize_);
    shadowMap_ = loadShadowMap(shadowSize_);
    shadowsReady_ = shadowFront_.id > 0 && shadowBack_.id > 0 && shadowMap_.id > 0 && depthShader_.id != 0;
}

void Renderer::addPart(std::vector<Part>& parts, const MeshBuilder& mb) {
    if (mb.empty()) return;
    parts.push_back({mb.build(), mb.boundsMin(), mb.boundsMax()});
}

void Renderer::unloadParts(std::vector<Part>& parts) {
    for (auto& p : parts) UnloadMesh(p.mesh);
    parts.clear();
}

void Renderer::updateRenderScale(float dt) {
    if (opts_.renderScale > 0) { scale_ = std::clamp(opts_.renderScale / 100.0f, 0.25f, 1.0f); return; }
    dt = std::clamp(dt, 0.0f, 0.25f);
    frameAvg_ += (dt - frameAvg_) * 0.1f;
    scaleTimer_ += dt;
    scaleHold_ -= dt;
    if (scaleTimer_ < 0.5f) return;
    scaleTimer_ = 0;
    if (frameAvg_ > 1.0f / 40.0f && scale_ > 0.5f) {          // under 40 fps: draw smaller
        scale_ = std::max(0.5f, scale_ - 0.1f);
        scaleHold_ = 6.0f;
    } else if (frameAvg_ < 1.0f / 54.0f && scaleHold_ <= 0 && scale_ < 1.0f) {   // room to spare: sharper
        scale_ = std::min(1.0f, scale_ + 0.05f);
        scaleHold_ = 2.0f;
    }
}

void Renderer::setOptions(const GraphicsOptions& o) {
    bool shadowChange = o.shadows != opts_.shadows || (o.shadows > 0 && !shadowsReady_);
    opts_ = o;
    opts_.maxLights = std::clamp(opts_.maxLights, 1, 32);
    opts_.brightness = std::clamp(opts_.brightness, 0.5f, 1.6f);
    if (shader_.id && shadowChange) reloadShadowMap();
    if (shader_.id) uni(UBrightness, &opts_.brightness, SHADER_UNIFORM_FLOAT);
}

void Renderer::unloadChunk(ChunkGfx& g) {
    if (g.hasGround) UnloadMesh(g.ground);
    g.hasGround = false;
    unloadParts(g.props);
    unloadParts(g.detail);
}

void Renderer::unloadBuilding(BuildingGfx& b) {
    if (b.hasWalls) UnloadMesh(b.walls);
    if (b.hasWallsLow) UnloadMesh(b.wallsLow);
    if (b.hasRoof) UnloadMesh(b.roof);
    unloadParts(b.interior);
    b.hasWalls = b.hasWallsLow = b.hasRoof = false;
}

void Renderer::invalidateAll() {
    for (auto& [k, g] : chunks_) unloadChunk(g);
    chunks_.clear();
    for (auto& [k, b] : buildings_) unloadBuilding(b);
    buildings_.clear();
    rigInit_ = false;
    world_ = nullptr;          // a new game has a new world (set again when drawn)
    shadowFrontView_.valid = false;
    shadowBuild_.active = false;
    shadowDirty_ = true;
    snapY_ = true;
}

void Renderer::shutdown() {
    invalidateAll();
    UnloadMesh(cube_);
    UnloadMesh(sphere_);
    UnloadMesh(cylinder_);
    UnloadMesh(cone_);
    playerModel_.shutdown();
    for (auto& [k, m] : vehicleMeshes_) UnloadMesh(m);
    vehicleMeshes_.clear();
    for (auto& [k, g] : npcs_) if (g.model) g.model->shutdown();
    npcs_.clear();
    for (auto& [k, t] : signTex_) UnloadTexture(t.tex);
    signTex_.clear();
    for (RenderTexture2D* t : {&shadowFront_, &shadowBack_, &shadowMap_})
        if (t->id) {
            rlUnloadTexture(t->depth.id);
            rlUnloadFramebuffer(t->id);
        }
    if (sceneRT_.id) UnloadRenderTexture(sceneRT_);
    UnloadShader(depthShader_);
    UnloadShader(silhouetteShader_);
    if (prog_[1].id) UnloadShader(prog_[1]);
    UnloadShader(shader_);
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void Renderer::updateCamera() {
    float d = 120.0f;
    Vector3 t{target_.x, targetY_, target_.y};
    cam_.target = t;
    cam_.position = {t.x + std::cos(pitch_) * std::sin(yaw_) * d, t.y + std::sin(pitch_) * d,
                     t.z + std::cos(pitch_) * std::cos(yaw_) * d};
    cam_.fovy = zoom_;
}

void Renderer::faceTowards(Vec2 dir) {
    // the camera sits on the `dir` side, at one of the four diagonal angles
    float best = yaw_, bestDot = -2;
    for (int k = 0; k < 4; ++k) {
        float y = 0.785f + k * PI * 0.5f;
        float d = std::sin(y) * dir.x + std::cos(y) * dir.y;
        if (d > bestDot) { bestDot = d; best = y; }
    }
    yaw_ = yawTarget_ = best;
    updateCamera();
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
    snapY_ = true;
    if (world_) targetY_ = world_->standHeight(target);
    updateCamera();
}

void Renderer::rotate(int steps) { yawTarget_ += steps * PI * 0.5f; }

void Renderer::zoom(float f) { zoom_ = std::clamp(zoom_ * f, 10.0f, 90.0f); }

Vec2 Renderer::pickGround(Vector2 mouse) {
    Ray r = GetScreenToWorldRay(mouse, cam_);
    if (std::fabs(r.direction.y) < 1e-5f) return target_;
    // Where the ray meets the ground: guess its height, see where the ray is
    // at that height, look at the ground there, and again
    float h = targetY_;
    Vec2 p = target_;
    for (int i = 0; i < 6; ++i) {
        float t = (h - r.position.y) / r.direction.y;
        Vec2 q{r.position.x + r.direction.x * t, r.position.z + r.direction.z * t};
        if (i > 0 && distance(p, q) < 0.02f) { p = q; break; }
        p = q;
        if (!world_) break;
        h = world_->standHeight(p);
    }
    return p;
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
            float gy = world.standHeight(Vec2{x + 0.5f, y + 0.5f});
            BoundingBox bb{{x + inset, gy, y + inset}, {x + 1 - inset, gy + h, y + 1 - inset}};
            RayCollision c = GetRayCollisionBox(r, bb);
            if (c.hit && c.distance < best) { best = c.distance; out = Vec2{x + 0.5f, y + 0.5f}; }
        }
    return out;
}

Vector2 Renderer::toScreen(Vec2 p, float h) const {
    float gy = world_ ? world_->standHeight(p) : 0.0f;
    return GetWorldToScreen(Vector3{p.x, gy + h, p.y}, cam_);
}

Vec2 Renderer::screenToWorldDir(float sx, float sy) const {
    Vec2 fwd{-std::sin(yaw_), -std::cos(yaw_)};
    Vec2 right{std::cos(yaw_), -std::sin(yaw_)};
    return right * sx + fwd * (-sy);
}

// ---------------------------------------------------------------------------
// Uniform helpers
// ---------------------------------------------------------------------------

void Renderer::setUnlit(bool on) {
    float v = on ? 1.0f : 0.0f;
    uni(UUnlit, &v, SHADER_UNIFORM_FLOAT);
}

void Renderer::setEmissive(Vector3 e) {
    if (e.x == emissive_.x && e.y == emissive_.y && e.z == emissive_.z) return;
    emissive_ = e;
    uni(UEmissive, &e, SHADER_UNIFORM_VEC3);
}

void Renderer::setSheltered(bool on) {
    float v = on ? 1.0f : 0.0f;
    if (v == sheltered_) return;
    sheltered_ = v;
    uni(USheltered, &v, SHADER_UNIFORM_FLOAT);
}

void Renderer::setWallCut(int mode) {
    float v = (float)mode;
    uni(UWallCut, &v, SHADER_UNIFORM_FLOAT);
}

// The same test world.vs makes: is this wall piece cut down?
bool Renderer::wallCutAt(Vec2 centre, int mode) const {
    if (mode <= 0) return false;
    Vector3 v = Vector3Subtract(cam_.target, cam_.position);
    Vec2 toCam = Vec2{-v.x, -v.z}.normalized();
    Vec2 rel = centre - playerPos_;
    float ahead = dot(rel, toCam), aside = std::fabs(cross(rel, toCam));
    return ahead > 0.05f && (mode == 1 || (aside < 4.5f && ahead < 30.0f));
}

// ---------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------

static constexpr float kShadowMargin = 12.0f;   // metres the cached shadow map reaches past the view

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
    uni(USunDir, &sunDir, SHADER_UNIFORM_VEC3);
    uni(USunColor, &sunCol, SHADER_UNIFORM_VEC3);
    uni(UAmbient, &amb, SHADER_UNIFORM_VEC3);
    uni(USky, &sky, SHADER_UNIFORM_VEC3);

    const Player& p = game.player();
    Vector3 pp = V(p.pos.x, playerY_ + 1.0f, p.pos.y);
    uni(UPlayer, &pp, SHADER_UNIFORM_VEC3);
    Vector3 view = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    uni(UViewDir, &view, SHADER_UNIFORM_VEC3);
    float cut = 3.2f, on = 1.0f;
    uni(UCutRadius, &cut, SHADER_UNIFORM_FLOAT);
    uni(UCutaway, &on, SHADER_UNIFORM_FLOAT);
    setWallCut(0);
    uni(UTime, &time, SHADER_UNIFORM_FLOAT);
    float snow = w.snowCover(), wet = w.groundWetness();
    uni(USnow, &snow, SHADER_UNIFORM_FLOAT);
    uni(UWet, &wet, SHADER_UNIFORM_FLOAT);
    float rainNow = wx.kind == WeatherKind::Snow ? 0.0f : wx.rain;
    uni(URainNow, &rainNow, SHADER_UNIFORM_FLOAT);
    setSheltered(false);
    float vision = game.dead() || p.asleep || !opts_.visionCone ? 0.0f : 1.0f;
    uni(UVision, &vision, SHADER_UNIFORM_FLOAT);
    Vector2 facing{std::cos(p.facing), std::sin(p.facing)};
    uni(UFacing, &facing, SHADER_UNIFORM_VEC2);

    float spotOn = p.flashlightOn ? 1.0f : 0.0f;
    Vector3 spotPos = V(p.pos.x, playerY_ + 1.3f, p.pos.y);
    Vector3 spotDir = Vector3Normalize(V(std::cos(p.facing), -0.35f, std::sin(p.facing)));
    uni(USpotOn, &spotOn, SHADER_UNIFORM_FLOAT);
    uni(USpotPos, &spotPos, SHADER_UNIFORM_VEC3);
    uni(USpotDir, &spotDir, SHADER_UNIFORM_VEC3);

    Vector4 lights[32];
    Vector4 cols[32];
    int n = 0;
    for (const auto& l : lightList) {
        if (n >= opts_.maxLights) break;
        float inten = l.intensity;
        if (l.colour.z > l.colour.x + 0.2f) inten *= 0.6f + 0.4f * std::fabs(std::sin(time * 7.0f + l.pos.x) * std::sin(time * 3.1f));
        lights[n] = {l.pos.x, l.pos.y, l.pos.z, l.radius};
        cols[n] = {l.colour.x, l.colour.y, l.colour.z, inten};
        ++n;
    }
    if (n == 0) { lights[0] = {0, 0, 0, 1}; cols[0] = {0, 0, 0, 0}; }
    uni(ULights, lights, SHADER_UNIFORM_VEC4, std::max(1, n));
    uni(ULightColors, cols, SHADER_UNIFORM_VEC4, std::max(1, n));
    uni(ULightCount, &n, SHADER_UNIFORM_INT);

    Color daySky = mixColor(hexColor(0x9fb4c4), hexColor(0x8a9096), wx.cloud);
    skyColor_ = mixColor(hexColor(0x0a0d14), daySky, day);
    Vector3 fogC = V(skyColor_.r / 255.0f, skyColor_.g / 255.0f, skyColor_.b / 255.0f);
    float fogStart = 70.0f - 40.0f * wx.rain - 55.0f * wx.fog;
    float fogEnd = 140.0f - 70.0f * wx.rain - 105.0f * wx.fog;
    uni(UFogColor, &fogC, SHADER_UNIFORM_VEC3);
    uni(UFogStart, &fogStart, SHADER_UNIFORM_FLOAT);
    uni(UFogEnd, &fogEnd, SHADER_UNIFORM_FLOAT);

    power_ = w.powerOn();
    float glow = power_ ? std::clamp((dark_ - 0.3f) * 2.0f, 0.0f, 1.0f) : 0.0f;
    lampGlow_ = Vector3Scale(V(1.0f, 0.75f, 0.42f), glow * 1.4f);
    windowGlow_ = Vector3Scale(V(0.85f, 0.62f, 0.32f), glow);
    uni(ULampGlow, &lampGlow_, SHADER_UNIFORM_VEC3);
    setEmissive(V(0, 0, 0));

    float sOn = (shadowsReady_ && (day > 0.05f || moon > 0.3f)) ? 1.0f : 0.0f;
    float texel = 1.0f / shadowSize_;
    uni(UShadowOn, &sOn, SHADER_UNIFORM_FLOAT);
    uni(UShadowTexel, &texel, SHADER_UNIFORM_FLOAT);
    uniMatrix(ULightVP, lightVP_);
    if (shadowsReady_) {
        int slot = 10;
        rlActiveTextureSlot(slot);
        rlEnableTexture(shadowMap_.depth.id);
        rlActiveTextureSlot(0);
        uni(UShadowMap, &slot, SHADER_UNIFORM_INT);
    }
    setUnlit(false);
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

// An orthographic camera sees a box: test the corners of an axis-aligned box
// against its sides. depth = distance of the nearest corner along the view.
bool Renderer::inView(const Camera3D& c, float aspect, Vector3 mn, Vector3 mx, float* depth) {
    Vector3 f = Vector3Normalize(Vector3Subtract(c.target, c.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, c.up));
    Vector3 u = Vector3CrossProduct(r, f);
    float hh = c.fovy * 0.5f, hw = hh * aspect;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, d0 = 1e9f;
    for (int i = 0; i < 8; ++i) {
        Vector3 p = V(i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z);
        Vector3 d = Vector3Subtract(p, c.position);
        float x = Vector3DotProduct(d, r), y = Vector3DotProduct(d, u);
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        d0 = std::min(d0, Vector3DotProduct(d, f));
    }
    if (depth) *depth = d0;
    return x1 >= -hw && x0 <= hw && y1 >= -hh && y0 <= hh;
}

uint64_t Renderer::buildingStamp(World& world, const Building& b) {
    uint64_t st = 1469598103934665603ull;
    for (int cy = floorDiv(b.rect.y, kChunkSize); cy <= floorDiv(b.rect.bottom() - 1, kChunkSize); ++cy)
        for (int cx = floorDiv(b.rect.x, kChunkSize); cx <= floorDiv(b.rect.right() - 1, kChunkSize); ++cx) {
            Chunk* c = world.chunkIfLoaded(cx, cy);
            st = (st ^ (c ? c->version : 0u)) * 1099511628211ull;
        }
    return st;
}

bool Renderer::occludes(World& world, int id) {
    const Building* b = world.building(id);
    if (!b) return false;
    Vector3 toCam = Vector3Subtract(cam_.position, cam_.target);
    Vec2 c = Vec2{toCam.x, toCam.z}.normalized();
    float rise = std::tan(pitch_);
    float height = b->floors * 2.8f + (b->roof == RoofKind::Flat ? 0.4f : 2.5f);
    if (b->type == BuildingType::Church) height += 12.0f;
    IRect r = b->rect.expanded(1);
    for (float t = 1.0f; t < 45.0f; t += 1.5f) {
        if (playerY_ + 1.6f + t * rise > b->floorY + height) break;   // the line of sight is above the roof from here on
        Vec2 q = playerPos_ + c * t;
        if (r.contains((int)std::floor(q.x), (int)std::floor(q.y))) return true;
    }
    return false;
}

bool Renderer::nearSightLine(Vector3 mn, Vector3 mx) const {
    // The see-through hole round the player reaches as far toward the camera
    // as the tallest tree could stand in the way
    Vector3 cv = Vector3Subtract(cam_.position, cam_.target);
    Vec2 toCam = Vec2{cv.x, cv.z}.normalized();
    for (float t = 0; t <= 30.0f; t += 2.0f) {
        Vec2 q = playerPos_ + toCam * t;
        float dx = std::max({mn.x - q.x, 0.0f, q.x - mx.x}), dy = std::max({mn.z - q.y, 0.0f, q.y - mx.z});
        if (dx * dx + dy * dy < 5.0f * 5.0f) return true;
    }
    return false;
}

void Renderer::drawStatic(Game& game, const std::vector<DrawItem>& chunks, const std::vector<DrawItem>& blds, bool shadowPass) {
    World& world = game.world();
    Matrix identity = MatrixIdentity();
    Material& m = shadowPass ? depthMat_ : mat_;
    active_ = &m;
    m.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    matCut_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    // Trees and props cell by cell, nearest first so the GPU can skip the
    // pixels of whatever is hidden behind them. Only cells near the line from
    // you to the camera use the shader with the see-through hole.
    Vector3 fwd = Vector3Normalize(Vector3Subtract(passCam_.target, passCam_.position));
    std::vector<std::pair<float, const Part*>> order;
    for (const auto& d : chunks) {
        for (const Part& p : d.chunk->props)
            if (partVisible(p)) {
                Vector3 c = Vector3Scale(Vector3Add(p.mn, p.mx), 0.5f);
                order.push_back({Vector3DotProduct(Vector3Subtract(c, passCam_.position), fwd), &p});
            }
        (shadowPass ? prof_.sunChunks : prof_.chunks) += 1;
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [depth, p] : order) {
        DrawMesh(p->mesh, !shadowPass && nearSightLine(p->mn, p->mx) ? matCut_ : m, identity);
        (shadowPass ? prof_.sunVerts : prof_.verts) += p->mesh.vertexCount;
    }
    if (!shadowPass)   // small plants: too small to cast a shadow you'd notice
        for (const auto& d : chunks)
            for (const Part& p : d.chunk->detail)
                if (partVisible(p)) {
                    DrawMesh(p.mesh, m, identity);
                    prof_.verts += p.mesh.vertexCount;
                }
    std::vector<int> ids;
    if (!shadowPass) cutBuildings_.clear();
    for (const auto& d : blds) {
        int id = d.building;
        ids.push_back(id);
        auto it = buildings_.find(id);
        if (it == buildings_.end()) continue;
        if (!shadowPass) setEmissive(power_ && world.buildingLightsOn(id) ? windowGlow_ : V(0, 0, 0));
        // Inside it, or it stands between you and the camera: only the ground
        // floor, no roof, and the walls in front of you cut down to a low
        // stub on the GPU (like Project Zomboid). Inside, every wall in front
        // of you; from outside, only those near your line of sight.
        int cut = shadowPass ? 0 : id == currentBuilding_ ? 1 : occludes(world, id) ? 2 : 0;
        const BuildingGfx& bg = it->second;
        double bv = 0;
        if (!shadowPass) setSheltered(true);   // walls and furniture: no snow on them
        if (cut && bg.hasWallsLow) {
            cutBuildings_.push_back({id, cut});
            if (const Building* b = world.building(id)) uni(UWallFloor, &b->floorY, SHADER_UNIFORM_FLOAT);
            setWallCut(cut);
            DrawMesh(bg.wallsLow, m, identity);
            setWallCut(0);
            bv += bg.wallsLow.vertexCount;
        } else if (bg.hasWalls) {
            cut = 0;
            DrawMesh(bg.walls, m, identity);
            bv += bg.walls.vertexCount;
        }
        // The furniture is only drawn when you can see in, and only the cells in view
        if (cut)
            for (const Part& p : bg.interior)
                if (partVisible(p)) { DrawMesh(p.mesh, m, identity); bv += p.mesh.vertexCount; }
        if (!shadowPass) setSheltered(false);
        if (bg.hasRoof && !cut) { DrawMesh(bg.roof, m, identity); bv += bg.roof.vertexCount; }
        (shadowPass ? prof_.sunBuildingVerts : prof_.buildingVerts) += bv;
    }
    if (!shadowPass) setEmissive(V(0, 0, 0));
    drawDoors(world, ids, !shadowPass);
    active_ = &mat_;
}

void Renderer::drawScene(Game& game, float time) {
    passCam_ = cam_;
    passAspect_ = aspect_;
    drawStatic(game, viewChunks_, viewBuildings_, false);
    // Where something still hides you, a faint outline of you shows through
    drawSilhouette();
    active_ = &mat_;
    drawEntities(game, time, false);
    // The ground last: whatever stands on it has already covered part of it
    Matrix identity = MatrixIdentity();
    for (const auto& d : viewChunks_)
        if (d.chunk->hasGround) {
            DrawMesh(d.chunk->ground, mat_, identity);
            prof_.verts += d.chunk->ground.vertexCount;
        }
}

#ifndef __EMSCRIPTEN__
extern "C" void glDepthFunc(unsigned int func);
#endif

void Renderer::drawSilhouette() {
    if (!playerModel_.ready() || !silhouetteShader_.id) return;
    rlDrawRenderBatchActive();
    glDepthFunc(0x0204);   // GL_GREATER: only where something nearer is already drawn
    rlDisableDepthMask();
    silhouetteMat_.maps[MATERIAL_MAP_DIFFUSE].color = Color{150, 190, 230, 120};
    DrawMesh(playerModel_.mesh(), silhouetteMat_, MatrixIdentity());
    rlEnableDepthMask();
    glDepthFunc(0x0203);   // GL_LEQUAL, raylib's default
}

Camera3D Renderer::lightCamera(Vec2 centre, Vector3 sunDir) const {
    Camera3D lc{};
    lc.target = V(centre.x, targetY_, centre.y);
    lc.position = Vector3Add(lc.target, Vector3Scale(sunDir, 150.0f));
    lc.up = {0, 1, 0};
    lc.projection = CAMERA_ORTHOGRAPHIC;
    // what the view shows, plus a margin so you can walk a while before it's redrawn
    lc.fovy = zoom_ * 2.4f + 20.0f + 2.0f * kShadowMargin;
    return lc;
}

// One step of drawing the cached map: a share of the trees and buildings
void Renderer::shadowStep(Game& game) {
    ShadowBuild& sb = shadowBuild_;
    std::vector<DrawItem> chunks, blds;
    size_t c0 = sb.chunks.size() * sb.step / sb.steps, c1 = sb.chunks.size() * (sb.step + 1) / sb.steps;
    size_t b0 = sb.buildings.size() * sb.step / sb.steps, b1 = sb.buildings.size() * (sb.step + 1) / sb.steps;
    for (size_t i = c0; i < c1; ++i) {
        auto it = chunks_.find(sb.chunks[i]);
        if (it != chunks_.end()) chunks.push_back({&it->second, -1, 0});
    }
    for (size_t i = b0; i < b1; ++i) blds.push_back({nullptr, sb.buildings[i], 0});
    BeginTextureMode(shadowBack_);
    if (sb.step == 0) {
        rlClearColor(255, 255, 255, 255);
        rlClearScreenBuffers();
    }
    rlDisableBackfaceCulling();
    BeginMode3D(sb.view.cam);   // (inside the square map, so the projection is square)
    sb.view.vp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    passCam_ = sb.view.cam;
    passAspect_ = 1.0f;
    drawStatic(game, chunks, blds, true);
    EndMode3D();
    rlEnableBackfaceCulling();
    EndTextureMode();
    if (++sb.step >= sb.steps) {   // done: the new map takes over
        std::swap(shadowFront_, shadowBack_);
        shadowFrontView_ = sb.view;
        shadowFrontView_.valid = true;
        sb.active = false;
    }
}

void Renderer::renderShadows(Game& game, float time) {
    if (!shadowsReady_) return;
    Vector3 sunDir = sunDirection(game.world().clock());
    // Start redrawing the cached map when it no longer fits what you see
    ShadowView& fv = shadowFrontView_;
    float moved = fv.valid ? distance(fv.centre, target_) : 1e9f;
    float sunTurn = fv.valid ? std::acos(std::clamp(Vector3DotProduct(fv.sun, sunDir), -1.0f, 1.0f)) * RAD2DEG : 1e9f;
    bool stale = !fv.valid || moved > kShadowMargin * 0.5f || sunTurn > 0.3f || std::fabs(fv.zoom - zoom_) > 0.5f || shadowDirty_ ||
                 std::fabs(fv.cam.target.y - targetY_) > 4.0f;
    // So far off that the old map doesn't cover the view (or no map yet): all at once
    bool urgent = !fv.valid || moved > kShadowMargin * 0.9f || std::fabs(fv.zoom - zoom_) > 0.5f;
    if (stale && (!shadowBuild_.active || urgent)) {
        ShadowBuild& sb = shadowBuild_;
        sb = ShadowBuild{};
        sb.active = true;
        sb.steps = urgent ? 1 : 4;
        sb.view.centre = target_;
        sb.view.sun = sunDir;
        sb.view.zoom = zoom_;
        sb.view.cam = lightCamera(target_, sunDir);
        // Only what the sun can see casts into the map: nearest the centre first
        for (auto& [key, g] : chunks_)
            if (inView(sb.view.cam, 1.0f, g.mn, g.mx)) sb.chunks.push_back(key);
        for (auto& [id, b] : buildings_)
            if (inView(sb.view.cam, 1.0f, b.boxMin, b.boxMax)) sb.buildings.push_back(id);
        shadowDirty_ = false;
    }
    if (shadowBuild_.active) shadowStep(game);
    if (!fv.valid) return;

    // This frame's map: the cached one plus everything that moves
    rlBindFramebuffer(RL_READ_FRAMEBUFFER, shadowFront_.id);
    rlBindFramebuffer(RL_DRAW_FRAMEBUFFER, shadowMap_.id);
    rlBlitFramebuffer(0, 0, shadowSize_, shadowSize_, 0, 0, shadowSize_, shadowSize_, 0x00000100);   // GL_DEPTH_BUFFER_BIT
    rlBindFramebuffer(RL_READ_FRAMEBUFFER, 0);
    rlBindFramebuffer(RL_DRAW_FRAMEBUFFER, 0);
    BeginTextureMode(shadowMap_);
    rlDisableBackfaceCulling();
    BeginMode3D(fv.cam);
    passCam_ = fv.cam;
    passAspect_ = 1.0f;
    active_ = &depthMat_;
    depthMat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    drawEntities(game, time, true);
    active_ = &mat_;
    EndMode3D();
    rlEnableBackfaceCulling();
    EndTextureMode();
    lightVP_ = fv.vp;
}

// ---------------------------------------------------------------------------
// Signs: lettering rendered once into a texture, drawn as quads on facades
// ---------------------------------------------------------------------------

const Renderer::SignTex& Renderer::signTexture(const std::string& text, Color fg, Color bg, bool dot) {
    char key[64];
    std::snprintf(key, sizeof key, "|%08x|%08x|%d", (unsigned)ColorToInt(fg), (unsigned)ColorToInt(bg), dot ? 1 : 0);
    std::string k = text + key;
    auto it = signTex_.find(k);
    if (it != signTex_.end()) return it->second;
    Font& f = ui::font(true);
    const float size = 64;
    Vector2 m = MeasureTextEx(f, text.c_str(), size, 1);
    int pad = 14;
    int w = (int)m.x + pad * 2 + (dot ? (int)(size * 0.95f) : 0), h = (int)size + pad;
    RenderTexture2D rt = LoadRenderTexture(w, h);
    BeginTextureMode(rt);
    ClearBackground(bg.a ? bg : Color{fg.r, fg.g, fg.b, 0});
    DrawTextEx(f, text.c_str(), {(float)pad, pad * 0.5f}, size, 1, fg);
    if (dot) DrawCircle((int)(w - pad - size * 0.42f), (int)(h * 0.42f), size * 0.38f, Color{238, 186, 60, 255});
    EndTextureMode();
    SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
    SignTex st;
    st.tex = rt.texture;
    st.aspect = (float)w / (float)h;
    rlUnloadFramebuffer(rt.id);   // keep only the texture
    return signTex_[k] = st;
}

void Renderer::drawSigns(Game& game, const std::vector<int>& buildingIds) {
    World& world = game.world();
    float day = world.clock().daylight();
    bool power = world.powerOn();
    Vector3 camDir = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    for (int id : buildingIds) {
        auto it = buildings_.find(id);
        if (it == buildings_.end()) continue;
        for (const SignGfx& sg : it->second.signs) {
            if (sg.interior && id != currentBuilding_) continue;
            auto st = signTex_.find(sg.text + [&] {
                char key[64];
                std::snprintf(key, sizeof key, "|%08x|%08x|%d", (unsigned)ColorToInt(sg.fg), (unsigned)ColorToInt(sg.bg), sg.dot ? 1 : 0);
                return std::string(key);
            }());
            if (st == signTex_.end()) continue;
            Vector3 up = V(0, 1, 0);
            Vector3 nrm = Vector3CrossProduct(sg.right, up);   // faces the reader
            if (Vector3DotProduct(nrm, camDir) > 0.05f) continue;   // seen from behind
            float hh = sg.height, ww = hh * st->second.aspect;
            Vector3 r = Vector3Scale(sg.right, ww * 0.5f), u = Vector3Scale(up, hh * 0.5f);
            Vector3 c = Vector3Add(sg.centre, Vector3Scale(nrm, 0.01f));
            Vector3 bl = Vector3Subtract(Vector3Subtract(c, r), u), br = Vector3Subtract(Vector3Add(c, r), u);
            Vector3 tr = Vector3Add(Vector3Add(c, r), u), tl = Vector3Add(Vector3Subtract(c, r), u);
            float light = sg.lit && power && day < 0.5f ? 1.0f : std::clamp(0.25f + day, 0.25f, 1.0f);
            unsigned char l = (unsigned char)(255 * light);
            rlSetTexture(st->second.tex.id);
            rlBegin(RL_QUADS);
            rlColor4ub(l, l, l, 255);
            rlNormal3f(nrm.x, nrm.y, nrm.z);
            rlTexCoord2f(0, 0); rlVertex3f(bl.x, bl.y, bl.z);
            rlTexCoord2f(1, 0); rlVertex3f(br.x, br.y, br.z);
            rlTexCoord2f(1, 1); rlVertex3f(tr.x, tr.y, tr.z);
            rlTexCoord2f(0, 1); rlVertex3f(tl.x, tl.y, tl.z);
            rlEnd();
            rlSetTexture(0);
        }
    }
}

extern "C" void glFinish(void);

void Renderer::draw(Game& game, float time, float dt, const HoverInfo& hover) {
    auto sync = [&] { if (finishEachPhase) { rlDrawRenderBatchActive(); glFinish(); } };
    double tStart = GetTime();
    World& world = game.world();
    world_ = &world;
    playerPos_ = game.player().pos;
    currentBuilding_ = world.tile((int)std::floor(playerPos_.x), (int)std::floor(playerPos_.y)).building;
    updatePlayerRig(game, time, dt);
    updateNpcRigs(game, time, dt);
    // The camera looks at the ground the player stands on, easing over steps
    float ty = world.standHeight(target_);
    if (snapY_ || std::fabs(ty - targetY_) > 6.0f) targetY_ = ty;
    else targetY_ += (ty - targetY_) * std::min(1.0f, dt * 5.0f);
    snapY_ = false;
    updateCamera();
    double tRig = GetTime();

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
        shadowDirty_ = true;
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
        shadowDirty_ = true;
    }
    if (buildings_.size() > 900) {
        for (auto it = buildings_.begin(); it != buildings_.end();) {
            if (!std::binary_search(buildingIds.begin(), buildingIds.end(), it->first)) {
                unloadBuilding(it->second);
                it = buildings_.erase(it);
            } else ++it;
        }
    }
    // Furniture inside a building lives in the building's mesh: rebuild it
    // when one of the chunks under it changed (something built or moved)
    for (int id : buildingIds) {
        auto it = buildings_.find(id);
        const Building* b = world.building(id);
        if (it == buildings_.end() || !b || built >= 8) continue;
        if (buildingStamp(world, *b) != it->second.stamp) {
            unloadBuilding(it->second);
            BuildingGfx g;
            buildBuilding(world, *b, g);
            it->second = std::move(g);
            ++built;
            shadowDirty_ = true;
        }
    }

    // --- culling: what can the camera see? -----------------------------------------
    aspect_ = sw / std::max(1.0f, sh);
    viewChunks_.clear();
    viewBuildings_.clear();
    for (auto& [key, g] : chunks_) {
        float depth;
        if (inView(cam_, aspect_, g.mn, g.mx, &depth)) viewChunks_.push_back({&g, -1, depth});
    }
    for (int id : buildingIds) {
        auto it = buildings_.find(id);
        if (it == buildings_.end()) continue;
        float depth;
        if (inView(cam_, aspect_, it->second.boxMin, it->second.boxMax, &depth)) viewBuildings_.push_back({nullptr, id, depth});
    }
    auto nearFirst = [](const DrawItem& a, const DrawItem& b) { return a.depth < b.depth; };
    std::sort(viewChunks_.begin(), viewChunks_.end(), nearFirst);
    std::sort(viewBuildings_.begin(), viewBuildings_.end(), nearFirst);

    // Lights, nearest to the view centre first
    std::vector<LightSrc> lights;
    float day = world.clock().daylight();
    bool night = day < 0.6f;
    for (const auto& f : world.campfires()) {
        if (!f.lit) continue;
        float fy = world.standHeight(f.pos);
        if (f.stove) { lights.push_back({V(f.pos.x, fy + 0.4f, f.pos.y), 3.5f, V(0.55f, 0.65f, 1.0f), 0.8f}); continue; }
        float flicker = 0.85f + 0.15f * std::sin(time * 13.0f + f.id) * std::sin(time * 7.3f);
        lights.push_back({V(f.pos.x, fy + 0.8f, f.pos.y), 12.0f * flicker, V(1.0f, 0.52f, 0.2f), 1.7f});
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
    if (p.flashlightOn) lights.push_back({V(p.pos.x + std::cos(p.facing) * 0.6f, playerY_ + 1.2f, p.pos.y + std::sin(p.facing) * 0.6f), 3.0f, V(1, 0.95f, 0.85f), 0.6f});
    Vector3 ctr = V(target_.x, targetY_, target_.y);
    std::sort(lights.begin(), lights.end(), [&](const LightSrc& a, const LightSrc& b) {
        return Vector3DistanceSqr(a.pos, ctr) < Vector3DistanceSqr(b.pos, ctr);
    });
    if (1.0f - day > 0.3f)
        lights.insert(lights.begin(), LightSrc{V(p.pos.x, playerY_ + 2.0f, p.pos.y), 12.0f, V(0.32f, 0.38f, 0.55f), (1.0f - day) * 0.5f});

    for (int id : buildingIds) {
        auto it = buildings_.find(id);
        if (it == buildings_.end()) continue;
        for (const auto& sg : it->second.signs) signTexture(sg.text, sg.fg, sg.bg, sg.dot);
    }
    double tPrep = GetTime();
    renderShadows(game, time);
    sync();
    double tShadow = GetTime();
    setLighting(game, time, lights);
    // Drawn smaller and scaled up when the render resolution is below 100%
    updateRenderScale(GetFrameTime());
    int rw = std::max(16, (int)(sw * scale_)), rh = std::max(16, (int)(sh * scale_));
    bool scaled = scale_ < 0.99f;
    if (scaled && (sceneRT_.id == 0 || sceneRT_.texture.width != rw || sceneRT_.texture.height != rh)) {
        if (sceneRT_.id) UnloadRenderTexture(sceneRT_);
        sceneRT_ = LoadRenderTexture(rw, rh);
        SetTextureFilter(sceneRT_.texture, TEXTURE_FILTER_BILINEAR);
    }
    if (scaled && sceneRT_.id) BeginTextureMode(sceneRT_);
    else scaled = false;
    ClearBackground(skyColor_);
    BeginMode3D(cam_);
    drawScene(game, time);
    sync();
    double tScene = GetTime();
    std::vector<int> visibleIds;
    for (const auto& d : viewBuildings_) visibleIds.push_back(d.building);
    drawSigns(game, visibleIds);
    rlDisableBackfaceCulling();
    drawCursor(game, hover, time);
    rlEnableBackfaceCulling();
    if (opts_.weather) drawWeather(game, time);
    EndMode3D();
    if (scaled) {
        EndTextureMode();
        DrawTexturePro(sceneRT_.texture, Rectangle{0, 0, (float)rw, (float)-rh}, Rectangle{0, 0, sw, sh}, Vector2{0, 0}, 0, WHITE);
    }
    sync();
    double tEnd = GetTime();
    prof_.rig += (tRig - tStart) * 1000; prof_.prep += (tPrep - tRig) * 1000; prof_.shadow += (tShadow - tPrep) * 1000;
    prof_.scene += (tScene - tShadow) * 1000; prof_.rest += (tEnd - tScene) * 1000; ++prof_.frames;
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
    // the character's own body shape, skin and hair
    BodyShape shape{look.look.gender == Gender::Woman, look.look.hairStyle};
    if (!playerModel_.ready() || playerModel_.shape() != shape) {
        playerModel_.shutdown();
        playerModel_.init(shape);
    }
    CharacterLook L = lookOf(look.worn, &look.body);
    L.skin = hexColor(look.look.skin);
    L.hair = hexColor(look.look.hair);
    playerModel_.skin(playerRig_, L);

    Vector3 sunDir = Vector3Normalize(V(-0.5f, 0.8f, 0.4f));
    Vector3 sunCol = V(0.9f, 0.82f, 0.7f), amb = V(0.38f, 0.4f, 0.46f), sky = V(0.55f, 0.62f, 0.7f);
    uni(USunDir, &sunDir, SHADER_UNIFORM_VEC3);
    uni(USunColor, &sunCol, SHADER_UNIFORM_VEC3);
    uni(UAmbient, &amb, SHADER_UNIFORM_VEC3);
    uni(USky, &sky, SHADER_UNIFORM_VEC3);
    float zero = 0, far = 1000;
    int nl = 0;
    for (Uni u : {UCutaway, USpotOn, USnow, UWet, UVision, UShadowOn, UUnlit, UWallCut})
        uni(u, &zero, SHADER_UNIFORM_FLOAT);
    uni(ULightCount, &nl, SHADER_UNIFORM_INT);
    uni(UFogStart, &far, SHADER_UNIFORM_FLOAT);
    uni(UFogEnd, &far, SHADER_UNIFORM_FLOAT);
    Vector3 pp = V(0, 1, 0);
    uni(UPlayer, &pp, SHADER_UNIFORM_VEC3);
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
    if (anim.ride == 1) {
        // the bike under the rider (the seat is a little behind its centre)
        Vehicle bike;
        bike.kind = VehicleKind::Bike;
        bike.variant = 3;
        Matrix t = MatrixMultiply(MatrixRotateY(-facing), MatrixTranslate(std::cos(facing) * 0.16f, 0, std::sin(facing) * 0.16f));
        DrawMesh(vehicleMesh(bike), mat_, t);
    }
    drawCharacter(playerRig_, playerModel_, look.worn, &look.body, anim.ride ? nullptr : weapon);
    EndMode3D();
}

} // namespace alone
