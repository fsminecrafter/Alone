#include "AudioSystem.h"
#include "ObjectSystem.h"       // AudioTrackEntry, AudioEmitterEntry, TRACK_FLAG_LOOP, EMITTER_FLAG_LOOP
#include <nds/arm9/sound.h>
#include "ipc_fifo.h"           // FIFO_USER_01, fifoSendValue32, fifoSetValue32Handler
                                // (compatibility shim — libnds on this install has no fifocommon.h)

AudioSystem g_audio;

// ---------------------------------------------------------------------------
// IPC FIFO message layout to ARM7
// We use FIFO_USER_01 (the first user-defined channel).
// The ARM7 half runs a matching handler (see AudioArm7.cpp).
//
// Message word layout (32-bit):
//   bits [31:24] = command  (AUDIO_CMD_*)
//   bits [23:16] = channel  (0-15)
//   bits  [7: 0] = volume   (0-127 for VOL/PLAY)
//
// For PLAY we send a second word: pointer to the Arm7PlayInfo staging struct.
// ---------------------------------------------------------------------------

struct Arm7PlayInfo {
    u32  dataAddr;      // 32-bit ARM7-space address of raw PCM (past DsndHeader)
    u32  sampleCount;
    u16  loopStart;
    u8   rateDiv;
    u8   flags;         // DSND_FLAG_*
    u8   volume;
    u8   channelId;
    u16  pad;
};

static Arm7PlayInfo s_arm7PlayInfos[AUDIO_MAX_CHANNELS];

static void sendFifoPlay(int ch)
{
    DC_FlushRange(&s_arm7PlayInfos[ch], sizeof(Arm7PlayInfo));
    fifoSendValue32(FIFO_USER_01,
                    AUDIO_PACK(AUDIO_CMD_PLAY, ch, s_arm7PlayInfos[ch].volume));
    fifoSendValue32(FIFO_USER_01, (u32)&s_arm7PlayInfos[ch]);
}

static void sendFifoVol(int ch, u8 vol)
{
    fifoSendValue32(FIFO_USER_01, AUDIO_PACK(AUDIO_CMD_VOL, ch, vol));
}

static void sendFifoStop(int ch)
{
    fifoSendValue32(FIFO_USER_01, AUDIO_PACK(AUDIO_CMD_STOP, ch, 0));
}

// ---------------------------------------------------------------------------
// Constructor / init
// ---------------------------------------------------------------------------
AudioSystem::AudioSystem()
    : trackCount(0), emitterCount(0), currentTrack(0), nextTrackIdx(0),
      fadeStepA(0), fadeStepB(0), fading(false),
      playlistActive(false), musicVolume(100),
      _camX(0), _camY(0), _camZ(0)
{
    memset(channels, 0, sizeof(channels));
    memset(emitters, 0, sizeof(emitters));
    memset(tracks,   0, sizeof(tracks));
    // AudioChannel has no channelId field; channel identity is its array index.
    // RuntimeEmitter.channelId is -1 when not playing.
    for (int i = 0; i < AUDIO_MAX_EMITTERS; i++)
        emitters[i].channelId = -1;
}

AudioSystem::~AudioSystem() { stopAllChannels(); }

void AudioSystem::init()
{
    // ARM9 never receives on the user FIFO — just enable it for sending.
    fifoSetValue32Handler(FIFO_USER_01, nullptr, nullptr);
    soundEnable();
}

