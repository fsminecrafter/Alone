#include "client/Icons.h"
#include "client/MeshBuilder.h"
#include "client/Ui.h"
#include <cmath>

namespace alone {

void drawItemIcon(const ItemDef* def, Rectangle r) {
    if (!def) return;
    Color c = hexColor(def->colour);
    Color dark = shade(c, 0.6f), light = shade(c, 1.3f);
    float cx = r.x + r.width / 2, cy = r.y + r.height / 2, s = r.width / 2;
    const std::string& i = def->icon;
    auto rect = [&](float x, float y, float w, float h, Color col) {
        DrawRectangleRec({cx + x * s, cy + y * s, w * s, h * s}, col);
    };
    if (i == "can") {
        rect(-0.5f, -0.6f, 1.0f, 1.2f, c);
        rect(-0.5f, -0.6f, 1.0f, 0.15f, LIGHTGRAY);
        rect(-0.5f, 0.45f, 1.0f, 0.15f, LIGHTGRAY);
        rect(-0.5f, -0.15f, 1.0f, 0.3f, light);
    } else if (i == "bottle") {
        rect(-0.3f, -0.4f, 0.6f, 1.1f, Fade(c, 0.9f));
        rect(-0.15f, -0.75f, 0.3f, 0.35f, c);
        rect(-0.18f, -0.85f, 0.36f, 0.12f, DARKBLUE);
    } else if (i == "carton" || i == "jar" || i == "tube") {
        rect(-0.4f, -0.6f, 0.8f, 1.2f, c);
        rect(-0.4f, -0.6f, 0.8f, 0.25f, dark);
    } else if (i == "meat" || i == "fish") {
        DrawEllipse((int)cx, (int)cy, s * 0.8f, s * 0.5f, c);
        DrawEllipse((int)(cx - s * 0.2f), (int)(cy - s * 0.1f), s * 0.35f, s * 0.2f, light);
        if (i == "fish") DrawTriangle({cx + s * 0.7f, cy}, {cx + s, cy + s * 0.4f}, {cx + s, cy - s * 0.4f}, c);
    } else if (i == "fat" || i == "cheese") {
        rect(-0.6f, -0.35f, 1.2f, 0.7f, c);
        rect(-0.6f, -0.35f, 1.2f, 0.18f, light);
    } else if (i == "berry") {
        for (int k = 0; k < 6; ++k) {
            float a = k * 1.05f;
            DrawCircle((int)(cx + std::cos(a) * s * 0.35f), (int)(cy + std::sin(a) * s * 0.35f), s * 0.25f, c);
        }
    } else if (i == "mushroom") {
        rect(-0.15f, -0.1f, 0.3f, 0.7f, Color{230, 222, 200, 255});
        DrawCircleSector({cx, cy - s * 0.05f}, s * 0.65f, 180, 360, 12, c);
        if (def->id == "fly_agaric") {
            DrawCircle((int)(cx - s * 0.25f), (int)(cy - s * 0.35f), s * 0.08f, WHITE);
            DrawCircle((int)(cx + s * 0.2f), (int)(cy - s * 0.45f), s * 0.08f, WHITE);
        }
    } else if (i == "bread" || i == "box" || i == "bag") {
        rect(-0.55f, -0.5f, 1.1f, 1.0f, c);
        rect(-0.55f, -0.5f, 1.1f, 0.2f, dark);
    } else if (i == "bar") {
        rect(-0.7f, -0.25f, 1.4f, 0.5f, c);
        rect(-0.7f, -0.25f, 0.5f, 0.5f, Color{180, 40, 40, 255});
    } else if (i == "potato" || i == "fruit") {
        DrawCircle((int)cx, (int)cy, s * 0.55f, c);
        DrawCircle((int)(cx - s * 0.15f), (int)(cy - s * 0.15f), s * 0.15f, light);
    } else if (i == "carrot") {
        DrawTriangle({cx - s * 0.2f, cy - s * 0.5f}, {cx, cy + s * 0.7f}, {cx + s * 0.2f, cy - s * 0.5f}, c);
        rect(-0.1f, -0.75f, 0.2f, 0.25f, GREEN);
    } else if (i == "pot") {
        rect(-0.6f, -0.3f, 1.2f, 0.8f, c);
        rect(-0.8f, -0.3f, 0.2f, 0.12f, dark);
        rect(0.6f, -0.3f, 0.2f, 0.12f, dark);
    } else if (i == "knife") {
        DrawTriangle({cx - s * 0.7f, cy + s * 0.1f}, {cx + s * 0.4f, cy + s * 0.1f}, {cx + s * 0.4f, cy - s * 0.2f}, c);
        rect(0.4f, -0.12f, 0.45f, 0.3f, Color{90, 60, 40, 255});
    } else if (i == "axe" || i == "hammer" || i == "crowbar") {
        DrawLineEx({cx - s * 0.6f, cy + s * 0.7f}, {cx + s * 0.4f, cy - s * 0.5f}, s * 0.15f, i == "crowbar" ? c : Color{120, 90, 60, 255});
        if (i != "crowbar") rect(0.15f, -0.75f, 0.5f, 0.4f, i == "axe" ? LIGHTGRAY : DARKGRAY);
    } else if (i == "rifle") {
        DrawLineEx({cx - s * 0.9f, cy + s * 0.2f}, {cx + s * 0.9f, cy - s * 0.2f}, s * 0.12f, DARKGRAY);
        DrawLineEx({cx - s * 0.9f, cy + s * 0.25f}, {cx - s * 0.1f, cy + s * 0.1f}, s * 0.25f, c);
    } else if (i == "ammo") {
        for (int k = -1; k <= 1; ++k) rect(k * 0.35f - 0.1f, -0.5f, 0.2f, 1.0f, c);
    } else if (i == "shirt" || i == "sweater" || i == "jacket") {
        rect(-0.45f, -0.5f, 0.9f, 1.0f, c);
        rect(-0.8f, -0.5f, 0.35f, i == "shirt" ? 0.4f : 0.9f, c);
        rect(0.45f, -0.5f, 0.35f, i == "shirt" ? 0.4f : 0.9f, c);
        if (i == "jacket") rect(-0.03f, -0.5f, 0.06f, 1.0f, dark);
    } else if (i == "pants") {
        rect(-0.45f, -0.6f, 0.9f, 0.3f, c);
        rect(-0.45f, -0.3f, 0.38f, 0.9f, c);
        rect(0.07f, -0.3f, 0.38f, 0.9f, c);
    } else if (i == "boot") {
        rect(-0.4f, -0.6f, 0.4f, 1.0f, c);
        rect(-0.4f, 0.2f, 0.9f, 0.4f, c);
    } else if (i == "hat" || i == "glove") {
        DrawCircleSector({cx, cy + s * 0.2f}, s * 0.6f, 180, 360, 12, c);
        rect(-0.6f, 0.15f, 1.2f, 0.2f, dark);
    } else if (i == "pills") {
        rect(-0.35f, -0.5f, 0.7f, 1.0f, c);
        rect(-0.35f, -0.65f, 0.7f, 0.2f, WHITE);
    } else if (i == "bandage") {
        DrawCircle((int)cx, (int)cy, s * 0.6f, c);
        DrawCircle((int)cx, (int)cy, s * 0.2f, dark);
    } else if (i == "book") {
        rect(-0.5f, -0.65f, 1.0f, 1.3f, c);
        rect(-0.4f, -0.55f, 0.8f, 0.2f, WHITE);
    } else if (i == "log" || i == "stick" || i == "rope" || i == "wire") {
        DrawLineEx({cx - s * 0.7f, cy + s * 0.4f}, {cx + s * 0.7f, cy - s * 0.4f}, s * (i == "log" ? 0.45f : 0.15f), c);
    } else if (i == "lighter" || i == "flashlight" || i == "tool") {
        rect(-0.25f, -0.6f, 0.5f, 1.2f, c);
        rect(-0.25f, -0.6f, 0.5f, 0.25f, LIGHTGRAY);
    } else {
        rect(-0.5f, -0.5f, 1.0f, 1.0f, c);
    }
}

void drawSymptomIcon(const std::string& id, Rectangle r, Color c) {
    float cx = r.x + r.width / 2, cy = r.y + r.height / 2, s = r.width / 2;
    auto line = [&](float x0, float y0, float x1, float y1, float w = 0.1f) {
        DrawLineEx({cx + x0 * s, cy + y0 * s}, {cx + x1 * s, cy + y1 * s}, std::max(1.5f, w * s), c);
    };
    auto glyph = [&](const char* g, float size = 1.1f) {
        float sz = s * size;
        Vector2 m = ui::measure(g, sz, true);
        ui::text(g, cx - m.x / 2, cy - m.y / 2, sz, c, true);
    };
    if (id == "hunger") {
        line(-0.35f, -0.6f, -0.35f, 0.6f); line(-0.5f, -0.6f, -0.5f, -0.2f); line(-0.2f, -0.6f, -0.2f, -0.2f);
        line(0.35f, -0.6f, 0.35f, 0.6f, 0.14f); DrawEllipse((int)(cx + 0.35f * s), (int)(cy - 0.35f * s), s * 0.15f, s * 0.28f, c);
    } else if (id == "craving") {
        DrawEllipse((int)(cx - 0.1f * s), (int)(cy - 0.15f * s), s * 0.45f, s * 0.38f, c);
        line(0.2f, 0.15f, 0.6f, 0.6f, 0.15f);
    } else if (id == "thirst" || id == "bleeding" || id == "skin") {
        DrawCircle((int)cx, (int)(cy + 0.15f * s), s * 0.42f, c);
        DrawTriangle({cx, cy - 0.7f * s}, {cx - 0.4f * s, cy + 0.05f * s}, {cx + 0.4f * s, cy + 0.05f * s}, c);
    } else if (id == "full") {
        DrawCircleLines((int)cx, (int)cy, s * 0.6f, c);
        DrawCircle((int)cx, (int)cy, s * 0.35f, c);
    } else if (id == "nausea") {
        for (int k = 0; k < 3; ++k) {
            float y = -0.4f + k * 0.4f;
            for (int j = 0; j < 6; ++j) {
                float x0 = -0.6f + j * 0.2f, x1 = x0 + 0.2f;
                line(x0, y + ((j & 1) ? 0.1f : -0.1f), x1, y + ((j & 1) ? -0.1f : 0.1f), 0.09f);
            }
        }
    } else if (id == "cramps") {
        line(-0.1f, -0.7f, -0.35f, 0.0f, 0.13f); line(-0.35f, 0.0f, 0.15f, -0.05f, 0.13f); line(0.15f, -0.05f, -0.1f, 0.7f, 0.13f);
    } else if (id == "weakness") {
        line(-0.6f, 0.4f, 0.0f, 0.4f, 0.16f); line(0.0f, 0.4f, 0.3f, -0.1f, 0.16f);
        DrawCircle((int)(cx + 0.38f * s), (int)(cy - 0.25f * s), s * 0.18f, c);
    } else if (id == "headache") {
        DrawCircleLines((int)cx, (int)(cy + 0.1f * s), s * 0.5f, c);
        line(-0.2f, -0.75f, 0.0f, -0.5f); line(0.0f, -0.5f, 0.15f, -0.75f); line(0.15f, -0.75f, 0.35f, -0.5f);
    } else if (id == "dizziness") {
        for (int k = 0; k < 40; ++k) {
            float a = k * 0.35f, rr = 0.05f + k * 0.016f;
            float a2 = (k + 1) * 0.35f, rr2 = 0.05f + (k + 1) * 0.016f;
            line(std::cos(a) * rr, std::sin(a) * rr, std::cos(a2) * rr2, std::sin(a2) * rr2, 0.08f);
        }
    } else if (id == "confusion") {
        glyph("?", 1.3f);
    } else if (id == "cold") {
        for (int k = 0; k < 3; ++k) {
            float a = k * PI / 3;
            line(std::cos(a) * 0.65f, std::sin(a) * 0.65f, -std::cos(a) * 0.65f, -std::sin(a) * 0.65f, 0.1f);
        }
    } else if (id == "hot") {
        DrawCircle((int)cx, (int)cy, s * 0.32f, c);
        for (int k = 0; k < 8; ++k) {
            float a = k * PI / 4;
            line(std::cos(a) * 0.45f, std::sin(a) * 0.45f, std::cos(a) * 0.7f, std::sin(a) * 0.7f, 0.09f);
        }
    } else if (id == "fever") {
        line(0, -0.65f, 0, 0.3f, 0.2f);
        DrawCircle((int)cx, (int)(cy + 0.42f * s), s * 0.22f, c);
    } else if (id == "tired") {
        glyph("Zz", 0.95f);
    } else if (id == "pain") {
        line(0.2f, -0.7f, -0.25f, 0.05f, 0.14f); line(-0.25f, 0.05f, 0.25f, 0.0f, 0.14f); line(0.25f, 0.0f, -0.2f, 0.7f, 0.14f);
    } else if (id == "aches") {
        line(-0.45f, 0.45f, 0.45f, -0.45f, 0.16f);
        DrawCircle((int)(cx - 0.5f * s), (int)(cy + 0.5f * s), s * 0.15f, c);
        DrawCircle((int)(cx + 0.5f * s), (int)(cy - 0.5f * s), s * 0.15f, c);
    } else if (id == "mouth") {
        DrawRectangleRounded({cx - 0.35f * s, cy - 0.5f * s, 0.7f * s, 0.7f * s}, 0.4f, 6, c);
        line(-0.25f, 0.2f, -0.2f, 0.65f, 0.14f); line(0.25f, 0.2f, 0.2f, 0.65f, 0.14f);
    } else if (id == "glands") {
        DrawCircle((int)cx, (int)cy, s * 0.45f, c);
    } else if (id == "hallucination") {
        DrawEllipseLines((int)cx, (int)cy, s * 0.7f, s * 0.38f, c);
        DrawCircle((int)cx, (int)cy, s * 0.22f, c);
    } else {
        glyph("!", 1.2f);
    }
}

} // namespace alone
