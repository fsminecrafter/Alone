// Making your character at the start of a new game: name, gender, skin,
// hair, and what you're wearing when you wake up. You stand in the house you
// start in, so what you see is how you'll look in the game.
#include "client/App.h"
#include "client/Input.h"
#include "client/MeshBuilder.h"
#include "client/Ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace alone {

namespace {

const uint32_t kSkins[] = {0xf3d6c0, 0xe8c4a8, 0xe2b896, 0xd4a07a, 0xb8865e, 0x8d5a3b, 0x6a4028, 0x4a2c1c};
const uint32_t kHairs[] = {0x1a1410, 0x3a2a20, 0x5a4030, 0x8a6a48, 0xb88a4a, 0xd8c080, 0xc8c8c4, 0x9a3a20};
// 0 = the item's own colour
const uint32_t kDyes[] = {0, 0x2a2a2e, 0xe8e4dc, 0x7a7a80, 0x2a3a5a, 0x8a2a2a, 0x3e5a36, 0x6a4a2a, 0xc8a030, 0xb06a8a};

struct SlotChoice { ClothingSlot slot; const char* label; std::vector<const char*> items; };
const std::vector<SlotChoice>& slotChoices() {
    static const std::vector<SlotChoice> v = {
        {ClothingSlot::Head, "Head", {"", "cap", "wool_hat"}},
        {ClothingSlot::Torso, "Shirt", {"tshirt", "flannel_shirt", "thermal_top"}},
        {ClothingSlot::Sweater, "Sweater", {"", "fleece", "hoodie", "wool_sweater", "cardigan"}},
        {ClothingSlot::Jacket, "Jacket", {"", "rain_jacket", "work_jacket"}},
        {ClothingSlot::Legs, "Trousers", {"jeans", "sweatpants", "overalls"}},
        {ClothingSlot::Feet, "Shoes", {"sneakers", "hiking_boots", "rubber_boots", "slippers"}},
    };
    return v;
}

const char* kManNames[] = {"Erik", "Johan", "Oskar", "Lars", "Anders", "Mikael", "Karl", "Nils", "Viktor", "Ali"};
const char* kWomanNames[] = {"Anna", "Maja", "Sara", "Karin", "Emma", "Ida", "Linnea", "Frida", "Elsa", "Amira"};
const char* kOtherNames[] = {"Alex", "Robin", "Kim", "Sam", "Charlie", "Love", "Noa", "Mika", "Eli", "Ronja"};

bool swatch(Rectangle r, Color c, bool on) {
    bool hov = ui::hovered(r);
    DrawRectangleRounded(r, 0.3f, 4, c);
    if (on) DrawRectangleRoundedLinesEx(r, 0.3f, 4, 2.5f, ui::theme().accent);
    else if (hov) DrawRectangleRoundedLinesEx(r, 0.3f, 4, 1.5f, ui::theme().text);
    return hov && input::pressed(MOUSE_BUTTON_LEFT);
}

} // namespace

void App::randomCharacter() {
    auto rnd = [](int n) { return GetRandomValue(0, n - 1); };
    CharacterChoice& c = create_;
    c.look.gender = (Gender)rnd(3);
    const char** names = c.look.gender == Gender::Man ? kManNames : c.look.gender == Gender::Woman ? kWomanNames : kOtherNames;
    c.look.name = names[rnd(10)];
    c.look.skin = kSkins[rnd(8)];
    c.look.hair = kHairs[rnd(8)];
    c.look.hairStyle = c.look.gender == Gender::Woman ? (rnd(4) == 0 ? 0 : 1 + rnd(3)) : c.look.gender == Gender::Man ? (rnd(4) == 0 ? 4 : 0) : rnd(kHairStyles);
    for (const auto& sc : slotChoices()) {
        c.wear[(size_t)sc.slot] = sc.items[(size_t)rnd((int)sc.items.size())];
        c.tint[(size_t)sc.slot] = rnd(3) == 0 ? kDyes[rnd(10)] : 0;
    }
}

void App::beginGame() {
    if (create_.look.name.empty()) create_.look.name = "Alex";
    game_.dressPlayer(create_);
    saveSettings();   // remembers the character for next time
    state_ = State::Playing;
    overlay_ = Overlay::None;
    nameFocus_ = false;
    renderer_.setZoom(30);
    renderer_.snapTo(game_.player().pos);
    game_.log("You are " + create_.look.name + ".", MessageKind::Info);
}

