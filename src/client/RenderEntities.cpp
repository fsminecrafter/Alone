// Moving things: the player character on its skeleton, wildlife, doors,
// campfires, items on the ground, weather, and the isometric tile cursor.
#include "client/RenderCommon.h"
#include "client/Renderer.h"
#include "core/Rng.h"
#include "raymath.h"
#include "rlgl.h"

namespace alone {

using namespace rc;

// ---------------------------------------------------------------------------
// Primitive helpers
// ---------------------------------------------------------------------------

void Renderer::drawPrim(const Mesh& m, Vector3 pos, Vector3 scale, float yaw, Color c, float pitch, float roll) {
    Matrix t = MatrixScale(scale.x, scale.y, scale.z);
    if (pitch != 0) t = MatrixMultiply(t, MatrixRotateX(pitch));
    if (roll != 0) t = MatrixMultiply(t, MatrixRotateZ(roll));
    t = MatrixMultiply(t, MatrixRotateY(yaw));
    t = MatrixMultiply(t, MatrixTranslate(pos.x, pos.y, pos.z));
    active_->maps[MATERIAL_MAP_DIFFUSE].color = c;
    DrawMesh(m, *active_, t);
}

void Renderer::drawTube(Vector3 a, Vector3 b, float r, Color c) {
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 1e-4f) return;
    Vector3 dir = Vector3Scale(d, 1.0f / len);
    Vector3 up{0, 1, 0};
    Vector3 axis = Vector3CrossProduct(up, dir);
    float s = Vector3Length(axis);
    Matrix rot = s < 1e-5f ? (dir.y > 0 ? MatrixIdentity() : MatrixRotateX(PI)) : MatrixRotate(Vector3Scale(axis, 1.0f / s), std::acos(std::clamp(dir.y, -1.0f, 1.0f)));
    Matrix t = MatrixMultiply(MatrixMultiply(MatrixScale(r, len, r), rot), MatrixTranslate(a.x, a.y, a.z));
    active_->maps[MATERIAL_MAP_DIFFUSE].color = c;
    DrawMesh(cylinder_, *active_, t);
    // round the joints
    Matrix j = MatrixMultiply(MatrixScale(r, r, r), MatrixTranslate(b.x, b.y, b.z));
    DrawMesh(sphere_, *active_, j);
}

void Renderer::drawBlob(Vector3 c, Vector3 r, Color col, float yaw) {
    drawPrim(sphere_, c, r, yaw, col);
}

void Renderer::drawOnBone(const Mesh& m, const Matrix& bone, Vector3 lc, Vector3 sc, Color c) {
    Matrix t = MatrixMultiply(MatrixMultiply(MatrixScale(sc.x, sc.y, sc.z), MatrixTranslate(lc.x, lc.y, lc.z)), bone);
    active_->maps[MATERIAL_MAP_DIFFUSE].color = c;
    DrawMesh(m, *active_, t);
}

// ---------------------------------------------------------------------------
// The player
// ---------------------------------------------------------------------------

void Renderer::updatePlayerRig(Game& game, float time, float dt) {
    const Player& p = game.player();
    if (!rigInit_) { lastPlayerPos_ = p.pos; rigInit_ = true; }
    float moved = distance(p.pos, lastPlayerPos_);
    if (moved > 5.0f) moved = 0;   // teleported
    lastPlayerPos_ = p.pos;
    playerDistance_ += moved;
    float inst = dt > 0 ? moved / dt : 0;
    playerSpeed_ += (inst - playerSpeed_) * std::min(1.0f, dt * 10.0f);
    AnimInput in;
    in.speed = playerSpeed_;
    in.distance = playerDistance_;
    in.sneaking = p.sneaking;
    in.asleep = p.asleep;
    in.dead = game.dead();
    in.swing = p.swing;
    const ItemInstance* w = game.weapon();
    in.aiming = w && w->def->firearm && p.lastAttack < 1.5f;
    const CurrentAction& a = game.currentAction();
    ActionKind k = a.active ? a.option.kind : ActionKind::None;
    in.eating = k == ActionKind::Eat || k == ActionKind::Drink || k == ActionKind::TakePill || k == ActionKind::DrinkSource;
    in.working = a.active && !in.eating && k != ActionKind::Sleep && k != ActionKind::Rest && k != ActionKind::Wait;
    in.time = time;
    if (w && w->def->firearm) in.swing = 0;
    playerRig_.update(in, dt);
    playerRig_.place(V(p.pos.x, 0, p.pos.y), p.facing);
}

