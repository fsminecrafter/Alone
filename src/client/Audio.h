#pragma once
#include "game/Game.h"
#include "raylib.h"

namespace alone {

// Procedurally synthesised sound: an ambient bed (rain, wind, fire) streamed
// continuously, plus short effects generated once at start-up.
class Audio {
public:
    void init();      // after InitAudioDevice()
    void shutdown();
    void update(Game& game, float volume);   // ambience follows the weather
    void play(const SoundEvent& e, Vec2 listener, float volume);

private:
    bool ready_ = false;
    struct Effect { Sound sound{}; bool loaded = false; };
    static constexpr int kEffects = 13;   // number of Sfx values
    Effect effects_[kEffects][3];   // a few variants per effect
};

} // namespace alone