// ---------------------------------------------------------------------------
// loadFromWorld
// ---------------------------------------------------------------------------
void AudioSystem::loadFromWorld(FILE* fd, u8 numTracks, u8 numEmitters)
{
    trackCount = (numTracks < AUDIO_MAX_TRACKS) ? numTracks : AUDIO_MAX_TRACKS;
    for (u8 i = 0; i < numTracks; i++) {
        AudioTrackEntry te;
        if (fread(&te, sizeof(te), 1, fd) != 1) break;
        if (i < trackCount) {
            memcpy(tracks[i].filename, te.filename, AUDIO_MAX_PATH);
            tracks[i].baseVolume = te.volume;
            tracks[i].loop = (te.flags & TRACK_FLAG_LOOP) != 0;
        }
    }

    emitterCount = (numEmitters < AUDIO_MAX_EMITTERS) ? numEmitters : AUDIO_MAX_EMITTERS;
    for (u8 i = 0; i < numEmitters; i++) {
        AudioEmitterEntry ee;
        if (fread(&ee, sizeof(ee), 1, fd) != 1) break;
        if (i < emitterCount) {
            RuntimeEmitter& em = emitters[i];
            em.active      = true;
            em.x           = (float)ee.worldX / 4096.0f;
            em.y           = (float)ee.worldY / 4096.0f;
            em.z           = (float)ee.worldZ / 4096.0f;
            em.innerRadius = (float)ee.radiusInner / 16.0f;
            em.outerRadius = (float)ee.radiusOuter / 16.0f;
            em.baseVolume  = ee.volume;
            em.loop        = (ee.flags & EMITTER_FLAG_LOOP) != 0;
            memcpy(em.filename, ee.filename, AUDIO_MAX_PATH);
            em.channelId = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
void AudioSystem::update(float camX, float camY, float camZ)
{
    _camX = camX; _camY = camY; _camZ = camZ;

    // --- Music cross-fade ---
    if (fading) {
        if (fadeStepA > 0) {
            fadeStepA--;
            u8 vol = (u8)((u32)channels[0].targetVol * fadeStepA / AUDIO_FADE_STEPS);
            channels[0].volume = vol;
            arm7SetVolume(0, vol);
        }
        if (fadeStepB < AUDIO_FADE_STEPS) {
            fadeStepB++;
            u8 vol = (u8)((u32)channels[1].targetVol * fadeStepB / AUDIO_FADE_STEPS);
            channels[1].volume = vol;
            arm7SetVolume(1, vol);
        }
        if (fadeStepA == 0 && fadeStepB >= AUDIO_FADE_STEPS) {
            arm7Stop(0);
            unloadChannel(0);
            fading = false;
        }
    } else if (playlistActive) {
        if (!channels[1].active && !channels[0].active)
            nextTrack();
    }

    // --- Positional emitters ---
    for (u8 i = 0; i < emitterCount; i++) {
        RuntimeEmitter& em = emitters[i];
        if (!em.active) continue;

        float dist = fDist(em.x, em.y, em.z, _camX, _camY, _camZ);
        float att  = attenuate(dist, em.innerRadius, em.outerRadius);
        u8 vol = (u8)((float)em.baseVolume * att);

        if (vol == 0) {
            if (em.channelId >= 0) {
                arm7Stop(em.channelId);
                unloadChannel(em.channelId);
                em.channelId = -1;
            }
        } else {
            if (em.channelId < 0) {
                int ch = findFreeChannel(false);
                if (ch >= 0 && loadDsnd(ch, em.filename)) {
                    DsndHeader* hdr = (DsndHeader*)channels[ch].data;
                    arm7Play(ch, hdr, channels[ch].data + sizeof(DsndHeader),
                             channels[ch].dataBytes - sizeof(DsndHeader), vol);
                    em.channelId = ch;
                    channels[ch].volume  = vol;
                    channels[ch].looping = em.loop;
                }
            } else if (channels[em.channelId].volume != vol) {
                channels[em.channelId].volume = vol;
                arm7SetVolume(em.channelId, vol);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Music playlist
// ---------------------------------------------------------------------------
void AudioSystem::startPlaylist()
{
    if (trackCount == 0) return;
    playlistActive = true;
    currentTrack = 0;
    if (loadDsnd(1, tracks[0].filename)) {
        DsndHeader* hdr = (DsndHeader*)channels[1].data;
        channels[1].targetVol = (u8)((u32)tracks[0].baseVolume * musicVolume / 127);
        fadeStepA = 0; fadeStepB = 0; fading = true;
        arm7Play(1, hdr, channels[1].data + sizeof(DsndHeader),
                 channels[1].dataBytes - sizeof(DsndHeader), 0);
    }
}

void AudioSystem::stopPlaylist()
{
    playlistActive = false;
    arm7Stop(0); arm7Stop(1);
    unloadChannel(0); unloadChannel(1);
    fading = false;
}

void AudioSystem::nextTrack()
{
    if (trackCount == 0) return;
    currentTrack = (currentTrack + 1) % trackCount;
    unloadChannel(0);
    channels[0] = channels[1];
    memset(&channels[1], 0, sizeof(AudioChannel));

    if (loadDsnd(1, tracks[currentTrack].filename)) {
        DsndHeader* hdr = (DsndHeader*)channels[1].data;
        channels[1].targetVol = (u8)((u32)tracks[currentTrack].baseVolume * musicVolume / 127);
        fadeStepA = AUDIO_FADE_STEPS;
        fadeStepB = 0;
        fading = true;
        arm7Play(1, hdr, channels[1].data + sizeof(DsndHeader),
                 channels[1].dataBytes - sizeof(DsndHeader), 0);
    }
}

void AudioSystem::prevTrack()
{
    if (trackCount == 0) return;
    currentTrack = (currentTrack + trackCount - 1) % trackCount;
    nextTrack();
}

void AudioSystem::setMusicVolume(u8 vol)
{
    musicVolume = vol;
    if (channels[1].active) {
        u8 scaled = (u8)((u32)channels[1].targetVol * vol / 127);
        arm7SetVolume(1, scaled);
    }
}

// ---------------------------------------------------------------------------
// One-shot positional sound
// ---------------------------------------------------------------------------
int AudioSystem::playEmitterOnce(const char* path, float x, float y, float z, u8 vol)
{
    float dist = fDist(x, y, z, _camX, _camY, _camZ);
    float att = attenuate(dist, 4.0f, 32.0f);
    u8 scaledVol = (u8)((float)vol * att);
    if (scaledVol == 0) return -1;

    int ch = findFreeChannel(false);
    if (ch < 0) return -1;
    if (!loadDsnd(ch, path)) return -1;

    DsndHeader* hdr = (DsndHeader*)channels[ch].data;
    arm7Play(ch, hdr, channels[ch].data + sizeof(DsndHeader),
             channels[ch].dataBytes - sizeof(DsndHeader), scaledVol);
    channels[ch].volume  = scaledVol;
    channels[ch].looping = false;
    return ch;
}

void AudioSystem::stopChannel(int ch)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    arm7Stop(ch);
    unloadChannel(ch);
}

void AudioSystem::stopAllChannels()
{
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++)
        stopChannel(i);
    fifoSendValue32(FIFO_USER_01, AUDIO_PACK(AUDIO_CMD_STOP_ALL, 0, 0));
}

bool AudioSystem::isChannelActive(int ch) const
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return false;
    return channels[ch].active;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
bool AudioSystem::loadDsnd(int ch, const char* path)
{
    unloadChannel(ch);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    u32 sz = (u32)ftell(f);
    rewind(f);
    if (sz < sizeof(DsndHeader)) { fclose(f); return false; }

    u8* buf = (u8*)malloc(sz);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, sz, f);
    fclose(f);

    DsndHeader* hdr = (DsndHeader*)buf;
    if (hdr->magic != DSND_MAGIC) { free(buf); return false; }

    channels[ch].data      = buf;
    channels[ch].dataBytes = sz;
    channels[ch].active    = false;
    strncpy(channels[ch].filename, path, AUDIO_MAX_PATH-1);
    return true;
}

void AudioSystem::unloadChannel(int ch)
{
    if (channels[ch].data) {
        free(channels[ch].data);
        channels[ch].data = nullptr;
    }
    channels[ch].active    = false;
    channels[ch].dataBytes = 0;
}

int AudioSystem::findFreeChannel(bool music)
{
    int start = music ? 0 : 2;
    for (int i = start; i < AUDIO_MAX_CHANNELS; i++)
        if (!channels[i].active && channels[i].data == nullptr)
            return i;
    return -1;
}

void AudioSystem::arm7Play(int ch, const DsndHeader* hdr, u8* data, u32 /*bytes*/, u8 vol)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    s_arm7PlayInfos[ch].dataAddr    = (u32)data;
    s_arm7PlayInfos[ch].sampleCount = hdr->sampleCount;
    s_arm7PlayInfos[ch].loopStart   = hdr->loopStart;
    s_arm7PlayInfos[ch].rateDiv     = hdr->rateDiv;
    s_arm7PlayInfos[ch].flags       = hdr->flags;
    s_arm7PlayInfos[ch].volume      = vol;
    s_arm7PlayInfos[ch].channelId   = (u8)ch;
    channels[ch].active = true;
    sendFifoPlay(ch);
}

void AudioSystem::arm7SetVolume(int ch, u8 vol)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    sendFifoVol(ch, vol);
}

void AudioSystem::arm7Stop(int ch)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    sendFifoStop(ch);
    channels[ch].active = false;
}