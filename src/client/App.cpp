#include "client/App.h"
#include "client/Icons.h"
#include "client/Ui.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace alone {

namespace {

constexpr float kBaseW = 1280, kBaseH = 720;

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list a;
    va_start(a, f);
    vsnprintf(buf, sizeof buf, f, a);
    va_end(a);
    return buf;
}

Color messageColour(MessageKind k) {
    switch (k) {
        case MessageKind::Good: return ui::theme().good;
        case MessageKind::Bad: return ui::theme().bad;
        case MessageKind::Body: return ui::theme().body;
        default: return ui::theme().text;
    }
}

Color symptomColour(SymptomColour c, int level) {
    static const Color warm[4] = {{214, 190, 92, 255}, {222, 150, 64, 255}, {214, 92, 60, 255}, {170, 36, 36, 255}};
    static const Color cold[4] = {{150, 196, 222, 255}, {104, 160, 214, 255}, {70, 110, 200, 255}, {50, 70, 170, 255}};
    static const Color neutral[4] = {{176, 176, 170, 255}, {150, 150, 146, 255}, {124, 124, 120, 255}, {100, 100, 98, 255}};
    int i = std::clamp(level - 1, 0, 3);
    return c == SymptomColour::Cold ? cold[i] : c == SymptomColour::Neutral ? neutral[i] : warm[i];
}

std::string settingsPath() {
    return (std::filesystem::path(GetApplicationDirectory()) / "settings.ini").string();
}

} // namespace

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int App::run(int argc, char** argv) {
    std::string dataOverride;
    int autoShots = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--debug") debug_ = true;
        else if (a == "--data" && i + 1 < argc) dataOverride = argv[++i];
        else if (a == "--seed" && i + 1 < argc) seed_ = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--screenshots") autoShots = 1;
    }
    Log::init((std::filesystem::path(GetApplicationDirectory()) / "alone.log").string());
    Paths::init(argv[0], dataOverride);
    loadSettings();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow((int)kBaseW, (int)kBaseH, "Alone");
    SetWindowMinSize(960, 540);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    ui::init();
    if (!renderer_.init()) { CloseWindow(); return 1; }
    InitAudioDevice();
    audio_ = IsAudioDeviceReady();
    sound_.init();
    if (audio_) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(Paths::data("assets/audio"), ec))
            if (e.path().extension() == ".mp3" || e.path().extension() == ".ogg") tracks_.push_back(e.path().string());
        std::sort(tracks_.begin(), tracks_.end());
    }
    applySettings();

    if (!game_.loadData()) {
        Log::error("Game data failed to load");
    }
    if (seed_ == 1) seed_ = (uint32_t)std::time(nullptr);
    game_.setBaseTimeScale(settings_.minutesPerSecond);
    game_.newGame(seed_);
    renderer_.snapTo(game_.player().pos);
    hasSave_ = Paths::exists(savePath()) && !autoShots;

    int frame = 0;
    while (!WindowShouldClose() && !quit_) {
        float dt = std::min(GetFrameTime(), 0.1f);
        time_ += dt;
        ui::beginFrame();
        update(dt);

        BeginDrawing();
        renderer_.draw(game_, time_, hover_, state_ == State::Playing && !ui::mouseOverUi() && overlay_ != Overlay::Map);
        if (state_ == State::Menu) drawMenu();
        else {
            drawHud();
            switch (overlay_) {
                case Overlay::Inventory: drawInventory(); break;
                case Overlay::Health: drawHealth(); break;
                case Overlay::Map: drawMap(); break;
                case Overlay::Pause: drawPause(); break;
                case Overlay::Settings: drawSettings(); break;
                case Overlay::Help: drawHelp(); break;
                default: break;
            }
            drawContextMenu();
            if (state_ == State::Dead) drawDeath();
            if (showDebug_) drawDebug();
        }
        if (settings_.showFps) DrawFPS(GetScreenWidth() - 90, GetScreenHeight() - 24);
        ui::endFrame();
        EndDrawing();

        // --screenshots: automated tour used for documentation / smoke tests
        if (autoShots) {
            ++frame;
            struct Stop { float px, py, hour; const char* file; Overlay ov; };
            static const Stop stops[] = {
                {0, 0, 0, "screenshot_menu.png", Overlay::None},
                {0, 0, 0, "screenshot_game.png", Overlay::None},
                {0, 0, 0, "screenshot_context.png", Overlay::None},
                {0, 0, 0, "screenshot_inventory.png", Overlay::Inventory},
                {0, 0, 0, "screenshot_map.png", Overlay::Map},
                {640, 364, 13, "screenshot_torget.png", Overlay::None},
                {663, 372, 15, "screenshot_church.png", Overlay::None},
                {300, 520, 11, "screenshot_forest.png", Overlay::None},
                {474, 352, 16, "screenshot_nolhaga.png", Overlay::None},
                {722, 515, 18.3f, "screenshot_gerdsken_dusk.png", Overlay::None},
                {640, 418, 22, "screenshot_station_night.png", Overlay::None},
                {760, 410, 12, "screenshot_e20.png", Overlay::None},
                {880, 120, 10, "screenshot_wildlife.png", Overlay::None},
                {0, 0, 0, "screenshot_symptoms.png", Overlay::None},
                {0, 0, 0, "screenshot_death.png", Overlay::None},
            };
            int n = (int)(sizeof(stops) / sizeof(stops[0]));
            int idx = frame / 45, sub = frame % 45;
            if (idx >= n) quit_ = true;
            else {
                const Stop& st = stops[idx];
                if (sub == 0) {
                    menu_.open = false;
                    if (idx == 1) newGame();
                    if (idx == 2 || idx == 3) {
                        // Find a kitchen container in the start house
                        Player& pl = game_.player();
                        const Building* b = game_.world().buildingAt((int)pl.pos.x, (int)pl.pos.y);
                        if (b) for (const auto& f : b->furniture) {
                            if (f.container.empty()) continue;
                            Vec2 fp{f.x + 0.5f, f.y + 0.5f};
                            pl.pos = fp + Vec2{f.facing == 1 ? 1.0f : f.facing == 3 ? -1.0f : 0.0f, f.facing == 2 ? 1.0f : f.facing == 0 ? -1.0f : 0.0f};
                            renderer_.snapTo(pl.pos);
                            if (idx == 2) {
                                menu_.options = game_.actionsAt(fp);
                                menu_.open = true;
                                menu_.title.clear();
                                menu_.pos = renderer_.toScreen(fp, 1.0f);
                            } else {
                                game_.world().container(f.x, f.y).searched = true;
                                ActionOption o;
                                o.kind = ActionKind::OpenContainer;
                                o.x = f.x; o.y = f.y;
                                game_.perform(o);
                            }
                            break;
                        }
                    }
                    overlay_ = st.ov;
                    if (st.px > 0) {
                        float mpp = game_.map().metresPerPixel();
                        game_.player().pos = Vec2{st.px * mpp, st.py * mpp};
                        double now = game_.world().clock().hourOfDay();
                        double add = st.hour - now;
                        if (add < 0) add += 24;
                        game_.world().clock().advance(add * 60);
                        renderer_.snapTo(game_.player().pos);
                    }
                }
                if (std::string(st.file) == "screenshot_symptoms.png") {
                    // A body in trouble: lean-meat diet, a cold, and a bad wound
                    BodyState& b = game_.player().body.mutableState();
                    b.ammonia = 0.45f;
                    b.avgKcalProtein = 1400; b.avgKcalFat = 100; b.avgKcalCarbs = 50;
                    b.vitaminCMg = 250;
                    if (sub == 0) {
                        Wound w; w.severity = 0.5f; w.bleeding = 8; w.part = BodyPart::LeftLeg; w.type = WoundType::Bite;
                        game_.player().body.addWound(w);
                    }
                    SetMousePosition(GetScreenWidth() - 40, 40);
                }
                if (std::string(st.file) == "screenshot_wildlife.png" && sub == 1) {
                    Rng rng(4);
                    Vec2 p0 = game_.player().pos;
                    const char* ids[] = {"hare", "roe_deer", "wild_boar", "moose", "roe_deer"};
                    for (int i = 0; i < 5; ++i) {
                        Animal& a = game_.animals().spawn(game_.animals().find(ids[i]), p0 + Vec2{-6.0f + i * 3.5f, -4.0f + (i % 2) * 3.0f}, rng);
                        a.state = AnimalState::Idle;
                        a.stateTimer = 1000;
                        a.heading = 0.4f * i;
                    }
                    Animal& d = game_.animals().spawn(game_.animals().find("roe_deer"), p0 + Vec2{3, 4}, rng);
                    d.state = AnimalState::Dead;
                }
                if (std::string(st.file) == "screenshot_death.png" && sub == 0) {
                    game_.player().body.mutableState().ammonia = 1.2f;
                    menu_.open = false;
                }
                if (sub == 44) TakeScreenshot(st.file);
            }
        }
    }
    if (!autoShots) autosave();
    saveSettings();
    if (musicLoaded_) UnloadMusicStream(music_);
    sound_.shutdown();
    if (audio_) CloseAudioDevice();
    if (mapReady_) UnloadTexture(mapTex_);
    renderer_.shutdown();
    ui::shutdown();
    CloseWindow();
    return 0;
}

