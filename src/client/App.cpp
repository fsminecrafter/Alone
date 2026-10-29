#include "client/App.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "client/Icons.h"
#include "client/Input.h"
#include "client/Ui.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "raymath.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "rlgl.h"
#include <ctime>
#include <filesystem>

namespace alone {

namespace {

bool rc_isWater(Ground g) { return g == Ground::WaterShallow || g == Ground::WaterDeep; }

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

// Where saves and settings live: next to the executable, or in the
// browser's IndexedDB (mounted at /persist) for the web build.
std::string storageDir() {
#ifdef __EMSCRIPTEN__
    return "/persist";
#else
    return GetApplicationDirectory();
#endif
}

std::string settingsPath() {
    return (std::filesystem::path(storageDir()) / "settings.ini").string();
}

// Laptops with an NVIDIA card next to the built-in graphics (PRIME): ask the
// driver to run the game on the NVIDIA GPU, unless the player chose already
// (or set ALONE_NO_GPU_SWITCH). Windows does the same through the exports in
// main.cpp; Mesa's own switching (DRI_PRIME) is left to the player.
bool preferDiscreteGpu() {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    if (getenv("ALONE_NO_GPU_SWITCH") || getenv("__NV_PRIME_RENDER_OFFLOAD") || getenv("__GLX_VENDOR_LIBRARY_NAME") ||
        getenv("DRI_PRIME"))
        return false;
    std::error_code ec;
    if (!std::filesystem::exists("/proc/driver/nvidia/version", ec)) return false;
    bool lib = false;
    for (const char* dir : {"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", "/usr/lib/aarch64-linux-gnu"})
        lib |= std::filesystem::exists(std::filesystem::path(dir) / "libGLX_nvidia.so.0", ec);
    if (!lib) return false;
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
    Log::info("NVIDIA GPU found: asking to render on it (set ALONE_NO_GPU_SWITCH=1 to stop this)");
    return true;
#else
    return false;
#endif
}

void undoGpuPreference() {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    unsetenv("__NV_PRIME_RENDER_OFFLOAD");
    unsetenv("__GLX_VENDOR_LIBRARY_NAME");
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int App::run(int argc, char** argv) {
    std::string dataOverride;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--debug") debug_ = true;
        else if (a == "--data" && i + 1 < argc) dataOverride = argv[++i];
        else if (a == "--seed" && i + 1 < argc) seed_ = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--screenshots") autoShots_ = 1, tour_ = true;
        else if (a == "--anim-showcase" && i + 1 < argc) showcaseDir_ = argv[++i], tour_ = true;
        else if (a == "--bench") bench_ = true, tour_ = true;
    }
#ifdef __EMSCRIPTEN__
    Log::init("/tmp/alone.log");
#else
    Log::init((std::filesystem::path(storageDir()) / "alone.log").string());
#endif
    Paths::init(argv[0], dataOverride);
#ifdef __EMSCRIPTEN__
    // In a browser (often a laptop or Chromebook GPU): lighter defaults
    settings_.msaa = 0;
    settings_.gfx.shadows = 1;
    settings_.gfx.maxLights = 16;
#endif
    loadSettings();
    if (tour_) settings_.gfx.renderScale = 100;   // screenshots and benchmarks at full resolution

    unsigned flags = FLAG_WINDOW_RESIZABLE;
    // ALONE_HIDDEN=1: render without showing a window (screenshot tours, showcase)
    if (const char* hid = getenv("ALONE_HIDDEN"); hid && *hid == '1') flags |= FLAG_WINDOW_HIDDEN;
    if (const char* m = getenv("ALONE_MSAA")) settings_.msaa = atoi(m) ? 1 : 0;   // for benchmarks
    if (settings_.msaa) flags |= FLAG_MSAA_4X_HINT;
#ifndef __EMSCRIPTEN__
    if (settings_.vsync) flags |= FLAG_VSYNC_HINT;
#endif
    SetConfigFlags(flags);
    bool switched = preferDiscreteGpu();
    InitWindow((int)kBaseW, (int)kBaseH, "Alone");
    if (!IsWindowReady() && switched) {   // the discrete GPU didn't work out: try again without it
        Log::warn("Could not open the window on the NVIDIA GPU, using the default one");
        undoGpuPreference();
        InitWindow((int)kBaseW, (int)kBaseH, "Alone");
    }
    if (!IsWindowReady()) { Log::error("Could not open a window"); return 1; }
    SetWindowMinSize(960, 540);
    SetExitKey(KEY_NULL);
#ifndef __EMSCRIPTEN__
    SetTargetFPS(settings_.fpsCap);
#endif
    ui::init();
    input::init();
    if (!renderer_.init()) { CloseWindow(); return 1; }
    InitAudioDevice();
    audio_ = IsAudioDeviceReady();
    sound_.init();
    if (audio_) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(Paths::data("assets/audio"), ec))
            if (e.path().extension() == ".mp3" || e.path().extension() == ".ogg") tracks_.push_back(e.path().string());
        std::sort(tracks_.begin(), tracks_.end());
        for (const auto& e : std::filesystem::directory_iterator(Paths::data("assets/audio/danger"), ec))
            if (e.path().extension() == ".mp3" || e.path().extension() == ".ogg") dangerTracks_.push_back(e.path().string());
        std::sort(dangerTracks_.begin(), dangerTracks_.end());
        Log::info("Music: %d tracks, %d danger tracks", (int)tracks_.size(), (int)dangerTracks_.size());
    }
    applySettings();

    if (!game_.loadData()) {
        Log::error("Game data failed to load");
    }
    if (bench_) {
        int r = runBench();
        renderer_.shutdown();
        ui::shutdown();
        CloseWindow();
        return r;
    }
    if (!showcaseDir_.empty()) {
        int r = runShowcase(showcaseDir_);
        renderer_.shutdown();
        ui::shutdown();
        CloseWindow();
        return r;
    }
    if (seed_ == 1) seed_ = (uint32_t)std::time(nullptr);
    game_.setBaseTimeScale(settings_.minutesPerSecond);
    game_.newGame(seed_);
    renderer_.snapTo(game_.player().pos);
    hasSave_ = Paths::exists(savePath()) && !autoShots_;

#ifdef __EMSCRIPTEN__
    // The browser drives the frames
    emscripten_set_main_loop_arg([](void* app) { static_cast<App*>(app)->frame(); }, this, 0, true);
#else
    while (!WindowShouldClose() && !quit_) frame();
    shutdownApp();
#endif
    return 0;
}

