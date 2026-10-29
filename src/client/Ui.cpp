#include "client/Input.h"
#include "client/Ui.h"
#include "core/Paths.h"
#include <algorithm>
#include <cmath>

namespace alone::ui {

namespace {
Theme g_theme;
Font g_font{}, g_bold{};
bool g_loaded = false;
std::vector<Rectangle> g_claimed, g_claimedPrev;
std::string g_tipTitle, g_tipBody;
Color g_tipColour;
bool g_tip = false;
}

const Theme& theme() { return g_theme; }

static float g_userScale = 1.0f;
void setUserScale(float k) { g_userScale = k; }
float scale() { return std::max(0.75f, GetScreenHeight() / 720.0f) * g_userScale; }

void init() {
    // Latin-1 so Swedish å ä ö and friends render
    std::vector<int> cps;
    for (int c = 32; c < 127; ++c) cps.push_back(c);
    for (int c = 160; c < 256; ++c) cps.push_back(c);
    for (int c : {0x2013, 0x2014, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x00B0, 0x2713, 0x2717, 0x25B8, 0x2605, 0x2665, 0x2302}) cps.push_back(c);
    g_font = LoadFontEx(Paths::data("fonts/DejaVuSans.ttf").c_str(), 40, cps.data(), (int)cps.size());
    g_bold = LoadFontEx(Paths::data("fonts/DejaVuSans-Bold.ttf").c_str(), 40, cps.data(), (int)cps.size());
    if (g_font.texture.id == 0) g_font = GetFontDefault();
    if (g_bold.texture.id == 0) g_bold = g_font;
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_bold.texture, TEXTURE_FILTER_BILINEAR);
    g_loaded = true;
}

void shutdown() {
    if (!g_loaded) return;
    UnloadFont(g_font);
    UnloadFont(g_bold);
}

void beginFrame() {
    g_claimedPrev = g_claimed;
    g_claimed.clear();
    g_tip = false;
}

void endFrame() {
    if (!g_tip) return;
    float s = scale();
    float w = 330 * s;
    float size = 15 * s;
    auto lines = wrap(g_tipBody, w - 20 * s, size);
    float h = 16 * s + (g_tipTitle.empty() ? 0 : 24 * s) + lines.size() * size * 1.3f;
    Vector2 m = GetMousePosition();
    float x = m.x + 18, y = m.y + 18;
    if (x + w > GetScreenWidth()) x = m.x - w - 10;
    if (y + h > GetScreenHeight()) y = GetScreenHeight() - h - 4;
    Rectangle r{x, y, w, h};
    DrawRectangleRec(r, Color{12, 13, 15, 240});
    DrawRectangleLinesEx(r, 1, g_theme.border);
    float ty = y + 8 * s;
    if (!g_tipTitle.empty()) {
        text(g_tipTitle, x + 10 * s, ty, 18 * s, g_tipColour, true);
        ty += 24 * s;
    }
    for (const auto& l : lines) {
        text(l, x + 10 * s, ty, size, g_theme.text);
        ty += size * 1.3f;
    }
}

Font& font(bool bold) { return bold ? g_bold : g_font; }

void text(const std::string& s, float x, float y, float size, Color c, bool bold) {
    DrawTextEx(font(bold), s.c_str(), {std::round(x), std::round(y)}, size, 0.5f, c);
}

void textCentered(const std::string& s, float cx, float y, float size, Color c, bool bold) {
    Vector2 m = measure(s, size, bold);
    text(s, cx - m.x / 2, y, size, c, bold);
}

Vector2 measure(const std::string& s, float size, bool bold) {
    return MeasureTextEx(font(bold), s.c_str(), size, 0.5f);
}

std::vector<std::string> wrap(const std::string& s, float width, float size, bool bold) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string cand = line.empty() ? word : line + " " + word;
        if (measure(cand, size, bold).x > width && !line.empty()) {
            lines.push_back(line);
            line = word;
        } else {
            line = cand;
        }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flushWord();
        else if (c == '\n') { flushWord(); lines.push_back(line); line.clear(); }
        else word += c;
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

float paragraph(const std::string& s, float x, float y, float width, float size, Color c, float gap) {
    for (const auto& l : wrap(s, width, size)) {
        text(l, x, y, size, c);
        y += size * gap;
    }
    return y;
}

void panel(Rectangle r, Color c, bool border) {
    DrawRectangleRec(r, c);
    if (border) DrawRectangleLinesEx(r, 1, g_theme.border);
    claim(r);
}

bool hovered(Rectangle r) { return CheckCollisionPointRec(GetMousePosition(), r); }

bool clicked(Rectangle r, int b) { return hovered(r) && input::pressed(b); }

bool button(Rectangle r, const std::string& label, bool enabled, float size) {
    bool h = enabled && hovered(r);
    Color bg = !enabled ? Color{30, 30, 32, 220} : h ? Color{70, 60, 44, 240} : g_theme.panelLight;
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, h ? g_theme.accent : g_theme.border);
    float s = size * scale();
    Vector2 m = measure(label, s);
    text(label, r.x + (r.width - m.x) / 2, r.y + (r.height - m.y) / 2, s, enabled ? g_theme.text : g_theme.dim);
    claim(r);
    return h && input::pressed(MOUSE_BUTTON_LEFT);
}

bool slider(Rectangle r, float& v, float lo, float hi) {
    DrawRectangleRec(r, g_theme.panelLight);
    DrawRectangleLinesEx(r, 1, g_theme.border);
    float t = (v - lo) / (hi - lo);
    DrawRectangle((int)r.x, (int)r.y, (int)(r.width * t), (int)r.height, Color{140, 110, 60, 255});
    claim(r);
    if (hovered(r) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        float nt = std::clamp((GetMousePosition().x - r.x) / r.width, 0.0f, 1.0f);
        v = lo + nt * (hi - lo);
        return true;
    }
    return false;
}

void claim(Rectangle r) { g_claimed.push_back(r); }

bool mouseOverUi() {
    Vector2 m = GetMousePosition();
    for (const auto& r : g_claimedPrev)
        if (CheckCollisionPointRec(m, r)) return true;
    for (const auto& r : g_claimed)
        if (CheckCollisionPointRec(m, r)) return true;
    return false;
}

void tooltip(const std::string& title, const std::string& body, Color tc) {
    g_tip = true;
    g_tipTitle = title;
    g_tipBody = body;
    g_tipColour = tc;
}

} // namespace alone::ui