std::string App::savePath() const {
    return (std::filesystem::path(GetApplicationDirectory()) / "save.txt").string();
}

void App::autosave() {
    if (state_ == State::Playing && game_.running() && !game_.dead()) {
        if (game_.save(savePath())) hasSave_ = true;
    }
}

void App::continueGame() {
    game_.setBaseTimeScale(settings_.minutesPerSecond);
    if (!game_.load(savePath())) {
        hasSave_ = false;
        newGame();
        return;
    }
    renderer_.invalidateAll();
    renderer_.snapTo(game_.player().pos);
    state_ = State::Playing;
    overlay_ = Overlay::None;
    menu_.open = false;
    selectedUid_ = 0;
}

void App::newGame() {
    seed_ = seed_ * 1664525u + 1013904223u;
    game_.setBaseTimeScale(settings_.minutesPerSecond);
    game_.newGame(seed_);
    renderer_.invalidateAll();
    renderer_.snapTo(game_.player().pos);
    state_ = State::Playing;
    overlay_ = Overlay::None;
    menu_.open = false;
    selectedUid_ = 0;
}

void App::update(float dt) {
    updateMusic(dt);
    sound_.update(game_, state_ == State::Menu ? settings_.sfxVolume * 0.5f : settings_.sfxVolume);
    for (const auto& e : game_.takeSounds())
        if (state_ != State::Menu) sound_.play(e, game_.player().pos, settings_.sfxVolume);
    if (IsKeyPressed(KEY_F11)) { settings_.fullscreen = !settings_.fullscreen; applySettings(); }
    if (state_ == State::Menu) {
        menuOrbit_ += dt;
        renderer_.follow(game_.player().pos + Vec2{std::cos(menuOrbit_ * 0.05f) * 6, std::sin(menuOrbit_ * 0.05f) * 6}, dt);
        PlayerInput none;
        game_.update(dt * 0.2f, none);
        return;
    }
    if (state_ == State::Playing) updatePlaying(dt);
    else {
        PlayerInput none;
        game_.update(dt, none);
        renderer_.follow(game_.player().pos, dt);
    }
}

void App::updatePlaying(float dt) {
    // Keyboard shortcuts
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (menu_.open) menu_.open = false;
        else if (overlay_ == Overlay::Settings) overlay_ = settingsReturn_;
        else if (overlay_ != Overlay::None) overlay_ = Overlay::None;
        else overlay_ = Overlay::Pause;
    }
    bool blocking = overlay_ == Overlay::Pause || overlay_ == Overlay::Settings || overlay_ == Overlay::Help || overlay_ == Overlay::Map;
    if (!blocking || overlay_ == Overlay::Map) {
        if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_I)) { overlay_ = overlay_ == Overlay::Inventory ? Overlay::None : Overlay::Inventory; menu_.open = false; }
        if (IsKeyPressed(KEY_M)) overlay_ = overlay_ == Overlay::Map ? Overlay::None : Overlay::Map;
        if (IsKeyPressed(KEY_H)) overlay_ = overlay_ == Overlay::Health ? Overlay::None : Overlay::Health;
    }
    if (IsKeyPressed(KEY_F1)) overlay_ = overlay_ == Overlay::Help ? Overlay::None : Overlay::Help;
    if (debug_ && IsKeyPressed(KEY_F3)) showDebug_ = !showDebug_;

    PlayerInput in;
    if (!blocking) {
        float sx = 0, sy = 0;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) sy -= 1;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) sy += 1;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) sx -= 1;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) sx += 1;
        in.move = renderer_.screenToWorldDir(sx, sy);
        in.run = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        in.sneak = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_C);
        if (IsKeyPressed(KEY_Q)) renderer_.rotate(-1);
        if (IsKeyPressed(KEY_E)) renderer_.rotate(1);
        if (IsKeyPressed(KEY_ONE)) game_.setTimeSpeed(1);
        if (IsKeyPressed(KEY_TWO)) game_.setTimeSpeed(5);
        if (IsKeyPressed(KEY_THREE)) game_.setTimeSpeed(20);
        if (in.move.lengthSq() > 0 || in.run) { if (game_.timeSpeed() > 1) game_.setTimeSpeed(1); }
        if (IsKeyPressed(KEY_L) && game_.player().inventory.findTag("light") >= 0)
            game_.player().flashlightOn = !game_.player().flashlightOn;
        if (IsKeyPressed(KEY_Z)) {
            ActionOption o;
            o.kind = ActionKind::Sleep;
            o.label = "Sleeping";
            game_.perform(o);
        }
        if (IsKeyPressed(KEY_R)) {
            ActionOption o;
            o.kind = ActionKind::Rest;
            o.label = "Resting";
            game_.perform(o);
        }
        worldInput();
        in.aim = hover_;
        in.attack = (IsKeyPressed(KEY_SPACE)) ||
                    (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ui::mouseOverUi() && !menu_.open && overlay_ != Overlay::Map);
    }
    if (overlay_ != Overlay::Map && !ui::mouseOverUi()) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0) renderer_.zoom(wheel > 0 ? 0.88f : 1.13f);
    }

    game_.update(dt, in);

    // Dizziness and confusion make the view sway a little
    Capacity cap = game_.player().body.capacity();
    float sway = (1.0f - cap.awareness) * 0.6f;
    Vec2 target = game_.player().pos + Vec2{std::sin(time_ * 0.9f) * sway, std::cos(time_ * 0.7f) * sway};
    renderer_.follow(target, dt);

    autosaveTimer_ += dt;
    if (autosaveTimer_ > 180.0f) { autosaveTimer_ = 0; autosave(); }

    if (game_.dead()) {
        // Permadeath: the save goes with you
        std::remove(savePath().c_str());
        hasSave_ = false;
        state_ = State::Dead;
        overlay_ = Overlay::None;
        menu_.open = false;
    }
}

