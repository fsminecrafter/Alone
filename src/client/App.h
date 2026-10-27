#pragma once
#include "client/Audio.h"
#include "client/Renderer.h"
#include "game/Game.h"
#include "raylib.h"
#include <string>
#include <vector>

namespace alone {

class App {
public:
    int run(int argc, char** argv);

private:
    enum class State { Menu, Playing, Dead };
    enum class Overlay { None, Inventory, Map, Health, Pause, Settings, Help };

    struct ContextMenu {
        bool open = false;
        Vector2 pos{};
        std::string title;
        std::vector<ActionOption> options;
    };

    struct Settings {
        float musicVolume = 0.45f;
        float sfxVolume = 0.7f;
        float minutesPerSecond = 1.0f;
        bool fullscreen = false;
        bool showFps = false;
    };

    // flow
    void newGame();
    void continueGame();
    void autosave();
    std::string savePath() const;
    void update(float dt);
    void updatePlaying(float dt);
    void worldInput();
    void openItemMenu(const ItemRef& ref, Vector2 at);
    Color hoverColour();

    // drawing
    void drawMenu();
    void drawHud();
    void drawSymptoms();
    void drawCursor();
    void drawClockFace(Vector2 centre, float radius, bool alarmClock);
    void drawSkyDial(Vector2 centre, float radius);
    void drawThermometer(Rectangle r, float celsius);
    static std::string roughTimeOfDay(float hour);
    static std::string feelOfTemperature(float celsius);
    void drawMessages();
    void drawActionBar();
    void drawContextMenu();
    void drawInventory();
    void drawItemList(const std::string& title, Rectangle area, std::vector<ItemRef>& refs, int pane, ItemLoc target);
    void drawItemTooltip(const ItemInstance& it);
    void drawHealth();
    void drawMap();
    void drawPause();
    void drawSettings();
    void drawHelp();
    void drawDeath();
    void drawDebug();
    void buildMapTexture();

    // settings & audio
    void loadSettings();
    void saveSettings();
    void applySettings();
    void updateMusic(float dt);

    Game game_;
    Renderer renderer_;
    Audio sound_;
    State state_ = State::Menu;
    Overlay overlay_ = Overlay::None;
    Overlay settingsReturn_ = Overlay::None;
    ContextMenu menu_;
    Settings settings_;
    bool debug_ = false, showDebug_ = false, quit_ = false;
    float time_ = 0;
    float menuOrbit_ = 0;
    float autosaveTimer_ = 0;
    bool hasSave_ = false;
    bool tour_ = false;              // --screenshots / --anim-showcase run
    std::string showcaseDir_;
    int runShowcase(const std::string& dir);
    Vec2 hover_;
    uint32_t seed_ = 1;

    // inventory window
    uint32_t selectedUid_ = 0;
    uint32_t lastClickUid_ = 0;
    double lastClickTime_ = 0;
    float scroll_[2] = {0, 0};

    // map
    Texture2D mapTex_{};
    bool mapReady_ = false;
    Vector2 mapCentre_{};
    float mapZoom_ = 1;

    // audio
    bool audio_ = false;
    Music music_{};
    bool musicLoaded_ = false;
    float musicGap_ = 4;
    std::vector<std::string> tracks_;        // calm exploration music
    std::vector<std::string> dangerTracks_;  // data/assets/audio/danger: when hunted
    int track_ = -1, dangerTrack_ = -1;
    bool playingDanger_ = false;
    float musicFade_ = 1;                    // volume multiplier for cross-fades
    float dangerHold_ = 0;                   // keeps the danger music going a little after the threat
};

} // namespace alone
