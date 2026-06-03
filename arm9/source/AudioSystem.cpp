#include "AudioSystem.h"
#include "ObjectSystem.h"   // AudioTrackEntry, AudioEmitterEntry, TRACK_FLAG_LOOP, EMITTER_FLAG_LOOP
#include "ipc_fifo.h"       // FIFO_USER_01, fifoSendValue32, fifoSetValue32Handler

AudioSystem g_audio;

// ---------------------------------------------------------------------------
// Streaming ring buffers live in EWRAM so the ARM7 can DMA-read them without
// going through the ARM9's cache.  Two slots: one per music channel (0 and 1).
// The __attribute__ ensures they land in .ewram, not in the 96 KB DTCM/ITCM.
// ---------------------------------------------------------------------------
static StreamBuffer s_streamBufs[2] __attribute__((section(".ewram")));

// ---------------------------------------------------------------------------
// IPC FIFO helpers
// ---------------------------------------------------------------------------
struct Arm7PlayInfo {
    u32  dataAddr;
    u32  sampleCount;
    u16  loopStart;
    u8   rateDiv;
    u8   flags;
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
    // Send pointer as separate raw packet
    fifoSendPtr(FIFO_USER_01, (u32)&s_arm7PlayInfos[ch]);
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
      playlistFadeFrames(64), playlistDelayFrames(0), playlistDelayCounter(0),
      _camX(0), _camY(0), _camZ(0)
{
    memset(channels, 0, sizeof(channels));
    memset(emitters, 0, sizeof(emitters));
    memset(tracks,   0, sizeof(tracks));
    for (int i = 0; i < AUDIO_MAX_EMITTERS; i++)
        emitters[i].channelId = -1;
}

AudioSystem::~AudioSystem() { stopAllChannels(); }

void AudioSystem::init()
{
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
            tracks[i].loop       = (te.flags & TRACK_FLAG_LOOP) != 0;
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

    AudioSettingsHeader sh;
    long pos = ftell(fd);
    if (pos >= 0 && fread(&sh, sizeof(sh), 1, fd) == 1) {
        if (memcmp(sh.magic, "AUSF", 4) == 0) {
            setPlaylistSettings(sh.fadeFrames, sh.delayFrames);
        } else {
            fseek(fd, pos, SEEK_SET);
        }
    }
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
void AudioSystem::update(float camX, float camY, float camZ)
{
    _camX = camX; _camY = camY; _camZ = camZ;

    // ── Music crossfade ──
    if (fading) {
        u8 fade_len = playlistFadeFrames ? playlistFadeFrames : 1;
        if (fadeStepA > 0) {
            fadeStepA--;
            u8 vol = (u8)((u32)channels[0].targetVol * fadeStepA / fade_len);
            channels[0].volume = vol;
            arm7SetVolume(0, vol);
        }
        if (fadeStepB < fade_len) {
            fadeStepB++;
            u8 vol = (u8)((u32)channels[1].targetVol * fadeStepB / fade_len);
            channels[1].volume = vol;
            arm7SetVolume(1, vol);
        }
        if (fadeStepA == 0 && fadeStepB >= fade_len) {
            arm7Stop(0);
            unloadChannel(0);
            fading = false;
        }
    } else if (playlistActive) {
        // Both channels quiet → advance to next track
        bool a_dead = !channels[0].active &&
                      !(channels[0].streaming && channels[0].streamBuf &&
                        channels[0].streamBuf->active);
        bool b_dead = !channels[1].active &&
                      !(channels[1].streaming && channels[1].streamBuf &&
                        channels[1].streamBuf->active);
        if (a_dead && b_dead) {
            if (playlistDelayFrames == 0) {
                nextTrack();
            } else if (playlistDelayCounter == 0) {
                playlistDelayCounter = playlistDelayFrames;
            } else {
                playlistDelayCounter--;
                if (playlistDelayCounter == 0)
                    nextTrack();
            }
        }
    }

    // ── Stream buffer refill (music channels 0 and 1) ──
    for (int ch = 0; ch < 2; ch++) {
        AudioChannel& c = channels[ch];
        if (!c.streaming || !c.streamBuf || !c.streamBuf->active) continue;
        if (c.streamEof && !c.looping) continue;

        // Refill whichever block ARM9 has NOT just filled
        int nextBlock = 1 - (int)(c.streamBuf->arm9_block);
        _streamFillBlock(ch, nextBlock);

        // Flush that block's bytes from ARM9 data cache so ARM7 sees fresh data
        u32 blockBytes = c.streamIs16
            ? (u32)(STREAM_BLOCK_SAMPLES * 2)
            : (u32)STREAM_BLOCK_SAMPLES;
        DC_FlushRange(c.streamBuf->data + nextBlock * blockBytes, blockBytes);
    }

    // ── Positional emitters ──
    for (u8 i = 0; i < emitterCount; i++) {
        RuntimeEmitter& em = emitters[i];
        if (!em.active) continue;

        float dist = fDist(em.x, em.y, em.z, _camX, _camY, _camZ);
        float att  = attenuate(dist, em.innerRadius, em.outerRadius);
        u8 vol     = (u8)((float)em.baseVolume * att);

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
                    arm7Play(ch, hdr,
                             channels[ch].data + sizeof(DsndHeader),
                             channels[ch].dataBytes - sizeof(DsndHeader), vol);
                    em.channelId         = ch;
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
    currentTrack   = 0;

    channels[1].volume    = 0;
    channels[1].targetVol = (u8)((u32)tracks[0].baseVolume * musicVolume / 127);
    channels[1].looping   = tracks[0].loop;

    if (openDsndStream(1, tracks[0].filename)) {
        fadeStepA = 0;
        fadeStepB = 0;
        if (playlistFadeFrames == 0) {
            channels[1].volume = channels[1].targetVol;
            arm7SetVolume(1, channels[1].volume);
            fading = false;
        } else {
            fading = true;
        }
    }
}

void AudioSystem::stopPlaylist()
{
    playlistActive = false;
    arm7Stop(0);
    arm7Stop(1);
    unloadChannel(0);
    unloadChannel(1);
    fading = false;
}

void AudioSystem::nextTrack()
{
    if (trackCount == 0) return;
    currentTrack = (currentTrack + 1) % trackCount;

    unloadChannel(0);
    channels[0] = channels[1];
    memset(&channels[1], 0, sizeof(AudioChannel));

    channels[1].looping   = tracks[currentTrack].loop;
    channels[1].targetVol =
        (u8)((u32)tracks[currentTrack].baseVolume * musicVolume / 127);

    if (openDsndStream(1, tracks[currentTrack].filename)) {
        fadeStepA = playlistFadeFrames;
        fadeStepB = 0;
        if (playlistFadeFrames == 0) {
            channels[1].volume = channels[1].targetVol;
            arm7SetVolume(1, channels[1].volume);
            fading = false;
        } else {
            fading = true;
        }
        playlistDelayCounter = 0;
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

void AudioSystem::setPlaylistSettings(u8 fadeFrames, u8 delayFrames)
{
    playlistFadeFrames = fadeFrames;
    playlistDelayFrames = delayFrames;
    playlistDelayCounter = 0;
}

// ---------------------------------------------------------------------------
// One-shot positional sound  (fully loaded, not streamed)
// ---------------------------------------------------------------------------
int AudioSystem::playEmitterOnce(const char* path,
                                  float x, float y, float z, u8 vol)
{
    float dist = fDist(x, y, z, _camX, _camY, _camZ);
    float att  = attenuate(dist, 4.0f, 32.0f);
    u8 scaledVol = (u8)((float)vol * att);
    if (scaledVol == 0) return -1;

    int ch = findFreeChannel(false);
    if (ch < 0) return -1;
    if (!loadDsnd(ch, path)) return -1;

    DsndHeader* hdr = (DsndHeader*)channels[ch].data;
    arm7Play(ch, hdr,
             channels[ch].data + sizeof(DsndHeader),
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
// Private: full load (SFX / emitters)
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
    channels[ch].streaming = false;
    strncpy(channels[ch].filename, path, AUDIO_MAX_PATH - 1);
    return true;
}

// ---------------------------------------------------------------------------
// Private: streaming open (music channels)
// ---------------------------------------------------------------------------
bool AudioSystem::openDsndStream(int ch, const char* path)
{
    unloadChannel(ch);

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    DsndHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != DSND_MAGIC) {
        fclose(f); return false;
    }

    bool is16   = (hdr.flags & DSND_FLAG_16BIT)  != 0;
    bool isAdpc = (hdr.flags & DSND_FLAG_ADPCM)  != 0;
    if (isAdpc) is16 = false;   // ADPCM uses its own byte packing

    // Total payload bytes in file
    long payloadStart = ftell(f);
    fseek(f, 0, SEEK_END);
    u32 totalPayload = (u32)(ftell(f) - payloadStart);
    fseek(f, payloadStart, SEEK_SET);

    // Grab the stream buffer slot (channel 0 → slot 0, channel 1 → slot 1)
    int slot = ch & 1;
    StreamBuffer* buf = &s_streamBufs[slot];
    memset(buf, 0, sizeof(StreamBuffer));
    buf->active   = 1;
    buf->is16bit  = is16 ? 1 : 0;
    buf->isAdpcm  = isAdpc ? 1 : 0;

    AudioChannel& c     = channels[ch];
    c.streaming         = true;
    c.active            = false;
    c.data              = nullptr;
    c.dataBytes         = 0;
    c.streamFd          = f;
    c.streamDataOffset  = (u32)payloadStart;
    c.streamTotalBytes  = totalPayload;
    c.streamFilePos     = 0;
    c.streamBuf         = buf;
    c.streamBufIdx      = slot;
    c.streamIs16        = is16;
    c.streamIsAdpcm     = isAdpc;
    c.streamEof         = false;
    c.streamRateDiv     = hdr.rateDiv;
    c.streamFlags       = hdr.flags;
    c.streamLoopStart   = hdr.loopStart;
    c.streamSampleCount = hdr.sampleCount;
    strncpy(c.filename, path, AUDIO_MAX_PATH - 1);

    // Pre-fill both halves before starting playback so ARM7 never reads zeroes
    _streamFillBlock(ch, 0);
    _streamFillBlock(ch, 1);

    // Flush entire buffer
    u32 totalBytes = is16
        ? (u32)(STREAM_BUF_BYTES_16)
        : (u32)(STREAM_BUF_BYTES);
    DC_FlushRange(buf->data, totalBytes);

    // Tell ARM7 to play the ring buffer in REPEAT (loop) mode.
    // sampleCount = total ring size so the channel length covers the full ring;
    // loopStart   = 0 so it wraps back to the very start of buf->data.
    // ARM7 decodes ADPCM via SOUND_FORMAT_ADPCM — no software decompression.
    arm7PlayStream(ch, buf,
                   hdr.rateDiv,
                   // Force LOOP flag so hardware wraps at ring boundary
                   (hdr.flags & ~DSND_FLAG_LOOP) | DSND_FLAG_LOOP,
                   0,                       // loopStart = ring start
                   is16 ? (u32)(STREAM_BLOCK_SAMPLES * STREAM_BLOCKS)
                        : (u32)(STREAM_BUF_BYTES),
                   channels[ch].volume);
    c.active = true;
    return true;
}

// ---------------------------------------------------------------------------
// Private: fill one half-buffer from file
// ---------------------------------------------------------------------------
void AudioSystem::_streamFillBlock(int ch, int block)
{
    AudioChannel& c = channels[ch];
    if (!c.streamFd || !c.streamBuf) return;
    if (c.streamEof && !c.looping)   return;

    // How many bytes make up one block?
    u32 blockBytes = c.streamIs16
        ? (u32)(STREAM_BLOCK_SAMPLES * 2)
        : (u32)(STREAM_BLOCK_SAMPLES);
    // ADPCM: 2 nibbles per byte → STREAM_BLOCK_SAMPLES nibbles = half the bytes
    if (c.streamIsAdpcm)
        blockBytes = STREAM_BLOCK_SAMPLES / 2;

    u8* dst = c.streamBuf->data + (u32)block * blockBytes;
    u32 remaining = c.streamTotalBytes - c.streamFilePos;
    u32 toRead    = (blockBytes < remaining) ? blockBytes : remaining;

    if (toRead > 0) {
        fread(dst, 1, toRead, c.streamFd);
        c.streamFilePos += toRead;
    }

    // Pad the tail of this block with silence if we hit EOF
    if (toRead < blockBytes) {
        u8 silence = c.streamIs16 ? 0x00 : 0x80; // 0x80 = silence for unsigned PCM8
        memset(dst + toRead, silence, blockBytes - toRead);
        c.streamEof = true;

        if (c.looping) {
            // Seek back to the start of the payload for the next fill
            fseek(c.streamFd, (long)c.streamDataOffset, SEEK_SET);
            c.streamFilePos = 0;
            c.streamEof     = false;
        } else {
            c.streamBuf->active = 0;
        }
    }

    c.streamBuf->arm9_block = (u8)block;
}

// ---------------------------------------------------------------------------
// Private: unload channel (handles both streamed and non-streamed)
// ---------------------------------------------------------------------------
void AudioSystem::unloadChannel(int ch)
{
    AudioChannel& c = channels[ch];

    if (c.streaming) {
        if (c.streamBuf) c.streamBuf->active = 0;
        if (c.streamFd)  { fclose(c.streamFd); c.streamFd = nullptr; }
        c.streamBuf      = nullptr;
        c.streamBufIdx   = 0;
        c.streamFd       = nullptr;
        c.streaming      = false;
        c.streamEof      = false;
    }

    if (c.data) {
        free(c.data);
        c.data = nullptr;
    }

    c.active    = false;
    c.dataBytes = 0;
}

// ---------------------------------------------------------------------------
// Private: ARM7 IPC — full load
// ---------------------------------------------------------------------------
void AudioSystem::arm7Play(int ch, const DsndHeader* hdr,
                            u8* data, u32 /*bytes*/, u8 vol)
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

// ---------------------------------------------------------------------------
// Private: ARM7 IPC — streaming (ring buffer already in EWRAM)
// ---------------------------------------------------------------------------
void AudioSystem::arm7PlayStream(int ch, StreamBuffer* buf,
                                  u8 rateDiv, u8 flags,
                                  u16 loopStart, u32 sampleCount, u8 vol)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;

    // Point ARM7 directly at the EWRAM ring buffer
    s_arm7PlayInfos[ch].dataAddr    = (u32)buf->data;
    s_arm7PlayInfos[ch].sampleCount = sampleCount;
    s_arm7PlayInfos[ch].loopStart   = loopStart;
    s_arm7PlayInfos[ch].rateDiv     = rateDiv;
    s_arm7PlayInfos[ch].flags       = flags;
    s_arm7PlayInfos[ch].volume      = vol;
    s_arm7PlayInfos[ch].channelId   = (u8)ch;

    sendFifoPlay(ch);
}

// ---------------------------------------------------------------------------
// Private: ARM7 IPC — volume / stop
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Private: find a free SFX channel (skips music channels 0-1)
// ---------------------------------------------------------------------------
int AudioSystem::findFreeChannel(bool music)
{
    int start = music ? 0 : 2;
    for (int i = start; i < AUDIO_MAX_CHANNELS; i++)
        if (!channels[i].active && channels[i].data == nullptr
                && !channels[i].streaming)
            return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Public: Debug info getters
// ---------------------------------------------------------------------------
const char* AudioSystem::getCurrentTrackFilename() const
{
    if (currentTrack >= trackCount) return "";
    return tracks[currentTrack].filename;
}

u32 AudioSystem::getPlaybackPositionSamples() const
{
    // For streaming music (channel 1), calculate sample position from file position
    if (channels[1].streaming && channels[1].streamBuf) {
        if (channels[1].streamIsAdpcm) {
            // ADPCM: 2 samples per byte, but skip the 4-byte preamble on first read
            u32 streamBytes = channels[1].streamFilePos;
            if (streamBytes > 0) streamBytes -= 4;  // Account for preamble
            return (streamBytes * 2);
        } else {
            // PCM: calculate from byte position
            u32 bytesPerSample = channels[1].streamIs16 ? 2 : 1;
            return channels[1].streamFilePos / bytesPerSample;
        }
    }
    return 0;
}

u32 AudioSystem::getPlaybackPositionSeconds() const
{
    const u32 rateDivTable[] = { 32768, 16384, 8192, 5512 };
    if (currentTrack >= trackCount) return 0;
    
    // Get the streaming channel's rate info
    if (channels[1].streaming) {
        u32 sampleRate = rateDivTable[channels[1].streamRateDiv];
        u32 samples = getPlaybackPositionSamples();
        return samples / sampleRate;
    }
    return 0;
}

void AudioSystem::getTrackFormatInfo(int& outSampleRate, int& outBits, bool& outIsAdpcm) const
{
    outSampleRate = 0;
    outBits = 0;
    outIsAdpcm = false;

    if (!channels[1].streaming) return;

    const u32 rateDivTable[] = { 32768, 16384, 8192, 5512 };
    outSampleRate = rateDivTable[channels[1].streamRateDiv];
    outBits = channels[1].streamIs16 ? 16 : 8;
    outIsAdpcm = channels[1].streamIsAdpcm;
}