void App::worldInput() {
    Vector2 mouse = GetMousePosition();
    hover_ = renderer_.pick(mouse, game_.world());
    if (ui::mouseOverUi() || overlay_ == Overlay::Map) return;
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        menu_.options = game_.actionsAt(hover_);
        menu_.open = !menu_.options.empty();
        menu_.pos = mouse;
        menu_.title = "";
    }
    if (IsKeyPressed(KEY_F)) {
        // Interact: first enabled option near where the player is looking / standing
        auto opts = game_.actionsAt(hover_);
        for (auto& o : opts)
            if (o.enabled && o.kind != ActionKind::Rest && o.kind != ActionKind::Sleep) { game_.perform(o); break; }
    }
}

// ---------------------------------------------------------------------------
// Menu screens
// ---------------------------------------------------------------------------

void App::drawMenu() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    DrawRectangleGradientH(0, 0, (int)(W * 0.55f), (int)H, Color{0, 0, 0, 210}, Color{0, 0, 0, 0});
    float x = 70 * s, y = H * 0.22f;
    ui::text("ALONE", x, y, 96 * s, ui::theme().text, true);
    ui::text("Alingsås, Västra Götaland. Autumn.", x + 4 * s, y + 100 * s, 22 * s, ui::theme().accent);
    ui::paragraph("Everyone is gone. Find food, keep warm, stay healthy. Your body will tell you when something is wrong - "
                  "but not what.", x + 4 * s, y + 140 * s, 460 * s, 17 * s, ui::theme().dim);
    float by = y + 230 * s;
    Rectangle b{x, by, 260 * s, 46 * s};
    if (hasSave_) {
        if (ui::button(b, "Continue", true, 20)) continueGame();
        b.y += 58 * s;
    }
    if (ui::button(b, "New game", true, 20)) newGame();
    b.y += 58 * s;
    if (ui::button(b, "Settings", true, 20)) { settingsReturn_ = Overlay::None; overlay_ = Overlay::Settings; }
    b.y += 58 * s;
    if (ui::button(b, "Quit", true, 20)) quit_ = true;
    ui::text("v0.1 - early prototype", x, H - 40 * s, 14 * s, ui::theme().dim);
    if (overlay_ == Overlay::Settings) drawSettings();
}

void App::drawPause() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 140});
    Rectangle p{W / 2 - 160 * s, H / 2 - 170 * s, 320 * s, 340 * s};
    ui::panel(p);
    ui::textCentered("Paused", W / 2, p.y + 20 * s, 30 * s, ui::theme().text, true);
    Rectangle b{p.x + 30 * s, p.y + 80 * s, p.width - 60 * s, 42 * s};
    if (ui::button(b, "Resume")) overlay_ = Overlay::None;
    b.y += 52 * s;
    if (ui::button(b, "Controls")) overlay_ = Overlay::Help;
    b.y += 52 * s;
    if (ui::button(b, "Settings")) { settingsReturn_ = Overlay::Pause; overlay_ = Overlay::Settings; }
    b.y += 52 * s;
    if (ui::button(b, "Save and quit to menu")) { autosave(); state_ = State::Menu; overlay_ = Overlay::None; }
    b.y += 52 * s;
    if (ui::button(b, "Save and quit game")) quit_ = true;
}

void App::drawSettings() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 120});
    Rectangle p{W / 2 - 230 * s, H / 2 - 215 * s, 460 * s, 430 * s};
    ui::panel(p);
    ui::textCentered("Settings", W / 2, p.y + 18 * s, 28 * s, ui::theme().text, true);
    float x = p.x + 30 * s, y = p.y + 80 * s, w = p.width - 60 * s;
    ui::text(fmt("Music volume: %d%%", (int)(settings_.musicVolume * 100)), x, y, 17 * s, ui::theme().text);
    if (ui::slider({x, y + 26 * s, w, 16 * s}, settings_.musicVolume, 0, 1) && musicLoaded_) SetMusicVolume(music_, settings_.musicVolume);
    y += 56 * s;
    ui::text(fmt("Sound effects: %d%%", (int)(settings_.sfxVolume * 100)), x, y, 17 * s, ui::theme().text);
    ui::slider({x, y + 26 * s, w, 16 * s}, settings_.sfxVolume, 0, 1);
    y += 56 * s;
    ui::text("Length of a day", x, y, 17 * s, ui::theme().text);
    struct Opt { const char* label; float mps; };
    Opt opts[] = {{"48 min", 0.5f}, {"24 min", 1.0f}, {"12 min", 2.0f}};
    for (int i = 0; i < 3; ++i) {
        Rectangle b{x + i * (w / 3), y + 26 * s, w / 3 - 8 * s, 34 * s};
        bool active = std::fabs(settings_.minutesPerSecond - opts[i].mps) < 0.01f;
        if (ui::button(b, std::string(active ? "> " : "") + opts[i].label)) {
            settings_.minutesPerSecond = opts[i].mps;
            game_.setBaseTimeScale(settings_.minutesPerSecond);
        }
    }
    y += 80 * s;
    if (ui::button({x, y, w / 2 - 6 * s, 36 * s}, settings_.fullscreen ? "Fullscreen: on" : "Fullscreen: off")) {
        settings_.fullscreen = !settings_.fullscreen;
        applySettings();
    }
    if (ui::button({x + w / 2 + 6 * s, y, w / 2 - 6 * s, 36 * s}, settings_.showFps ? "Show FPS: on" : "Show FPS: off"))
        settings_.showFps = !settings_.showFps;
    y += 60 * s;
    if (ui::button({x, y, w, 40 * s}, "Back")) { saveSettings(); overlay_ = settingsReturn_; }
}

