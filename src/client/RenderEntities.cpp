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
    t = MatrixMultiply(t, MatrixTranslate(pos.x, pos.y + liftY_, pos.z));
    active_->maps[MATERIAL_MAP_DIFFUSE].color = c;
    DrawMesh(m, *active_, t);
}

void Renderer::drawTube(Vector3 a, Vector3 b, float r, Color c) {
    a.y += liftY_;
    b.y += liftY_;
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

CharacterLook Renderer::lookOf(const std::array<ItemInstance, 9>& wornItems, const Body* body) {
    CharacterLook L;
    auto worn = [&](ClothingSlot sl) -> const ItemInstance& { return wornItems[(size_t)sl]; };
    auto col = [&](ClothingSlot sl) { return hexColor(worn(sl).def->colour); };
    // Underclothes when nothing is worn in a slot
    L.top = worn(ClothingSlot::Torso).def ? col(ClothingSlot::Torso) : hexColor(0xd8ccb8);
    if (worn(ClothingSlot::Sweater).def) L.top = col(ClothingSlot::Sweater);
    if (worn(ClothingSlot::Jacket).def) L.jacket = col(ClothingSlot::Jacket);
    L.shortSleeves = !worn(ClothingSlot::Sweater).def && !worn(ClothingSlot::Jacket).def &&
                     (!worn(ClothingSlot::Torso).def || worn(ClothingSlot::Torso).def->shortSleeves);
    L.pants = worn(ClothingSlot::Legs).def ? col(ClothingSlot::Legs) : hexColor(0xc8b8a0);
    if (worn(ClothingSlot::Feet).def) L.shoes = col(ClothingSlot::Feet);
    else L.barefoot = true;
    if (worn(ClothingSlot::Hands).def) L.gloves = col(ClothingSlot::Hands);
    if (worn(ClothingSlot::Head).def) L.hat = col(ClothingSlot::Head);
    // Blood soaks into the clothes around a wound
    if (body)
    for (const auto& w : body->state().wounds) {
        float v = std::min(1.0f, w.bleeding * 0.06f + w.severity * 0.6f * (1.0f - w.healed));
        L.blood[(int)w.part] = std::max(L.blood[(int)w.part], v);
    }
    return L;
}

namespace {
int weaponKind(const ItemDef* w) {
    if (!w) return 0;
    if (w->firearm) return 3;
    if (w->hasTag("knife") && !w->hasTag("axe")) return 2;
    return w->reach < 0.75f ? 2 : 1;
}
} // namespace

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
    in.asleep = p.asleep || p.fallen > 0;   // (tripped: lying where you fell)
    in.dead = game.dead();
    in.swing = p.swing;
    const ItemInstance* w = game.weapon();
    in.weapon = weaponKind(w ? w->def : nullptr);
    in.aiming = w && w->def->firearm && (p.lastAttack < 1.5f || p.aiming);
    const CurrentAction& a = game.currentAction();
    ActionKind k = a.active ? a.option.kind : ActionKind::None;
    in.eating = k == ActionKind::Eat || k == ActionKind::Drink || k == ActionKind::TakePill || k == ActionKind::DrinkSource;
    in.working = a.active && !in.eating && k != ActionKind::Sleep && k != ActionKind::Rest && k != ActionKind::Wait;
    in.time = time;
    if (w && w->def->firearm) in.swing = 0;
    // How the body feels shows in how you move
    const Body& b = p.body;
    in.limpL = std::min(1.0f, b.impairment(BodyPart::LeftLeg) + 0.7f * b.impairment(BodyPart::LeftFoot));
    in.limpR = std::min(1.0f, b.impairment(BodyPart::RightLeg) + 0.7f * b.impairment(BodyPart::RightFoot));
    in.armHurtL = std::min(1.0f, b.impairment(BodyPart::LeftArm) + 0.8f * b.impairment(BodyPart::LeftHand));
    in.armHurtR = std::min(1.0f, b.impairment(BodyPart::RightArm) + 0.8f * b.impairment(BodyPart::RightHand));
    BodySignals g = b.signals();
    in.hunch = std::clamp(std::max({g.pain * 0.9f, g.weakness * 0.8f, g.cramps * 0.7f, g.tired * 0.35f}) - 0.15f, 0.0f, 1.0f);
    in.shiver = b.state().shivering;
    in.nausea = std::clamp(std::max(g.nausea, g.cramps) * 1.6f - 0.5f, 0.0f, 1.0f);
    in.winded = p.stamina < 0.35f ? (0.35f - p.stamina) / 0.35f : 0.0f;
    // On a bike or in a car: pedal or sit, in the seat
    const Vehicle* ride = game.playerVehicle();
    Vec2 seat = p.pos;
    if (ride) {
        in.ride = ride->kind == VehicleKind::Car ? 2 : 1;
        in.speed = 0;
        in.weapon = 0;
        in.swing = 0;
        in.aiming = false;
        in.working = false;
        in.winded = 0;
        Vec2 f = ride->forward(), side{-f.y, f.x};
        seat = ride->kind == VehicleKind::Car ? ride->pos - f * 0.3f - side * 0.36f : ride->pos - f * 0.16f;
    }
    playerRig_.update(in, dt);
    float stand = game.world().standHeight(seat);
    if (std::fabs(stand - playerY_) > 3.0f || moved == 0) playerY_ = stand;   // teleported: no slow rise
    else playerY_ += (stand - playerY_) * std::min(1.0f, dt * 14.0f);
    playerRig_.place(V(seat.x, playerY_, seat.y), p.facing);
    playerModel_.skin(playerRig_, lookOf(p.worn, &p.body));

    // Blood drips from wounds that nothing covers; rain washes it off outdoors
    float bleed = 0;
    for (int part = 0; part < (int)BodyPart::Count; ++part)
        if (!b.dressing((BodyPart)part).on()) bleed += b.bleedingAt((BodyPart)part);
    World& world = game.world();
    if (bleed > 0.4f && !game.dead()) {
        bloodTimer_ -= dt * std::min(6.0f, bleed * 0.25f) + moved * 0.8f;
        if (bloodTimer_ <= 0) {
            bloodTimer_ = 1.0f;
            uint32_t h = hash32((uint32_t)(time * 1000.0f));
            Vec2 at = p.pos + Vec2{(hashFloat(h) - 0.5f) * 0.4f, (hashFloat(hash32(h)) - 0.5f) * 0.4f};
            BloodDrop d;
            d.pos = at;
            d.size = 0.05f + 0.06f * hashFloat(hash32(h + 1)) + std::min(0.08f, bleed * 0.004f);
            d.indoors = world.tile((int)std::floor(at.x), (int)std::floor(at.y)).building >= 0;
            d.y = world.standHeight(at);
            bloodDrops_.push_back(d);
            if (bloodDrops_.size() > 500) bloodDrops_.erase(bloodDrops_.begin());
        }
    }
    float rain = world.clock().weather().rain;
    if (rain > 0.05f && !bloodDrops_.empty()) {
        float wash = dt * rain / 40.0f * std::max(1.0f, game.timeSpeed());
        for (auto& d : bloodDrops_)
            if (!d.indoors) d.amount -= wash;
        bloodDrops_.erase(std::remove_if(bloodDrops_.begin(), bloodDrops_.end(), [](const BloodDrop& d) { return d.amount <= 0; }),
                          bloodDrops_.end());
    }
    for (auto& d : bloodDrops_) d.age += dt;
}