void Renderer::drawCharacter(const CharacterRig& rig, const Player& p, const ItemDef* weapon) {
    auto col = [&](ClothingSlot s, uint32_t def) {
        const ItemInstance& it = p.worn[(size_t)s];
        return it.def ? hexColor(it.def->colour) : hexColor(def);
    };
    Color top = col(ClothingSlot::Torso, 0xd8ccb8);
    Color sleeves = top;
    if (p.worn[(size_t)ClothingSlot::Sweater].def) top = sleeves = col(ClothingSlot::Sweater, 0);
    bool jacket = p.worn[(size_t)ClothingSlot::Jacket].def != nullptr;
    if (jacket) top = sleeves = col(ClothingSlot::Jacket, 0);
    Color legs = col(ClothingSlot::Legs, 0xd8b898);
    Color shoes = col(ClothingSlot::Feet, 0xc8a080);
    Color skin = hexColor(0xe2b896), hair = hexColor(0x4a3626);
    Color hands = p.worn[(size_t)ClothingSlot::Hands].def ? col(ClothingSlot::Hands, 0) : skin;

    // Legs
    for (int s = 0; s < 2; ++s) {
        int th = s ? BThighR : BThighL, sh = s ? BShinR : BShinL, ft = s ? BFootR : BFootL;
        drawTube(rig.joint(th), rig.tip(th), 0.085f, legs);
        drawTube(rig.joint(sh), rig.tip(sh), 0.068f, legs);
        drawOnBone(cube_, rig.bone(ft), V(0.07f, -0.03f, 0), V(0.24f, 0.09f, 0.11f), shoes);
    }
    // Hips and torso
    drawOnBone(sphere_, rig.bone(BPelvis), V(0, 0.04f, 0), V(0.14f, 0.11f, 0.19f), legs);
    drawOnBone(cube_, rig.bone(BSpine), V(0, 0.12f, 0), V(0.21f, 0.26f, 0.32f), top);
    drawOnBone(cube_, rig.bone(BChest), V(0, 0.11f, 0), V(0.24f, 0.26f, 0.4f), top);
    drawOnBone(sphere_, rig.bone(BChest), V(0, 0.2f, 0), V(0.12f, 0.08f, 0.21f), top);   // shoulders
    if (jacket) drawOnBone(cube_, rig.bone(BSpine), V(0, -0.02f, 0), V(0.24f, 0.08f, 0.35f), shade(top, 0.85f));   // hem
    // Arms
    for (int s = 0; s < 2; ++s) {
        int ua = s ? BUpperArmR : BUpperArmL, fa = s ? BForearmR : BForearmL, hd = s ? BHandR : BHandL;
        drawTube(rig.joint(ua), rig.tip(ua), 0.06f, sleeves);
        drawTube(rig.joint(fa), rig.tip(fa), 0.05f, sleeves);
        drawOnBone(sphere_, rig.bone(hd), V(0, -0.05f, 0), V(0.045f, 0.065f, 0.035f), hands);
    }
    // Neck and head
    drawTube(rig.joint(BNeck), rig.tip(BNeck), 0.05f, skin);
    drawOnBone(sphere_, rig.bone(BHead), V(0.01f, 0.1f, 0), V(0.105f, 0.125f, 0.095f), skin);
    drawOnBone(sphere_, rig.bone(BHead), V(0.1f, 0.09f, 0), V(0.025f, 0.03f, 0.02f), shade(skin, 0.93f));     // nose
    for (float z : {-0.04f, 0.04f}) drawOnBone(sphere_, rig.bone(BHead), V(0.088f, 0.125f, z), V(0.012f, 0.012f, 0.012f), hexColor(0x202020));
    if (p.worn[(size_t)ClothingSlot::Head].def)
        drawOnBone(sphere_, rig.bone(BHead), V(-0.005f, 0.17f, 0), V(0.11f, 0.08f, 0.1f), col(ClothingSlot::Head, 0));
    else
        drawOnBone(sphere_, rig.bone(BHead), V(-0.02f, 0.16f, 0), V(0.105f, 0.08f, 0.1f), hair);
    // Backpack
    if (p.worn[(size_t)ClothingSlot::Back].def) {
        Color bp = col(ClothingSlot::Back, 0);
        drawOnBone(cube_, rig.bone(BChest), V(-0.21f, 0.04f, 0), V(0.18f, 0.42f, 0.3f), bp);
        drawOnBone(cube_, rig.bone(BChest), V(-0.31f, -0.06f, 0), V(0.06f, 0.18f, 0.24f), shade(bp, 0.85f));
    }
    // Weapon in the right hand, pointing forward out of the fist
    if (weapon) {
        Color wc = hexColor(weapon->colour);
        float len = weapon->firearm ? 1.1f : std::max(0.28f, weapon->reach * 0.6f);
        const Matrix& hand = rig.bone(BHandR);
        if (weapon->firearm) {
            drawOnBone(cube_, hand, V(0.25f, -0.08f, 0), V(len, 0.06f, 0.05f), hexColor(0x2a2a2a));
            drawOnBone(cube_, hand, V(-0.05f, -0.1f, 0), V(0.4f, 0.1f, 0.06f), wc);
        } else {
            drawOnBone(cube_, hand, V(0, -0.08f - len * 0.5f, 0), V(0.04f, len, 0.04f), wc);
            if (weapon->hasTag("axe")) drawOnBone(cube_, hand, V(0.06f, -0.08f - len * 0.95f, 0), V(0.14f, 0.1f, 0.02f), hexColor(0xb0b0b4));
        }
    }
}