void App::drawHelp() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 120});
    Rectangle p{W / 2 - 300 * s, H / 2 - 250 * s, 600 * s, 500 * s};
    ui::panel(p);
    ui::textCentered("Controls", W / 2, p.y + 16 * s, 28 * s, ui::theme().text, true);
    const char* lines[][2] = {
        {"W A S D", "Walk (hold Shift to run, Ctrl to sneak)"},
        {"Right click", "Interact with what's under the cursor"},
        {"F", "Quick interact (doors, search)"},
        {"Left click / Space", "Attack with what you hold"},
        {"Tab / I", "Inventory: double-click to move items, right-click for options"},
        {"H", "Look yourself over (wounds)"},
        {"M", "Map of Alingsås"},
        {"Z / R", "Sleep / rest for an hour"},
        {"L", "Flashlight on/off"},
        {"Q / E", "Rotate camera, mouse wheel zooms"},
        {"1 / 2 / 3", "Time speed x1 / x5 / x20"},
        {"Esc", "Pause"},
    };
    float y = p.y + 70 * s;
    for (auto& l : lines) {
        ui::text(l[0], p.x + 30 * s, y, 17 * s, ui::theme().accent, true);
        ui::text(l[1], p.x + 210 * s, y, 17 * s, ui::theme().text);
        y += 30 * s;
    }
    ui::paragraph("Hover over a symptom on the right of the screen to read how it feels. The game never tells you "
                  "what causes it - pay attention to what you eat and drink, and how you live.",
                  p.x + 30 * s, y + 10 * s, p.width - 60 * s, 15 * s, ui::theme().dim);
    if (ui::button({p.x + p.width / 2 - 80 * s, p.y + p.height - 54 * s, 160 * s, 38 * s}, "Close")) overlay_ = Overlay::None;
}

void App::drawDeath() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 185});
    const DeathInfo& d = game_.death();
    float y = H * 0.24f;
    ui::textCentered("You died.", W / 2, y, 64 * s, ui::theme().bad, true);
    y += 86 * s;
    int days = (int)d.daysSurvived;
    int hours = (int)((d.daysSurvived - days) * 24);
    ui::textCentered(fmt("You survived %d day%s and %d hour%s. You died in %s.", days, days == 1 ? "" : "s", hours,
                         hours == 1 ? "" : "s", d.place.c_str()),
                     W / 2, y, 20 * s, ui::theme().text);
    y += 50 * s;
    ui::textCentered(std::string("Cause of death: ") + deathCauseName(d.cause), W / 2, y, 24 * s, ui::theme().accent, true);
    y += 42 * s;
    float w = 620 * s;
    for (const auto& l : ui::wrap(deathCauseExplanation(d.cause), w, 17 * s)) {
        ui::textCentered(l, W / 2, y, 17 * s, ui::theme().dim);
        y += 24 * s;
    }
    y += 30 * s;
    if (ui::button({W / 2 - 210 * s, y, 200 * s, 44 * s}, "Try again")) newGame();
    if (ui::button({W / 2 + 10 * s, y, 200 * s, 44 * s}, "Main menu")) state_ = State::Menu;
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

void App::drawHud() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    World& w = game_.world();
    const Clock& c = w.clock();

    // Screen effects: night & cold vignette, sickness tint
    BodySignals sig = game_.player().body.signals();
    float vig = std::max({sig.cold * 0.6f, sig.weakness * 0.5f, sig.dizziness * 0.6f, sig.pain * 0.4f});
    if (vig > 0.05f) {
        Color edge = sig.cold > sig.pain ? Color{20, 40, 80, (unsigned char)(vig * 160)} : Color{60, 0, 0, (unsigned char)(vig * 150)};
        int band = (int)(H * 0.25f);
        DrawRectangleGradientV(0, 0, (int)W, band, edge, Color{0, 0, 0, 0});
        DrawRectangleGradientV(0, (int)H - band, (int)W, band, Color{0, 0, 0, 0}, edge);
        DrawRectangleGradientH(0, 0, band, (int)H, edge, Color{0, 0, 0, 0});
        DrawRectangleGradientH((int)W - band, 0, band, (int)H, Color{0, 0, 0, 0}, edge);
    }
    if (sig.hallucination > 0.1f) {
        float a = sig.hallucination * (0.5f + 0.5f * std::sin(time_ * 1.3f)) * 50;
        DrawRectangle(0, 0, (int)W, (int)H, Color{(unsigned char)(120 + 100 * std::sin(time_)), 40, (unsigned char)(140 + 100 * std::cos(time_ * 0.7f)), (unsigned char)a});
    }
    if (game_.player().asleep) DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 170});

    // Top-left: time, weather, place
    Rectangle p{12 * s, 12 * s, 300 * s, 96 * s};
    ui::panel(p, Color{10, 12, 14, 170});
    ui::text(c.timeString(), p.x + 12 * s, p.y + 8 * s, 30 * s, ui::theme().text, true);
    ui::text(c.dateString(), p.x + 106 * s, p.y + 10 * s, 16 * s, ui::theme().dim);
    ui::text(fmt("Day %d", c.day()), p.x + 106 * s, p.y + 28 * s, 14 * s, ui::theme().dim);
    float t = w.outdoorTemp();
    ui::text(fmt("%.0f°C, %s", t, weatherName(c.weather().kind)), p.x + 12 * s, p.y + 48 * s, 16 * s, ui::theme().text);
    ui::text(game_.location(), p.x + 12 * s, p.y + 70 * s, 15 * s, ui::theme().accent);
    if (game_.timeSpeed() > 1 || game_.currentAction().timeScale > 1.5f) {
        float sp = game_.timeSpeed() * (game_.currentAction().active ? game_.currentAction().timeScale : 1);
        ui::text(fmt(">> x%.0f", sp), p.x + p.width - 70 * s, p.y + 10 * s, 16 * s, ui::theme().accent, true);
    }

    drawSymptoms();
    drawMessages();
    drawActionBar();

    // Bottom right: what you hold, hints
    const ItemInstance* wpn = game_.weapon();
    Rectangle hb{W - 250 * s, H - 70 * s, 238 * s, 58 * s};
    ui::panel(hb, Color{10, 12, 14, 170});
    drawItemIcon(wpn ? wpn->def : nullptr, {hb.x + 8 * s, hb.y + 9 * s, 40 * s, 40 * s});
    ui::text(wpn ? game_.itemName(*wpn) : "Bare hands", hb.x + 56 * s, hb.y + 10 * s, 15 * s, ui::theme().text);
    ui::text("Tab  M  H  F1", hb.x + 56 * s, hb.y + 32 * s, 13 * s, ui::theme().dim);
    // Breath
    float st = game_.player().stamina;
    if (st < 0.99f) {
        Rectangle sb{W / 2 - 80 * s, H - 26 * s, 160 * s, 6 * s};
        DrawRectangleRec(sb, Color{0, 0, 0, 140});
        DrawRectangle((int)sb.x, (int)sb.y, (int)(sb.width * st), (int)sb.height, Color{200, 200, 190, 200});
    }
}

void App::drawSymptoms() {
    float s = ui::scale();
    float W = (float)GetScreenWidth();
    auto syms = game_.currentSymptoms();
    float size = 46 * s;
    float x = W - size - 14 * s, y = 14 * s;
    for (const auto& sym : syms) {
        Rectangle r{x, y, size, size};
        Color col = symptomColour(sym.def->colour, sym.level);
        DrawRectangleRounded(r, 0.25f, 6, Color{14, 15, 17, 215});
        DrawRectangleRoundedLinesEx(r, 0.25f, 6, 2.0f, col);
        drawSymptomIcon(sym.def->id, {r.x + size * 0.18f, r.y + size * 0.12f, size * 0.64f, size * 0.64f}, col);
        for (int i = 0; i < 4; ++i) {
            Color dot = i < sym.level ? col : Color{60, 60, 60, 200};
            DrawRectangle((int)(r.x + size * 0.18f + i * size * 0.17f), (int)(r.y + size * 0.82f), (int)(size * 0.12f), (int)(size * 0.07f), dot);
        }
        ui::claim(r);
        if (ui::hovered(r)) ui::tooltip(sym.name(), sym.description(), col);
        y += size + 6 * s;
        if (y > GetScreenHeight() - 100 * s) { y = 14 * s; x -= size + 6 * s; }
    }
}