void App::frame() {
    float dt = std::min(GetFrameTime(), 0.1f);
    time_ += dt;
    ui::beginFrame();
    update(dt);

    BeginDrawing();
    HoverInfo hv;
    hv.point = hover_;
    hv.show = state_ == State::Playing && !ui::mouseOverUi() && overlay_ != Overlay::Map && !menu_.open;
    hv.colour = hoverColour();
    if (state_ == State::Playing && game_.player().aiming && game_.player().vehicle < 0 && overlay_ == Overlay::None) {
        hv.aiming = true;
        hv.from = game_.player().pos;
        const ItemInstance* w = game_.weapon();
        hv.range = w ? w->def->reach + (w->def->firearm ? 0.0f : 0.5f) : 1.4f;
        hv.ranged = w && w->def->firearm;
    }
    if (buildKind_ >= 0 && state_ == State::Playing) {
        hv.buildKind = buildKind_;
        hv.bx = buildX_; hv.by = buildY_; hv.bdir = buildDir_;
        hv.buildOk = game_.buildOption((BuildKind)buildKind_, buildX_, buildY_, buildDir_).enabled;
    }
    renderer_.draw(game_, time_, dt, hv);
    if (state_ == State::Menu) drawMenu();
    else if (state_ == State::Create) drawCreate();
    else {
        drawSurvivorLabels();
        drawHud();
        drawVehicleHud();
        switch (overlay_) {
            case Overlay::Inventory: drawInventory(); break;
            case Overlay::Talk: drawTalk(); break;
            case Overlay::Trade: drawTrade(); break;
            case Overlay::Journal: drawJournal(); break;
            case Overlay::Health: drawHealth(); break;
            case Overlay::Build: drawBuildMenu(); break;
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
    drawCursor();
    input::endFrame();
    EndDrawing();

    // --screenshots: automated tour used for documentation / smoke tests
    if (autoShots_) {
        ++frame_;
        struct Stop { float px, py, hour; const char* file; Overlay ov; };
        static const Stop stops[] = {
            {0, 0, 0, "screenshot_menu.png", Overlay::None},
            {0, 0, 0, "screenshot_game.png", Overlay::None},
            {0, 0, 0, "screenshot_context.png", Overlay::None},
            {0, 0, 0, "screenshot_inventory.png", Overlay::Inventory},
            {0, 0, 0, "screenshot_map.png", Overlay::Map},
            {640, 364, 13, "screenshot_torget.png", Overlay::None},
            {641, 370, 12.5f, "screenshot_rain.png", Overlay::None},
            {767.3f, 409.9f, 14.5f, "screenshot_hills.png", Overlay::None},
            {1, 1, 14, "screenshot_vimpeln.png", Overlay::None},
            {1, 1, 15, "screenshot_vimpeln_inside.png", Overlay::None},
            {1, 1, 12, "screenshot_kungsgatan.png", Overlay::None},
            {618.9f, 352.5f, 15, "screenshot_church.png", Overlay::None},
            {300, 520, 11, "screenshot_forest.png", Overlay::None},
            {474, 352, 16, "screenshot_nolhaga.png", Overlay::None},
            {700.0f, 510.5f, 18.3f, "screenshot_gerdsken_dusk.png", Overlay::None},
            {621.3f, 412.0f, 21.5f, "screenshot_station_night.png", Overlay::None},
            {760, 410, 12, "screenshot_e20.png", Overlay::None},
            {880, 120, 10, "screenshot_wildlife.png", Overlay::None},
            {0, 0, 0, "screenshot_watch.png", Overlay::None},
            {0, 0, 0, "screenshot_health.png", Overlay::Health},
            {0, 0, 0, "screenshot_health_info.png", Overlay::Health},
            {0, 0, 0, "screenshot_camp.png", Overlay::None},
            {1086.6f, 322.1f, 11, "screenshot_hjortmarka.png", Overlay::None},
            {0, 0, 0, "screenshot_settings_audio.png", Overlay::Settings},
            {0, 0, 0, "screenshot_settings_graphics.png", Overlay::Settings},
            {0, 0, 0, "screenshot_symptoms.png", Overlay::None},
            {0, 0, 0, "screenshot_build.png", Overlay::Build},
            {0, 0, 0, "screenshot_people.png", Overlay::None},
            {0, 0, 0, "screenshot_trade.png", Overlay::None},
            {0, 0, 0, "screenshot_journal.png", Overlay::Journal},
            {0, 0, 0, "screenshot_parley.png", Overlay::None},
            {1, 1, 13, "screenshot_vimpeln_carpark.png", Overlay::None},
            {0, 0, 0, "screenshot_fuel_station.png", Overlay::None},
            {0, 0, 0, "screenshot_aim.png", Overlay::None},
            {0, 0, 0, "screenshot_create.png", Overlay::None},
            {0, 0, 0, "screenshot_sleeping.png", Overlay::None},
            {0, 0, 0, "screenshot_friends.png", Overlay::None},
            {0, 0, 0, "screenshot_death.png", Overlay::None},
        };
        int n = (int)(sizeof(stops) / sizeof(stops[0]));
        // ALONE_SHOTS=storken,forest renders only matching stops (plus the menu and new game)
        static const char* only = getenv("ALONE_SHOTS");
        if (only && frame_ % 45 == 0)
            while (frame_ / 45 >= 5 && frame_ / 45 < n && !std::strstr(stops[frame_ / 45].file, "death") &&
                   [&] {
                       std::string f = stops[frame_ / 45].file;
                       std::stringstream ss(only);
                       std::string tok;
                       while (std::getline(ss, tok, ','))
                           if (!tok.empty() && f.find(tok) != std::string::npos) return false;
                       return true;
                   }())
                frame_ += 45;
        int idx = frame_ / 45, sub = frame_ % 45;
        if (idx >= n) quit_ = true;
        else {
            const Stop& st = stops[idx];
            if (sub == 0) {
                menu_.open = false;
                if (idx == 1) newGame(false);
                if (idx == 2 || idx == 3) {
                    // Find a kitchen container in the start house
                    Player& pl = game_.player();
                    const Building* b = game_.world().buildingAt((int)pl.pos.x, (int)pl.pos.y);
                    // a container with something in it (kitchens first)
                    std::vector<const Furniture*> order;
                    if (b) {
                        for (const auto& f : b->furniture)
                            if (f.container == "kitchen_cupboard" || f.container == "pantry" || f.container == "fridge") order.push_back(&f);
                        for (const auto& f : b->furniture)
                            if (!f.container.empty()) order.push_back(&f);
                    }
                    const Furniture* pick = nullptr;
                    for (const Furniture* f : order)
                        if (!game_.world().container(f->x, f->y).items.empty()) { pick = f; break; }
                    if (!pick && !order.empty()) pick = order.front();
                    if (pick) for (const auto& f : std::vector<Furniture>{*pick}) {
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
                if (std::string(st.file) == "screenshot_map.png")
                    for (auto& sv : game_.survivors())
                        if (sv.def->id == "elin")
                            for (int k = 0; k < 4; ++k) game_.talk(sv.id, Topic::Others);
                if (st.px > 0) {
                    float mpp = game_.map().metresPerPixel();
                    game_.player().pos = Vec2{st.px * mpp, st.py * mpp};
                    double now = game_.world().clock().hourOfDay();
                    double add = st.hour - now;
                    if (add < 0) add += 24;
                    game_.world().clock().advance(add * 60);
                    renderer_.snapTo(game_.player().pos);
                }
                {
                    // Landmarks: stand outside the entrance, or in the food court
                    std::string f = st.file;
                    const char* name = f.find("vimpeln") != std::string::npos ? "Vimpeln"
                                     : f.find("kungsgatan") != std::string::npos ? "Gallerian Storken" : nullptr;
                    if (name) {
                        Vec2 at;
                        for (const auto& ob : game_.map().osmBuildings())
                            if (ob.name == name) at = ob.centre;
                        game_.world().tile((int)at.x, (int)at.y);
                        for (const auto& b : game_.world().gen().allBuildings()) {
                            if (b.name != name) continue;
                            Vec2 out = (b.doorPos - b.rect.centre()).normalized();
                            at = b.doorPos + out * (f.find("vimpeln.png") != std::string::npos ? 11.0f : 5.0f);
                            if (f.find("inside") != std::string::npos) {
                                for (const auto& r : b.rooms)
                                    if (r.type == RoomType::Dining) at = r.rect.centre() + Vec2{0.5f, 0.5f};
                                for (const auto& r : b.rooms)
                                    if (r.loot == "food_court_counter") at = r.rect.centre() + Vec2{0.5f, 0.5f};
                            } else {
                                renderer_.faceTowards(out);   // look at the front of it
                            }
                        }
                        game_.player().pos = at;
                        renderer_.snapTo(at);
                    }
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
            if (sub == 0) healthTab_ = std::string(st.file) == "screenshot_health_info.png" ? 1 : 0;
            if (sub == 0 && std::string(st.file).find("settings_") != std::string::npos)
                settingsTab_ = std::string(st.file).find("graphics") != std::string::npos ? 1 : 0;
            if (std::string(st.file) == "screenshot_hjortmarka.png" && sub == 0) {
                Weather fine;
                fine.kind = WeatherKind::Cloudy;
                fine.cloud = 0.4f;
                game_.world().clock().forceWeather(fine, game_.world().clock().totalMinutes() + 600);
            }
            if (std::string(st.file) == "screenshot_health.png" && sub == 0) {
                // A few wounds in different states of care
                Body& body = game_.player().body;
                body.mutableState().wounds.clear();
                for (auto& d : body.mutableState().dressings) d = Dressing{};
                game_.animals().animals().clear();
                Wound cut; cut.part = BodyPart::LeftArm; cut.type = WoundType::Laceration; cut.severity = 0.55f; cut.bleeding = 6;
                body.addWound(cut);
                body.applyDressing(BodyPart::LeftArm, DressingKind::Rag, false, 0.7f);
                body.mutableState().dressings[(int)BodyPart::LeftArm].blood = 0.6f;
                Wound sc; sc.part = BodyPart::RightHand; sc.type = WoundType::Scratch; sc.severity = 0.15f; sc.bleeding = 1;
                body.addWound(sc);
                body.applyDressing(BodyPart::RightHand, DressingKind::Bandaid, true, 0);
                Wound bite; bite.part = BodyPart::LeftLeg; bite.type = WoundType::Bite; bite.severity = 0.45f; bite.bleeding = 9;
                bite.infection = 0.4f;
                body.addWound(bite);
                Wound burn; burn.part = BodyPart::RightFoot; burn.type = WoundType::Burn; burn.severity = 0.3f; burn.healed = 0.5f;
                body.addWound(burn);
                body.applyDressing(BodyPart::RightFoot, DressingKind::Bandage, true, 0.1f);
            }
            if (std::string(st.file) == "screenshot_station_night.png" && sub == 0) {
                Weather clear;
                clear.kind = WeatherKind::Clear;
                clear.cloud = 0.1f;
                game_.world().clock().forceWeather(clear, game_.world().clock().totalMinutes() + 600);
            }
            if (std::string(st.file) == "screenshot_rain.png" && sub == 0) {
                // A downpour on the square: the streaks, rings and puddles
                Weather wx;
                wx.kind = WeatherKind::Rain;
                wx.rain = 0.9f;
                wx.cloud = 0.95f;
                wx.wind = 6.0f;
                double now = game_.world().clock().totalMinutes();
                game_.world().clock().forceWeather(wx, now + 70);
                game_.world().update(60.0f, game_.player().pos);   // an hour of it: everything's soaked
            }
            if (std::string(st.file) == "screenshot_camp.png" && sub == 0) {
                // A camp outside the start house on a sunny afternoon: a tent, a storm
                // kitchen, a little plank cabin
                World& w = game_.world();
                Player& pl = game_.player();
                Weather fine;
                fine.kind = WeatherKind::Clear;
                fine.cloud = 0.15f;
                fine.wind = 2.0f;
                double now = w.clock().totalMinutes();
                double add = 16.5 - w.clock().hourOfDay();
                if (add < 0) add += 24;
                w.clock().advance(add * 60);
                w.clock().forceWeather(fine, now + add * 60 + 600);
                pl.body.mutableState().wounds.clear();
                for (auto& d : pl.body.mutableState().dressings) d = Dressing{};
                game_.animals().animals().clear();
                const Building* home = w.gen().findStartHouse(game_.map().spawn());
                Vec2 base = pl.pos;
                if (home) base = home->doorPos + (home->doorPos - home->rect.centre()).normalized() * 7.0f;
                int bx = (int)std::floor(base.x), by = (int)std::floor(base.y);
                Tent t; t.id = w.newId(); t.rect = IRect{bx + 3, by - 1, 2, 2}; t.dir = 3; t.item = "tent_dome";
                w.tents().push_back(t);
                Tent bivvy; bivvy.id = w.newId(); bivvy.rect = IRect{bx + 6, by - 1, 1, 2}; bivvy.dir = 2; bivvy.item = "tent_small";
                w.tents().push_back(bivvy);
                Campfire stove; stove.pos = Vec2{bx + 1.5f, by + 2.5f}; stove.id = w.newId(); stove.stove = true;
                stove.fuelMl = 80; stove.lit = true; stove.fuelMinutes = 20;
                w.campfires().push_back(stove);
                for (int i = 0; i < 3; ++i) {
                    w.addBuild(Built{BuildKind::Wall, bx - 4 + i, by - 2, 0});
                    w.addBuild(Built{BuildKind::Floor, bx - 4 + i, by - 2, 0});
                    w.addBuild(Built{BuildKind::Floor, bx - 4 + i, by - 1, 0});
                    w.addBuild(Built{BuildKind::Roof, bx - 4 + i, by - 2, 0});
                    w.addBuild(Built{BuildKind::Roof, bx - 4 + i, by - 1, 0});
                }
                w.addBuild(Built{BuildKind::Wall, bx - 4, by - 2, 1});
                w.addBuild(Built{BuildKind::Wall, bx - 4, by - 1, 1});
                w.addBuild(Built{BuildKind::Window, bx - 1, by - 2, 1});
                w.addBuild(Built{BuildKind::Wall, bx - 1, by - 1, 1});
                w.addBuild(Built{BuildKind::Door, bx - 3, by, 0});
                w.addBuild(Built{BuildKind::Wall, bx - 4, by, 0});
                w.addBuild(Built{BuildKind::Window, bx - 2, by, 0});
                w.addBuild(Built{BuildKind::Crate, bx, by - 2, 0});
                pl.pos = Vec2{bx + 0.6f, by + 2.6f};
                pl.facing = 3.6f;
                renderer_.snapTo(pl.pos);
            }
            if (sub == 0 && idx > 0 && std::string(stops[idx - 1].file) == "screenshot_symptoms.png") {
                // after the starving-body shot: a healthy body, and the pointer off the symptoms
                game_.player().body = Body(seed_ + 99);
                SetMousePosition(GetScreenWidth() / 2, GetScreenHeight() / 2);
            }
            if (std::string(st.file) == "screenshot_build.png" && sub == 0) {
                Rng rng(5);
                for (int i = 0; i < 4; ++i) game_.player().inventory.add(game_.items().create("plank", rng));
                game_.player().inventory.add(game_.items().create("nails", rng));
                game_.player().inventory.add(game_.items().create("hammer", rng));
                buildPick_ = (int)BuildKind::Door;
            }
            Survivor* elin = nullptr;
            for (auto& sv : game_.survivors())
                if (sv.def->id == "elin") elin = &sv;
            std::string file = st.file;
            if ((file == "screenshot_people.png" || file == "screenshot_trade.png") && elin && sub == 0) {
                game_.player().pos = elin->pos + Vec2{1.3f, 0.4f};
                elin->target = elin->pos;
                game_.animals().animals().clear();
                renderer_.snapTo(game_.player().pos);
                double add = 12.5 - game_.world().clock().hourOfDay();
                if (add < 0) add += 24;
                game_.world().clock().advance(add * 60);
                openTalk(elin->id);
                if (file == "screenshot_people.png") talkLine_ = game_.talk(elin->id, Topic::Request);
                else {
                    overlay_ = Overlay::Trade;
                    tradeMine_.clear(); tradeTheirs_.clear();
                    for (const auto& it : game_.player().inventory.items()) if (it.def->category == ItemCategory::Food) { tradeMine_.push_back(it.uid); break; }
                    for (const auto& it : elin->inventory.items()) if (it.def->id == "bandage") { tradeTheirs_.push_back(it.uid); break; }
                }
            }
            if (file == "screenshot_create.png" && sub == 0) {
                CharacterChoice c = CharacterChoice::defaults();
                c.look.name = "Maja";
                c.look.gender = Gender::Woman;
                c.look.hairStyle = 2;
                c.look.hair = 0x9a3a20;
                c.look.skin = 0xe8c4a8;
                c.wear[(size_t)ClothingSlot::Sweater] = "hoodie";
                c.tint[(size_t)ClothingSlot::Sweater] = 0x3e5a36;
                c.wear[(size_t)ClothingSlot::Head] = "";
                create_ = c;
                newGame(true);
                double add = 12.5 - game_.world().clock().hourOfDay();
                if (add < 0) add += 24;
                game_.world().clock().advance(add * 60);
            }
            if (file == "screenshot_sleeping.png" && sub == 0 && elin) {
                if (state_ == State::Create) beginGame();
                double add = 2.0 - game_.world().clock().hourOfDay();
                if (add < 0) add += 24;
                game_.world().clock().advance(add * 60);
                elin->state = SurvivorState::Sleep;
                elin->pos = elin->bed;
                game_.player().pos = elin->bedSide + (elin->bedSide - elin->bed) * 0.8f;
                game_.player().flashlightOn = true;
                Rng rng(4);
                game_.player().inventory.add(game_.items().create("flashlight", rng));
                game_.animals().animals().clear();
                renderer_.setZoom(14);
                renderer_.snapTo(game_.player().pos);
            }
            if (file == "screenshot_friends.png" && sub == 0 && elin) {
                double add = 13.0 - game_.world().clock().hourOfDay();
                if (add < 0) add += 24;
                game_.world().clock().advance(add * 60);
                if (elin->asleep()) { elin->state = SurvivorState::Home; elin->pos = elin->bedSide; }
                elin->met = elin->known = true;
                elin->trust = 72;
                elin->romance = 60;
                elin->lover = true;
                elin->shownBond = Bond::Lover;
                game_.player().pos = elin->pos + Vec2{1.3f, 0.4f};
                renderer_.setZoom(30);
                renderer_.snapTo(game_.player().pos);
                openTalk(elin->id);
                talkLine_ = game_.talk(elin->id, Topic::Chat);
            }
            if (file == "screenshot_journal.png" && sub == 0) {
                for (auto& sv : game_.survivors()) {
                    if (sv.def->id == "elin") { sv.met = sv.known = true; sv.requestKnown = true; sv.trust = 30; }
                    if (sv.def->id == "gunnar" || sv.def->id == "sofia" || sv.def->id == "kalle" || sv.def->id == "jonas") sv.known = true;
                }
            }
            if (file == "screenshot_vimpeln_carpark.png" && sub == 1) {
                // On a bike in the Vimpeln car park, with a car someone got going
                Vec2 p0 = game_.player().pos;
                int cx = 0, cy = 0;
                bool found = false;
                for (int r = 2; r < 40 && !found; ++r)
                    for (int dy = -r; dy <= r && !found; ++dy)
                        for (int dx = -r; dx <= r && !found; ++dx)
                            if (game_.world().tile((int)p0.x + dx, (int)p0.y + dy).prop == Prop::Car) { cx = (int)p0.x + dx; cy = (int)p0.y + dy; found = true; }
                Vehicle car;
                car.id = 900;
                car.kind = VehicleKind::Car;
                car.pos = found ? Vec2{cx + 0.5f, cy + 4.5f} : p0 + Vec2{4, 2};
                car.heading = 0.6f;
                car.variant = 3 * 16 + 4;   // a red van
                car.running = true;
                game_.vehicles().push_back(car);
                Vehicle bike;
                bike.id = 901;
                bike.kind = VehicleKind::Bike;
                bike.pos = car.pos + Vec2{-3.0f, 2.5f};
                bike.heading = -0.4f;
                bike.variant = 1;
                game_.vehicles().push_back(bike);
                Vehicle parked = bike;
                parked.id = 902;
                parked.pos = bike.pos + Vec2{1.8f, 1.2f};
                parked.variant = 2;
                parked.heading = 1.2f;
                game_.vehicles().push_back(parked);
                game_.player().vehicle = 901;
                game_.player().pos = bike.pos;
                renderer_.snapTo(bike.pos);
                game_.animals().animals().clear();
            }
            if (file == "screenshot_parley.png" && sub == 0) {
                // Shouting across the yard at the man in the police station
                for (auto& sv : game_.survivors())
                    if (sv.def->id == "jonas") {
                        const Building* b = game_.world().buildingAt((int)sv.pos.x, (int)sv.pos.y);
                        Vec2 out = b ? (b->doorPos - b->rect.centre()).normalized() : Vec2{1, 0};
                        sv.pos = b ? b->doorPos + out * 1.5f : sv.pos;
                        game_.player().pos = sv.pos + out * 11.0f;
                        renderer_.snapTo(game_.player().pos);
                        openTalk(sv.id);
                        talkLine_ = game_.talk(sv.id, Topic::Parley);
                    }
                game_.animals().animals().clear();
            }
            if (file == "screenshot_fuel_station.png" && sub == 0) {
                game_.player().vehicle = -1;
                for (const auto& b : game_.world().gen().allBuildings())
                    if (b.type == BuildingType::GasStation && b.name == "Preem") {
                        Vec2 out = (b.pathEnd - b.doorPos).normalized();
                        game_.player().pos = b.doorPos + out * 9.0f;
                        renderer_.snapTo(game_.player().pos);
                    }
                double add = 14 - game_.world().clock().hourOfDay();
                if (add < 0) add += 24;
                game_.world().clock().advance(add * 60);
                game_.animals().animals().clear();
            }
            if (file == "screenshot_aim.png") {
                if (sub == 0) {
                    Rng rng(2);
                    // a loaded rifle in the hands, the bow slung on the back
                    ItemInstance bow = game_.items().create("hunting_bow", rng);
                    game_.player().inventory.add(bow);
                    game_.player().inventory.add(game_.items().create("arrows", rng));
                    ItemInstance rifle = game_.items().create("hunting_rifle", rng);
                    rifle.usesLeft = 4;
                    game_.player().inventory.add(rifle);
                    game_.player().inventory.add(game_.items().create("rifle_ammo", rng));
                    game_.player().weaponUid = rifle.uid;
                    game_.player().stowedUid = bow.uid;
                }
                Vector2 m = renderer_.toScreen(game_.player().pos + Vec2{5, -3}, 0);
                SetMousePosition((int)m.x, (int)m.y);
                PlayerInput aim;
                aim.aimHold = true;
                aim.aim = game_.player().pos + Vec2{5, -3};
                hover_ = aim.aim;
                game_.update(0.016f, aim);
            }
            if (std::string(st.file) == "screenshot_watch.png" && sub == 0) {
                Rng rng(9);
                game_.player().inventory.add(game_.items().create("wristwatch", rng));
                game_.player().inventory.add(game_.items().create("thermometer", rng));
            }
            if (std::string(st.file) == "screenshot_death.png" && sub == 0) {
                game_.player().body.mutableState().ammonia = 1.2f;
                menu_.open = false;
            }
            if (sub == 44) TakeScreenshot(st.file);
        }
    }
}

void App::shutdownApp() {
    if (!autoShots_) autosave();
    if (!tour_) saveSettings();   // a tour's character and resolution aren't yours
    if (musicLoaded_) UnloadMusicStream(music_);
    sound_.shutdown();
    if (audio_) CloseAudioDevice();
    if (mapReady_) UnloadTexture(mapTex_);
    renderer_.shutdown();
    input::shutdown();
    ui::shutdown();
    CloseWindow();
}

std::string App::savePath() const {
    return (std::filesystem::path(storageDir()) / "save.txt").string();
}

void App::autosave() {
    if (state_ == State::Playing && game_.running() && !game_.dead()) {
        if (game_.save(savePath())) hasSave_ = true;
        persistFiles();
    }
}

void App::persistFiles() {
#ifdef __EMSCRIPTEN__
    EM_ASM(if (typeof FS !== 'undefined' && FS.syncfs) FS.syncfs(false, function(err) { if (err) console.warn('save sync failed', err); }););
#endif
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
    selected_ = ItemRef{};
}

Color App::hoverColour() {
    // Red over animals, warm over things you can use, white otherwise
    for (const auto& a : game_.animals().animals())
        if (distance(a.pos, hover_) < std::max(0.7f, a.def->size * 0.6f)) return a.alive() ? Color{230, 80, 60, 255} : Color{230, 170, 80, 255};
    int tx = (int)std::floor(hover_.x), ty = (int)std::floor(hover_.y);
    World& w = game_.world();
    const Tile& t = w.tile(tx, ty);
    int dx, dy, dd;
    if (!w.containerKindAt(tx, ty).empty() || w.findDoorNear(hover_, 0.6f, dx, dy, dd) || t.prop == Prop::LingonBush ||
        t.prop == Prop::BlueberryBush || t.prop == Prop::RoseBush || t.prop == Prop::Bed || t.prop == Prop::Toilet ||
        t.prop == Prop::RainBarrel || rc_isWater(t.ground))
        return Color{240, 200, 110, 255};
    return Color{235, 235, 230, 255};
}

void App::newGame(bool create) {
    seed_ = seed_ * 1664525u + 1013904223u;
    game_.setBaseTimeScale(settings_.minutesPerSecond);
    game_.setAutoDrink(settings_.autoDrink);
    game_.newGame(seed_, create ? &create_ : nullptr);
    renderer_.invalidateAll();
    renderer_.snapTo(game_.player().pos);
    state_ = State::Playing;
    overlay_ = Overlay::None;
    menu_.open = false;
    selected_ = ItemRef{};
    if (create) {
        // Make your character first, standing in the house you wake up in
        state_ = State::Create;
        nameFocus_ = false;
        createSpin_ = 0;
        renderer_.setZoom(4.2f);
    }
}

void App::update(float dt) {
    updateMusic(dt);
    if (audio_) {
        bool mute = settings_.muteUnfocused && !IsWindowFocused() && !tour_;
        SetMasterVolume(mute ? 0.0f : settings_.masterVolume);
    }
    sound_.update(game_, state_ == State::Menu ? settings_.ambienceVolume * 0.5f : settings_.ambienceVolume);
    for (const auto& e : game_.takeSounds())
        if (state_ != State::Menu) sound_.play(e, game_.player().pos, settings_.sfxVolume);
    if (IsKeyPressed(KEY_F11)) { settings_.fullscreen = !settings_.fullscreen; applySettings(); }
#ifdef __EMSCRIPTEN__
    if (int fs = EM_ASM_INT({ return Module.fullscreenState ? Module.fullscreenState() : -1; }); fs >= 0 && (fs == 1) != settings_.fullscreen &&
        EM_ASM_INT({ return Module.fullscreenPending ? 0 : 1; }))
        settings_.fullscreen = fs == 1;
#endif
    if (state_ == State::Menu) {
        menuOrbit_ += dt;
        renderer_.follow(game_.player().pos + Vec2{std::cos(menuOrbit_ * 0.05f) * 6, std::sin(menuOrbit_ * 0.05f) * 6}, dt);
        PlayerInput none;
        game_.update(dt * 0.2f, none);
        return;
    }
    if (state_ == State::Create) { updateCreate(dt); return; }
    if (state_ == State::Playing) updatePlaying(dt);
    else {
        PlayerInput none;
        game_.update(dt, none);
        renderer_.follow(game_.player().pos, dt);
    }
}

void App::updatePlaying(float dt) {
    // When a search finishes, open what was searched so you can look through it
    if (lootPending_) {
        if (game_.hasOpenContainer()) {
            // only worth opening if there's something in it
            if (!game_.world().container(game_.openContainerX(), game_.openContainerY()).items.empty()) overlay_ = Overlay::Inventory;
            else game_.closeContainer();
            lootPending_ = false;
        }
        else if (!game_.currentAction().active) lootPending_ = false;   // cancelled
    }
    // Keyboard shortcuts
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (menu_.open) menu_.open = false;
        else if (buildKind_ >= 0) buildKind_ = -1;
        else if (overlay_ == Overlay::Settings) overlay_ = settingsReturn_;
        else if (overlay_ != Overlay::None) overlay_ = Overlay::None;
        else overlay_ = Overlay::Pause;
    }
    bool blocking = overlay_ == Overlay::Pause || overlay_ == Overlay::Settings || overlay_ == Overlay::Help || overlay_ == Overlay::Map;
    if (!blocking || overlay_ == Overlay::Map) {
        if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_I)) { overlay_ = overlay_ == Overlay::Inventory ? Overlay::None : Overlay::Inventory; menu_.open = false; }
        if (IsKeyPressed(KEY_M)) overlay_ = overlay_ == Overlay::Map ? Overlay::None : Overlay::Map;
        if (IsKeyPressed(KEY_N)) { settings_.minimap = !settings_.minimap; saveSettings(); }
        if (IsKeyPressed(KEY_H)) overlay_ = overlay_ == Overlay::Health ? Overlay::None : Overlay::Health;
        if (IsKeyPressed(KEY_J)) overlay_ = overlay_ == Overlay::Journal ? Overlay::None : Overlay::Journal;
        if (IsKeyPressed(KEY_B)) {
            if (buildKind_ >= 0) buildKind_ = -1;
            else overlay_ = overlay_ == Overlay::Build ? Overlay::None : Overlay::Build;
        }
    }
    if (IsKeyPressed(KEY_F1)) overlay_ = overlay_ == Overlay::Help ? Overlay::None : Overlay::Help;
    // Someone came up to you to talk (Jonas, gun raised)
    if (int who = game_.takeTalkRequest(); who && (overlay_ == Overlay::None || overlay_ == Overlay::Inventory)) openTalk(who);
    if (debug_ && IsKeyPressed(KEY_F3)) showDebug_ = !showDebug_;

    PlayerInput in;
    if (!blocking) {
        float sx = 0, sy = 0;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) sy -= 1;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) sy += 1;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) sx -= 1;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) sx += 1;
        in.move = renderer_.screenToWorldDir(sx, sy);
        // In a car or on a bike: W/S throttle and brake, A/D steer
        in.throttle = -sy;
        in.steer = sx;
        in.run = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        in.sneak = IsKeyDown(KEY_C);
        in.aimHold = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (IsKeyPressed(KEY_Q)) renderer_.rotate(-1);
        if (IsKeyPressed(KEY_E)) renderer_.rotate(1);
        if (IsKeyPressed(KEY_ONE)) game_.toggleStow();   // the weapon on your back <-> the one in your hands
        if (IsKeyPressed(KEY_TWO)) game_.setTimeSpeed(game_.timeSpeed() == 5 ? 1 : 5);
        if (IsKeyPressed(KEY_THREE)) game_.setTimeSpeed(game_.timeSpeed() == 20 ? 1 : 20);
        if (in.move.lengthSq() > 0 || in.run) { if (game_.timeSpeed() > 1) game_.setTimeSpeed(1); }
        if (IsKeyPressed(KEY_L) && game_.player().inventory.findTag("light") >= 0)
            game_.player().flashlightOn = !game_.player().flashlightOn;
        if (IsKeyPressed(KEY_Z)) {
            ActionOption o;
            o.kind = ActionKind::Sleep;
            o.label = "Sleeping";
            game_.perform(o);
        }
        if (IsKeyPressed(KEY_R) && game_.weapon() && game_.weapon()->def->magazine > 0) {
            if (game_.canReload()) game_.reload();   // holding a gun: R reloads
        } else if (IsKeyPressed(KEY_R)) {
            ActionOption o;
            o.kind = ActionKind::Rest;
            o.label = "Resting";
            game_.perform(o);
        }
        worldInput();
        in.aim = hover_;
        in.attack = (IsKeyPressed(KEY_SPACE)) ||
                    (input::pressed(MOUSE_BUTTON_LEFT) && !ui::mouseOverUi() && !menu_.open && overlay_ != Overlay::Map);
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
        persistFiles();
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
    if (buildKind_ >= 0) { updateBuildPlacement(); return; }
    if (input::pressed(MOUSE_BUTTON_RIGHT)) {
        menu_.options = game_.actionsAt(hover_);
        menu_.open = !menu_.options.empty();
        menu_.pos = mouse;
        menu_.title = "";
    }
    ActionOption o;
    if (IsKeyPressed(KEY_F) && interactTarget(o)) {
        game_.perform(o);
        if (o.kind == ActionKind::OpenContainer) overlay_ = Overlay::Inventory;
        if (o.kind == ActionKind::Search) lootPending_ = true;
        if (o.kind == ActionKind::Talk) openTalk(o.targetId);
    }
}