// ---------------------------------------------------------------------------
// Wildlife
// ---------------------------------------------------------------------------

void Renderer::drawAnimal(const Animal& a, float time) {
    const AnimalDef& d = *a.def;
    Color c = hexColor(d.colour);
    float s = d.size;
    Vec2 fwd{std::cos(a.heading), std::sin(a.heading)};
    Vec2 side{-fwd.y, fwd.x};
    bool dead = !a.alive();
    float speed = a.vel.length();
    float phase = time * (3.0f + speed * 2.2f) + a.id;
    auto P = [&](float f, float y, float sd) { return V(a.pos.x + fwd.x * f + side.x * sd, y, a.pos.y + fwd.y * f + side.y * sd); };
    float yaw = -a.heading;

    if (d.id == "hare") {
        float hop = dead ? 0 : std::fabs(std::sin(phase)) * std::min(1.0f, speed * 0.25f) * 0.25f;
        float y = dead ? 0.08f : 0.17f + hop;
        Color belly = hexColor(0xd8d0c0);
        if (dead) {
            drawPrim(sphere_, P(0, 0.09f, 0), V(s * 0.5f, 0.1f, s * 0.22f), yaw, c);
            drawPrim(sphere_, P(s * 0.45f, 0.08f, 0.05f), V(0.09f, 0.07f, 0.08f), yaw, c);
            return;
        }
        drawPrim(sphere_, P(-0.02f, y, 0), V(s * 0.45f, s * 0.3f, s * 0.27f), yaw, c);
        drawPrim(sphere_, P(-0.05f, y - 0.05f, 0), V(s * 0.38f, s * 0.2f, s * 0.22f), yaw, belly);
        Vector3 head = P(s * 0.42f, y + s * 0.2f, 0);
        drawPrim(sphere_, head, V(0.1f, 0.085f, 0.08f), yaw, c);
        for (int i = -1; i <= 1; i += 2) {   // long ears with dark tips
            Vector3 e0 = P(s * 0.36f, y + s * 0.3f, 0.035f * i);
            Vector3 e1 = P(s * 0.25f, y + s * 0.62f, 0.06f * i);
            drawTube(e0, e1, 0.025f, c);
            drawPrim(sphere_, e1, V(0.025f, 0.03f, 0.025f), yaw, hexColor(0x2a2420));
        }
        drawPrim(sphere_, P(-s * 0.48f, y + 0.05f, 0), V(0.05f, 0.05f, 0.05f), yaw, hexColor(0xf0ece4));
        float leg = std::sin(phase) * std::min(1.0f, speed * 0.2f);
        for (int i = -1; i <= 1; i += 2) {
            drawTube(P(-s * 0.25f, y - 0.05f, 0.08f * i), P(-s * 0.32f + leg * 0.12f, 0.02f, 0.08f * i), 0.04f, c);
            drawTube(P(s * 0.2f, y - 0.05f, 0.06f * i), P(s * 0.25f - leg * 0.1f, 0.02f, 0.06f * i), 0.025f, c);
        }
        return;
    }

    bool boar = d.id == "wild_boar", moose = d.id == "moose", deer = d.id == "roe_deer";
    float legH = boar ? s * 0.34f : moose ? s * 0.55f : s * 0.52f;
    float bodyH = boar ? s * 0.4f : moose ? s * 0.33f : s * 0.27f;
    float by = legH + bodyH * 0.5f;
    if (dead) {
        drawPrim(sphere_, P(0, bodyH * 0.45f, 0), V(s * 0.55f, bodyH * 0.45f, s * 0.2f), yaw, c, 0, 0);
        drawPrim(sphere_, P(s * 0.6f, bodyH * 0.3f, 0.1f), V(s * 0.16f, s * 0.1f, s * 0.1f), yaw, shade(c, 0.9f));
        for (int i = 0; i < 4; ++i) {
            float f = (i < 2 ? 0.3f : -0.3f) * s;
            drawTube(P(f, bodyH * 0.4f, 0.1f * s), P(f + 0.1f * s, 0.05f, legH * 0.9f), 0.03f + 0.03f * s, shade(c, 0.85f));
        }
        return;
    }
    // body: barrel plus chest/hindquarters
    drawPrim(sphere_, P(0, by, 0), V(s * 0.52f, bodyH * 0.5f, s * (boar ? 0.21f : 0.17f)), yaw, c);
    drawPrim(sphere_, P(s * 0.28f, by + bodyH * 0.08f, 0), V(s * 0.25f, bodyH * 0.55f, s * (boar ? 0.22f : 0.18f)), yaw, c);
    if (moose) drawPrim(sphere_, P(s * 0.22f, by + bodyH * 0.45f, 0), V(s * 0.22f, bodyH * 0.35f, s * 0.12f), yaw, c);   // hump
    if (deer) drawPrim(sphere_, P(-s * 0.48f, by + 0.02f, 0), V(0.09f, 0.11f, 0.11f), yaw, hexColor(0xf2eee2));          // white rump
    // legs with a knee
    float moving = std::min(1.0f, speed * 0.4f);
    for (int i = 0; i < 4; ++i) {
        bool front = i < 2;
        float f = (front ? 0.36f : -0.36f) * s;
        float sd = (i % 2 ? 0.12f : -0.12f) * s;
        float sw = std::sin(phase + (i == 0 || i == 3 ? 0.0f : PI)) * moving * 0.18f * s;
        float lift = std::max(0.0f, std::cos(phase + (i == 0 || i == 3 ? 0.0f : PI))) * moving * 0.08f * s;
        Vector3 hip = P(f, by - bodyH * 0.2f, sd);
        Vector3 knee = P(f + sw * 0.5f + (front ? 0.0f : -0.06f * s), legH * 0.55f + lift, sd);
        Vector3 hoof = P(f + sw, 0.02f + lift * 0.5f, sd);
        float r = (boar ? 0.05f : 0.035f) + 0.02f * s;
        drawTube(hip, knee, r, c);
        drawTube(knee, hoof, r * 0.7f, shade(c, 0.85f));
    }
    // neck and head
    float graze = (a.state == AnimalState::Idle) ? 0.5f + 0.5f * std::sin(time * 0.5f + a.id) : 0.0f;
    Vector3 neck0 = P(s * 0.42f, by + bodyH * 0.2f, 0);
    Vector3 head = boar ? P(s * 0.68f, by - bodyH * 0.05f, 0)
                        : P(s * (0.62f + 0.05f * graze), by + s * (0.38f - 0.55f * graze), 0);
    if (!boar) drawTube(neck0, head, (moose ? 0.14f : 0.07f) * s + 0.03f, c);
    drawPrim(sphere_, head, V(s * (moose ? 0.24f : boar ? 0.2f : 0.15f), s * (boar ? 0.17f : 0.1f), s * (boar ? 0.14f : 0.08f)), yaw, shade(c, 0.92f));
    Vector3 snout = P(s * (boar ? 0.86f : 0.74f + 0.05f * graze), head.y - s * (boar ? 0.04f : 0.03f), 0);
    drawTube(head, snout, s * (boar ? 0.09f : moose ? 0.1f : 0.05f), shade(c, 0.85f));
    if (boar) drawPrim(sphere_, snout, V(0.06f, 0.07f, 0.08f), yaw, hexColor(0x3a2a24));
    // ears
    for (int i = -1; i <= 1; i += 2) {
        Vector3 e0 = Vector3Add(head, V(side.x * 0.06f * i * s, 0.05f * s, side.y * 0.06f * i * s));
        Vector3 e1 = Vector3Add(e0, V(side.x * 0.08f * i * s - fwd.x * 0.03f, 0.1f * s, side.y * 0.08f * i * s - fwd.y * 0.03f));
        drawTube(e0, e1, 0.025f * s + 0.01f, c);
    }
    // antlers: moose bulls get broad palms, roe bucks small spikes
    if ((moose || deer) && (a.id & 1)) {
        for (int i = -1; i <= 1; i += 2) {
            Vector3 base = Vector3Add(head, V(side.x * 0.06f * i, 0.09f * s, side.y * 0.06f * i));
            if (moose) {
                Vector3 tip = Vector3Add(base, V(side.x * 0.45f * i - fwd.x * 0.1f, 0.2f, side.y * 0.45f * i - fwd.y * 0.1f));
                drawTube(base, tip, 0.04f, hexColor(0xd0c09a));
                drawPrim(sphere_, tip, V(0.25f, 0.05f, 0.18f), yaw + 0.4f * i, hexColor(0xd0c09a));
            } else {
                drawTube(base, Vector3Add(base, V(side.x * 0.03f * i, 0.18f, side.y * 0.03f * i)), 0.015f, hexColor(0xc8b088));
            }
        }
    }
    // tail
    drawTube(P(-s * 0.5f, by + bodyH * 0.2f, 0), P(-s * 0.58f, by - bodyH * 0.1f, 0), boar ? 0.02f : 0.03f, c);
}