void App::drawMessages() {
    float s = ui::scale();
    float H = (float)GetScreenHeight();
    const auto& msgs = game_.messages();
    float y = H - 30 * s;
    int shown = 0;
    float maxW = 560 * s;
    for (auto it = msgs.rbegin(); it != msgs.rend() && shown < 7; ++it, ++shown) {
        float a = 1.0f - std::clamp((it->age - 14.0f) / 4.0f, 0.0f, 1.0f);
        if (a <= 0) break;
        auto lines = ui::wrap(it->text, maxW, 16 * s);
        y -= lines.size() * 20 * s;
        Color c = messageColour(it->kind);
        c.a = (unsigned char)(a * 255);
        float ly = y;
        for (const auto& l : lines) {
            DrawTextEx(ui::font(), l.c_str(), {14 * s + 1, ly + 1}, 16 * s, 0.5f, Color{0, 0, 0, (unsigned char)(a * 200)});
            ui::text(l, 14 * s, ly, 16 * s, c);
            ly += 20 * s;
        }
    }
}

void App::drawActionBar() {
    const CurrentAction& a = game_.currentAction();
    if (!a.active) return;
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    Rectangle r{W / 2 - 160 * s, H - 92 * s, 320 * s, 48 * s};
    ui::panel(r, Color{10, 12, 14, 200});
    std::string label = a.option.label;
    if (a.option.kind == ActionKind::Sleep) label = "Sleeping...";
    ui::textCentered(label, W / 2, r.y + 6 * s, 16 * s, ui::theme().text);
    if (a.option.kind != ActionKind::Sleep) {
        float t = std::clamp(a.elapsed / a.duration, 0.0f, 1.0f);
        Rectangle b{r.x + 12 * s, r.y + 30 * s, r.width - 24 * s, 8 * s};
        DrawRectangleRec(b, Color{40, 40, 40, 255});
        DrawRectangle((int)b.x, (int)b.y, (int)(b.width * t), (int)b.height, ui::theme().accent);
    } else {
        ui::textCentered("(move to wake up)", W / 2, r.y + 26 * s, 13 * s, ui::theme().dim);
    }
}

void App::drawContextMenu() {
    if (!menu_.open) return;
    float s = ui::scale();
    float rowH = 26 * s, w = 0;
    for (const auto& o : menu_.options) w = std::max(w, ui::measure(o.label, 16 * s).x);
    w += 28 * s;
    float h = rowH * menu_.options.size() + 8 * s + (menu_.title.empty() ? 0 : rowH);
    float x = std::min(menu_.pos.x, GetScreenWidth() - w - 4), y = std::min(menu_.pos.y, GetScreenHeight() - h - 4);
    Rectangle r{x, y, w, h};
    ui::panel(r, Color{14, 15, 17, 240});
    float ry = y + 4 * s;
    if (!menu_.title.empty()) {
        ui::text(menu_.title, x + 12 * s, ry + 3 * s, 15 * s, ui::theme().accent, true);
        ry += rowH;
    }
    bool clickedSomething = false;
    for (const auto& o : menu_.options) {
        Rectangle rr{x + 2, ry, w - 4, rowH};
        bool hov = ui::hovered(rr);
        if (hov && o.enabled) DrawRectangleRec(rr, ui::theme().highlight);
        ui::text(o.label, x + 12 * s, ry + 4 * s, 16 * s, o.enabled ? ui::theme().text : ui::theme().dim);
        if (hov && !o.enabled && !o.reason.empty()) ui::tooltip("", o.reason);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            clickedSomething = true;
            if (o.enabled) {
                ActionOption copy = o;
                menu_.open = false;
                game_.perform(copy);
                if (copy.kind == ActionKind::Search || copy.kind == ActionKind::OpenContainer) overlay_ = Overlay::Inventory;
                return;
            }
        }
        ry += rowH;
    }
    if (!clickedSomething && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) && !ui::hovered(r))
        menu_.open = false;
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

void App::openItemMenu(const ItemRef& ref, Vector2 at) {
    ItemInstance* it = game_.resolve(ref);
    if (!it) return;
    menu_.options = game_.actionsForItem(ref);
    menu_.open = !menu_.options.empty();
    menu_.pos = at;
    menu_.title = game_.itemName(*it);
}

void App::drawItemTooltip(const ItemInstance& it) {
    const ItemDef& d = *it.def;
    std::string body = d.description;
    auto add = [&](const std::string& line) { body += (body.empty() ? "" : "\n") + line; };
    add(fmt("Weight: %.2f kg", it.weightKg()));
    if (d.isEdible() && d.capacityMl <= 0) {
        float sp = it.spoilage();
        if (d.spoilHours > 0) add(sp >= 1 ? "It smells rotten." : sp > 0.6f ? "It doesn't smell quite right." : sp > 0.3f ? "Still fine." : "Fresh.");
        if (d.raw) add("Raw.");
        if (d.hasTag("labelled")) {
            Nutrients n = d.nutrients.scaled(it.portion);
            add(fmt("Label: %.0f kcal - fat %.0f g, carbohydrate %.0f g, protein %.0f g", n.kcal(), n.fat, n.carbs, n.protein));
        }
    }
    if (d.capacityMl > 0) add(fmt("Holds %.0f of %.0f ml%s", it.liquidMl, d.capacityMl, it.liquidMl > 0 && !it.liquidSafe ? " (untreated)" : ""));
    if (d.category == ItemCategory::Clothing && d.clo > 0) {
        std::string warmth = d.clo >= 0.8f ? "very warm" : d.clo >= 0.4f ? "warm" : d.clo >= 0.15f ? "a little warm" : "barely warm";
        add("It's " + warmth + (d.waterproof > 0.8f ? " and waterproof." : d.waterproof > 0.4f ? " and keeps some rain out." : "."));
    }
    if (d.carryBonusKg > 0) add(fmt("Lets you carry %.0f kg more.", d.carryBonusKg));
    if (d.uses > 0 && d.category != ItemCategory::Food) add(fmt("%d left.", it.usesLeft));
    ui::tooltip(game_.itemName(it), body);
}