void Renderer::drawCharacter(const CharacterRig& rig, const CharacterModel& model, const std::array<ItemInstance, 9>& wornItems,
                             const Body* body, const ItemDef* weapon) {
    // The body: one skinned mesh, already posed in world space
    if (model.ready()) {
        active_->maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        DrawMesh(model.mesh(), *active_, MatrixIdentity());
    }
    auto worn = [&](ClothingSlot sl) { return wornItems[(size_t)sl].def; };
    // A beanie's folded brim, a jacket collar
    if (const ItemDef* hat = worn(ClothingSlot::Head))
        drawOnBone(cylinder_, rig.bone(BHead), V(0.004f, 0.112f, 0), V(0.113f, 0.04f, 0.101f), shade(hexColor(hat->colour), 0.88f));
    if (const ItemDef* j = worn(ClothingSlot::Jacket))
        drawOnBone(cylinder_, rig.bone(BChest), V(0.0f, 0.19f, 0), V(0.072f, 0.075f, 0.082f), shade(hexColor(j->colour), 0.85f));
    // Backpack with shoulder straps
    if (const ItemDef* bag = worn(ClothingSlot::Back)) {
        Color bp = hexColor(bag->colour);
        drawOnBone(sphere_, rig.bone(BChest), V(-0.2f, 0.03f, 0), V(0.105f, 0.21f, 0.155f), bp);
        drawOnBone(sphere_, rig.bone(BChest), V(-0.285f, -0.06f, 0), V(0.05f, 0.09f, 0.11f), shade(bp, 0.85f));
        for (float z : {-0.1f, 0.1f}) {
            Vector3 a = Vector3Transform(V(-0.12f, 0.22f, z), rig.bone(BChest));
            Vector3 b = Vector3Transform(V(0.11f, 0.17f, z * 1.05f), rig.bone(BChest));
            Vector3 c = Vector3Transform(V(0.12f, -0.12f, z * 1.15f), rig.bone(BChest));
            drawTube(a, b, 0.018f, shade(bp, 0.7f));
            drawTube(b, c, 0.018f, shade(bp, 0.7f));
        }
    }
    // Dressings: bands round the hurt part, white going brown and red
    static const int partBone[] = {BHead, BChest, BForearmL, BForearmR, BHandL, BHandR, BShinL, BShinR, BFootL, BFootR};
    for (int part = 0; part < (int)BodyPart::Count && body; ++part) {
        const Dressing& d = body->dressing((BodyPart)part);
        if (!d.on()) continue;
        Color base = d.kind == DressingKind::Bandage ? Color{238, 236, 228, 255}
                   : d.kind == DressingKind::Rag ? Color{206, 196, 176, 255} : Color{222, 186, 152, 255};
        Color c = mixColor(base, Color{108, 86, 58, 255}, std::min(1.0f, d.dirt));
        c = mixColor(c, Color{138, 20, 18, 255}, std::min(0.85f, d.blood * 0.9f));
        bool aid = d.kind == DressingKind::Bandaid;
        const Matrix& bm = rig.bone(partBone[part]);
        switch ((BodyPart)part) {
            case BodyPart::Head:
                if (aid) drawOnBone(cube_, bm, V(0.098f, 0.145f, 0.02f), V(0.008f, 0.02f, 0.045f), c);
                else drawOnBone(cylinder_, bm, V(0.008f, 0.118f, 0), V(0.108f, 0.045f, 0.097f), c);
                break;
            case BodyPart::Torso:
                if (aid) drawOnBone(cube_, bm, V(0.125f, 0.0f, -0.05f), V(0.008f, 0.04f, 0.05f), c);
                else drawOnBone(cylinder_, bm, V(0.012f, -0.05f, 0), V(0.128f, 0.09f, 0.198f), c);
                break;
            case BodyPart::LeftArm: case BodyPart::RightArm:
                drawOnBone(cylinder_, bm, V(0, aid ? -0.16f : -0.21f, 0), V(0.05f, aid ? 0.03f : 0.11f, 0.048f), c);
                break;
            case BodyPart::LeftHand: case BodyPart::RightHand:
                drawOnBone(cylinder_, bm, V(0.004f, aid ? -0.07f : -0.1f, 0), V(0.049f, aid ? 0.02f : 0.055f, 0.025f), c);
                break;
            case BodyPart::LeftLeg: case BodyPart::RightLeg:
                drawOnBone(cylinder_, bm, V(-0.006f, aid ? -0.2f : -0.24f, 0), V(0.062f, aid ? 0.03f : 0.13f, 0.06f), c);
                break;
            case BodyPart::LeftFoot: case BodyPart::RightFoot:
                drawOnBone(cylinder_, bm, V(-0.005f, 0.035f, 0), V(0.05f, aid ? 0.02f : 0.05f, 0.052f), c);
                break;
            default: break;
        }
    }
    if (!weapon) return;
    // The weapon sticks out of the fist along the hand's forward axis; bending
    // the wrist points it. The cutting edge faces the way the blow travels.
    Color wc = hexColor(weapon->colour);
    Color steel = hexColor(0x9a9ea4), edge = hexColor(0xd2d6da), dark = hexColor(0x2c2c2e);
    Matrix grip = MatrixMultiply(MatrixTranslate(0.0f, -0.075f, 0.0f), rig.bone(BHandR));
    if (weapon->firearm) {
        Matrix frame = grip;
        if (rig.aimWeight() > 0.5f) {   // aiming: level along the chest's forward axis, stock in the shoulder
            frame = rig.bone(BChest);
            frame.m12 = grip.m12; frame.m13 = grip.m13; frame.m14 = grip.m14;
        }
        drawOnBone(cube_, frame, V(-0.22f, -0.03f, 0), V(0.42f, 0.085f, 0.045f), wc);           // stock
        drawOnBone(cube_, frame, V(0.07f, 0.015f, 0), V(0.26f, 0.06f, 0.042f), dark);           // action
        drawOnBone(cube_, frame, V(0.33f, 0.0f, 0), V(0.32f, 0.05f, 0.04f), wc);                // fore-end
        drawOnBone(cube_, frame, V(0.52f, 0.03f, 0), V(0.62f, 0.024f, 0.024f), dark);           // barrel
        drawOnBone(cube_, frame, V(0.08f, 0.075f, 0), V(0.24f, 0.038f, 0.038f), dark);          // scope
        return;
    }
    if (weapon->hasTag("axe")) {
        drawOnBone(cube_, grip, V(0.27f, 0, 0), V(0.70f, 0.032f, 0.026f), wc);                  // handle
        drawOnBone(cube_, grip, V(0.585f, -0.04f, 0), V(0.075f, 0.12f, 0.024f), steel);         // head
        drawOnBone(cube_, grip, V(0.585f, -0.112f, 0), V(0.092f, 0.03f, 0.012f), edge);         // the edge, leading
        drawOnBone(cube_, grip, V(0.585f, 0.035f, 0), V(0.052f, 0.04f, 0.034f), steel);         // poll
    } else if (weapon->hasTag("knife") || weapon->reach < 0.75f) {
        drawOnBone(cube_, grip, V(0.02f, 0, 0), V(0.11f, 0.03f, 0.024f), dark);                 // handle
        drawOnBone(cube_, grip, V(0.082f, 0, 0), V(0.014f, 0.05f, 0.03f), steel);               // guard
        drawOnBone(cube_, grip, V(0.17f, -0.004f, 0), V(0.17f, 0.032f, 0.005f), edge);          // blade
    } else if (weapon->id == "hammer") {
        drawOnBone(cube_, grip, V(0.14f, 0, 0), V(0.36f, 0.03f, 0.024f), wc);
        drawOnBone(cube_, grip, V(0.31f, 0, 0), V(0.045f, 0.13f, 0.036f), steel);
    } else if (weapon->id == "crowbar") {
        drawOnBone(cube_, grip, V(0.3f, 0, 0), V(0.72f, 0.024f, 0.02f), wc);
        drawOnBone(cube_, grip, V(0.66f, -0.035f, 0), V(0.024f, 0.09f, 0.02f), wc);
    } else if (weapon->id == "bandy_stick") {
        drawOnBone(cube_, grip, V(0.4f, 0, 0), V(0.96f, 0.028f, 0.02f), wc);
        drawOnBone(cube_, grip, V(0.9f, -0.05f, 0), V(0.14f, 0.09f, 0.014f), wc);
    } else {
        float len = std::max(0.3f, weapon->reach * 0.6f);
        drawOnBone(cube_, grip, V(len * 0.45f, 0, 0), V(len, 0.035f, 0.03f), wc);
    }
}