// ---------------------------------------------------------------------------
// World objects
// ---------------------------------------------------------------------------

void Renderer::drawDoors(World& world, const std::vector<int>& ids) {
    for (int id : ids) {
        const Building* b = world.building(id);
        if (!b) continue;
        Color c = hexColor(id % 3 == 0 ? 0x5a3a28 : id % 3 == 1 ? 0x7a5a3c : 0x2a4a3a);
        for (const auto& e : b->edges) {
            for (int dir = 0; dir < 2; ++dir) {
                if (!(e.bits & (dir == 0 ? DoorN : DoorW))) continue;
                bool open = world.doorOpen(e.x, e.y, dir);
                float ax = (float)e.x, az = (float)e.y;
                if (dir == 0) {
                    if (!open) drawPrim(cube_, V(ax + 0.5f, 1.04f, az), V(0.84f, 2.06f, 0.06f), 0, c);
                    else drawPrim(cube_, V(ax + 0.1f, 1.04f, az + 0.42f), V(0.06f, 2.06f, 0.84f), 0, c);
                } else {
                    if (!open) drawPrim(cube_, V(ax, 1.04f, az + 0.5f), V(0.06f, 2.06f, 0.84f), 0, c);
                    else drawPrim(cube_, V(ax + 0.42f, 1.04f, az + 0.1f), V(0.84f, 2.06f, 0.06f), 0, c);
                }
            }
        }
    }
}