bool App::interactTarget(ActionOption& out) {
    const Player& p = game_.player();
    if (game_.currentAction().active || p.asleep) return false;
    Vec2 dir{std::cos(p.facing), std::sin(p.facing)};
    auto usable = [](const ActionOption& o) {
        return o.enabled && o.kind != ActionKind::Rest && o.kind != ActionKind::Sleep && o.kind != ActionKind::Build &&
               o.kind != ActionKind::Dismantle;   // tearing things down needs a right-click
    };
    // A door in front of you (or right beside you) comes first
    for (Vec2 pt : {p.pos + dir * 0.7f, p.pos + dir * 0.3f, p.pos})
        for (const auto& o : game_.actionsAt(pt))
            if (o.kind == ActionKind::ToggleDoor && o.enabled) { out = o; return true; }
    // then whatever is in front of you, then what the mouse points at
    for (Vec2 pt : {p.pos + dir * 0.8f, hover_})
        for (const auto& o : game_.actionsAt(pt))
            if (usable(o)) { out = o; return true; }
    return false;
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
#ifndef __EMSCRIPTEN__
    if (ui::button(b, "Quit", true, 20)) quit_ = true;
#endif
    ui::text("v0.3 - early prototype", x, H - 40 * s, 14 * s, ui::theme().dim);
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
#ifndef __EMSCRIPTEN__
    if (ui::button(b, "Save and quit game")) quit_ = true;
#endif
}

void App::drawSettings() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 120});
    float pw = std::min(600 * s, W - 20), ph = std::min(640 * s, H - 16);
    Rectangle p{W / 2 - pw / 2, H / 2 - ph / 2, pw, ph};
    ui::panel(p);
    ui::claim(p);
    ui::textCentered("Settings", W / 2, p.y + 14 * s, 28 * s, ui::theme().text, true);

    // Tabs
    const char* tabs[] = {"Audio", "Graphics", "Game"};
    float tw = (p.width - 60 * s) / 3;
    for (int i = 0; i < 3; ++i) {
        Rectangle tb{p.x + 30 * s + i * tw, p.y + 56 * s, tw - 6 * s, 34 * s};
        bool cur = i == settingsTab_;
        if (ui::button(tb, tabs[i], true, 18)) settingsTab_ = i;
        if (cur) DrawRectangle((int)tb.x, (int)(tb.y + tb.height - 3 * s), (int)tb.width, (int)(3 * s), ui::theme().accent);
    }
    float x = p.x + 30 * s, w = p.width - 60 * s, y = p.y + 104 * s;
    float labelW = w * 0.42f, cx = x + labelW, cw = w - labelW;
    const float row = 37 * s;
    bool changed = false;

    auto label = [&](const std::string& t, const std::string& value = "") {
        ui::text(t, x, y + 8 * s, 16 * s, ui::theme().text);
        if (!value.empty()) ui::text(value, cx - ui::measure(value, 14 * s).x - 10 * s, y + 9 * s, 14 * s, ui::theme().dim);
    };
    auto slider = [&](const std::string& t, float& v, float lo, float hi, const std::string& shown) {
        label(t, shown);
        if (ui::slider({cx, y + 12 * s, cw, 14 * s}, v, lo, hi)) changed = true;
        y += row;
    };
    auto choice = [&](const std::string& t, const std::vector<std::string>& opts, int& idx) {
        label(t);
        float bw = cw / opts.size();
        int was = idx;
        for (size_t i = 0; i < opts.size(); ++i) {
            Rectangle r{cx + i * bw, y + 2 * s, bw - 4 * s, 30 * s};
            if (ui::button(r, opts[i], true, 15) && idx != (int)i) { idx = (int)i; changed = true; }
            if ((int)i == was) {   // the current choice
                DrawRectangleRec(r, Color{214, 168, 92, 70});
                DrawRectangleLinesEx(r, 2, ui::theme().accent);
            }
        }
        y += row;
    };
    auto toggle = [&](const std::string& t, bool& v) {
        int idx = v ? 1 : 0;
        choice(t, {"Off", "On"}, idx);
        v = idx == 1;
    };
    auto pct = [](float v) { return fmt("%d%%", (int)std::lround(v * 100)); };

    if (settingsTab_ == 0) {
        slider("Master volume", settings_.masterVolume, 0, 1, pct(settings_.masterVolume));
        slider("Music", settings_.musicVolume, 0, 1, pct(settings_.musicVolume));
        slider("Sound effects", settings_.sfxVolume, 0, 1, pct(settings_.sfxVolume));
        slider("Ambience (rain, wind)", settings_.ambienceVolume, 0, 1, pct(settings_.ambienceVolume));
        toggle("Mute in background", settings_.muteUnfocused);
        if (!audio_) ui::text("No audio device found.", x, y + 8 * s, 15 * s, ui::theme().bad);
    } else if (settingsTab_ == 1) {
        toggle("Fullscreen", settings_.fullscreen);
#ifndef __EMSCRIPTEN__
        toggle("V-sync", settings_.vsync);
        {
            static const int caps[] = {30, 60, 120, 144, 0};
            int idx = 1;
            for (int i = 0; i < 5; ++i) if (caps[i] == settings_.fpsCap) idx = i;
            choice("Frame rate limit", {"30", "60", "120", "144", "None"}, idx);
            settings_.fpsCap = caps[idx];
        }
#endif
        {
            static const int scales[] = {0, 100, 75, 50};
            int idx = 0;
            for (int i = 0; i < 4; ++i) if (scales[i] == settings_.gfx.renderScale) idx = i;
            std::string autoLabel = settings_.gfx.renderScale == 0 ? fmt("Auto (%d%%)", (int)std::lround(renderer_.renderScale() * 100)) : "Auto";
            choice("Render resolution", {autoLabel, "100%", "75%", "50%"}, idx);
            settings_.gfx.renderScale = scales[idx];
        }
        choice("Shadows", {"Off", "Low", "High", "Ultra"}, settings_.gfx.shadows);
        {
            static const int lights[] = {8, 16, 32};
            int idx = settings_.gfx.maxLights <= 8 ? 0 : settings_.gfx.maxLights <= 16 ? 1 : 2;
            choice("Night lights", {"8", "16", "32"}, idx);
            settings_.gfx.maxLights = lights[idx];
        }
        toggle("Rain & snow", settings_.gfx.weather);
        toggle("Vision cone", settings_.gfx.visionCone);
        toggle("Screen effects", settings_.screenEffects);
        toggle("Minimap (N)", settings_.minimap);
        slider("Brightness", settings_.gfx.brightness, 0.7f, 1.4f, pct(settings_.gfx.brightness));
        {
            static const float sizes[] = {0.85f, 1.0f, 1.2f};
            int idx = settings_.uiScale < 0.95f ? 0 : settings_.uiScale > 1.1f ? 2 : 1;
            int before = idx;
            choice("Interface size", {"Small", "Normal", "Large"}, idx);
            if (idx != before) settings_.uiScale = sizes[idx];
        }
#ifndef __EMSCRIPTEN__
        {
            int idx = settings_.msaa;
            choice("Anti-aliasing (restart)", {"Off", "4x"}, idx);
            settings_.msaa = idx;
        }
#endif
        toggle("Show FPS", settings_.showFps);
    } else {
        struct Opt { const char* label; float mps; };
        Opt opts[] = {{"48 min", 0.5f}, {"24 min", 1.0f}, {"12 min", 2.0f}};
        int idx = 1;
        for (int i = 0; i < 3; ++i) if (std::fabs(settings_.minutesPerSecond - opts[i].mps) < 0.01f) idx = i;
        choice("Length of a day", {opts[0].label, opts[1].label, opts[2].label}, idx);
        settings_.minutesPerSecond = opts[idx].mps;
        game_.setBaseTimeScale(settings_.minutesPerSecond);
        y += 6 * s;
        ui::paragraph("A day in Alingsås lasts this long in real time. Sleeping and the speed keys (2/3) still "
                      "fast-forward. The game saves automatically every few minutes.",
                      x, y, w, 14 * s, ui::theme().dim);
        y += 50 * s;
        toggle("Drink clean water by yourself", settings_.autoDrink);
        game_.setAutoDrink(settings_.autoDrink);
        ui::paragraph("When you're thirsty, take a sip from an open bottle of clean water. Never lake water, "
                      "and never anything still sealed.", x, y, w, 14 * s, ui::theme().dim);
    }
    if (changed) applySettings();
    Rectangle back{p.x + 30 * s, p.y + p.height - 52 * s, w, 38 * s};
    if (ui::button(back, "Back")) { applySettings(); saveSettings(); overlay_ = settingsReturn_; }
}