void Renderer::drawBlood(float time) {
    if (bloodDrops_.empty()) return;
    (void)time;
    for (const auto& d : bloodDrops_) {
        // fresh blood is bright, it dries dark; rain thins it out
        Color c = mixColor(Color{120, 10, 10, 255}, Color{58, 20, 16, 255}, std::min(1.0f, d.age / 120.0f));
        float r = d.size * (0.4f + 0.6f * std::max(0.0f, d.amount));
        drawPrim(cylinder_, V(d.pos.x, d.y + 0.012f, d.pos.y), V(r, 0.004f, r * 0.85f), d.size * 31.0f, c);
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

void Renderer::drawDoors(World& world, const std::vector<int>& ids, bool mainPass) {
    // Doors the player built
    for (const Built& b : world.builds()) {
        if (b.kind != BuildKind::Door || distance(Vec2{(float)b.x, (float)b.y}, playerPos_) > 80) continue;
        liftY_ = world.map().elevation(b.dir == 0 ? b.x + 0.5f : (float)b.x, b.dir == 0 ? (float)b.y : b.y + 0.5f);
        bool open = world.doorOpen(b.x, b.y, b.dir);
        float ax = (float)b.x, az = (float)b.y;
        Color c = hexColor(0x8a6a44);
        if (b.dir == 0) {
            if (!open) drawPrim(cube_, V(ax + 0.5f, 1.07f, az), V(0.8f, 2.12f, 0.06f), 0, c);
            else drawPrim(cube_, V(ax + 0.12f, 1.07f, az + 0.4f), V(0.06f, 2.12f, 0.8f), 0, c);
        } else {
            if (!open) drawPrim(cube_, V(ax, 1.07f, az + 0.5f), V(0.06f, 2.12f, 0.8f), 0, c);
            else drawPrim(cube_, V(ax + 0.4f, 1.07f, az + 0.12f), V(0.8f, 2.12f, 0.06f), 0, c);
        }
    }
    for (int id : ids) {
        const Building* b = world.building(id);
        if (!b) continue;
        int cutMode = 0;
        if (mainPass)
            for (const auto& cb : cutBuildings_)
                if (cb.id == id) cutMode = cb.mode;
        liftY_ = b->floorY;
        Color c = hexColor(id % 3 == 0 ? 0x5a3a28 : id % 3 == 1 ? 0x7a5a3c : 0x2a4a3a);
        for (const auto& e : b->edges) {
            for (int dir = 0; dir < 2; ++dir) {
                if (!(e.bits & (dir == 0 ? DoorN : DoorW))) continue;
                bool open = world.doorOpen(e.x, e.y, dir);
                float ax = (float)e.x, az = (float)e.y;
                // In a wall that's cut down, the door is cut down with it
                Vec2 centre = dir == 0 ? Vec2{ax + 0.5f, az} : Vec2{ax, az + 0.5f};
                float h = wallCutAt(centre, cutMode) ? 0.5f : 2.06f, y = h * 0.5f + 0.01f;
                if (dir == 0) {
                    if (!open) drawPrim(cube_, V(ax + 0.5f, y, az), V(0.84f, h, 0.06f), 0, c);
                    else drawPrim(cube_, V(ax + 0.1f, y, az + 0.42f), V(0.06f, h, 0.84f), 0, c);
                } else {
                    if (!open) drawPrim(cube_, V(ax, y, az + 0.5f), V(0.06f, h, 0.84f), 0, c);
                    else drawPrim(cube_, V(ax + 0.42f, y, az + 0.1f), V(0.84f, h, 0.06f), 0, c);
                }
            }
        }
    }
    liftY_ = 0;
}

void Renderer::drawEntities(Game& game, float time, bool shadowPass) {
    World& w = game.world();
    Vec2 pp = game.player().pos;
    for (const auto& a : game.animals().animals()) {
        if (a.butchered || distance(a.pos, pp) > 90) continue;
        liftY_ = w.standHeight(a.pos);
        drawAnimal(a, time);
    }
    liftY_ = 0;
    if (!shadowPass) {
        for (const auto& [key, stack] : w.allGroundItems()) {
            if (stack.empty()) continue;
            int x = (int)(key >> 32), y = (int)(uint32_t)key;
            if (std::abs(x - pp.x) > 70 || std::abs(y - pp.y) > 70) continue;
            liftY_ = w.standHeight(Vec2{x + 0.5f, y + 0.5f});
            for (size_t i = 0; i < stack.size() && i < 5; ++i) {
                float ox = 0.25f + 0.22f * (i % 3), oz = 0.3f + 0.35f * (i / 3);
                drawPrim(cube_, V(x + ox, 0.07f, y + oz), V(0.22f, 0.14f, 0.16f), i * 0.6f, hexColor(stack[i].def->colour));
            }
        }
    }
    for (const auto& t : w.tents())
        if (distance(t.rect.centre(), pp) < 90) {
            liftY_ = w.standHeight(t.rect.centre());
            drawTent(t, !shadowPass && t.rect.contains((int)std::floor(pp.x), (int)std::floor(pp.y)));
        }
    liftY_ = 0;
    for (const auto& f : w.campfires()) {
        liftY_ = w.standHeight(f.pos);
        if (f.stove) { if (distance(f.pos, pp) < 90) drawStove(f, time, shadowPass); continue; }
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
        liftY_ = w.standHeight(s.pos);
        drawTube(V(s.pos.x - 0.15f, 0.02f, s.pos.y), V(s.pos.x - 0.15f, 0.35f, s.pos.y), 0.02f, hexColor(0x6a5034));
        drawPrim(sphere_, V(s.pos.x, 0.15f, s.pos.y), V(0.13f, 0.13f, 0.02f), 0, hexColor(0xc0a040));
    }
    liftY_ = 0;
    if (!shadowPass) drawBlood(time);
    drawVehicles(game, shadowPass);
    drawSurvivors(game);
    const Vehicle* ride = game.playerVehicle();
    drawCharacter(playerRig_, playerModel_, game.player().worn, &game.player().body,
                  game.weapon() && !ride ? game.weapon()->def : nullptr);
}

void Renderer::drawWeather(Game& game, float time) {
    World& world = game.world();
    const Weather& w = world.clock().weather();
    if (w.rain <= 0.01f) return;
    bool snow = w.kind == WeatherKind::Snow;
    rlDrawRenderBatchActive();
    rlDisableDepthMask();      // see-through: streaks don't hide each other
    rlDisableBackfaceCulling();
    Vector3 view = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    float px = zoom_ / std::max(1.0f, (float)GetScreenHeight());   // metres per screen pixel
    float span = zoom_ * 2.3f;
    Vec2 c = target_;
    // Nothing falls indoors (or under the roof of a tent)
    auto sheltered = [&](float x, float z) {
        int tx = (int)std::floor(x), tz = (int)std::floor(z);
        if (world.tile(tx, tz).building >= 0) return true;
        return world.roofAt(tx, tz) || world.tentAt(Vec2{x, z}) != nullptr;
    };
    Vec2 windDir{0.8f, 0.6f};
    float slant = std::clamp(w.wind * 0.035f, 0.0f, 0.45f);
    if (!snow) {
        // Streaks: thin quads facing the camera, faint at the top, slanting with the wind
        int drops = (int)(1400 * w.rain);
        float fallSpeed = 16.0f, height = 16.0f;
        rlBegin(RL_QUADS);
        for (int i = 0; i < drops; ++i) {
            uint32_t h = hash32(i * 7919 + 1);
            float x = c.x + (hashFloat(h) - 0.5f) * span;
            float z = c.y + (hashFloat(hash32(h)) - 0.5f) * span;
            float y = std::fmod(hashFloat(hash32(h + 5)) * height - time * fallSpeed * (0.85f + 0.3f * hashFloat(h + 9)), height);
            if (y < 0) y += height;
            // each drop lands somewhere new on its next fall
            int cycle = (int)std::floor((hashFloat(hash32(h + 5)) * height - time * fallSpeed) / height);
            x += (hashFloat(hash32(h + (uint32_t)cycle * 31u)) - 0.5f) * 3.0f;
            if (sheltered(x, z)) continue;
            y += world.map().elevation(x, z);
            float len = (0.55f + 0.5f * hashFloat(h + 11)) * (0.6f + 0.6f * w.rain);
            Vector3 bottom = V(x, y, z);
            Vector3 dir = Vector3Normalize(V(windDir.x * slant, 1.0f, windDir.y * slant));
            Vector3 top = Vector3Add(bottom, Vector3Scale(dir, len));
            Vector3 side = Vector3Scale(Vector3Normalize(Vector3CrossProduct(dir, view)), px * 0.9f);
            unsigned char a = (unsigned char)(70 + 90 * w.rain * hashFloat(h + 13));
            rlColor4ub(196, 212, 226, 0);
            rlVertex3f(top.x - side.x, top.y - side.y, top.z - side.z);
            rlColor4ub(196, 212, 226, a);
            rlVertex3f(bottom.x - side.x, bottom.y - side.y, bottom.z - side.z);
            rlVertex3f(bottom.x + side.x, bottom.y + side.y, bottom.z + side.z);
            rlColor4ub(196, 212, 226, 0);
            rlVertex3f(top.x + side.x, top.y + side.y, top.z + side.z);
        }
        rlEnd();
        // Splashes: little rings spreading where the drops hit
        int splashes = (int)(260 * w.rain);
        const float life = 0.35f;
        rlBegin(RL_LINES);
        for (int i = 0; i < splashes; ++i) {
            uint32_t h = hash32(i * 104729 + 7);
            float t = time / life + hashFloat(h);
            int cycle = (int)std::floor(t);
            float age = t - cycle;
            uint32_t k = hash32(h + (uint32_t)cycle * 977u);
            float x = c.x + (hashFloat(k) - 0.5f) * span * 0.8f, z = c.y + (hashFloat(hash32(k)) - 0.5f) * span * 0.8f;
            if (sheltered(x, z)) continue;
            const Tile& tl = world.tile((int)std::floor(x), (int)std::floor(z));
            float gy = (isWater(tl.ground) ? world.map().waterLevel(x, z) : world.map().elevation(x, z)) + groundHeight(tl.ground) + 0.06f;
            float r = 0.04f + 0.22f * age;
            unsigned char a = (unsigned char)(150 * (1.0f - age) * (0.5f + 0.5f * w.rain));
            rlColor4ub(206, 220, 232, a);
            const int n = 10;
            for (int j = 0; j < n; ++j) {
                float a0 = j * 2 * PI / n, a1 = (j + 1) * 2 * PI / n;
                rlVertex3f(x + std::cos(a0) * r, gy, z + std::sin(a0) * r * 0.9f);
                rlVertex3f(x + std::cos(a1) * r, gy, z + std::sin(a1) * r * 0.9f);
            }
        }
        rlEnd();
    } else {
        // Snow: soft flakes drifting down, swaying
        int flakes = (int)(1100 * w.rain);
        float height = 14.0f;
        Vector3 right = Vector3Normalize(Vector3CrossProduct(view, V(0, 1, 0)));
        Vector3 up = Vector3CrossProduct(right, view);
        rlBegin(RL_QUADS);
        for (int i = 0; i < flakes; ++i) {
            uint32_t h = hash32(i * 7919 + 3);
            float x = c.x + (hashFloat(h) - 0.5f) * span;
            float z = c.y + (hashFloat(hash32(h)) - 0.5f) * span;
            float speed = 0.8f + 0.7f * hashFloat(h + 3);
            float y = std::fmod(hashFloat(hash32(h + 5)) * height - time * speed, height);
            if (y < 0) y += height;
            x += std::sin(time * 0.9f + i * 0.37f) * 0.45f + windDir.x * slant * y;
            z += std::cos(time * 0.7f + i * 0.21f) * 0.3f + windDir.y * slant * y;
            if (sheltered(x, z)) continue;
            y += world.map().elevation(x, z);
            float r = px * (1.6f + 1.8f * hashFloat(h + 7));
            Vector3 p = V(x, y, z);
            Vector3 a = Vector3Scale(right, r), b = Vector3Scale(up, r);
            unsigned char al = (unsigned char)(180 + 60 * hashFloat(h + 9));
            rlColor4ub(246, 248, 252, al);
            rlVertex3f(p.x - a.x - b.x, p.y - a.y - b.y, p.z - a.z - b.z);
            rlVertex3f(p.x + a.x - b.x, p.y + a.y - b.y, p.z + a.z - b.z);
            rlVertex3f(p.x + a.x + b.x, p.y + a.y + b.y, p.z + a.z + b.z);
            rlVertex3f(p.x - a.x + b.x, p.y - a.y + b.y, p.z - a.z + b.z);
        }
        rlEnd();
    }
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

void Renderer::drawTent(const Tent& t, bool inside) {
    static const uint32_t cols[] = {0x4a7a3a, 0xd0702a, 0x2a5a8a, 0xb03030};
    Color fab = hexColor(cols[t.id % 4]);
    float x0 = (float)t.rect.x, z0 = (float)t.rect.y, x1 = (float)t.rect.right(), z1 = (float)t.rect.bottom();
    // groundsheet
    drawPrim(cube_, V((x0 + x1) * 0.5f, 0.01f, (z0 + z1) * 0.5f), V(x1 - x0 - 0.05f, 0.02f, z1 - z0 - 0.05f), 0, shade(fab, 0.5f));
    if (inside) return;   // inside it: see yourself
    if (t.rect.w == t.rect.h) {
        // a dome
        drawBlob(V((x0 + x1) * 0.5f, 0.0f, (z0 + z1) * 0.5f), V((x1 - x0) * 0.5f, 1.25f, (z1 - z0) * 0.5f), fab);
        Vec2 door = t.rect.centre() + Vec2{t.dir == 1 ? 1.0f : t.dir == 3 ? -1.0f : 0.0f, t.dir == 2 ? 1.0f : t.dir == 0 ? -1.0f : 0.0f} * (t.rect.w * 0.45f);
        drawBlob(V(door.x, 0.35f, door.y), V(0.32f, 0.55f, 0.32f), shade(fab, 0.45f));
        for (int i = 0; i < 2; ++i) {   // poles crossing over the top
            float a = i * PI * 0.5f + PI * 0.25f;
            Vector3 p0 = V(t.rect.centre().x + std::cos(a) * (x1 - x0) * 0.5f, 0, t.rect.centre().y + std::sin(a) * (z1 - z0) * 0.5f);
            Vector3 p1 = V(t.rect.centre().x, 1.27f, t.rect.centre().y);
            drawTube(p0, p1, 0.02f, hexColor(0x2a2a2c));
        }
    } else {
        // a low tunnel: an A-frame along its length
        bool alongX = t.rect.w > t.rect.h;
        Vector3 c = V((x0 + x1) * 0.5f, 0, (z0 + z1) * 0.5f);
        float len = alongX ? (x1 - x0) : (z1 - z0);
        for (int side = -1; side <= 1; side += 2) {
            float yaw = alongX ? 0 : PI * 0.5f;
            drawPrim(cube_, V(c.x + (alongX ? 0 : side * 0.24f), 0.42f, c.z + (alongX ? side * 0.24f : 0)),
                     V(alongX ? len - 0.1f : 0.04f, 0.95f, alongX ? 0.04f : len - 0.1f), 0,
                     shade(fab, side < 0 ? 0.9f : 1.0f), alongX ? side * 0.55f : 0, alongX ? 0 : -side * 0.55f);
            (void)yaw;
        }
    }
}

void Renderer::drawStove(const Campfire& f, float time, bool shadowPass) {
    Vector3 p = V(f.pos.x, 0, f.pos.y);
    Color alu = hexColor(0xc8ccd0);
    drawPrim(cylinder_, p, V(0.12f, 0.16f, 0.12f), 0, alu);                 // windshield
    drawPrim(cylinder_, V(p.x, 0.16f, p.z), V(0.1f, 0.09f, 0.1f), 0, shade(alu, 0.8f));   // pot
    drawPrim(cube_, V(p.x + 0.16f, 0.22f, p.z), V(0.12f, 0.015f, 0.025f), 0, hexColor(0x2a2a2c));   // handle
    if (f.lit && !shadowPass) {
        setUnlit(true);
        float fl = 0.85f + 0.15f * std::sin(time * 23.0f + f.id);
        drawPrim(cone_, V(p.x, 0.08f, p.z), V(0.06f, 0.1f * fl, 0.06f), time, hexColor(0x5a8aff));
        setUnlit(false);
    }
}

void Renderer::drawBuildGhost(const HoverInfo& h, float time) {
    if (h.buildKind < 0) return;
    Color c = h.buildOk ? Color{90, 220, 110, 255} : Color{230, 70, 60, 255};
    c = Fade(c, 0.55f + 0.2f * std::sin(time * 5.0f));
    float ax = (float)h.bx, az = (float)h.by;
    BuildKind k = (BuildKind)h.buildKind;
    float gy = world_ ? world_->map().elevation(ax + 0.5f, az + 0.5f) : 0.0f;
    rlPushMatrix();
    rlTranslatef(0, gy, 0);
    rlDisableDepthTest();
    if (isEdgeBuild(k)) {
        float y1 = k == BuildKind::Door ? 2.1f : 2.5f;
        if (h.bdir == 0) DrawCube(V(ax + 0.5f, y1 * 0.5f, az), 1.0f, y1, 0.12f, c);
        else DrawCube(V(ax, y1 * 0.5f, az + 0.5f), 0.12f, y1, 1.0f, c);
    } else if (k == BuildKind::Roof) {
        DrawCube(V(ax + 0.5f, 2.56f, az + 0.5f), 1.04f, 0.12f, 1.04f, c);
    } else if (k == BuildKind::Floor) {
        DrawCube(V(ax + 0.5f, 0.03f, az + 0.5f), 1.0f, 0.05f, 1.0f, c);
    } else {
        float hgt = k == BuildKind::Bed ? 0.5f : k == BuildKind::Fence ? 1.0f : 0.85f;
        DrawCube(V(ax + 0.5f, hgt * 0.5f, az + 0.5f), 0.9f, hgt, 0.9f, c);
    }
    rlEnableDepthTest();
    rlPopMatrix();
}

void Renderer::drawCursor(Game& game, const HoverInfo& hover, float time) {
    if (hover.buildKind >= 0) { drawBuildGhost(hover, time); return; }
    if (hover.aiming) {
        // An isometric reticle on the ground where you aim, and a dashed line to it
        World& world = game.world();
        Vec2 to = hover.point - hover.from;
        float d = to.length();
        bool inRange = d <= hover.range;
        Color c = inRange ? Color{235, 80, 60, 255} : Color{230, 225, 210, 200};
        auto g = [&](Vec2 p) { return V(p.x, world.standHeight(p) + 0.05f, p.y); };
        rlDisableDepthTest();
        float pulse = 0.08f * std::sin(time * 6.0f);
        for (float r : {0.55f + pulse, 0.22f}) {
            Vec2 q[4] = {hover.point + Vec2{r, 0}, hover.point + Vec2{0, r}, hover.point + Vec2{-r, 0}, hover.point + Vec2{0, -r}};
            for (int i = 0; i < 4; ++i) DrawLine3D(g(q[i]), g(q[(i + 1) % 4]), c);
        }
        // ticks out from the diamond
        for (Vec2 dir : {Vec2{1, 0}, Vec2{0, 1}, Vec2{-1, 0}, Vec2{0, -1}})
            DrawLine3D(g(hover.point + dir * 0.7f), g(hover.point + dir * 0.95f), c);
        Color fill = Fade(c, 0.18f);
        Vec2 p0 = hover.point + Vec2{0.22f, 0}, p1 = hover.point + Vec2{0, 0.22f}, p2 = hover.point + Vec2{-0.22f, 0}, p3 = hover.point + Vec2{0, -0.22f};
        DrawTriangle3D(g(p0), g(p3), g(p2), fill);
        DrawTriangle3D(g(p0), g(p2), g(p1), fill);
        if (d > 0.5f) {
            Vec2 dir = to * (1.0f / d);
            float end = std::min(d - 0.6f, hover.ranged ? d : hover.range);
            for (float t = 0.5f; t < end; t += 0.6f)
                DrawLine3D(g(hover.from + dir * t), g(hover.from + dir * std::min(t + 0.3f, end)), Fade(c, 0.7f));
        }
        rlEnableDepthTest();
        return;
    }
    if (!hover.show) return;
    int hx = (int)std::floor(hover.point.x), hy = (int)std::floor(hover.point.y);
    World& world = game.world();
    const Tile& ht = world.tile(hx, hy);
    bool level = ht.building >= 0 || isWater(ht.ground) || ht.ground == Ground::Deck;
    float flat = world.standHeight(Vec2{hx + 0.5f, hy + 0.5f});
    auto at = [&](float x, float z) { return V(x, (level ? flat : world.map().elevation(x, z)) + 0.04f, z); };
    Color c = hover.colour;
    // translucent fill
    rlDisableDepthTest();
    Color fill = Fade(c, 0.16f + 0.05f * std::sin(time * 4.0f));
    Vector3 cs[4] = {at((float)hx, (float)hy), at(hx + 1.0f, (float)hy), at(hx + 1.0f, hy + 1.0f), at((float)hx, hy + 1.0f)};
    DrawTriangle3D(cs[0], cs[3], cs[2], fill);
    DrawTriangle3D(cs[0], cs[2], cs[1], fill);
    // corner brackets
    float k = 0.3f;
    for (int i = 0; i < 4; ++i) {
        Vector3 a = cs[i], prev = cs[(i + 3) % 4], next = cs[(i + 1) % 4];
        DrawLine3D(a, Vector3Lerp(a, next, k), c);
        DrawLine3D(a, Vector3Lerp(a, prev, k), c);
    }
    rlEnableDepthTest();
}

} // namespace alone