void Renderer::drawEntities(Game& game, float time, bool shadowPass) {
    World& w = game.world();
    Vec2 pp = game.player().pos;
    for (const auto& a : game.animals().animals()) {
        if (a.butchered || distance(a.pos, pp) > 90) continue;
        drawAnimal(a, time);
    }
    if (!shadowPass) {
        for (const auto& [key, stack] : w.allGroundItems()) {
            if (stack.empty()) continue;
            int x = (int)(key >> 32), y = (int)(uint32_t)key;
            if (std::abs(x - pp.x) > 70 || std::abs(y - pp.y) > 70) continue;
            for (size_t i = 0; i < stack.size() && i < 5; ++i) {
                float ox = 0.25f + 0.22f * (i % 3), oz = 0.3f + 0.35f * (i / 3);
                drawPrim(cube_, V(x + ox, 0.07f, y + oz), V(0.22f, 0.14f, 0.16f), i * 0.6f, hexColor(stack[i].def->colour));
            }
        }
    }
    for (const auto& f : w.campfires()) {
        if (distance(f.pos, pp) > 80) continue;
        for (int i = 0; i < 8; ++i) {
            float a = i * 2 * PI / 8;
            drawPrim(sphere_, V(f.pos.x + std::cos(a) * 0.5f, 0.07f, f.pos.y + std::sin(a) * 0.5f), V(0.14f, 0.1f, 0.12f), a, hexColor(0x6e6e6a));
        }
        if (f.fuelMinutes > 0 || f.lit) {
            for (int i = 0; i < 4; ++i) {
                float a = i * PI * 0.5f + 0.3f;
                drawTube(V(f.pos.x + std::cos(a) * 0.45f, 0.05f, f.pos.y + std::sin(a) * 0.45f), V(f.pos.x, 0.45f, f.pos.y), 0.05f, hexColor(0x4a3020));
            }
        }
        if (f.lit && !shadowPass) {
            setUnlit(true);
            float fl = 0.8f + 0.2f * std::sin(time * 17.0f + f.id) * std::sin(time * 5.3f);
            drawPrim(cone_, V(f.pos.x, 0.1f, f.pos.y), V(0.34f, 0.9f * fl, 0.34f), time, hexColor(0xff6a10));
            drawPrim(cone_, V(f.pos.x + 0.05f, 0.1f, f.pos.y), V(0.2f, 1.15f * fl, 0.2f), -time * 1.3f, hexColor(0xffc838));
            drawPrim(sphere_, V(f.pos.x, 0.12f, f.pos.y), V(0.32f, 0.08f, 0.32f), 0, hexColor(0xff8a20));
            setUnlit(false);
        }
    }
    for (const auto& s : w.snares()) {
        if (distance(s.pos, pp) > 60) continue;
        drawTube(V(s.pos.x - 0.15f, 0.02f, s.pos.y), V(s.pos.x - 0.15f, 0.35f, s.pos.y), 0.02f, hexColor(0x6a5034));
        drawPrim(sphere_, V(s.pos.x, 0.15f, s.pos.y), V(0.13f, 0.13f, 0.02f), 0, hexColor(0xc0a040));
    }
    drawCharacter(playerRig_, game.player(), game.weapon() ? game.weapon()->def : nullptr);
}