void App::drawHelp() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 120});
    Rectangle p{W / 2 - 300 * s, H / 2 - 250 * s, 600 * s, 500 * s};
    ui::panel(p);
    ui::textCentered("Controls", W / 2, p.y + 16 * s, 28 * s, ui::theme().text, true);
    const char* lines[][2] = {
        {"W A S D", "Walk (Shift run, C crouch); driving: W gas, S brake/reverse, A/D steer"},
        {"Ctrl (hold)", "Aim: face the cursor, steadier shots, walk while you aim"},
        {"Right click", "Interact with what's under the cursor"},
        {"F", "Quick interact (doors, search, talk, get in/out of a car)"},
        {"Left click / Space", "Attack with what you hold"},
        {"Tab / I", "Inventory: double-click moves one, shift-click a stack, drag clothes onto you"},
        {"J", "Journal: the people you know of and what they need"},
        {"H", "Health: look yourself over, treat wounds"},
        {"B", "Build (carpentry): walls, doors, floors, roofs, crates"},
        {"M", "Map of Alingsås"},
        {"Z / R", "Sleep / rest for an hour (R with a gun: reload)"},
        {"1", "Swap the weapon in your hands with the one on your back"},
        {"L", "Flashlight on/off"},
        {"Q / E", "Rotate camera, mouse wheel zooms"},
        {"2 / 3", "Time speed x5 / x20 (again: normal)"},
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

// --bench: frame times at a few typical places (town, forest, inside a
// shopping centre), for checking performance changes.
int App::runBench() {
    // A/B switches: ALONE_BENCH_SHADOWS=0..3, ALONE_BENCH_SIZE=640x360
    if (const char* v = getenv("ALONE_BENCH_SHADOWS")) { settings_.gfx.shadows = atoi(v); applySettings(); }
    if (const char* v = getenv("ALONE_BENCH_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(v, "%dx%d", &w, &h) == 2) SetWindowSize(w, h);
    }
    game_.setBaseTimeScale(settings_.minutesPerSecond);
    game_.newGame(4242);
    state_ = State::Playing;
    struct Spot { const char* name; Vec2 at; };
    std::vector<Spot> spots = {{"start house", game_.player().pos}};
    for (const auto& ob : game_.map().osmBuildings()) {
        if (ob.name == "Gallerian Storken") spots.push_back({"Kungsgatan", ob.centre + Vec2{0, ob.h * 0.5f + 6.0f}});
        if (ob.name == "Vimpeln") spots.push_back({"inside Vimpeln", ob.centre});
    }
    spots.push_back({"Hjortmarka forest", Vec2{7443.2f, 2206.7f}});
    renderer_.finishEachPhase = getenv("ALONE_BENCH_FINISH") != nullptr;
    std::printf("%-20s %8s %8s %8s %8s | %6s %6s %6s %6s %6s %6s\n", "place", "frame", "update", "render", "fps",
                "rig", "prep", "shadow", "scene", "rest", "hud+swap");
    for (const auto& sp : spots) {
        game_.player().pos = sp.at;
        renderer_.snapTo(sp.at);
        // let the chunks around the spot get built first
        for (int i = 0; i < 90; ++i) {
            update(1.0f / 30.0f);
            BeginDrawing();
            HoverInfo hv;
            renderer_.draw(game_, time_, 1.0f / 30.0f, hv);
            drawHud();
            EndDrawing();
        }
        if (const char* dir = getenv("ALONE_BENCH_SHOTS")) {   // a picture of each place, to check what's drawn
            Image img = LoadImageFromScreen();
            std::string name = std::string(dir) + "/bench_" + sp.name + ".png";
            for (char& ch : name) if (ch == ' ') ch = '_';
            ExportImage(img, name.c_str());
            UnloadImage(img);
        }
        double tu = 0, tr = 0, t0 = GetTime();
        const int frames = 120;
        renderer_.profile() = Renderer::Profile{};
        for (int i = 0; i < frames; ++i) {
            double a = GetTime();
            update(1.0f / 30.0f);
            double b = GetTime();
            BeginDrawing();
            HoverInfo hv;
            renderer_.draw(game_, time_, 1.0f / 30.0f, hv);
            drawHud();
            EndDrawing();
            double c = GetTime();
            tu += b - a; tr += c - b;
        }
        double total = (GetTime() - t0) / frames;
        const auto& pr = renderer_.profile();
        double n = std::max(1, pr.frames);
        double inDraw = (pr.rig + pr.prep + pr.shadow + pr.scene + pr.rest) / n;
        std::printf("%-20s %7.1fms %7.1fms %7.1fms %8.1f | %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f\n", sp.name, total * 1000,
                    tu / frames * 1000, tr / frames * 1000, 1.0 / total, pr.rig / n, pr.prep / n, pr.shadow / n, pr.scene / n,
                    pr.rest / n, tr / frames * 1000 - inDraw);
        std::printf("%-20s chunks %4.1f (sun %4.1f), vertices %.2fM (sun %.2fM), buildings %.2fM (sun %.2fM) per frame\n", "",
                    pr.chunks / n, pr.sunChunks / n, pr.verts / n / 1e6, pr.sunVerts / n / 1e6, pr.buildingVerts / n / 1e6,
                    pr.sunBuildingVerts / n / 1e6);
    }
    return 0;
}

// --anim-showcase <dir>: renders the character animations frame by frame
// (30 fps) for the README's GIF/MP4.
int App::runShowcase(const std::string& dir) {
    std::filesystem::create_directories(dir);
    const int size = 480;
    SetWindowSize(size, size);
    for (int i = 0; i < 4; ++i) { BeginDrawing(); ClearBackground(BLACK); EndDrawing(); }
    Rng rng(3);
    Player look;
    auto wear = [&](const char* id, ClothingSlot slot) { look.worn[(size_t)slot] = game_.items().create(id, rng); };
    wear("jeans", ClothingSlot::Legs);
    wear("flannel_shirt", ClothingSlot::Torso);
    wear("rain_jacket", ClothingSlot::Jacket);
    wear("hiking_boots", ClothingSlot::Feet);
    wear("wool_hat", ClothingSlot::Head);
    // facing: -0.79 shows the right side, 0.79 faces the camera
    struct Clip { const char* name; float seconds, speed; bool sneak, aim, eat, work, sleep; const char* weapon; float spin; float facing;
                  float limp = 0, arm = 0, hunch = 0, shiver = 0, nausea = 0, winded = 0;
                  int ride = 0;        // 1 on a bike
                  bool hold = false;   // carry the weapon without swinging it
                  int who = -1; };     // one of the made-up characters below (-1: the default one)
    // Characters made on the new-game screen: body, hair, clothes in their own colours
    struct Who { const char* name; Gender g; int hair; uint32_t skin, hairCol; const char* top; const char* sweater; uint32_t sweaterTint;
                 const char* jacket; const char* legs; const char* feet; const char* hat; };
    const Who people[] = {
        {"Maja", Gender::Woman, 2, 0xe8c4a8, 0x9a3a20, "tshirt", "hoodie", 0x3e5a36, "", "jeans", "sneakers", ""},
        {"Ali", Gender::Man, 4, 0x8d5a3b, 0x1a1410, "flannel_shirt", "", 0, "work_jacket", "overalls", "hiking_boots", ""},
        {"Linnea", Gender::Woman, 1, 0xf3d6c0, 0xd8c080, "thermal_top", "cardigan", 0xb06a8a, "", "sweatpants", "rubber_boots", ""},
        {"Robin", Gender::Other, 3, 0xd4a07a, 0x3a2a20, "tshirt", "fleece", 0, "rain_jacket", "jeans", "sneakers", "cap"},
    };
    const Clip clips[] = {
        {"Idle", 2.0f, 0, false, false, false, false, false, nullptr, 0.0f, 0.5f},
        {"Walk", 3.0f, 1.4f, false, false, false, false, false, nullptr, 0.5f, 0.0f},
        {"Run", 2.6f, 4.6f, false, false, false, false, false, nullptr, 0.5f, -0.4f},
        {"Sneak", 2.8f, 0.8f, true, false, false, false, false, nullptr, 0.4f, 0.2f},
        {"Axe swing", 2.6f, 0, false, false, false, false, false, "axe", 0.0f, -0.45f},
        {"Knife", 2.0f, 0, false, false, false, false, false, "hunting_knife", 0.0f, -0.3f},
        {"Aim rifle", 2.2f, 0, false, true, false, false, false, "hunting_rifle", 0.35f, -0.6f},
        {"Eat", 2.4f, 0, false, false, true, false, false, nullptr, 0.0f, 0.6f},
        {"Search / work", 2.4f, 0, false, false, false, true, false, nullptr, 0.0f, 0.3f},
        {"Limping (hurt leg)", 3.0f, 1.0f, false, false, false, false, false, nullptr, 0.45f, 0.0f, 0.9f},
        {"Hurt arm", 2.2f, 1.2f, false, false, false, false, false, nullptr, 0.3f, 0.4f, 0, 0.9f},
        {"In pain", 2.4f, 0.9f, false, false, false, false, false, nullptr, 0.3f, 0.2f, 0, 0, 0.9f},
        {"Freezing", 2.4f, 0, false, false, false, false, false, nullptr, 0.0f, 0.6f, 0, 0, 0, 1.0f},
        {"Nauseous", 2.2f, 0, false, false, false, false, false, nullptr, 0.0f, 0.5f, 0, 0, 0, 0, 1.0f},
        {"Out of breath", 2.2f, 0, false, false, false, false, false, nullptr, 0.0f, -0.2f, 0, 0, 0, 0, 0, 1.0f},
        {"Sleep", 2.0f, 0, false, false, false, false, true, nullptr, 0.0f, 0.4f},
        {"Walk, axe in hand", 2.6f, 1.4f, false, false, false, false, false, "axe", 0.4f, 0.1f, 0, 0, 0, 0, 0, 0, 0, true},
        {"Riding a bike", 3.2f, 4.5f, false, false, false, false, false, nullptr, 0.45f, -0.5f, 0, 0, 0, 0, 0, 0, 1},
        {"Maja", 2.2f, 0, false, false, false, false, false, nullptr, 1.4f, 0.6f, 0, 0, 0, 0, 0, 0, 0, false, 0},
        {"Ali", 2.2f, 0, false, false, false, false, false, nullptr, 1.4f, 0.6f, 0, 0, 0, 0, 0, 0, 0, false, 1},
        {"Linnea", 2.2f, 0, false, false, false, false, false, nullptr, 1.4f, 0.6f, 0, 0, 0, 0, 0, 0, 0, false, 2},
        {"Robin", 2.2f, 1.4f, false, false, false, false, false, nullptr, 0.8f, 0.6f, 0, 0, 0, 0, 0, 0, 0, false, 3},
    };
    const Player defaultLook = look;
    auto dress = [&](int who) {
        if (who < 0) { look = defaultLook; return; }
        const Who& w = people[who];
        look = Player{};
        look.look.name = w.name;
        look.look.gender = w.g;
        look.look.hairStyle = w.hair;
        look.look.skin = w.skin;
        look.look.hair = w.hairCol;
        auto put = [&](const char* id, ClothingSlot slot, uint32_t tint) {
            if (!*id) return;
            ItemInstance it = game_.items().create(id, rng);
            it.tint = tint;
            look.worn[(size_t)slot] = it;
        };
        put(w.top, ClothingSlot::Torso, 0);
        put(w.sweater, ClothingSlot::Sweater, w.sweaterTint);
        put(w.jacket, ClothingSlot::Jacket, 0);
        put(w.legs, ClothingSlot::Legs, 0);
        put(w.feet, ClothingSlot::Feet, 0);
        put(w.hat, ClothingSlot::Head, 0);
    };
    const char* onlyClip = getenv("ALONE_CLIP");
    const float dt = 1.0f / 30.0f;
    int frame = 0;
    float facing = 0.6f, distance = 0, t = 0;
    for (const Clip& c : clips) {
        if (onlyClip && !std::strstr(c.name, onlyClip)) continue;
        facing = c.facing;
        dress(c.who);
        const ItemDef* weapon = c.weapon ? game_.items().find(c.weapon) : nullptr;
        float swing = 0, nextSwing = 0.2f;
        int frames = (int)(c.seconds / dt);
        for (int f = 0; f < frames; ++f) {
            t += dt;
            distance += c.speed * dt;
            facing += c.spin * dt;
            if (c.weapon && !c.hold && !game_.items().find(c.weapon)->firearm) {
                nextSwing -= dt;
                if (nextSwing <= 0) { swing = 1; nextSwing = 1.0f; }
                swing = std::max(0.0f, swing - dt * 3.0f);
            }
            AnimInput in;
            in.speed = c.speed;
            in.distance = distance;
            in.sneaking = c.sneak;
            in.aiming = c.aim;
            in.eating = c.eat;
            in.working = c.work;
            in.asleep = c.sleep;
            in.swing = swing;
            in.time = t;
            in.weapon = !weapon ? 0 : weapon->firearm ? 3 : weapon->hasTag("knife") ? 2 : 1;
            in.limpL = c.limp; in.armHurtL = c.arm; in.hunch = c.hunch; in.shiver = c.shiver;
            in.nausea = c.nausea; in.winded = c.winded;
            in.ride = c.ride;
            if (c.ride) in.speed = 0;   // pedalling goes by distance, as in the game
            BeginDrawing();
            ClearBackground(Color{24, 26, 30, 255});
            DrawCircleGradient(size / 2, size / 2, size * 0.7f, Color{58, 62, 66, 255}, Color{24, 26, 30, 255});
            renderer_.drawShowcase(in, facing, dt, look, weapon);
            float fs = 26;
            std::string title = c.who >= 0 ? std::string("Make your own: ") + c.name : c.name;
            if (c.who >= 0) fs = 22;
            Vector2 m = ui::measure(title, fs, true);
            ui::text(title, size / 2 - m.x / 2, size - 46, fs, Color{230, 226, 214, 255}, true);
            ui::text("ALONE", 14, 12, 16, Color{214, 168, 92, 255}, true);
            rlDrawRenderBatchActive();   // flush the text before reading the frame back
            Image img = LoadImageFromScreen();
            EndDrawing();
            ExportImage(img, (std::filesystem::path(dir) / fmt("frame_%04d.png", frame++)).string().c_str());
            UnloadImage(img);
        }
    }
    Log::info("Showcase: %d frames written to %s", frame, dir.c_str());
    return 0;
}

// Our own mouse cursor: a small outlined arrow, a crosshair when a firearm is ready.
void App::drawCursor() {
    // The system pointer with the game's pictures on it: it moves smoothly
    // however slowly the game draws
    if (tour_) { input::setPointer(input::Pointer::Hidden); return; }
    const ItemInstance* wpn = state_ == State::Playing ? game_.weapon() : nullptr;
    bool overUi = ui::mouseOverUi() || overlay_ != Overlay::None || menu_.open || state_ != State::Playing;
    bool aiming = state_ == State::Playing && game_.player().aiming;
    if (((wpn && wpn->def->firearm) || aiming) && !overUi) input::setPointer(input::Pointer::Crosshair);
    else input::setPointer(overUi ? input::Pointer::Arrow : input::Pointer::ArrowWorld);
}

std::string App::roughTimeOfDay(float h) {
    if (h < 4.5f) return "Middle of the night";
    if (h < 6.5f) return "Early morning";
    if (h < 9.0f) return "Morning";
    if (h < 11.0f) return "Late morning";
    if (h < 13.0f) return "Around midday";
    if (h < 15.0f) return "Early afternoon";
    if (h < 17.5f) return "Late afternoon";
    if (h < 19.5f) return "Evening";
    if (h < 22.0f) return "Late evening";
    return "Night";
}

std::string App::feelOfTemperature(float c) {
    if (c < -12) return "Bitterly cold";
    if (c < -2) return "Freezing";
    if (c < 5) return "Cold";
    if (c < 11) return "Chilly";
    if (c < 17) return "Cool";
    if (c < 23) return "Mild";
    if (c < 28) return "Warm";
    return "Hot";
}

// A wristwatch (leather strap, brass case) or a red twin-bell alarm clock.
void App::drawClockFace(Vector2 c, float r, bool alarm) {
    const Clock& clk = game_.world().clock();
    double mins = clk.totalMinutes();
    float hour = clk.hourOfDay();
    float minute = (float)std::fmod(mins, 60.0);
    float second = (float)std::fmod(mins * 60.0, 60.0);
    Color ink{34, 30, 26, 255};
    if (alarm) {
        Color red{176, 38, 32, 255}, dark{110, 20, 18, 255};
        // bells and hammer
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            Vector2 b{c.x + sgn * r * 0.62f, c.y - r * 0.86f};
            DrawCircleSector(b, r * 0.42f, 180, 360, 16, dark);
            DrawCircleSector(b, r * 0.36f, 180, 360, 16, red);
            DrawLineEx({c.x + sgn * r * 0.3f, c.y + r * 0.8f}, {c.x + sgn * r * 0.62f, c.y + r * 1.12f}, r * 0.1f, dark);
        }
        DrawLineEx({c.x, c.y - r * 1.05f}, {c.x, c.y - r * 1.25f}, r * 0.08f, dark);
        DrawCircleV(c, r * 1.02f, dark);
        DrawCircleV(c, r * 0.96f, red);
        DrawCircleV(c, r * 0.8f, Color{238, 230, 210, 255});
        r *= 0.8f;
    } else {
        // strap with stitching
        Color strap{92, 58, 36, 255}, stitch{150, 110, 70, 255};
        DrawRectangleRounded({c.x - r * 0.5f, c.y - r * 1.55f, r, r * 3.1f}, 0.3f, 6, strap);
        for (int sgn = -1; sgn <= 1; sgn += 2)
            for (float k = 0.9f; k < 1.5f; k += 0.18f)
                DrawLineEx({c.x - r * 0.36f, c.y + sgn * k * r}, {c.x - r * 0.24f, c.y + sgn * k * r}, 1.0f, stitch);
        DrawCircleV(c, r * 1.04f, Color{120, 92, 40, 255});
        DrawCircleV(c, r * 0.98f, Color{200, 166, 92, 255});
        DrawCircleV(c, r * 0.86f, Color{246, 240, 226, 255});
        // crown
        DrawRectangleRounded({c.x + r * 0.98f, c.y - r * 0.1f, r * 0.14f, r * 0.2f}, 0.4f, 4, Color{180, 146, 76, 255});
        r *= 0.86f;
    }
    // ticks and numerals
    for (int i = 0; i < 60; ++i) {
        float a = i / 60.0f * 2 * PI - PI / 2;
        bool hourTick = i % 5 == 0;
        float r0 = r * (hourTick ? 0.8f : 0.88f);
        DrawLineEx({c.x + std::cos(a) * r0, c.y + std::sin(a) * r0}, {c.x + std::cos(a) * r * 0.95f, c.y + std::sin(a) * r * 0.95f},
                   hourTick ? std::max(1.5f, r * 0.05f) : 1.0f, hourTick ? ink : Color{120, 112, 100, 255});
    }
    if (r > 26) {
        const char* nums[] = {"12", "3", "6", "9"};
        for (int i = 0; i < 4; ++i) {
            float a = i * PI / 2 - PI / 2;
            Vector2 m = ui::measure(nums[i], r * 0.24f, true);
            ui::text(nums[i], c.x + std::cos(a) * r * 0.62f - m.x / 2, c.y + std::sin(a) * r * 0.62f - m.y / 2, r * 0.24f, ink, true);
        }
    }
    // date window (wristwatch only)
    if (!alarm) {
        std::string d = clk.dateString().substr(0, 6);
        for (auto& ch : d) ch = (char)std::toupper((unsigned char)ch);
        Vector2 m = ui::measure(d, r * 0.17f, true);
        Rectangle wnd{c.x - m.x / 2 - 2, c.y + r * 0.32f, m.x + 4, m.y};
        DrawRectangleRec(wnd, WHITE);
        DrawRectangleLinesEx(wnd, 1, Color{150, 140, 120, 255});
        ui::text(d, wnd.x + 2, wnd.y, r * 0.17f, ink, true);
    }
    auto hand = [&](float frac, float len, float width, Color col) {
        float a = frac * 2 * PI - PI / 2;
        Vector2 tip{c.x + std::cos(a) * len, c.y + std::sin(a) * len};
        Vector2 tail{c.x - std::cos(a) * len * 0.18f, c.y - std::sin(a) * len * 0.18f};
        DrawLineEx(tail, tip, width, col);
    };
    hand(std::fmod(hour, 12.0f) / 12.0f, r * 0.5f, std::max(2.5f, r * 0.09f), ink);
    hand(minute / 60.0f, r * 0.76f, std::max(2.0f, r * 0.06f), ink);
    if (game_.timeSpeed() <= 1) hand(second / 60.0f, r * 0.82f, 1.2f, Color{196, 40, 32, 255});
    DrawCircleV(c, std::max(2.5f, r * 0.07f), ink);
}

// Without a clock: the sun or moon on its arc over the horizon.
void App::drawSkyDial(Vector2 c, float r) {
    const Clock& clk = game_.world().clock();
    float h = clk.hourOfDay();
    float day = clk.daylight();
    Color sky = ColorLerp(Color{18, 24, 44, 255}, Color{120, 160, 196, 255}, day);
    DrawCircleSector(c, r, 180, 360, 24, sky);
    DrawCircleSectorLines(c, r, 180, 360, 24, Color{90, 94, 98, 255});
    DrawLineEx({c.x - r - 3, c.y}, {c.x + r + 3, c.y}, 2, Color{70, 90, 60, 255});
    // the sun rises around 6-7 in autumn: map 6..18 onto the arc, the moon the other half
    bool sun = h >= 6 && h < 18;
    float f = sun ? (h - 6) / 12.0f : std::fmod(h + 6, 24.0f) / 12.0f;
    float a = PI + f * PI;
    Vector2 body{c.x + std::cos(a) * r * 0.72f, c.y + std::sin(a) * r * 0.72f};
    if (sun) DrawCircleV(body, r * 0.2f, Color{250, 214, 120, 255});
    else {
        DrawCircleV(body, r * 0.17f, Color{220, 224, 230, 255});
        DrawCircleV({body.x + r * 0.08f, body.y - r * 0.04f}, r * 0.15f, sky);
    }
}

void App::drawThermometer(Rectangle r, float celsius) {
    Color glass{220, 226, 232, 255}, red{200, 40, 36, 255};
    float bulb = r.width * 0.75f;
    DrawRectangleRounded({r.x + r.width * 0.25f, r.y, r.width * 0.5f, r.height - bulb}, 1.0f, 6, glass);
    DrawCircleV({r.x + r.width / 2, r.y + r.height - bulb * 0.8f}, bulb * 0.75f, glass);
    DrawCircleV({r.x + r.width / 2, r.y + r.height - bulb * 0.8f}, bulb * 0.55f, red);
    float f = std::clamp((celsius + 30) / 60.0f, 0.0f, 1.0f);
    float top = r.y + (r.height - bulb) * (1 - f);
    DrawRectangle((int)(r.x + r.width * 0.4f), (int)top, (int)std::max(2.0f, r.width * 0.2f), (int)(r.y + r.height - bulb - top + 2), red);
}

void App::drawHud() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    World& w = game_.world();
    const Clock& c = w.clock();

    // Screen effects: night & cold vignette, sickness tint
    BodySignals sig = game_.player().body.signals();
    float vig = std::max({sig.cold * 0.6f, sig.weakness * 0.5f, sig.dizziness * 0.6f, sig.pain * 0.4f});
    if (!settings_.screenEffects) vig = 0;
    if (vig > 0.05f) {
        Color edge = sig.cold > sig.pain ? Color{20, 40, 80, (unsigned char)(vig * 160)} : Color{60, 0, 0, (unsigned char)(vig * 150)};
        int band = (int)(H * 0.25f);
        DrawRectangleGradientV(0, 0, (int)W, band, edge, Color{0, 0, 0, 0});
        DrawRectangleGradientV(0, (int)H - band, (int)W, band, Color{0, 0, 0, 0}, edge);
        DrawRectangleGradientH(0, 0, band, (int)H, edge, Color{0, 0, 0, 0});
        DrawRectangleGradientH((int)W - band, 0, band, (int)H, Color{0, 0, 0, 0}, edge);
    }
    if (sig.hallucination > 0.1f && settings_.screenEffects) {
        float a = sig.hallucination * (0.5f + 0.5f * std::sin(time_ * 1.3f)) * 50;
        DrawRectangle(0, 0, (int)W, (int)H, Color{(unsigned char)(120 + 100 * std::sin(time_)), 40, (unsigned char)(140 + 100 * std::cos(time_ * 0.7f)), (unsigned char)a});
    }
    if (game_.player().asleep) DrawRectangle(0, 0, (int)W, (int)H, Color{0, 0, 0, 170});

    // Top-left: what you can tell about the time and the weather.
    // Without a watch you only see roughly what part of the day it is; a
    // wristwatch or an alarm clock puts a real clock face on screen. The exact
    // temperature needs a thermometer.
    const Inventory& inv = game_.player().inventory;
    int clockIdx = inv.findTag("clock");
    int thermoIdx = inv.findTag("thermometer");
    std::string clockId = clockIdx >= 0 ? inv.items()[clockIdx].def->id : "";
    float t = w.outdoorTemp();
    float face = 44 * s;
    Rectangle p{12 * s, 12 * s, 312 * s, (clockIdx >= 0 ? 112 : 92) * s};
    ui::panel(p, Color{10, 12, 14, 170});
    float tx;
    if (clockIdx >= 0) {
        Vector2 cc{p.x + 14 * s + face, p.y + p.height / 2};
        drawClockFace(cc, face, clockId == "alarm_clock");
        tx = cc.x + face + 16 * s;
        ui::text(c.timeString(), tx, p.y + 10 * s, 24 * s, ui::theme().text, true);
        ui::text(c.dateString(), tx, p.y + 38 * s, 14 * s, ui::theme().dim);
    } else {
        Vector2 cc{p.x + 14 * s + 26 * s, p.y + 34 * s};
        drawSkyDial(cc, 26 * s);
        tx = cc.x + 26 * s + 14 * s;
        ui::text(roughTimeOfDay(c.hourOfDay()), tx, p.y + 10 * s, 20 * s, ui::theme().text, true);
        ui::text(fmt("Day %d", c.day()), tx, p.y + 34 * s, 14 * s, ui::theme().dim);
    }
    std::string weather = weatherName(c.weather().kind);
    float ty = p.y + p.height - 46 * s;
    if (thermoIdx >= 0) {
        drawThermometer({tx, ty - 2 * s, 10 * s, 24 * s}, t);
        ui::text(fmt("%.0f°C, %s", t, weather.c_str()), tx + 16 * s, ty + 2 * s, 15 * s, ui::theme().text);
    } else {
        std::string wl = weather;
        for (auto& ch : wl) ch = (char)std::tolower((unsigned char)ch);
        ui::text(feelOfTemperature(t) + ", " + wl, tx, ty + 2 * s, 15 * s, ui::theme().text);
    }
    ui::text(game_.location(), tx, ty + 22 * s, 14 * s, ui::theme().accent);
    if (game_.timeSpeed() > 1 || game_.currentAction().timeScale > 1.5f) {
        float sp = game_.timeSpeed() * (game_.currentAction().active ? game_.currentAction().timeScale : 1);
        ui::text(fmt(">> x%.0f", sp), p.x + p.width - 70 * s, p.y + 10 * s, 16 * s, ui::theme().accent, true);
    }

    // The goal right now, under the clock (J opens the journal)
    {
        std::string goal = game_.objective();
        float gy = p.y + p.height + 6 * s;
        float gw = std::max(p.width, ui::measure(goal, 14 * s).x + 40 * s);
        Rectangle g{p.x, gy, gw, 26 * s};
        ui::panel(g, Color{10, 12, 14, 150});
        ui::text("\xE2\x98\x85", g.x + 8 * s, g.y + 4 * s, 14 * s, ui::theme().accent);
        ui::text(goal, g.x + 26 * s, g.y + 5 * s, 14 * s, ui::theme().text);
        float cy = g.y + g.height + 4 * s;
        for (const auto& sv : game_.survivors()) {
            if (!sv.joined || !sv.alive()) continue;
            float hp = sv.hp / sv.def->hp;
            Rectangle r{p.x, cy, 170 * s, 22 * s};
            ui::panel(r, Color{10, 12, 14, 140}, false);
            ui::text(sv.def->name, r.x + 8 * s, r.y + 3 * s, 13 * s, Color{120, 180, 230, 255});
            DrawRectangle((int)(r.x + 120 * s), (int)(r.y + 9 * s), (int)(42 * s), (int)(4 * s), Color{40, 40, 40, 255});
            DrawRectangle((int)(r.x + 120 * s), (int)(r.y + 9 * s), (int)(42 * s * hp), (int)(4 * s), hp > 0.5f ? ui::theme().good : ui::theme().bad);
            cy += 24 * s;
        }
    }

    if (buildKind_ >= 0) {
        const Game::BuildRecipe* r = Game::recipe((BuildKind)buildKind_);
        float s2 = ui::scale();
        std::string why;
        bool ok = game_.canAffordBuild((BuildKind)buildKind_, &why);
        std::string line = std::string("Building: ") + (r ? r->name : "?") + " - click to place, right-click to stop";
        Vector2 m = ui::measure(line, 16 * s2);
        Rectangle hint{W / 2 - m.x / 2 - 14 * s2, H - 92 * s2, m.x + 28 * s2, ok ? 34 * s2 : 54 * s2};
        ui::panel(hint, Color{14, 15, 17, 220});
        ui::text(line, hint.x + 14 * s2, hint.y + 8 * s2, 16 * s2, ui::theme().text);
        if (!ok) ui::text(why, hint.x + 14 * s2, hint.y + 30 * s2, 14 * s2, ui::theme().bad);
    }
    drawSymptoms();
    drawMessages();
    drawActionBar();

    // Bottom right: what you hold, hints
    const ItemInstance* wpn = game_.weapon();
    Rectangle hb{W - 250 * s, H - 70 * s, 238 * s, 58 * s};
    ui::panel(hb, Color{10, 12, 14, 170});
    drawItemIcon(wpn ? wpn->def : nullptr, {hb.x + 8 * s, hb.y + 9 * s, 40 * s, 40 * s});
    ui::text(wpn ? wpn->def->name : "Bare hands", hb.x + 56 * s, hb.y + 8 * s, 15 * s, ui::theme().text);
    if (wpn && wpn->def->firearm) {
        // rounds in the gun / rounds in your bags
        int carried = game_.ammoCarried(wpn->def);
        bool mag = wpn->def->magazine > 0;
        int loaded = mag ? wpn->usesLeft : (carried > 0 ? 1 : 0);
        bool reloading = game_.currentAction().active && game_.currentAction().option.kind == ActionKind::Reload;
        std::string ammo = mag ? fmt("%d/%d", loaded, carried) : fmt("%d", carried);
        Color ac = loaded > 0 ? ui::theme().text : ui::theme().bad;
        ui::text(ammo, hb.x + 56 * s, hb.y + 27 * s, 22 * s, ac, true);
        float ax = hb.x + 62 * s + ui::measure(ammo, 22 * s, true).x;
        std::string note = reloading ? "Reloading..." : !mag ? (carried ? "left" : "nothing to shoot")
                         : loaded == 0 ? (carried ? "Empty - R to reload" : "Empty") : loaded < wpn->def->magazine && carried ? "R reloads" : "";
        ui::text(note, ax, hb.y + 34 * s, 12 * s, loaded == 0 ? ui::theme().bad : ui::theme().dim);
    } else {
        ui::text("Tab  J  M  N  H  B  F1", hb.x + 56 * s, hb.y + 32 * s, 13 * s, ui::theme().dim);
    }
    // Your back: what 1 swaps into your hands
    if (const ItemInstance* back = game_.stowedWeapon()) {
        Rectangle bb{hb.x - 196 * s, hb.y, 190 * s, hb.height};
        ui::panel(bb, Color{10, 12, 14, 150});
        DrawRectangleRounded({bb.x + 6 * s, bb.y + 6 * s, 20 * s, 20 * s}, 0.25f, 4, Color{60, 62, 66, 255});
        ui::textCentered("1", bb.x + 16 * s, bb.y + 8 * s, 14 * s, ui::theme().text, true);
        drawItemIcon(back->def, {bb.x + 30 * s, bb.y + 11 * s, 36 * s, 36 * s});
        ui::text("On your back", bb.x + 72 * s, bb.y + 8 * s, 12 * s, ui::theme().dim);
        ui::paragraph(back->def->name, bb.x + 72 * s, bb.y + 24 * s, bb.width - 78 * s, 14 * s, ui::theme().text);
    }
    if (settings_.minimap && overlay_ == Overlay::None) drawMinimap();
    // Breath
    float st = game_.player().stamina;
    if (st < 0.99f) {
        Rectangle sb{W / 2 - 80 * s, H - 26 * s, 160 * s, 6 * s};
        DrawRectangleRec(sb, Color{0, 0, 0, 140});
        DrawRectangle((int)sb.x, (int)sb.y, (int)(sb.width * st), (int)sb.height, Color{200, 200, 190, 200});
    }
}

