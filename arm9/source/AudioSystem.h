#pragma once

// ---------------------------------------------------------------------------
// AudioSystem.h  —  Music playlist + 3-D positional audio for NDS
//
// Architecture
// ------------
//   ARM9 side  (this file + AudioSystem.cpp)
//     • Manages .dsnd sample data in EWRAM
//     • Pushes play/stop/volume commands to ARM7 via a small FIFO IPC buffer
//     • Updates positional emitter volumes once per frame
//
//   ARM7 side  (AudioArm7.cpp / AudioArm7.h)
//     • Runs inside the libnds default ARM7 binary (no custom ARM7 needed)
//     • Receives commands via the user FIFO channel
//     • Mixes up to 16 channels with individual volumes
//
//   .dsnd format (produced by the editor's DSify pipeline)
//     [0] u8  sampleRate divider (0 = 32768 Hz, 1 = 16384 Hz, 2 = 8192 Hz)
//     [1] u8  flags (DSND_FLAG_STEREO, DSND_FLAG_LOOP, DSND_FLAG_16BIT)
//     [2..3] u16 loopStart  (samples)
//     [4..7] u32 sampleCount
//     [8..]  raw PCM8 or PCM16 samples
//
//   Playlist  — loads tracks lazily; fades between songs (64-step linear fade,
//               1 step per frame ≈ 1 second crossfade at 60 fps).
//
//   Emitters  — positional audio attached to objects or chunks.  Distance
//               attenuation:  vol = clamp((outer - dist) / (outer - inner), 0, 1)
//               multiplied by the emitter's base volume.
// ---------------------------------------------------------------------------

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// IPC command packet  (fits in one 32-bit FIFO word)
// ---------------------------------------------------------------------------
#define AUDIO_CMD_PLAY     0x01
#define AUDIO_CMD_STOP     0x02
#define AUDIO_CMD_VOL      0x03
#define AUDIO_CMD_STOP_ALL 0x04

// We smuggle the command in the top 8 bits, channel in the next 4, volume in low 7
#define AUDIO_PACK(cmd, ch, vol) \
    (u32)(((cmd)<<24) | ((ch)<<16) | ((vol)&0x7F))

// ---------------------------------------------------------------------------
// DSND header
// ---------------------------------------------------------------------------
#define DSND_MAGIC        0x444E5344u  // "DSND"
#define DSND_FLAG_STEREO  (1<<0)
#define DSND_FLAG_LOOP    (1<<1)
#define DSND_FLAG_16BIT   (1<<2)

#pragma pack(push,1)
struct DsndHeader {
    u32 magic;          // DSND_MAGIC
    u8  rateDiv;        // 0=32768 Hz, 1=16384 Hz, 2=8192 Hz, 3=5512 Hz
    u8  flags;
    u16 loopStart;      // sample index
    u32 sampleCount;
    // raw PCM follows
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define AUDIO_MAX_CHANNELS   16
#define AUDIO_MAX_EMITTERS   32
#define AUDIO_MAX_TRACKS     16
#define AUDIO_MAX_PATH       48
#define AUDIO_FADE_STEPS     64    // frames for a full fade
#define AUDIO_MUSIC_CHANNEL  0     // channels 0-1 reserved for music (A/B cross-fade)

// ---------------------------------------------------------------------------
// Runtime types
// ---------------------------------------------------------------------------
struct AudioChannel {
    bool    active;
    bool    looping;
    u8      volume;       // current hardware volume 0-127
    u8      targetVol;    // target (for fade)
    char    filename[AUDIO_MAX_PATH];
    u8*     data;         // loaded DSND data (in EWRAM)
    u32     dataBytes;
};

struct RuntimeEmitter {
    bool    active;
    float   x, y, z;
    float   innerRadius;
    float   outerRadius;
    u8      baseVolume;
    bool    loop;
    char    filename[AUDIO_MAX_PATH];
    int     channelId;    // -1 = not playing
};

struct MusicTrack {
    char    filename[AUDIO_MAX_PATH];
    u8      baseVolume;
    bool    loop;
};

// ---------------------------------------------------------------------------
// AudioSystem  (ARM9 controller)
// ---------------------------------------------------------------------------
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    // Called once at startup
    void init();

    // Load audio sections from an already-opened .world v2 file.
    // fd must be positioned right after the AudioTableHeader.
    void loadFromWorld(FILE* fd, u8 trackCount, u8 emitterCount);

    // Per-frame update: advance music fade, update emitter volumes.
    // camX/Y/Z = camera/listener world position.
    void update(float camX, float camY, float camZ);

    // ---- Music ----
    // Start the playlist from the beginning (fades in first track).
    void startPlaylist();
    void stopPlaylist();
    void nextTrack();
    void prevTrack();
    void setMusicVolume(u8 vol);    // 0-127

    // ---- One-shot positional sound ----
    // Returns channel id or -1.
    int  playEmitterOnce(const char* path, float x, float y, float z, u8 vol = 100);
    void stopChannel(int channelId);
    void stopAllChannels();

    // ---- Internals used by ScriptComponent ----
    bool isChannelActive(int ch) const;

private:
    AudioChannel  channels[AUDIO_MAX_CHANNELS];
    RuntimeEmitter emitters[AUDIO_MAX_EMITTERS];
    MusicTrack     tracks[AUDIO_MAX_TRACKS];

    u8   trackCount;
    u8   emitterCount;
    u8   currentTrack;
    u8   nextTrackIdx;

    // Cross-fade state: channel 0 = outgoing, channel 1 = incoming
    u8   fadeStepA;     // current fade step for channel 0
    u8   fadeStepB;     // current fade step for channel 1
    bool fading;
    bool playlistActive;
    u8   musicVolume;

    float _camX, _camY, _camZ;

    // Load a .dsnd file into EWRAM and set up an AudioChannel
    bool loadDsnd(int ch, const char* path);
    void unloadChannel(int ch);

    // Send play command to ARM7 via FIFO
    void arm7Play(int ch, const DsndHeader* hdr, u8* data, u32 bytes, u8 vol);
    void arm7SetVolume(int ch, u8 vol);
    void arm7Stop(int ch);

    // Find a free channel (skip music channels 0-1 unless music=true)
    int  findFreeChannel(bool music = false);

    // Distance attenuation helper
    static float attenuate(float dist, float inner, float outer) {
        if (dist <= inner)  return 1.0f;
        if (dist >= outer)  return 0.0f;
        return (outer - dist) / (outer - inner);
    }

    static float fDist(float ax, float ay, float az,
                       float bx, float by, float bz) {
        float dx = ax-bx, dy = ay-by, dz = az-bz;
        return sqrtf(dx*dx + dy*dy + dz*dz);
    }
};

// Global singleton
extern AudioSystem g_audio;
