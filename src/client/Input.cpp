#include "client/Input.h"
#include "client/Ui.h"
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// raylib runs on GLFW (the desktop library, or Emscripten's version of it in
// the browser) and links it in; these are the few calls used here.
extern "C" {
struct GLFWwindow;
struct GLFWcursor;
struct GLFWimage { int width; int height; unsigned char* pixels; };
typedef void (*GLFWmousebuttonfun)(GLFWwindow*, int, int, int);
GLFWwindow* glfwGetCurrentContext(void);
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* window, GLFWmousebuttonfun callback);
#ifndef __EMSCRIPTEN__
GLFWcursor* glfwCreateCursor(const GLFWimage* image, int xhot, int yhot);
void glfwSetCursor(GLFWwindow* window, GLFWcursor* cursor);
void glfwDestroyCursor(GLFWcursor* cursor);
#endif
}

namespace alone::input {

namespace {

constexpr int kButtons = 8;
bool g_down[kButtons], g_up[kButtons];   // seen since the last endFrame()
GLFWmousebuttonfun g_raylibCallback = nullptr;

void onMouseButton(GLFWwindow* w, int button, int action, int mods) {
    if (button >= 0 && button < kButtons) {
        if (action == 1) g_down[button] = true;   // GLFW_PRESS
        else if (action == 0) g_up[button] = true;
    }
    if (g_raylibCallback) g_raylibCallback(w, button, action, mods);   // raylib keeps its own state
}

// --- pointer pictures -------------------------------------------------------------------

struct Shape { Image img{}; int hotX = 0, hotY = 0; };

// Drawn 4x larger and scaled down, for smooth edges
Shape arrowShape(float s, Color fill) {
    const int k = 4;
    float u = s * k;
    int w = (int)std::ceil(14 * u) + 4 * k, h = (int)std::ceil(22 * u) + 4 * k;
    Image img = GenImageColor(w, h, BLANK);
    Vector2 o{2.0f * k, 2.0f * k};
    auto P = [&](float x, float y) { return Vector2{o.x + x * u, o.y + y * u}; };
    Vector2 p[7] = {P(0, 0), P(0, 17), P(4.5f, 13), P(8, 20), P(10.5f, 19), P(7, 12), P(12.5f, 12)};
    int tri[5][3] = {{0, 1, 2}, {0, 2, 5}, {0, 5, 6}, {2, 3, 4}, {2, 4, 5}};
    Color edge{20, 18, 16, 255};
    // the outline: the same shape grown by a little, in dark
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            for (auto& t : tri) {
                Vector2 d{dx * 1.6f * k, dy * 1.6f * k};
                ImageDrawTriangle(&img, {p[t[0]].x + d.x, p[t[0]].y + d.y}, {p[t[1]].x + d.x, p[t[1]].y + d.y},
                                  {p[t[2]].x + d.x, p[t[2]].y + d.y}, edge);
            }
    for (auto& t : tri) ImageDrawTriangle(&img, p[t[0]], p[t[1]], p[t[2]], fill);
    ImageResize(&img, w / k, h / k);
    return {img, 2, 2};
}

Shape crosshairShape(float s) {
    const int k = 4;
    float r = 9 * s * k, g = 3 * s * k;
    int size = (int)(2 * r) + 8 * k;
    Image img = GenImageColor(size, size, BLANK);
    Vector2 c{size * 0.5f, size * 0.5f};
    for (int pass = 0; pass < 2; ++pass) {
        int thick = (pass == 0 ? 4 : 2) * k;
        Color col = pass == 0 ? Color{0, 0, 0, 200} : Color{236, 232, 220, 255};
        ImageDrawLineEx(&img, {c.x - r, c.y}, {c.x - g, c.y}, thick, col);
        ImageDrawLineEx(&img, {c.x + g, c.y}, {c.x + r, c.y}, thick, col);
        ImageDrawLineEx(&img, {c.x, c.y - r}, {c.x, c.y - g}, thick, col);
        ImageDrawLineEx(&img, {c.x, c.y + g}, {c.x, c.y + r}, thick, col);
    }
    ImageDrawCircleV(&img, c, (int)(1.5f * k), Color{220, 60, 50, 255});
    ImageResize(&img, size / k, size / k);
    return {img, size / k / 2, size / k / 2};
}

Shape makeShape(Pointer p, float s) {
    switch (p) {
        case Pointer::ArrowWorld: return arrowShape(s, Color{246, 226, 186, 255});
        case Pointer::Crosshair: return crosshairShape(s);
        default: return arrowShape(s, Color{236, 232, 220, 255});
    }
}

Pointer g_pointer = Pointer::Hidden;
float g_pointerScale = 0;
bool g_pointerSet = false;

#ifndef __EMSCRIPTEN__
GLFWcursor* g_cursors[3] = {};
#else
std::string g_css[3];
#endif

void buildPointers(float s) {
    for (int i = 0; i < 3; ++i) {
        Shape sh = makeShape((Pointer)i, s);
#ifndef __EMSCRIPTEN__
        if (g_cursors[i]) glfwDestroyCursor(g_cursors[i]);
        ImageFormat(&sh.img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        GLFWimage gi{sh.img.width, sh.img.height, (unsigned char*)sh.img.data};
        g_cursors[i] = glfwCreateCursor(&gi, sh.hotX, sh.hotY);
#else
        // the browser takes a picture as a CSS cursor
        int size = 0, len = 0;
        unsigned char* png = ExportImageToMemory(sh.img, ".png", &size);
        char* b64 = png ? EncodeDataBase64(png, size, &len) : nullptr;
        g_css[i] = b64 ? "url(data:image/png;base64," + std::string(b64, (size_t)std::max(0, len - (len > 0 && b64[len - 1] == 0 ? 1 : 0))) +
                             ") " + std::to_string(sh.hotX) + " " + std::to_string(sh.hotY) + ", default"
                       : "default";
        if (b64) MemFree(b64);
        if (png) MemFree(png);
#endif
        UnloadImage(sh.img);
    }
    g_pointerScale = s;
    g_pointerSet = false;
}

} // namespace

void init() {
    for (int i = 0; i < kButtons; ++i) g_down[i] = g_up[i] = false;
    if (GLFWwindow* w = glfwGetCurrentContext()) g_raylibCallback = glfwSetMouseButtonCallback(w, onMouseButton);
}

void shutdown() {
#ifndef __EMSCRIPTEN__
    for (auto*& c : g_cursors)
        if (c) { glfwDestroyCursor(c); c = nullptr; }
#endif
}

void endFrame() {
    for (int i = 0; i < kButtons; ++i) g_down[i] = g_up[i] = false;
}

bool pressed(int button) {
    return IsMouseButtonPressed(button) || (button >= 0 && button < kButtons && g_down[button]);
}

bool released(int button) {
    return IsMouseButtonReleased(button) || (button >= 0 && button < kButtons && g_up[button]);
}

void setPointer(Pointer p) {
    float s = std::max(1.0f, std::round(ui::scale() * 4.0f) / 4.0f);
    if (s != g_pointerScale) buildPointers(s);
    if (g_pointerSet && p == g_pointer) return;
    g_pointer = p;
    g_pointerSet = true;
#ifndef __EMSCRIPTEN__
    GLFWwindow* w = glfwGetCurrentContext();
    if (!w) return;
    if (p == Pointer::Hidden) { HideCursor(); return; }
    if (IsCursorHidden()) ShowCursor();
    glfwSetCursor(w, g_cursors[(int)p]);
#else
    // (the picture is base64 in a data URL: no quotes in it)
    std::string css = p == Pointer::Hidden ? "none" : g_css[(int)p];
    std::string js = "var c = document.getElementById('canvas'); if (c) c.style.cursor = '" + css + "';";
    emscripten_run_script(js.c_str());
#endif
}

} // namespace alone::input