// Bottom right: the land around you, turned the way the camera looks
void App::drawMinimap() {
    if (!mapReady_) buildMapTexture();
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    float size = 210 * s;
    Rectangle r{W - size - 12 * s, H - 76 * s - size, size, size};
    ui::panel(r, Color{10, 12, 14, 200});
    Rectangle in{r.x + 4 * s, r.y + 4 * s, r.width - 8 * s, r.height - 8 * s};
    const MapData& m = game_.map();
    const float radius = 110.0f;                  // metres from the centre to the edge
    float k = (in.width * 0.5f) / radius;         // screen pixels per metre
    Vec2 pp = game_.player().pos;
    float yaw = renderer_.yaw();
    Vec2 right{std::cos(yaw), -std::sin(yaw)}, fwd{-std::sin(yaw), -std::cos(yaw)};
    Vector2 c{in.x + in.width / 2, in.y + in.height / 2};
    auto toScreen = [&](Vec2 wpos) {
        Vec2 d = wpos - pp;
        return Vector2{c.x + (d.x * right.x + d.y * right.y) * k, c.y - (d.x * fwd.x + d.y * fwd.y) * k};
    };
    float reach = radius * 1.45f;                 // corners of the square, turned
    auto near = [&](Vec2 a) { return std::fabs(a.x - pp.x) < reach && std::fabs(a.y - pp.y) < reach; };

    BeginScissorMode((int)in.x, (int)in.y, (int)in.width, (int)in.height);
    // the ground: the map picture, turned with the camera
    float tmpp = m.terrainMetresPerPixel();
    float half = reach / tmpp;
    Rectangle src{pp.x / tmpp - half, pp.y / tmpp - half, half * 2, half * 2};
    float dw = half * 2 * tmpp * k;
    DrawTexturePro(mapTex_, src, {c.x, c.y, dw, dw}, {dw / 2, dw / 2}, yaw * RAD2DEG, WHITE);
    // buildings
    for (const auto& b : game_.world().gen().allBuildings()) {
        Vec2 bc = b.rect.centre();
        if (!near(bc)) continue;
        Vector2 q[4] = {toScreen({(float)b.rect.x, (float)b.rect.y}), toScreen({(float)b.rect.x, (float)b.rect.bottom()}),
                        toScreen({(float)b.rect.right(), (float)b.rect.bottom()}), toScreen({(float)b.rect.right(), (float)b.rect.y})};
        Color bcol = b.id == game_.world().tile((int)std::floor(pp.x), (int)std::floor(pp.y)).building ? Color{190, 170, 140, 255}
                                                                                                        : Color{150, 142, 132, 255};
        DrawTriangleFan(q, 4, bcol);
    }
    // roads and paths
    for (const auto& rd : m.roads()) {
        if (!near(rd.a) && !near(rd.b)) continue;
        bool path = isFootpath(rd.cls);
        float wdt = rd.cls == RoadClass::Motorway ? 9.0f : rd.cls == RoadClass::Major ? 7.0f : path ? 1.5f : rd.cls == RoadClass::Service ? 3.0f : 5.0f;
        Color col = rd.cls == RoadClass::Motorway ? Color{226, 190, 110, 255} : path ? Color{150, 130, 100, 220} : Color{246, 246, 240, 255};
        DrawLineEx(toScreen(rd.a), toScreen(rd.b), std::max(1.0f, wdt * k), col);
    }
    // bikes and cars you've used, people you know
    for (const auto& v : game_.vehicles()) {
        if (v.id == game_.player().vehicle || !near(v.pos)) continue;
        Vector2 vp = toScreen(v.pos);
        DrawRectangleRounded({vp.x - 4 * s, vp.y - 3 * s, 8 * s, 6 * s}, 0.4f, 4, Color{60, 60, 70, 255});
    }
    for (const auto& sv : game_.survivors()) {
        if (!sv.alive() || !near(sv.pos)) continue;
        bool see = sv.joined || (sv.met && distance(sv.pos, pp) < 40.0f);
        if (!see) continue;
        bool bad = sv.hostileTo() || sv.def->disposition == Disposition::Raider;
        Color col = bad ? Color{190, 50, 40, 255} : sv.joined ? Color{60, 140, 200, 255} : Color{50, 140, 70, 255};
        Vector2 sp = toScreen(sv.pos);
        DrawCircleV(sp, 4 * s, Color{250, 248, 240, 255});
        DrawCircleV(sp, 3 * s, col);
    }
    // you: an arrow the way you face
    {
        float f = game_.player().facing;
        Vec2 dir{std::cos(f), std::sin(f)};
        Vector2 dv{dir.x * right.x + dir.y * right.y, -(dir.x * fwd.x + dir.y * fwd.y)};
        Vector2 a{c.x + dv.x * 8 * s, c.y + dv.y * 8 * s};
        Vector2 sideV{-dv.y * 5 * s, dv.x * 5 * s};
        Vector2 b1{c.x - dv.x * 5 * s + sideV.x, c.y - dv.y * 5 * s + sideV.y};
        Vector2 b2{c.x - dv.x * 5 * s - sideV.x, c.y - dv.y * 5 * s - sideV.y};
        DrawTriangle(a, b2, b1, Color{200, 40, 40, 255});
        DrawTriangle(a, b1, b2, Color{200, 40, 40, 255});
    }
    EndScissorMode();
    // north
    Vector2 north = toScreen(pp + Vec2{0, -radius * 0.82f});
    north.x = std::clamp(north.x, in.x + 8 * s, in.x + in.width - 8 * s);
    north.y = std::clamp(north.y, in.y + 8 * s, in.y + in.height - 8 * s);
    DrawCircleV(north, 8 * s, Color{20, 22, 24, 220});
    ui::textCentered("N", north.x, north.y - 7 * s, 13 * s, ui::theme().text, true);
    ui::claim(r);
    if (ui::hovered(r)) {
        ui::tooltip("Minimap", "Click to open the map (M). N hides it.");
        if (input::pressed(MOUSE_BUTTON_LEFT)) overlay_ = Overlay::Map;
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
        if (y > GetScreenHeight() - (settings_.minimap ? 330 : 100) * s) { y = 14 * s; x -= size + 6 * s; }
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
    if (!a.active) {
        // What F would do now, as a small prompt
        ActionOption o;
        if (state_ != State::Playing || overlay_ != Overlay::None || menu_.open || buildKind_ >= 0 || !interactTarget(o)) return;
        float s = ui::scale();
        float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
        float tw = ui::measure(o.label, 16 * s).x;
        float w = tw + 56 * s;
        Rectangle r{W / 2 - w / 2, H - 70 * s, w, 32 * s};
        ui::panel(r, Color{10, 12, 14, 170});
        Rectangle key{r.x + 8 * s, r.y + 5 * s, 24 * s, 22 * s};
        DrawRectangleRounded(key, 0.25f, 4, Color{60, 62, 66, 255});
        ui::textCentered("F", key.x + key.width / 2, key.y + 2 * s, 16 * s, ui::theme().accent, true);
        ui::text(o.label, key.x + key.width + 10 * s, r.y + 7 * s, 16 * s, ui::theme().text);
        return;
    }
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
        if (hov && input::pressed(MOUSE_BUTTON_LEFT)) {
            clickedSomething = true;
            if (o.enabled) {
                ActionOption copy = o;
                menu_.open = false;
                game_.perform(copy);
                if (copy.kind == ActionKind::OpenContainer) overlay_ = Overlay::Inventory;
                if (copy.kind == ActionKind::Search) lootPending_ = true;
                if (copy.kind == ActionKind::Talk) openTalk(copy.targetId);
                return;
            }
        }
        ry += rowH;
    }
    if (!clickedSomething && (input::pressed(MOUSE_BUTTON_LEFT) || input::pressed(MOUSE_BUTTON_RIGHT)) && !ui::hovered(r))
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

// drawItemTooltip, drawInventory and drawBuildMenu live in AppInventory.cpp

void App::updateBuildPlacement() {
    BuildKind k = (BuildKind)buildKind_;
    int tx = (int)std::floor(hover_.x), ty = (int)std::floor(hover_.y);
    buildX_ = tx; buildY_ = ty; buildDir_ = 0;
    if (isEdgeBuild(k)) {
        // the edge of the tile nearest the mouse
        float fx = hover_.x - tx, fy = hover_.y - ty;
        float dn = fy, ds = 1 - fy, dw = fx, de = 1 - fx;
        float m = std::min({dn, ds, dw, de});
        if (m == dn) { buildDir_ = 0; }
        else if (m == ds) { buildY_ = ty + 1; buildDir_ = 0; }
        else if (m == dw) { buildDir_ = 1; }
        else { buildX_ = tx + 1; buildDir_ = 1; }
    }
    if (input::pressed(MOUSE_BUTTON_LEFT)) {
        ActionOption o = game_.buildOption(k, buildX_, buildY_, buildDir_);
        game_.perform(o);
    }
    if (input::pressed(MOUSE_BUTTON_RIGHT)) buildKind_ = -1;
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

namespace {

// Front view of the body for the health screen, in units of 1/100 of its
// height. The player's left side is on the right, like looking at someone.
struct PartShape { BodyPart part; float x, y, w, h; bool round; const char* label; };
const PartShape kBodyShapes[] = {
    {BodyPart::Head, -6.5f, 1.5f, 13.0f, 14.0f, true, "Head"},
    {BodyPart::Torso, -10.0f, 16.5f, 20.0f, 30.5f, false, "Torso"},
    {BodyPart::RightArm, -15.8f, 17.5f, 5.4f, 27.5f, false, "Right arm"},
    {BodyPart::LeftArm, 10.4f, 17.5f, 5.4f, 27.5f, false, "Left arm"},
    {BodyPart::RightHand, -16.2f, 45.5f, 6.2f, 8.0f, true, "Right hand"},
    {BodyPart::LeftHand, 10.0f, 45.5f, 6.2f, 8.0f, true, "Left hand"},
    {BodyPart::RightLeg, -9.6f, 47.5f, 8.8f, 40.5f, false, "Right leg"},
    {BodyPart::LeftLeg, 0.8f, 47.5f, 8.8f, 40.5f, false, "Left leg"},
    {BodyPart::RightFoot, -10.8f, 88.5f, 10.0f, 6.0f, false, "Right foot"},
    {BodyPart::LeftFoot, 0.8f, 88.5f, 10.0f, 6.0f, false, "Left foot"},
};

std::string capitalise(std::string s) {
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

} // namespace

void App::drawHealth() {
    float s = ui::scale();
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    float pw = std::min(860 * s, W - 20), ph = std::min(580 * s, H - 20);
    Rectangle p{W / 2 - pw / 2, H / 2 - ph / 2, pw, ph};
    ui::panel(p, Color{16, 17, 19, 238});
    ui::claim(p);
    // Tabs: your body, and who you are
    {
        const char* tabs[] = {"Body", "Info"};
        for (int i = 0; i < 2; ++i) {
            Rectangle tb{p.x + p.width - 240 * s + i * 112 * s, p.y + 12 * s, 106 * s, 32 * s};
            if (ui::button(tb, tabs[i], true, 16)) healthTab_ = i;
            if (i == healthTab_) DrawRectangle((int)tb.x, (int)(tb.y + tb.height - 3 * s), (int)tb.width, (int)(3 * s), ui::theme().accent);
        }
    }
    if (healthTab_ == 1) {
        drawHealthInfo(p);
        if (ui::button({p.x + p.width - 160 * s, p.y + p.height - 50 * s, 140 * s, 36 * s}, "Close")) overlay_ = Overlay::None;
        return;
    }
    ui::text("You look yourself over", p.x + 20 * s, p.y + 14 * s, 22 * s, ui::theme().text, true);
    const Body& body = game_.player().body;
    const BodyState& b = body.state();

    // --- the body diagram ----------------------------------------------------------
    float figH = p.height - 110 * s;
    float u = figH / 100.0f;
    float cx = p.x + 40 * s + 17 * u, top = p.y + 58 * s;
    Rectangle fig{cx - 22 * u, top - 4 * s, 44 * u, figH + 8 * s};
    DrawRectangleRounded(fig, 0.06f, 8, Color{24, 26, 29, 255});
    int hovered = -1;
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& sh : kBodyShapes) {
            Rectangle r{cx + sh.x * u, top + sh.y * u, sh.w * u, sh.h * u};
            bool hov = ui::hovered(r) && !menu_.open;
            if (pass == 0 && hov) hovered = (int)sh.part;
            if (pass == 1 && (int)sh.part != hovered) continue;   // hovered part drawn last, on top
            float sev = body.worstSeverityAt(sh.part);
            float bleed = body.bleedingAt(sh.part);
            float infl = 0;
            for (const auto& w : b.wounds) if (w.part == sh.part) infl = std::max(infl, w.infection);
            Color c{78, 80, 84, 255};
            if (sev > 0) c = mixColor(c, Color{150, 70, 60, 255}, std::min(1.0f, 0.35f + sev));
            if (infl > 0.12f) c = mixColor(c, Color{196, 110, 40, 255}, std::min(1.0f, infl * 1.4f));
            if (bleed > 0.3f) c = mixColor(c, Color{210, 30, 30, 255}, 0.45f + 0.35f * std::sin(time_ * 6.0f));
            float round = sh.round ? 0.9f : 0.45f;
            DrawRectangleRounded(r, round, 8, c);
            // the dressing: a band across the part, white turning brown and red
            const Dressing& d = body.dressing(sh.part);
            if (d.on()) {
                Color band = mixColor(Color{236, 234, 226, 255}, Color{118, 92, 62, 255}, std::min(1.0f, d.dirt * 1.1f));
                band = mixColor(band, Color{150, 24, 22, 255}, std::min(0.85f, d.blood));
                float bh = d.kind == DressingKind::Bandaid ? 2.2f * u : std::min(r.height * 0.55f, 9.0f * u);
                float bw = d.kind == DressingKind::Bandaid ? r.width * 0.55f : r.width + 1.0f * u;
                Rectangle br{r.x + r.width / 2 - bw / 2, r.y + r.height / 2 - bh / 2, bw, bh};
                DrawRectangleRounded(br, 0.3f, 4, band);
                if (d.kind != DressingKind::Bandaid)
                    for (float yy = br.y + bh * 0.33f; yy < br.y + bh; yy += bh * 0.33f)
                        DrawLineEx({br.x, yy}, {br.x + br.width, yy}, 1.0f, shade(band, 0.8f));
            }
            if ((int)sh.part == hovered) DrawRectangleRoundedLinesEx(r, round, 8, 2.0f * s, ui::theme().accent);
        }
    ui::text("R", fig.x + 6 * s, fig.y + 6 * s, 13 * s, ui::theme().dim, true);
    ui::text("L", fig.x + fig.width - 14 * s, fig.y + 6 * s, 13 * s, ui::theme().dim, true);

    // Right-click a part to treat it; hover to see it close up
    if (hovered >= 0) {
        BodyPart part = (BodyPart)hovered;
        std::string name = capitalise(bodyPartName(part));
        auto lines = game_.describeBodyPart(part);
        std::string body = lines.empty() ? "Nothing wrong here." : "";
        for (const auto& l : lines) body += (body.empty() ? "" : "\n") + l;
        ui::tooltip(name, body + "\n\nRight-click to treat.");
        if (input::pressed(MOUSE_BUTTON_RIGHT)) {
            menu_.options = game_.actionsForBodyPart(part);
            menu_.title = name;
            menu_.pos = GetMousePosition();
            menu_.open = !menu_.options.empty();
        }
    }

    // --- what you notice -------------------------------------------------------------
    float x = fig.x + fig.width + 28 * s, y = p.y + 60 * s, w = p.x + p.width - x - 24 * s;
    float fatPct = b.fatMassKg / body.bodyMassKg();
    float lean = b.leanMassKg / b.leanBaselineKg;
    std::string build = lean < 0.7f ? "You are skin and bones." : lean < 0.8f ? "You look gaunt, your ribs show." :
                        fatPct < 0.07f ? "You look thin." : fatPct < 0.12f ? "You look lean." : fatPct < 0.25f ? "You look healthy." :
                        "You're carrying some extra weight.";
    y = ui::paragraph(build, x, y, w, 16 * s, ui::theme().text);
    if (b.wetness > 0.15f) y = ui::paragraph(b.wetness > 0.6f ? "Your clothes are soaked through." : "Your clothes are damp.", x, y, w, 16 * s, ui::theme().text);
    if (b.feverC > 0.8f) y = ui::paragraph("Your forehead feels hot.", x, y, w, 16 * s, ui::theme().text);
    if (b.diarrhea > 0.3f) y = ui::paragraph("You've had to run to the toilet a lot.", x, y, w, 16 * s, ui::theme().text);
    if (body.totalBleeding() > 2.0f) y = ui::paragraph("There's blood on your clothes, and it keeps coming.", x, y, w, 16 * s, ui::theme().bad);
    y += 12 * s;
    ui::text("Wounds", x, y, 18 * s, ui::theme().accent, true);
    y += 28 * s;
    bool any = false;
    for (const auto& sh : kBodyShapes) {
        auto lines = game_.describeBodyPart(sh.part);
        if (lines.empty()) continue;
        any = true;
        bool hot = (int)sh.part == hovered;
        ui::text(capitalise(bodyPartName(sh.part)), x, y, 15 * s, hot ? ui::theme().accent : ui::theme().text, true);
        y += 20 * s;
        for (const auto& l : lines) {
            bool bad = l.find("bleed") != std::string::npos || l.find("seeping") != std::string::npos || l.find("swollen") != std::string::npos ||
                       l.find("red around") != std::string::npos || l.find("filthy") != std::string::npos || l.find("dirty") != std::string::npos;
            y = ui::paragraph(l, x + 12 * s, y, w - 12 * s, 14 * s, bad ? ui::theme().bad : ui::theme().dim);
        }
        y += 6 * s;
        if (y > p.y + p.height - 150 * s) { ui::text("...", x, y, 15 * s, ui::theme().dim); break; }
    }
    if (!any) { ui::text("No wounds.", x, y, 15 * s, ui::theme().dim); y += 22 * s; }
    ui::paragraph("Hover over a body part to look closer. Right-click it to disinfect, put on a bandaid, bandage or rag, "
                  "or take a dirty one off. Dirty dressings can be washed in water.",
                  x, p.y + p.height - 78 * s, w, 13 * s, ui::theme().dim);
    if (ui::button({p.x + p.width - 160 * s, p.y + p.height - 50 * s, 140 * s, 36 * s}, "Close")) overlay_ = Overlay::None;
}

void App::drawHealthInfo(Rectangle p) {
    float s = ui::scale();
    const Player& pl = game_.player();
    const Body& body = pl.body;
    const BodyState& b = body.state();
    const auto& clock = game_.world().clock();
    ui::text(pl.look.name, p.x + 20 * s, p.y + 14 * s, 24 * s, ui::theme().text, true);
    float col = (p.width - 60 * s) / 2;
    float lx = p.x + 20 * s, rx = p.x + 40 * s + col;
    float lw = 150 * s;
    auto heading = [&](const char* t, float x, float& y) {
        ui::text(t, x, y, 17 * s, ui::theme().accent, true);
        y += 26 * s;
    };
    auto row = [&](const std::string& k, const std::string& v, float x, float& y, Color c = ui::theme().text) {
        ui::text(k, x, y, 15 * s, ui::theme().dim);
        y = std::max(y + 21 * s, ui::paragraph(v, x + lw, y, col - lw, 15 * s, c));
    };

    // --- left: who you are, your body ------------------------------------------------
    float y = p.y + 60 * s;
    heading("You", lx, y);
    row("Name", pl.look.name, lx, y);
    row("Age", std::to_string(pl.look.age), lx, y);
    row("Gender", genderName(pl.look.gender), lx, y);
    row("Hair", hairStyleName(pl.look.hairStyle), lx, y);
    int day = std::max(1, (int)((clock.totalMinutes() - 9 * 60) / 1440.0) + 1);
    row("Surviving", fmt("Day %d", day), lx, y);
    row("Today", clock.dateString() + ", " + clock.timeString(), lx, y);
    y += 10 * s;
    heading("Body", lx, y);
    row("Weight", fmt("%.1f kg", body.bodyMassKg()), lx, y);
    float fatPct = b.fatMassKg / std::max(1.0f, body.bodyMassKg());
    row("Body fat", fmt("%.0f%%", fatPct * 100), lx, y);
    row("Temperature", b.coreTempC < 35.0f ? "Dangerously cold" : b.coreTempC < 36.3f ? "Cold" : b.coreTempC > 38.0f ? "Feverish" : "Fine", lx, y,
        b.coreTempC < 35.0f || b.coreTempC > 38.5f ? ui::theme().bad : ui::theme().text);
    row("Awake for", fmt("%.0f hours", b.hoursAwake), lx, y);
    row("Clothes", b.wetness > 0.6f ? "Soaked" : b.wetness > 0.15f ? "Damp" : "Dry", lx, y);
    {
        SleepComfort sc = game_.sleepComfort();
        std::string why;
        for (const auto& g : sc.good) why += "+ " + g + "\n";
        for (const auto& g : sc.bad) why += "- " + g + "\n";
        if (!why.empty()) why.pop_back();
        row(pl.asleep ? "Sleeping" : "Sleeping here", sc.word(), lx, y,
            sc.score >= 0.15f ? ui::theme().good : sc.score <= -0.15f ? ui::theme().bad : ui::theme().text);
        y = ui::paragraph(why, lx + lw, y, col - lw, 13 * s, ui::theme().dim) + 4 * s;
    }

    // --- right: what you carry, know, and who you know --------------------------------
    y = p.y + 60 * s;
    heading("Gear", rx, y);
    float clo = 0, wp = 0;
    for (const auto& c : pl.worn)
        if (c.def) { clo += c.def->clo; if (c.def->slot == ClothingSlot::Jacket) wp = std::max(wp, c.def->waterproof); }
    std::string warm = clo >= 2.2f ? "Dressed for winter" : clo >= 1.4f ? "Warmly dressed" : clo >= 0.8f ? "Dressed for autumn" : "Lightly dressed";
    row("Dressed", warm + (wp > 0.7f ? ", rainproof" : wp > 0.3f ? ", showerproof" : ""), rx, y);
    row("Carrying", fmt("%.1f / %.0f kg", game_.carriedKg(), game_.carryCapacityKg()), rx, y,
        game_.carriedKg() > game_.carryCapacityKg() ? ui::theme().bad : ui::theme().text);
    const ItemInstance* w = game_.weapon();
    row("In hand", w ? w->def->name : "Nothing", rx, y);
    y += 10 * s;
    heading("Know-how", rx, y);
    std::string know;
    if (pl.knowsCarpentry) know += "Carpentry: doors, windows and roofs\n";
    if (pl.knowsHotwire) know += "Hotwiring cars\n";
    if (game_.knowsMushrooms()) know += "Telling mushrooms apart\n";
    if (know.empty()) know = "Nothing special yet. Books and people can teach you.";
    else know.pop_back();
    y = ui::paragraph(know, rx, y, col, 15 * s, know.rfind("Nothing", 0) == 0 ? ui::theme().dim : ui::theme().text) + 10 * s;
    heading("People", rx, y);
    int met = 0;
    std::string with, close;
    for (const auto& sv : game_.survivors()) {
        if (!sv.met) continue;
        ++met;
        if (sv.joined && sv.alive()) with += (with.empty() ? "" : ", ") + sv.def->name;
        Bond bd = sv.bond();
        if (sv.alive() && (bd == Bond::Lover || bd == Bond::Family || bd == Bond::BestFriend))
            close += (close.empty() ? "" : ", ") + sv.def->name + " (" + bondName(bd) + ")";
    }
    row("Met", std::to_string(met), rx, y);
    row("With you", with.empty() ? "Nobody" : with, rx, y);
    if (!close.empty()) row("Closest", close, rx, y);
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
                case Zone::Paved: c = {214, 206, 192, 255}; break;
                case Zone::Parking: c = {196, 196, 196, 255}; break;
                case Zone::ForestDense: c = {76, 104, 66, 255}; break;
                case Zone::Thicket: c = {58, 84, 52, 255}; break;
                case Zone::Marsh: c = {112, 138, 128, 255}; break;
                case Zone::Rock: c = {150, 148, 140, 255}; break;
                default: c = {120, 120, 120, 255}; break;
            }
            px[y * w + x] = c;
        }
    // Hills: shaded from the north-west, with brown contour lines every 5 m
    // (every 25 m a little darker), like an orienteering map
    if (m.hasElevation()) {
        float mpp = m.terrainMetresPerPixel();
        std::vector<float> row0(w), row1(w);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) row1[x] = m.elevationLinear((x + 0.5f) * mpp, (y + 0.5f) * mpp);
            for (int x = 0; x < w; ++x) {
                Color& c = px[y * w + x];
                bool water = m.zoneAtPixel(x, y) == Zone::Water;
                float hgt = row1[x];
                float gx = (row1[std::min(w - 1, x + 1)] - row1[std::max(0, x - 1)]) / (2 * mpp);
                float gy = y > 0 ? (hgt - row0[x]) / mpp : 0.0f;
                float shadeF = water ? 1.0f : std::clamp(1.0f - (gx + gy) * 1.6f, 0.72f, 1.18f);
                c = shade(c, shadeF);
                if (!water && y > 0 && x > 0) {
                    int k = (int)std::floor(hgt / 5.0f);
                    bool line = k != (int)std::floor(row0[x] / 5.0f) || k != (int)std::floor(row1[x - 1] / 5.0f);
                    if (line) c = mixColor(c, k % 5 == 0 ? Color{150, 90, 40, 255} : Color{176, 120, 66, 255}, k % 5 == 0 ? 0.75f : 0.5f);
                }
            }
            std::swap(row0, row1);
        }
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
    DrawTextureEx(mapTex_, o, 0, k * m.terrainMetresPerPixel() / mpp, WHITE);
    // Buildings once zoomed in
    if (mapZoom_ > 1.8f)
        for (const auto& b : m.osmBuildings()) {
            Vector2 a = toScreen((b.centre.x - b.w * 0.5f) / mpp, (b.centre.y - b.h * 0.5f) / mpp);
            float bw = b.w / mpp * k, bh = b.h / mpp * k;
            if (a.x > area.x + area.width || a.y > area.y + area.height || a.x + bw < area.x || a.y + bh < area.y) continue;
            DrawRectangleV(a, {bw, bh}, b.kind == "mall" ? Color{170, 120, 100, 255} : Color{160, 150, 140, 255});
        }
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& r : m.roads()) {
            bool path = isFootpath(r.cls);
            if ((pass == 0) != path) continue;   // paths underneath
            if (path && mapZoom_ < 1.5f) continue;
            float wdt = r.cls == RoadClass::Motorway ? 4.0f : r.cls == RoadClass::Major ? 2.6f : path ? 0.7f
                      : r.cls == RoadClass::Service ? 1.0f : 1.6f;
            Color col = r.cls == RoadClass::Motorway ? Color{226, 190, 110, 255}
                      : path ? Color{150, 130, 100, 200} : Color{250, 250, 246, 255};
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
    // Important places: fuel, medicine, police, shops...
    {
        float ppm = k / mpp;   // screen pixels per metre
        Vector2 mouse = GetMousePosition();
        const Building* tip = nullptr;
        for (const auto& b : game_.world().gen().allBuildings()) {
            BuildingType t = b.type;
            bool major = t == BuildingType::GasStation || t == BuildingType::Hospital || t == BuildingType::Pharmacy ||
                         t == BuildingType::Police || t == BuildingType::Station || t == BuildingType::Mall;
            bool minor = t == BuildingType::Grocery || t == BuildingType::Hardware || t == BuildingType::FireStation ||
                         t == BuildingType::Church;
            if (!major && !(minor && ppm > 0.22f)) continue;
            if (b.isOutbuilding) continue;
            Vec2 c = b.rect.centre();
            Vector2 sp = toScreen(c.x / mpp, c.y / mpp);
            if (!CheckCollisionPointRec(sp, area)) continue;
            float r = (major ? 10.0f : 8.0f) * s;
            Color bg = t == BuildingType::GasStation ? Color{226, 130, 30, 255} : t == BuildingType::Hospital ? Color{200, 40, 40, 255}
                     : t == BuildingType::Pharmacy ? Color{40, 150, 70, 255} : t == BuildingType::Police ? Color{40, 70, 170, 255}
                     : t == BuildingType::Station ? Color{60, 60, 70, 255} : t == BuildingType::Mall ? Color{150, 60, 150, 255}
                     : t == BuildingType::Grocery ? Color{60, 130, 120, 255} : t == BuildingType::Hardware ? Color{120, 90, 60, 255}
                     : t == BuildingType::FireStation ? Color{220, 60, 30, 255} : Color{110, 110, 120, 255};
            DrawCircleV(sp, r + 1.5f * s, Color{250, 248, 240, 255});
            DrawCircleV(sp, r, bg);
            Color w{255, 255, 255, 255};
            switch (t) {
                case BuildingType::GasStation:   // a pump with its hose
                    DrawRectangleRec({sp.x - r * 0.45f, sp.y - r * 0.55f, r * 0.55f, r * 1.1f}, w);
                    DrawRectangleRec({sp.x - r * 0.35f, sp.y - r * 0.42f, r * 0.35f, r * 0.3f}, bg);
                    DrawLineEx({sp.x + r * 0.1f, sp.y - r * 0.2f}, {sp.x + r * 0.5f, sp.y + r * 0.1f}, 2 * s, w);
                    DrawLineEx({sp.x + r * 0.5f, sp.y + r * 0.1f}, {sp.x + r * 0.5f, sp.y + r * 0.5f}, 2 * s, w);
                    break;
                case BuildingType::Pharmacy: case BuildingType::Hospital:
                    if (t == BuildingType::Hospital) ui::textCentered("H", sp.x, sp.y - r * 0.7f, r * 1.4f, w, true);
                    else {
                        DrawRectangleRec({sp.x - r * 0.18f, sp.y - r * 0.6f, r * 0.36f, r * 1.2f}, w);
                        DrawRectangleRec({sp.x - r * 0.6f, sp.y - r * 0.18f, r * 1.2f, r * 0.36f}, w);
                    }
                    break;
                case BuildingType::Church:
                    DrawRectangleRec({sp.x - r * 0.12f, sp.y - r * 0.65f, r * 0.24f, r * 1.3f}, w);
                    DrawRectangleRec({sp.x - r * 0.45f, sp.y - r * 0.35f, r * 0.9f, r * 0.22f}, w);
                    break;
                default: {
                    const char* g = t == BuildingType::Police ? "P" : t == BuildingType::Station ? "T" : t == BuildingType::Mall ? "S"
                                  : t == BuildingType::Grocery ? "M" : t == BuildingType::Hardware ? "J" : "B";
                    ui::textCentered(g, sp.x, sp.y - r * 0.7f, r * 1.4f, w, true);
                }
            }
            if (CheckCollisionPointCircle(mouse, sp, r + 2)) tip = &b;
        }
        if (tip) {
            std::string what = tip->type == BuildingType::GasStation ? "Fuel station: fill a gas can at the pumps (or siphon with a hose)"
                             : tip->type == BuildingType::Pharmacy ? "Pharmacy" : tip->type == BuildingType::Hospital ? "Hospital"
                             : tip->type == BuildingType::Police ? "Police station" : tip->type == BuildingType::Station ? "Railway station"
                             : tip->type == BuildingType::Mall ? "Shopping centre" : tip->type == BuildingType::Grocery ? "Grocery store"
                             : tip->type == BuildingType::Hardware ? "Hardware store" : tip->type == BuildingType::FireStation ? "Fire station"
                             : "Church";
            ui::tooltip(tip->name.empty() ? buildingTypeName(tip->type) : tip->name, what);
        }
    }
    // People you know about, and where you left a bike or a car
    for (const auto& v : game_.vehicles()) {
        if (v.id == game_.player().vehicle) continue;
        Vector2 vp = toScreen(v.pos.x / mpp, v.pos.y / mpp);
        DrawRectangleRounded({vp.x - 6 * s, vp.y - 4 * s, 12 * s, 8 * s}, 0.4f, 4, Color{60, 60, 70, 255});
        if (CheckCollisionPointCircle(GetMousePosition(), vp, 8 * s)) ui::tooltip(v.kind == VehicleKind::Car ? "Your car" : "Your bike", "Where you left it.");
    }
    std::vector<Vector2> placedLabels;
    for (const auto& sv : game_.survivors()) {
        if (!sv.known && !sv.joined) continue;
        bool roams = sv.def->disposition == Disposition::Raider;
        Vec2 at = sv.joined ? sv.pos : roams ? sv.rumourPos : sv.met ? sv.pos : sv.rumoured ? sv.rumourPos : sv.home;
        if (!sv.alive()) at = sv.pos;
        Vector2 sp = toScreen(at.x / mpp, at.y / mpp);
        bool bad = sv.hostileTo() || sv.def->disposition == Disposition::Hostile || sv.def->disposition == Disposition::Raider;
        Color c = !sv.alive() ? Color{90, 90, 90, 255} : bad ? Color{190, 50, 40, 255} : sv.joined ? Color{60, 140, 200, 255} : Color{50, 140, 70, 255};
        if ((sv.met && !roams) || sv.joined || !sv.alive()) DrawCircleV(sp, 6 * s, c);
        else {   // only heard about: roughly there
            DrawCircleV(sp, 16 * s, Fade(c, 0.15f));
            DrawCircleLinesV(sp, 16 * s, c);
            DrawCircleLinesV(sp, 15 * s, c);
            ui::textCentered("?", sp.x, sp.y - 8 * s, 16 * s, c, true);
        }
        if (CheckCollisionPointCircle(GetMousePosition(), sp, 16 * s)) {
            std::string body = !sv.alive() ? "Dead." : sv.joined ? "With you." : sv.met && !roams ? sv.def->homeName
                             : std::string(roams ? "Last seen here, according to someone you spoke to." : "Someone told you they live around here.");
            if (!sv.def->describe.empty() && sv.alive()) body += "\n" + sv.def->describe;
            ui::tooltip(sv.def->name, body, c);
        }
        std::string label = sv.met && !roams ? sv.def->name : sv.def->name + "?";
        if (roams && sv.alive()) label += " (last seen)";
        if (!sv.alive()) label += " (dead)";
        // people at the same spot (a raider pair): stack their names
        float ly = sp.y - 8 * s;
        for (const Vector2& q : placedLabels)
            if (std::fabs(q.x - sp.x) < 30 * s && std::fabs(q.y - ly) < 16 * s) ly = q.y + 17 * s;
        placedLabels.push_back({sp.x, ly});
        bool ring = !((sv.met && !roams) || sv.joined || !sv.alive());
        ui::text(label, sp.x + (ring ? 20 : 9) * s, ly, 14 * s, c, true);
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
    if (m.hasElevation()) {   // the contour lines are 5 m apart
        float here = m.elevation(game_.player().pos.x, game_.player().pos.y);
        ui::text(fmt("You are %.0f m above sea level. Contours every 5 m.", here), area.x + 16 * s, area.y + 64 * s, 14 * s,
                 Color{60, 60, 64, 255});
    }
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
    const auto* s = c.find("settings");
    if (!s) return;
    auto vol = [&](const char* k, float& v) { v = std::clamp(s->num(k, v), 0.0f, 1.0f); };
    vol("master_volume", settings_.masterVolume);
    vol("music_volume", settings_.musicVolume);
    vol("sfx_volume", settings_.sfxVolume);
    vol("ambience_volume", settings_.ambienceVolume);
    settings_.muteUnfocused = s->flag("mute_unfocused", settings_.muteUnfocused);
    settings_.fullscreen = s->flag("fullscreen", false);
    settings_.vsync = s->flag("vsync", settings_.vsync);
    settings_.fpsCap = std::clamp((int)s->num("fps_cap", (float)settings_.fpsCap), 0, 360);
    // Settings saved before the render resolution option existed keep the
    // newer (lighter) web defaults for anti-aliasing, shadows and lights
    bool older = !s->values.count("render_scale");
#ifdef __EMSCRIPTEN__
    if (!older)
#else
    (void)older;
#endif
    {
        settings_.msaa = s->flag("msaa", settings_.msaa != 0) ? 1 : 0;
        settings_.gfx.shadows = std::clamp((int)s->num("shadows", (float)settings_.gfx.shadows), 0, 3);
        settings_.gfx.maxLights = std::clamp((int)s->num("max_lights", (float)settings_.gfx.maxLights), 8, 32);
    }
    settings_.gfx.weather = s->flag("weather_effects", settings_.gfx.weather);
    settings_.gfx.visionCone = s->flag("vision_cone", settings_.gfx.visionCone);
    settings_.gfx.brightness = std::clamp(s->num("brightness", settings_.gfx.brightness), 0.7f, 1.4f);
    settings_.gfx.renderScale = std::clamp((int)s->num("render_scale", (float)settings_.gfx.renderScale), 0, 100);
    settings_.uiScale = std::clamp(s->num("ui_scale", settings_.uiScale), 0.8f, 1.3f);
    settings_.screenEffects = s->flag("screen_effects", settings_.screenEffects);
    settings_.minimap = s->flag("minimap", settings_.minimap);
    settings_.showFps = s->flag("show_fps", false);
    settings_.minutesPerSecond = std::clamp(s->num("minutes_per_second", settings_.minutesPerSecond), 0.25f, 4.0f);
    settings_.autoDrink = s->flag("auto_drink", settings_.autoDrink);
    game_.setAutoDrink(settings_.autoDrink);
    // The character you made last time
    if (const auto* ch = c.find("character")) {
        CharacterChoice& cc = create_;
        cc.look.name = ch->str("name", cc.look.name).substr(0, 20);
        int g = std::clamp(ch->integer("gender", 0), 0, 2);
        cc.look.gender = (Gender)g;
        cc.look.skin = (uint32_t)std::strtoul(ch->str("skin", "e2b896").c_str(), nullptr, 16);
        cc.look.hair = (uint32_t)std::strtoul(ch->str("hair", "4a3626").c_str(), nullptr, 16);
        cc.look.hairStyle = std::clamp(ch->integer("hair_style", 0), 0, kHairStyles - 1);
        for (size_t sl = 1; sl < cc.wear.size(); ++sl) {
            std::string key = std::string("wear_") + slotName((ClothingSlot)sl);
            if (ch->has(key)) cc.wear[sl] = ch->str(key) == "-" ? "" : ch->str(key);
            if (ch->has(key + "_tint")) cc.tint[sl] = (uint32_t)std::strtoul(ch->str(key + "_tint").c_str(), nullptr, 16);
        }
    }
}

void App::saveSettings() {
    Config c;
    auto& s = c.getOrAdd("settings");
    auto set = [&](const std::string& k, const std::string& v) { if (!s.values.count(k)) s.keyOrder.push_back(k); s.values[k] = v; };
    auto flag = [](bool b) { return std::string(b ? "true" : "false"); };
    set("master_volume", fmt("%.2f", settings_.masterVolume));
    set("music_volume", fmt("%.2f", settings_.musicVolume));
    set("sfx_volume", fmt("%.2f", settings_.sfxVolume));
    set("ambience_volume", fmt("%.2f", settings_.ambienceVolume));
    set("mute_unfocused", flag(settings_.muteUnfocused));
    set("fullscreen", flag(settings_.fullscreen));
    set("vsync", flag(settings_.vsync));
    set("fps_cap", fmt("%d", settings_.fpsCap));
    set("msaa", flag(settings_.msaa != 0));
    set("shadows", fmt("%d", settings_.gfx.shadows));
    set("max_lights", fmt("%d", settings_.gfx.maxLights));
    set("weather_effects", flag(settings_.gfx.weather));
    set("vision_cone", flag(settings_.gfx.visionCone));
    set("brightness", fmt("%.2f", settings_.gfx.brightness));
    set("render_scale", fmt("%d", settings_.gfx.renderScale));
    set("ui_scale", fmt("%.2f", settings_.uiScale));
    set("screen_effects", flag(settings_.screenEffects));
    set("minimap", flag(settings_.minimap));
    set("show_fps", flag(settings_.showFps));
    set("minutes_per_second", fmt("%.2f", settings_.minutesPerSecond));
    set("auto_drink", flag(settings_.autoDrink));
    auto& ch = c.getOrAdd("character");
    auto cset = [&](const std::string& k, const std::string& v) { if (!ch.values.count(k)) ch.keyOrder.push_back(k); ch.values[k] = v; };
    cset("name", create_.look.name);
    cset("gender", fmt("%d", (int)create_.look.gender));
    cset("skin", fmt("%06x", create_.look.skin));
    cset("hair", fmt("%06x", create_.look.hair));
    cset("hair_style", fmt("%d", create_.look.hairStyle));
    for (size_t sl = 1; sl < create_.wear.size(); ++sl) {
        if (sl == (size_t)ClothingSlot::Back) continue;
        std::string key = std::string("wear_") + slotName((ClothingSlot)sl);
        cset(key, create_.wear[sl].empty() ? "-" : create_.wear[sl]);
        cset(key + "_tint", fmt("%06x", create_.tint[sl]));
    }
    c.save(settingsPath());
    persistFiles();
}

void App::applySettings() {
    if (IsWindowReady()) {
#ifdef __EMSCRIPTEN__
        // Browsers only go fullscreen from inside a click or key handler: the
        // page asks on the next one (see web/shell.html)
        EM_ASM({ if (Module.setFullscreen) Module.setFullscreen($0); }, settings_.fullscreen ? 1 : 0);
#else
        // Borderless, the size of the monitor: works on X11 and Wayland alike
        // and doesn't change the screen mode
        if (settings_.fullscreen != IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();
#endif
#ifndef __EMSCRIPTEN__
        if (settings_.vsync) SetWindowState(FLAG_VSYNC_HINT);
        else ClearWindowState(FLAG_VSYNC_HINT);
        SetTargetFPS(settings_.fpsCap);
#endif
        renderer_.setOptions(settings_.gfx);
    }
    ui::setUserScale(settings_.uiScale);
    if (audio_) SetMasterVolume(settings_.masterVolume);
    if (musicLoaded_) SetMusicVolume(music_, settings_.musicVolume * musicFade_);
}

void App::updateMusic(float dt) {
    if (!audio_ || (tracks_.empty() && dangerTracks_.empty())) return;
    // Being hunted switches to the danger music (if there is any)
    float threat = state_ == State::Playing ? game_.threat() : 0.0f;
    if (threat > 0.3f) dangerHold_ = 10.0f;
    else dangerHold_ = std::max(0.0f, dangerHold_ - dt);
    bool wantDanger = dangerHold_ > 0 && !dangerTracks_.empty();

    auto start = [&](const std::string& path, bool loop) {
        music_ = LoadMusicStream(path.c_str());
        if (!music_.ctxData) return false;
        music_.looping = loop;
        musicFade_ = loop ? 0.0f : 1.0f;   // danger fades in quickly, calm starts at full volume
        SetMusicVolume(music_, settings_.musicVolume * musicFade_);
        PlayMusicStream(music_);
        musicLoaded_ = true;
        return true;
    };
    auto stop = [&] {
        StopMusicStream(music_);
        UnloadMusicStream(music_);
        musicLoaded_ = false;
    };

    if (musicLoaded_) {
        UpdateMusicStream(music_);
        if (playingDanger_ != wantDanger) {
            // fade out whatever is playing before switching
            musicFade_ -= dt / (wantDanger ? 1.2f : 4.0f);
            SetMusicVolume(music_, settings_.musicVolume * std::max(0.0f, musicFade_));
            if (musicFade_ <= 0) {
                stop();
                musicGap_ = wantDanger ? 0.0f : (float)GetRandomValue(8, 20);
            }
            return;
        }
        if (musicFade_ < 1) {
            musicFade_ = std::min(1.0f, musicFade_ + dt / 1.5f);
            SetMusicVolume(music_, settings_.musicVolume * musicFade_);
        }
        if (!IsMusicStreamPlaying(music_) || (!music_.looping && GetMusicTimePlayed(music_) >= GetMusicTimeLength(music_) - 0.05f)) {
            stop();
            musicGap_ = (float)GetRandomValue(20, 90);
        }
        return;
    }
    if (wantDanger) {
        dangerTrack_ = (dangerTrack_ + 1) % (int)dangerTracks_.size();
        playingDanger_ = true;
        if (!start(dangerTracks_[dangerTrack_], true)) dangerHold_ = 0;
        return;
    }
    playingDanger_ = false;
    if (tracks_.empty()) return;
    musicGap_ -= dt;
    if (musicGap_ > 0) return;
    track_ = (track_ + 1 + GetRandomValue(0, (int)tracks_.size() - 1)) % (int)tracks_.size();
    if (!start(tracks_[track_], false)) musicGap_ = 60;
}

} // namespace alone