void App::drawItemList(const std::string& title, Rectangle area, std::vector<ItemRef>& refs, int pane, ItemLoc target) {
    float s = ui::scale();
    ui::text(title, area.x, area.y - 26 * s, 18 * s, ui::theme().accent, true);
    DrawRectangleRec(area, Color{8, 9, 10, 160});
    float rowH = 34 * s;
    float contentH = refs.size() * rowH;
    if (ui::hovered(area)) {
        scroll_[pane] -= GetMouseWheelMove() * rowH * 2;
    }
    scroll_[pane] = std::clamp(scroll_[pane], 0.0f, std::max(0.0f, contentH - area.height));
    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
    float y = area.y - scroll_[pane];
    for (const auto& ref : refs) {
        ItemInstance* it = game_.resolve(ref);
        if (!it) continue;
        Rectangle row{area.x, y, area.width, rowH - 2};
        bool hov = ui::hovered(row) && ui::hovered(area);
        if (it->uid == selectedUid_) DrawRectangleRec(row, Color{70, 60, 40, 200});
        else if (hov) DrawRectangleRec(row, Color{40, 40, 42, 200});
        drawItemIcon(it->def, {row.x + 5 * s, row.y + 3 * s, rowH - 8 * s, rowH - 8 * s});
        std::string name = game_.itemName(*it);
        if (it->uid == game_.player().weaponUid && pane == 0) name += "  (held)";
        ui::text(name, row.x + rowH + 4 * s, row.y + 8 * s, 15 * s, ui::theme().text);
        std::string wt = fmt("%.1f", it->weightKg());
        ui::text(wt, row.x + row.width - ui::measure(wt, 13 * s).x - 8 * s, row.y + 10 * s, 13 * s, ui::theme().dim);
        if (hov && !menu_.open) {
            drawItemTooltip(*it);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                double now = GetTime();
                if (lastClickUid_ == it->uid && now - lastClickTime_ < 0.35) {
                    game_.transfer(ref, target);
                    lastClickUid_ = 0;
                } else {
                    selectedUid_ = it->uid;
                    lastClickUid_ = it->uid;
                    lastClickTime_ = now;
                }
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) openItemMenu(ref, GetMousePosition());
        }
        y += rowH;
    }
    EndScissorMode();
    if (refs.empty()) ui::text("Nothing.", area.x + 10 * s, area.y + 10 * s, 15 * s, ui::theme().dim);
    ui::claim(area);
}

void App::drawInventory() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    Rectangle p{W / 2 - 520 * s, H / 2 - 290 * s, 1040 * s, 560 * s};
    p.x = std::max(4.0f, p.x);
    ui::panel(p, Color{16, 17, 19, 236});
    Player& pl = game_.player();
    float colW = (p.width - 80 * s) / 3;

    // Player inventory
    std::vector<ItemRef> mine;
    for (const auto& it : pl.inventory.items()) mine.push_back({ItemLoc::Player, 0, 0, 0, it.uid});
    float carried = game_.carriedKg(), cap = game_.carryCapacityKg();
    Rectangle a{p.x + 20 * s, p.y + 70 * s, colW, p.height - 90 * s};

    // Other side: open container in reach, otherwise the ground
    bool container = false;
    if (game_.hasOpenContainer()) {
        Vec2 cpos{game_.openContainerX() + 0.5f, game_.openContainerY() + 0.5f};
        if (distance(cpos, pl.pos) < 2.6f) container = true;
        else game_.closeContainer();
    }
    std::vector<ItemRef> other;
    std::string otherTitle = "On the ground";
    if (container) {
        int cx = game_.openContainerX(), cy = game_.openContainerY();
        ContainerState& cs = game_.world().container(cx, cy);
        const ContainerDef* cd = game_.items().container(cs.kind);
        otherTitle = cd ? cd->name : "Container";
        for (const auto& it : cs.items) other.push_back({ItemLoc::Container, cx, cy, 0, it.uid});
    } else {
        for (auto& [key, idx] : game_.world().groundItemsNear(pl.pos, 1.6f)) {
            auto* st = game_.world().groundStack(key);
            if (st && idx < (int)st->size()) other.push_back({ItemLoc::Ground, 0, 0, key, (*st)[idx].uid});
        }
    }

    drawItemList(fmt("Carrying  %.1f / %.0f kg", carried, cap), a, mine, 0, container ? ItemLoc::Container : ItemLoc::Ground);
    if (carried > cap) ui::text("Overloaded", a.x + a.width - 90 * s, a.y - 24 * s, 15 * s, ui::theme().bad);

    // Worn clothing
    Rectangle wa{a.x + colW + 20 * s, a.y, colW, a.height};
    ui::text("Wearing", wa.x, wa.y - 26 * s, 18 * s, ui::theme().accent, true);
    DrawRectangleRec(wa, Color{8, 9, 10, 160});
    float y = wa.y + 6 * s;
    for (int sl = 1; sl < (int)ClothingSlot::Count; ++sl) {
        const ItemInstance& it = pl.worn[sl];
        Rectangle row{wa.x, y, wa.width, 40 * s};
        bool hov = ui::hovered(row);
        if (hov) DrawRectangleRec(row, Color{40, 40, 42, 200});
        ui::text(slotName((ClothingSlot)sl), row.x + 10 * s, row.y + 4 * s, 12 * s, ui::theme().dim);
        if (it.def) {
            drawItemIcon(it.def, {row.x + 10 * s, row.y + 17 * s, 20 * s, 20 * s});
            ui::text(game_.itemName(it), row.x + 36 * s, row.y + 19 * s, 15 * s, ui::theme().text);
            ItemRef ref{ItemLoc::Worn, 0, 0, 0, it.uid};
            if (hov && !menu_.open) {
                drawItemTooltip(it);
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) openItemMenu(ref, GetMousePosition());
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    double now = GetTime();
                    if (lastClickUid_ == it.uid && now - lastClickTime_ < 0.35) { game_.transfer(ref, ItemLoc::Player); lastClickUid_ = 0; }
                    else { lastClickUid_ = it.uid; lastClickTime_ = now; }
                }
            }
        } else {
            ui::text("-", row.x + 36 * s, row.y + 19 * s, 15 * s, ui::theme().dim);
        }
        y += 44 * s;
    }
    ui::claim(wa);

    Rectangle oa{wa.x + colW + 20 * s, a.y, colW, a.height};
    drawItemList(otherTitle, oa, other, 1, ItemLoc::Player);
    if (!other.empty() && ui::button({oa.x + oa.width - 100 * s, oa.y - 30 * s, 100 * s, 24 * s}, "Take all", true, 14)) {
        for (const auto& r : other) game_.transfer(r, ItemLoc::Player);
    }
    ui::text("Double-click moves an item. Right-click for options.", p.x + 20 * s, p.y + 14 * s, 14 * s, ui::theme().dim);
    if (ui::button({p.x + p.width - 44 * s, p.y + 10 * s, 32 * s, 28 * s}, "X")) overlay_ = Overlay::None;
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

