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
    enum class State { Menu, Create, Playing, Dead };
    enum class Overlay { None, Inventory, Map, Health, Pause, Settings, Help, Build, Talk, Trade, Journal };

    struct ContextMenu {
        bool open = false;
        Vector2 pos{};
        std::string title;
        std::vector<ActionOption> options;
    };

    struct Settings {
        // Audio
        float masterVolume = 1.0f;
        float musicVolume = 0.45f;
        float sfxVolume = 0.7f;
        float ambienceVolume = 0.8f;
        bool muteUnfocused = true;
        // Graphics
        bool fullscreen = false;
        bool vsync = true;
        int fpsCap = 60;              // 0 = unlimited
        int msaa = 1;                 // 4x anti-aliasing, applied on restart
        GraphicsOptions gfx;
        float uiScale = 1.0f;
        bool screenEffects = true;    // cold/pain vignette, hallucination tint
        bool minimap = true;          // small map in the corner (N)
        bool showFps = false;
        // Game
        float minutesPerSecond = 1.0f;
        bool autoDrink = true;
    };

    // flow
    void newGame(bool create = true);   // create: make your character first
    // Character creation (AppCreate.cpp)
    void drawCreate();
    void updateCreate(float dt);
    void beginGame();
    void randomCharacter();
    CharacterChoice create_ = CharacterChoice::defaults();
    bool nameFocus_ = false;
    float createSpin_ = 0;
    void continueGame();
    void autosave();
    std::string savePath() const;
    void update(float dt);
    void updatePlaying(float dt);
    void worldInput();
    // What F does: the door or thing in front of you, else what the mouse is on
    bool interactTarget(ActionOption& out);
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
    void drawItemTooltip(const ItemInstance& it);
    // Inventory grid (AppInventory.cpp)
    struct ItemStack { std::vector<ItemRef> refs; ItemInstance* top = nullptr; };
    std::vector<ItemStack> stacksOf(const std::vector<ItemRef>& refs, int tab, int sort);
    // Draws a grid of item tiles; returns the height used. pane: 0 yours, 1 the other side
    void drawItemGrid(Rectangle area, std::vector<ItemStack>& stacks, int pane, ItemLoc target);
    void drawPaperDoll(Rectangle area);
    void drawItemDetails(Rectangle area);
    void drawItemTile(Rectangle r, const ItemInstance& it, int count, bool selected, bool hovered);
    void moveStack(const ItemStack& st, ItemLoc to, bool all);
    // Survivors (AppSurvivors.cpp)
    void drawTalk();
    void drawTrade();
    void drawJournal();
    void drawSurvivorLabels();
    void drawVehicleHud();
    void openTalk(int survivorId);
    int talkTo_ = -1;                // survivor id in conversation
    std::string talkLine_;           // what they just said
    std::vector<uint32_t> tradeMine_, tradeTheirs_;   // item uids offered in a trade
    float tradeScroll_[2] = {0, 0};
    void drawHealth();
    void drawMinimap();
    void drawHealthInfo(Rectangle p);   // the Info tab: who you are
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
    int settingsTab_ = 0;              // 0 audio, 1 graphics, 2 game
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
    int runBench();
    bool bench_ = false;
    void frame();                    // one iteration of the main loop
    void shutdownApp();
    void persistFiles();             // web: flush saves to browser storage
    int frame_ = 0;
    int autoShots_ = 0;
    Vec2 hover_;
    uint32_t seed_ = 1;

    // inventory window
    bool lootPending_ = false;       // a search is running: show the container when it's done
    int buildKind_ = -1;             // placing this BuildKind (-1: not building)
    int buildX_ = 0, buildY_ = 0, buildDir_ = 0;
    void drawBuildMenu();
    void updateBuildPlacement();
    struct Drag { ItemStack stack; bool armed = false, active = false, all = false; Vector2 start{}; int fromPane = -1; } drag_;
    Rectangle paneRect_[2]{};        // where the item grids were drawn (drop targets)
    ItemLoc paneLoc_[2]{ItemLoc::Player, ItemLoc::Ground};
    Rectangle dollRect_{};           // the worn-clothes figure (drop clothes here to put them on)
    ItemRef selected_{};             // item shown in the details strip
    int healthTab_ = 0;              // 0 body, 1 info
    int invTab_ = 0;                 // 0 all, 1 food, 2 medical, 3 gear, 4 clothing, 5 other
    int invSort_ = 0;                // 0 type, 1 name, 2 weight
    int buildTab_ = 0;
    int buildPick_ = 0;              // BuildKind shown in the build menu's details
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