void Renderer::drawWeather(Game& game, float time) {
    const Weather& w = game.world().clock().weather();
    if (w.rain <= 0.01f) return;
    bool inside = currentBuilding_ >= 0;
    bool snow = w.kind == WeatherKind::Snow;
    int drops = (int)(600 * w.rain);
    Vec2 c = target_;
    for (int i = 0; i < drops; ++i) {
        uint32_t h = hash32(i * 7919 + 1);
        float x = c.x + (hashFloat(h) - 0.5f) * zoom_ * 2.2f;
        float z = c.y + (hashFloat(hash32(h)) - 0.5f) * zoom_ * 2.2f;
        float fall = snow ? 1.2f : 14.0f;
        float y = std::fmod(hashFloat(hash32(h + 5)) * 14.0f - time * fall, 14.0f);
        if (y < 0) y += 14.0f;
        if (inside && distance(Vec2{x, z}, game.player().pos) < 9) continue;
        if (snow) {
            float drift = std::sin(time * 0.8f + i) * 0.5f;
            DrawCube(V(x + drift, y, z), 0.05f, 0.05f, 0.05f, Fade(WHITE, 0.85f));
        } else {
            DrawLine3D(V(x, y, z), V(x - w.wind * 0.03f, y + 0.7f, z), Fade(hexColor(0xb4c6d2), 0.5f));
        }
    }
}

void Renderer::drawCursor(Game& game, const HoverInfo& hover, float time) {
    if (!hover.show) return;
    int hx = (int)std::floor(hover.point.x), hy = (int)std::floor(hover.point.y);
    float y = std::max(0.02f, groundHeight(game.world().tile(hx, hy).ground) + 0.03f);
    Color c = hover.colour;
    // translucent fill
    rlDisableDepthTest();
    Color fill = Fade(c, 0.16f + 0.05f * std::sin(time * 4.0f));
    DrawTriangle3D(V((float)hx, y, (float)hy), V((float)hx, y, hy + 1.0f), V(hx + 1.0f, y, hy + 1.0f), fill);
    DrawTriangle3D(V((float)hx, y, (float)hy), V(hx + 1.0f, y, hy + 1.0f), V(hx + 1.0f, y, (float)hy), fill);
    // corner brackets
    float k = 0.3f;
    Vector3 cs[4] = {V((float)hx, y, (float)hy), V(hx + 1.0f, y, (float)hy), V(hx + 1.0f, y, hy + 1.0f), V((float)hx, y, hy + 1.0f)};
    for (int i = 0; i < 4; ++i) {
        Vector3 a = cs[i], prev = cs[(i + 3) % 4], next = cs[(i + 1) % 4];
        DrawLine3D(a, Vector3Lerp(a, next, k), c);
        DrawLine3D(a, Vector3Lerp(a, prev, k), c);
    }
    rlEnableDepthTest();
}

} // namespace alone