void App::drawHealth() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    Rectangle p{W / 2 - 280 * s, H / 2 - 230 * s, 560 * s, 460 * s};
    ui::panel(p, Color{16, 17, 19, 236});
    ui::text("You look yourself over", p.x + 20 * s, p.y + 16 * s, 22 * s, ui::theme().text, true);
    const Body& body = game_.player().body;
    const BodyState& b = body.state();
    float x = p.x + 24 * s, y = p.y + 60 * s, w = p.width - 48 * s;

    // What you can see in a mirror: build, skin colour, wet clothes
    float fatPct = b.fatMassKg / body.bodyMassKg();
    float lean = b.leanMassKg / b.leanBaselineKg;
    std::string build = lean < 0.7f ? "You are skin and bones." : lean < 0.8f ? "You look gaunt, your ribs show." :
                        fatPct < 0.07f ? "You look thin." : fatPct < 0.12f ? "You look lean." : fatPct < 0.25f ? "You look healthy." :
                        "You're carrying some extra weight.";
    y = ui::paragraph(build, x, y, w, 16 * s, ui::theme().text);
    if (b.wetness > 0.15f) y = ui::paragraph(b.wetness > 0.6f ? "Your clothes are soaked through." : "Your clothes are damp.", x, y, w, 16 * s, ui::theme().text);
    if (b.feverC > 0.8f) y = ui::paragraph("Your forehead feels hot.", x, y, w, 16 * s, ui::theme().text);
    if (b.diarrhea > 0.3f) y = ui::paragraph("You've had to run to the toilet a lot.", x, y, w, 16 * s, ui::theme().text);
    y += 14 * s;
    ui::text("Wounds", x, y, 18 * s, ui::theme().accent, true);
    y += 28 * s;
    if (b.wounds.empty()) { ui::text("No wounds.", x, y, 15 * s, ui::theme().dim); y += 22 * s; }
    for (const auto& wd : b.wounds) {
        std::string sev = wd.severity > 0.6f ? "Deep" : wd.severity > 0.3f ? "Nasty" : "Small";
        std::string line = sev + " " + woundTypeName(wd.type) + " on your " + bodyPartName(wd.part);
        std::string state;
        if (wd.bleeding > 0.5f && !(wd.bandaged && wd.bandageDirt < 0.8f)) state += wd.bleeding > 10 ? "bleeding heavily" : "bleeding";
        if (wd.bandaged) state += std::string(state.empty() ? "" : ", ") + (wd.bandageDirt > 0.6f ? "dirty bandage" : "bandaged");
        if (wd.infection > 0.3f) state += std::string(state.empty() ? "" : ", ") + (wd.infection > 0.6f ? "swollen and oozing" : "red and warm");
        else if (wd.healed > 0.5f) state += std::string(state.empty() ? "" : ", ") + "healing";
        ui::text(line, x, y, 15 * s, ui::theme().text);
        if (!state.empty()) ui::text(state, x + 20 * s, y + 18 * s, 14 * s, wd.infection > 0.3f || wd.bleeding > 0.5f ? ui::theme().bad : ui::theme().dim);
        y += 42 * s;
        if (y > p.y + p.height - 60 * s) break;
    }
    if (ui::button({p.x + p.width / 2 - 70 * s, p.y + p.height - 50 * s, 140 * s, 36 * s}, "Close")) overlay_ = Overlay::None;
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

void App::buildMapTexture() {
    const MapData& m = game_.map();
    int w = m.pixelWidth(), h = m.pixelHeight();
    Image img = GenImageColor(w, h, BLACK);
    Color* px = (Color*)img.data;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Color c;
            switch (m.zoneAtPixel(x, y)) {
                case Zone::Water: c = {120, 162, 186, 255}; break;
                case Zone::Forest: c = {96, 124, 84, 255}; break;
                case Zone::Woodland: c = {132, 156, 104, 255}; break;
                case Zone::Park: c = {140, 176, 112, 255}; break;
                case Zone::Residential: c = {206, 200, 186, 255}; break;
                case Zone::Field: c = {200, 186, 140, 255}; break;
                case Zone::Meadow: c = {176, 182, 128, 255}; break;
                case Zone::Commercial: c = {204, 186, 170, 255}; break;
                case Zone::Hospital: c = {214, 176, 170, 255}; break;
                default: c = {120, 120, 120, 255}; break;
            }
            px[y * w + x] = c;
        }
    mapTex_ = LoadTextureFromImage(img);
    SetTextureFilter(mapTex_, TEXTURE_FILTER_BILINEAR);
    UnloadImage(img);
    mapReady_ = true;
}

void App::drawMap() {
    if (!mapReady_) buildMapTexture();
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    const MapData& m = game_.map();
    float mpp = m.metresPerPixel();
    Rectangle area{20 * s, 20 * s, W - 40 * s, H - 40 * s};
    ui::panel(area, Color{30, 32, 34, 250});
    if (mapCentre_.x == 0 && mapCentre_.y == 0) mapCentre_ = {game_.player().pos.x / mpp, game_.player().pos.y / mpp};
    if (ui::hovered(area)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0) mapZoom_ = std::clamp(mapZoom_ * (wheel > 0 ? 1.2f : 1 / 1.2f), 0.6f, 12.0f);
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            float k = mapZoom_ * s * 1.5f;
            mapCentre_.x -= d.x / k;
            mapCentre_.y -= d.y / k;
        }
    }
    float k = mapZoom_ * s * 1.5f;
    Vector2 c{area.x + area.width / 2, area.y + area.height / 2};
    auto toScreen = [&](float px, float py) { return Vector2{c.x + (px - mapCentre_.x) * k, c.y + (py - mapCentre_.y) * k}; };
    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
    Vector2 o = toScreen(0, 0);
    DrawTextureEx(mapTex_, o, 0, k, WHITE);
    for (const auto& r : m.roads()) {
        float wdt = r.cls == RoadClass::Motorway ? 4.0f : r.cls == RoadClass::Major ? 2.6f : 1.6f;
        Color col = r.cls == RoadClass::Motorway ? Color{226, 190, 110, 255} : Color{250, 250, 246, 255};
        DrawLineEx(toScreen(r.a.x / mpp, r.a.y / mpp), toScreen(r.b.x / mpp, r.b.y / mpp), wdt * std::sqrt(mapZoom_) * s, col);
    }
    for (const auto& r : m.rails())
        DrawLineEx(toScreen(r.a.x / mpp, r.a.y / mpp), toScreen(r.b.x / mpp, r.b.y / mpp), 2.0f * s, Color{90, 80, 80, 255});
    for (const auto& l : m.landmarks()) {
        Vector2 sp = toScreen(l.centre.x / mpp, l.centre.y / mpp);
        DrawRectangle((int)sp.x - 3, (int)sp.y - 3, 6, 6, Color{150, 60, 50, 255});
        if (mapZoom_ > 2.2f) ui::text(l.name, sp.x + 6, sp.y - 8 * s, 13 * s, Color{60, 40, 30, 255});
    }
    for (const auto& d : m.districts()) {
        Vector2 sp = toScreen(d.centre.x / mpp, d.centre.y / mpp);
        Vector2 ms = ui::measure(d.name, 16 * s, true);
        ui::text(d.name, sp.x - ms.x / 2, sp.y - ms.y / 2, 16 * s, Color{50, 50, 56, 230}, true);
    }
    for (const auto& lb : m.labels()) {
        Vector2 sp = toScreen(lb.pos.x / mpp, lb.pos.y / mpp);
        float size = 13 * s;
        Vector2 ms = ui::measure(lb.text, size);
        DrawTextPro(ui::font(), lb.text.c_str(), sp, {ms.x / 2, ms.y / 2}, lb.angle, size, 0.5f, Color{70, 60, 50, 255});
    }
    for (const auto& wn : m.waterNames()) {
        Vector2 sp = toScreen(wn.pos.x / mpp, wn.pos.y / mpp);
        ui::text(wn.name, sp.x, sp.y, 17 * s, Color{40, 70, 110, 255}, true);
    }
    // You are here
    Vec2 pp = game_.player().pos;
    Vector2 ps = toScreen(pp.x / mpp, pp.y / mpp);
    float f = game_.player().facing;
    DrawCircleV(ps, 7 * s, Color{200, 40, 40, 255});
    DrawLineEx(ps, {ps.x + std::cos(f) * 14 * s, ps.y + std::sin(f) * 14 * s}, 3 * s, Color{200, 40, 40, 255});
    EndScissorMode();
    ui::text("Alingsås", area.x + 16 * s, area.y + 12 * s, 26 * s, Color{40, 40, 44, 255}, true);
    ui::text("Drag to pan, wheel to zoom, M to close", area.x + 16 * s, area.y + 44 * s, 14 * s, Color{60, 60, 64, 255});
    // scale bar: 500 m
    float bar = 500.0f / mpp * k;
    DrawRectangle((int)(area.x + 20 * s), (int)(area.y + area.height - 30 * s), (int)bar, (int)(4 * s), Color{40, 40, 40, 255});
    ui::text("500 m", area.x + 24 * s + bar, area.y + area.height - 38 * s, 13 * s, Color{40, 40, 40, 255});
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

