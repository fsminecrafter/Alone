#pragma once
#include "raylib.h"
#include <string>
#include <vector>

namespace alone::ui {

struct Theme {
    Color panel = {18, 20, 22, 225};
    Color panelLight = {34, 37, 40, 235};
    Color border = {78, 82, 86, 255};
    Color text = {226, 222, 212, 255};
    Color dim = {150, 148, 140, 255};
    Color accent = {214, 168, 92, 255};
    Color good = {140, 196, 120, 255};
    Color bad = {222, 102, 86, 255};
    Color body = {206, 176, 220, 255};
    Color highlight = {214, 168, 92, 60};
};
const Theme& theme();

void init();
void shutdown();
void beginFrame();           // call once per frame before widgets
void endFrame();             // draws tooltips
float scale();               // UI scale factor (screen height / 720, times the user's choice)
void setUserScale(float k);  // Settings > Graphics > Interface size

Font& font(bool bold = false);
void text(const std::string& s, float x, float y, float size, Color c, bool bold = false);
void textCentered(const std::string& s, float cx, float y, float size, Color c, bool bold = false);
Vector2 measure(const std::string& s, float size, bool bold = false);
std::vector<std::string> wrap(const std::string& s, float width, float size, bool bold = false);
float paragraph(const std::string& s, float x, float y, float width, float size, Color c, float lineGap = 1.25f);

void panel(Rectangle r, Color c = theme().panel, bool border = true);
bool hovered(Rectangle r);
bool clicked(Rectangle r, int button = MOUSE_BUTTON_LEFT);
bool button(Rectangle r, const std::string& label, bool enabled = true, float size = 18);
bool slider(Rectangle r, float& value, float lo, float hi);

// UI regions this frame: the game should ignore mouse clicks over them.
void claim(Rectangle r);
bool mouseOverUi();

void tooltip(const std::string& title, const std::string& body, Color titleColour = theme().text);

} // namespace alone::ui