void App::updateCreate(float dt) {
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (nameFocus_) nameFocus_ = false;
        else { state_ = State::Menu; renderer_.setZoom(30); return; }
    }
    // Typing the name
    if (nameFocus_) {
        for (int ch = GetCharPressed(); ch > 0; ch = GetCharPressed()) {
            if (create_.look.name.size() >= 20) continue;
            // UTF-8 for Swedish letters
            if (ch < 0x80) { if (ch >= 32) create_.look.name += (char)ch; }
            else if (ch < 0x800) { create_.look.name += (char)(0xC0 | (ch >> 6)); create_.look.name += (char)(0x80 | (ch & 0x3F)); }
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !create_.look.name.empty()) {
            // drop a whole UTF-8 character
            size_t n = create_.look.name.size() - 1;
            while (n > 0 && ((unsigned char)create_.look.name[n] & 0xC0) == 0x80) --n;
            create_.look.name.erase(n);
        }
        if (IsKeyPressed(KEY_ENTER)) nameFocus_ = false;
    } else {
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) createSpin_ -= dt * 2.5f;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) createSpin_ += dt * 2.5f;
        if (IsKeyPressed(KEY_ENTER)) { beginGame(); return; }
    }
    // Drag on the figure to turn it
    float W = (float)GetScreenWidth();
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && GetMouseX() > 480 * ui::scale()) createSpin_ += GetMouseDelta().x * 0.012f;

    // Stand facing the camera; the camera sits a little to the side of you
    Player& p = game_.player();
    Vec2 toCam = renderer_.screenToWorldDir(0, 1);
    p.facing = std::atan2(toCam.y, toCam.x) + createSpin_;
    Vec2 right = renderer_.screenToWorldDir(1, 0);
    float metresPerPx = 4.2f / std::max(1.0f, (float)GetScreenHeight());
    float shift = std::max(0.0f, (480 * ui::scale() + (W - 480 * ui::scale()) * 0.5f) - W * 0.5f) * metresPerPx;
    renderer_.follow(p.pos - right * shift + Vec2{0, 0}, dt);
}