void App::drawDebug() {
    float s = ui::scale();
    const Body& body = game_.player().body;
    const BodyState& b = body.state();
    std::vector<std::string> l = {
        "DEBUG (F3) - the player never sees this",
        fmt("mass %.1f kg  lean %.1f/%.1f  fat %.2f kg", body.bodyMassKg(), b.leanMassKg, b.leanBaselineKg, b.fatMassKg),
        fmt("glycogen liver %.0f g muscle %.0f g  blood sugar %.1f g", b.liverGlycogenG, b.muscleGlycogenG, b.bloodSugarG),
        fmt("stomach %.0f kcal (%.0f ml)  amino pool %.0f g  diet fat %.0f g", b.stomach.kcal(), b.stomachSolidMl + b.stomach.water, b.aminoPoolG, b.dietaryFatG),
        fmt("intake/day: protein %.0f fat %.0f carbs %.0f kcal", b.avgKcalProtein, b.avgKcalFat, b.avgKcalCarbs),
        fmt("ammonia %.2f  keto %.2f  starvation %.2f  debt %.0f kcal", b.ammonia, b.ketoAdaptation, b.starvationAdaptation, b.energyDebtKcal),
        fmt("water deficit %.2f L (%.1f%%)  blood %.2f L  vitC %.0f mg", b.waterDeficitL, body.dehydrationFraction() * 100, b.bloodVolumeL, b.vitaminCMg),
        fmt("core %.2f C  fever %.2f  shiver %.2f  sweat %.2f  wet %.2f", b.coreTempC, b.feverC, b.shivering, b.sweating, b.wetness),
        fmt("sleep pressure %.2f awake %.1f h  damage %.3f liver %.2f diarrhea %.2f", b.sleepPressure, b.hoursAwake, b.systemicDamage, b.liverDamage, b.diarrhea),
        fmt("expenditure %.2f kcal/min  air %.1f C", b.lastExpenditureKcalPerMin, game_.environment().airTempC),
    };
    for (const auto& inf : b.infections)
        l.push_back(fmt("infection %s: incubation %.0f h load %.2f immunity %.2f%s", pathogenName(inf.type), inf.incubationH, inf.load, inf.immunity, inf.treated ? " treated" : ""));
    for (const auto& t : b.toxins) l.push_back(fmt("toxin %s %.2f age %.0f h", toxinName(t.type), t.amount, t.ageH));
    l.push_back(fmt("chunks %zu  animals %zu  fps %d", game_.world().loadedChunks(), game_.animals().animals().size(), GetFPS()));
    float y = 120 * s;
    DrawRectangle((int)(10 * s), (int)(y - 6), (int)(560 * s), (int)(l.size() * 18 * s + 12), Color{0, 0, 0, 180});
    for (const auto& line : l) {
        ui::text(line, 18 * s, y, 13 * s, Color{140, 230, 140, 255});
        y += 18 * s;
    }
}

// ---------------------------------------------------------------------------
// Settings & audio
// ---------------------------------------------------------------------------

void App::loadSettings() {
    Config c;
    if (!Paths::exists(settingsPath()) || !c.load(settingsPath())) return;
    if (const auto* s = c.find("settings")) {
        settings_.musicVolume = std::clamp(s->num("music_volume", settings_.musicVolume), 0.0f, 1.0f);
        settings_.sfxVolume = std::clamp(s->num("sfx_volume", settings_.sfxVolume), 0.0f, 1.0f);
        settings_.minutesPerSecond = std::clamp(s->num("minutes_per_second", settings_.minutesPerSecond), 0.25f, 4.0f);
        settings_.fullscreen = s->flag("fullscreen", false);
        settings_.showFps = s->flag("show_fps", false);
    }
}

void App::saveSettings() {
    Config c;
    auto& s = c.getOrAdd("settings");
    auto set = [&](const std::string& k, const std::string& v) { if (!s.values.count(k)) s.keyOrder.push_back(k); s.values[k] = v; };
    set("music_volume", fmt("%.2f", settings_.musicVolume));
    set("sfx_volume", fmt("%.2f", settings_.sfxVolume));
    set("minutes_per_second", fmt("%.2f", settings_.minutesPerSecond));
    set("fullscreen", settings_.fullscreen ? "true" : "false");
    set("show_fps", settings_.showFps ? "true" : "false");
    c.save(settingsPath());
}

void App::applySettings() {
    if (settings_.fullscreen != IsWindowFullscreen()) {
        if (settings_.fullscreen) {
            int mon = GetCurrentMonitor();
            SetWindowSize(GetMonitorWidth(mon), GetMonitorHeight(mon));
        }
        ToggleFullscreen();
        if (!settings_.fullscreen) SetWindowSize((int)kBaseW, (int)kBaseH);
    }
    if (musicLoaded_) SetMusicVolume(music_, settings_.musicVolume);
}

void App::updateMusic(float dt) {
    if (!audio_ || tracks_.empty()) return;
    if (musicLoaded_) {
        UpdateMusicStream(music_);
        if (!IsMusicStreamPlaying(music_) || GetMusicTimePlayed(music_) >= GetMusicTimeLength(music_) - 0.05f) {
            StopMusicStream(music_);
            UnloadMusicStream(music_);
            musicLoaded_ = false;
            musicGap_ = (float)GetRandomValue(20, 90);
        }
        return;
    }
    musicGap_ -= dt;
    if (musicGap_ > 0) return;
    track_ = (track_ + 1 + GetRandomValue(0, (int)tracks_.size() - 1)) % (int)tracks_.size();
    music_ = LoadMusicStream(tracks_[track_].c_str());
    if (music_.ctxData) {
        music_.looping = false;
        SetMusicVolume(music_, settings_.musicVolume);
        PlayMusicStream(music_);
        musicLoaded_ = true;
    } else {
        musicGap_ = 60;
    }
}

} // namespace alone