void App::drawCreate() {
    float s = ui::scale();
    float H = (float)GetScreenHeight();
    CharacterChoice before = create_;
    std::string beforeName = create_.look.name;
    Rectangle p{16 * s, 16 * s, 460 * s, std::min(H - 32 * s, 600 * s)};
    ui::panel(p, Color{16, 17, 19, 236});
    ui::claim(p);
    float x = p.x + 18 * s, y = p.y + 14 * s, w = p.width - 36 * s;
    ui::text("Who are you?", x, y, 26 * s, ui::theme().text, true);
    y += 40 * s;
    float lw = 86 * s;   // label column
    auto label = [&](const char* t, float yy) { ui::text(t, x, yy + 5 * s, 15 * s, ui::theme().dim); };

    // Name
    label("Name", y);
    Rectangle nr{x + lw, y, w - lw, 30 * s};
    bool hovName = ui::hovered(nr);
    DrawRectangleRounded(nr, 0.15f, 4, nameFocus_ ? Color{44, 42, 36, 255} : Color{30, 31, 34, 255});
    DrawRectangleRoundedLinesEx(nr, 0.15f, 4, 1.5f, nameFocus_ ? ui::theme().accent : hovName ? ui::theme().text : ui::theme().border);
    std::string shown = create_.look.name;
    if (nameFocus_ && std::fmod(time_, 1.0f) < 0.55f) shown += "|";
    if (create_.look.name.empty() && !nameFocus_) ui::text("Type a name...", nr.x + 8 * s, nr.y + 6 * s, 16 * s, ui::theme().dim);
    else ui::text(shown, nr.x + 8 * s, nr.y + 6 * s, 16 * s, ui::theme().text);
    if (input::pressed(MOUSE_BUTTON_LEFT)) nameFocus_ = hovName;
    y += 42 * s;

    // Gender
    label("Gender", y);
    {
        const Gender gs[] = {Gender::Man, Gender::Woman, Gender::Other};
        float bw = (w - lw - 12 * s) / 3;
        for (int i = 0; i < 3; ++i) {
            Rectangle r{x + lw + i * (bw + 6 * s), y, bw, 30 * s};
            bool on = create_.look.gender == gs[i];
            if (on) DrawRectangleRounded(r, 0.2f, 4, Color{80, 64, 40, 255});
            if (ui::button(r, genderName(gs[i]), true, 15)) {
                if (create_.look.gender != gs[i]) {
                    create_.look.gender = gs[i];
                    // a sensible hairstyle to start from
                    if (gs[i] == Gender::Woman && create_.look.hairStyle == 0) create_.look.hairStyle = 1;
                    if (gs[i] == Gender::Man && (create_.look.hairStyle == 1 || create_.look.hairStyle == 3)) create_.look.hairStyle = 0;
                }
            }
            if (on) DrawRectangleRoundedLinesEx(r, 0.2f, 4, 2, ui::theme().accent);
        }
    }
    y += 42 * s;

    // Skin and hair colour
    label("Skin", y);
    for (int i = 0; i < 8; ++i)
        if (swatch({x + lw + i * 32 * s, y + 2 * s, 26 * s, 26 * s}, hexColor(kSkins[i]), create_.look.skin == kSkins[i])) create_.look.skin = kSkins[i];
    y += 38 * s;
    label("Hair", y);
    for (int i = 0; i < 8; ++i)
        if (swatch({x + lw + i * 32 * s, y + 2 * s, 26 * s, 26 * s}, hexColor(kHairs[i]), create_.look.hair == kHairs[i])) create_.look.hair = kHairs[i];
    y += 38 * s;
    label("Hairstyle", y);
    {
        float bw = (w - lw - 4 * 4 * s) / kHairStyles;
        for (int i = 0; i < kHairStyles; ++i) {
            Rectangle r{x + lw + i * (bw + 4 * s), y, bw, 30 * s};
            bool on = create_.look.hairStyle == i;
            if (on) DrawRectangleRounded(r, 0.2f, 4, Color{80, 64, 40, 255});
            if (ui::button(r, hairStyleName(i), true, 13)) create_.look.hairStyle = i;
            if (on) DrawRectangleRoundedLinesEx(r, 0.2f, 4, 2, ui::theme().accent);
        }
    }
    y += 46 * s;

    // Clothes: pick the item with the arrows, dye it with the swatches
    ui::text("Clothes", x, y, 17 * s, ui::theme().accent, true);
    y += 26 * s;
    for (const auto& sc : slotChoices()) {
        size_t sl = (size_t)sc.slot;
        label(sc.label, y);
        int cur = 0;
        for (size_t i = 0; i < sc.items.size(); ++i)
            if (create_.wear[sl] == sc.items[i]) cur = (int)i;
        int n = (int)sc.items.size();
        Rectangle l{x + lw, y, 24 * s, 28 * s}, r{x + lw + 176 * s, y, 24 * s, 28 * s};
        if (ui::button(l, "<", true, 15)) cur = (cur + n - 1) % n;
        if (ui::button(r, ">", true, 15)) cur = (cur + 1) % n;
        create_.wear[sl] = sc.items[(size_t)cur];
        const ItemDef* d = create_.wear[sl].empty() ? nullptr : game_.items().find(create_.wear[sl]);
        ui::textCentered(d ? d->name : "(nothing)", x + lw + 100 * s, y + 6 * s, 14 * s, d ? ui::theme().text : ui::theme().dim);
        if (d) {
            float sx = x + lw + 208 * s;
            for (int i = 0; i < 10; ++i) {
                Color c = hexColor(kDyes[i] ? kDyes[i] : d->colour);
                Rectangle sr{sx + i * 13.2f * s, y + 6 * s, 11 * s, 16 * s};
                if (swatch(sr, c, create_.tint[sl] == kDyes[i])) create_.tint[sl] = kDyes[i];
                if (i == 0 && ui::hovered(sr)) ui::tooltip("Original colour", "");
            }
        }
        y += 34 * s;
    }
    y += 4 * s;
    ui::paragraph("Drag the figure, or A/D, to turn around.", x, y, w, 13 * s, ui::theme().dim);

    // Buttons
    float by = p.y + p.height - 50 * s;
    if (ui::button({x, by, 96 * s, 36 * s}, "Back", true, 16)) { state_ = State::Menu; renderer_.setZoom(30); return; }
    if (ui::button({x + 104 * s, by, 130 * s, 36 * s}, "Randomise", true, 16)) randomCharacter();
    if (ui::button({x + w - 150 * s, by, 150 * s, 36 * s}, "Wake up", !create_.look.name.empty(), 18)) { beginGame(); return; }

    // Apply changes to the figure in the room
    bool changed = beforeName != create_.look.name || before.look.gender != create_.look.gender || before.look.skin != create_.look.skin ||
                   before.look.hair != create_.look.hair || before.look.hairStyle != create_.look.hairStyle || before.wear != create_.wear ||
                   before.tint != create_.tint;
    if (changed || game_.player().look.name != create_.look.name) game_.dressPlayer(create_);
    // Name under the figure
    Vector2 sp = renderer_.toScreen(game_.player().pos, 0.0f);
    ui::textCentered(create_.look.name.empty() ? "?" : create_.look.name, sp.x, sp.y + 14 * s, 20 * s, ui::theme().text, true);
}

} // namespace alone
