#include "AudioSystem.h"
#include "ObjectSystem.h"   // AudioTrackEntry, AudioEmitterEntry, TRACK_FLAG_LOOP, EMITTER_FLAG_LOOP
#include <calico.h>         // pxiSend() — ARM7 uses calico PXI, ARM9 must match
#include "../../common/include/logger.h"

// All audio IPC goes through calico PXI on PxiChannel_User0.
// Do NOT use libnds fifoSendValue32 / fifoSendPtr — those use a different
// framing protocol and will never reach the ARM7 pxiSetHandler.
#define AUDIO_PXI_CHANNEL   PxiChannel_User0

AudioSystem g_audio;

// ---------------------------------------------------------------------------
// Streaming ring buffers live in EWRAM so the ARM7 can DMA-read them without
// going through the ARM9's cache.  Two slots: one per music channel (0 and 1).
// ---------------------------------------------------------------------------
static StreamBuffer s_streamBufs[2] __attribute__((section(".ewram")));

// ---------------------------------------------------------------------------
// IPC play-info rendezvous
//
// ARM9 and ARM7 are separate binaries; "extern" does not work across them.
// Both sides access Arm7PlayInfo through AUDIO_PLAY_INFO_ARRAY(), a fixed
// physical address in upper EWRAM defined in AudioSystem.h.
//
// ARM9 writes via the volatile pointer and calls DC_FlushRange before sending
// the PXI packet.  ARM7 has no D-cache so reads main RAM directly.
// ---------------------------------------------------------------------------
static inline volatile Arm7PlayInfo& arm9PlayInfo(int ch)
{
    return AUDIO_PLAY_INFO_ARRAY()[ch];
}

// ---------------------------------------------------------------------------
// IPC FIFO helpers
// ---------------------------------------------------------------------------
static void sendFifoPlay(int ch)
{
    // Flush this channel's Arm7PlayInfo from D-cache to main RAM.
    DC_FlushRange(
        (void*)(AUDIO_SHARED_EWRAM_ADDR + (u32)ch * sizeof(Arm7PlayInfo)),
        sizeof(Arm7PlayInfo));

    u32 packet = AUDIO_PACK(AUDIO_CMD_PLAY, ch, arm9PlayInfo(ch).volume);
    logger_printf("[AUDIO IPC] sendFifoPlay ch%d: packet=0x%08lX "
                  "rendezvous=0x%08lX dataAddr=0x%08lX vol=%u\n",
                  ch, (unsigned long)packet,
                  (unsigned long)(AUDIO_SHARED_EWRAM_ADDR + (u32)ch * sizeof(Arm7PlayInfo)),
                  (unsigned long)arm9PlayInfo(ch).dataAddr,
                  (unsigned)arm9PlayInfo(ch).volume);

    pxiSend(AUDIO_PXI_CHANNEL, packet);
}

static void sendFifoVol(int ch, u8 vol)
{
    u32 packet = AUDIO_PACK(AUDIO_CMD_VOL, ch, vol);
    logger_printf("[AUDIO IPC] sendFifoVol ch%d vol=%u packet=0x%08lX\n",
                  ch, (unsigned)vol, (unsigned long)packet);
    pxiSend(AUDIO_PXI_CHANNEL, packet);
}

static void sendFifoStop(int ch)
{
    u32 packet = AUDIO_PACK(AUDIO_CMD_STOP, ch, 0);
    logger_printf("[AUDIO IPC] sendFifoStop ch%d packet=0x%08lX\n",
                  ch, (unsigned long)packet);
    pxiSend(AUDIO_PXI_CHANNEL, packet);
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
    // soundEnable() is a libnds call that touches REG_SOUNDCNT on ARM9.
    // Under calico ARM9 does not own sound hardware — ARM7 owns it and sets
    // REG_SOUNDCNT = SOUND_ENABLE | SOUND_VOL(127) in AudioArm7.cpp.  Removed.

    logger_printf("[AUDIO] ============================================\n");
    logger_printf("[AUDIO] AudioSystem::init()\n");
    logger_printf("[AUDIO]   AUDIO_SHARED_EWRAM_ADDR = 0x%08lX\n",
                  (unsigned long)AUDIO_SHARED_EWRAM_ADDR);
    logger_printf("[AUDIO]   sizeof(Arm7PlayInfo)    = %u\n",
                  (unsigned)sizeof(Arm7PlayInfo));
    logger_printf("[AUDIO]   array end               = 0x%08lX\n",
                  (unsigned long)(AUDIO_SHARED_EWRAM_ADDR
                                  + AUDIO_MAX_CHANNELS * sizeof(Arm7PlayInfo)));
    logger_printf("[AUDIO]   s_streamBufs[0]         = 0x%08lX\n",
                  (unsigned long)(u32)&s_streamBufs[0]);
    logger_printf("[AUDIO]   s_streamBufs[1]         = 0x%08lX\n",
                  (unsigned long)(u32)&s_streamBufs[1]);
    logger_printf("[AUDIO] ============================================\n");
}

// ---------------------------------------------------------------------------
// loadFromWorld
// ---------------------------------------------------------------------------
void AudioSystem::loadFromWorld(FILE* fd, u8 numTracks, u8 numEmitters)
{
    logger_printf("[AUDIO] loadFromWorld: numTracks=%u numEmitters=%u\n",
                  (unsigned)numTracks, (unsigned)numEmitters);

    trackCount = (numTracks < AUDIO_MAX_TRACKS) ? numTracks : AUDIO_MAX_TRACKS;
    for (u8 i = 0; i < numTracks; i++) {
        AudioTrackEntry te;
        if (fread(&te, sizeof(te), 1, fd) != 1) {
            logger_printf("[AUDIO]   track %u: fread failed — truncated file?\n",
                          (unsigned)i);
            break;
        }
        if (i < trackCount) {
            memcpy(tracks[i].filename, te.filename, AUDIO_MAX_PATH);
            tracks[i].baseVolume = te.volume;
            tracks[i].loop       = (te.flags & TRACK_FLAG_LOOP) != 0;
            logger_printf("[AUDIO]   track %u: '%s' baseVol=%u loop=%d\n",
                          (unsigned)i, tracks[i].filename,
                          (unsigned)tracks[i].baseVolume, (int)tracks[i].loop);
        }
    }

    emitterCount = (numEmitters < AUDIO_MAX_EMITTERS) ? numEmitters : AUDIO_MAX_EMITTERS;
    for (u8 i = 0; i < numEmitters; i++) {
        AudioEmitterEntry ee;
        if (fread(&ee, sizeof(ee), 1, fd) != 1) {
            logger_printf("[AUDIO]   emitter %u: fread failed\n", (unsigned)i);
            break;
        }
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
            logger_printf("[AUDIO]   emitter %u: '%s' vol=%u loop=%d "
                          "inner=%.1f outer=%.1f\n",
                          (unsigned)i, em.filename,
                          (unsigned)em.baseVolume, (int)em.loop,
                          em.innerRadius, em.outerRadius);
        }
    }

    AudioSettingsHeader sh;
    long pos = ftell(fd);
    if (pos >= 0 && fread(&sh, sizeof(sh), 1, fd) == 1) {
        if (memcmp(sh.magic, "AUSF", 4) == 0) {
            setPlaylistSettings(sh.fadeFrames, sh.delayFrames);
            logger_printf("[AUDIO]   AUSF: fadeFrames=%u delayFrames=%u\n",
                          (unsigned)sh.fadeFrames, (unsigned)sh.delayFrames);
        } else {
            fseek(fd, pos, SEEK_SET);
            logger_printf("[AUDIO]   no AUSF block — using defaults: "
                          "fade=%u delay=%u\n",
                          (unsigned)playlistFadeFrames,
                          (unsigned)playlistDelayFrames);
        }
    }

    logger_printf("[AUDIO] loadFromWorld done: %u tracks, %u emitters loaded\n",
                  (unsigned)trackCount, (unsigned)emitterCount);
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
            logger_printf("[AUDIO] crossfade complete: stopping ch0\n");
            arm7Stop(0);
            unloadChannel(0);
            fading = false;
        }
    } else if (playlistActive) {
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
    static const float kSampleRates[4] = { 32768.f, 16384.f, 8192.f, 5512.f };

    for (int ch = 0; ch < 2; ch++) {
        AudioChannel& c = channels[ch];
        if (!c.streaming || !c.streamBuf || !c.streamBuf->active) continue;
        if (c.streamEof && !c.looping) continue;

        float hz = kSampleRates[c.streamRateDiv < 4 ? c.streamRateDiv : 0];
        c.streamRefillAcc += hz / 60.0f;

        if (c.streamRefillAcc < (float)STREAM_BLOCK_SAMPLES)
            continue;

        c.streamRefillAcc -= (float)STREAM_BLOCK_SAMPLES;

        int nextBlock = 1 - (int)(c.streamBuf->arm9_block);
        _streamFillBlock(ch, nextBlock);

        u32 blockBytes;
        if (c.streamIsAdpcm)
            blockBytes = (u32)(STREAM_BLOCK_SAMPLES / 2);
        else if (c.streamIs16)
            blockBytes = (u32)(STREAM_BLOCK_SAMPLES * 2);
        else
            blockBytes = (u32)(STREAM_BLOCK_SAMPLES);

        DC_FlushRange(c.streamBuf->data + (u32)nextBlock * blockBytes, blockBytes);
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
    logger_printf("[AUDIO] startPlaylist: trackCount=%u musicVol=%u "
                  "fadeFrames=%u\n",
                  (unsigned)trackCount, (unsigned)musicVolume,
                  (unsigned)playlistFadeFrames);

    if (trackCount == 0) {
        logger_printf("[AUDIO] startPlaylist: ABORT — no tracks loaded\n");
        return;
    }

    playlistActive = true;
    currentTrack   = 0;

    channels[1].targetVol = (u8)((u32)tracks[0].baseVolume * musicVolume / 127);
    channels[1].looping   = tracks[0].loop;
    channels[1].volume    = (playlistFadeFrames == 0) ? channels[1].targetVol : 0;

    logger_printf("[AUDIO] startPlaylist: track0='%s' baseVol=%u "
                  "targetVol=%u startVol=%u loop=%d\n",
                  tracks[0].filename,
                  (unsigned)tracks[0].baseVolume,
                  (unsigned)channels[1].targetVol,
                  (unsigned)channels[1].volume,
                  (int)channels[1].looping);

    if (openDsndStream(1, tracks[0].filename)) {
        fadeStepA = 0;
        fadeStepB = 0;
        if (playlistFadeFrames == 0) {
            fading = false;
            arm7SetVolume(1, channels[1].volume);
            logger_printf("[AUDIO] startPlaylist: OK — no fade, playing immediately "
                          "at vol=%u\n", (unsigned)channels[1].volume);
        } else {
            fading = true;
            logger_printf("[AUDIO] startPlaylist: OK — fade-in over %u frames\n",
                          (unsigned)playlistFadeFrames);
        }
    } else {
        logger_printf("[AUDIO] startPlaylist: FAILED to open stream '%s'\n",
                      tracks[0].filename);
    }
}

void AudioSystem::stopPlaylist()
{
    logger_printf("[AUDIO] stopPlaylist\n");
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
    logger_printf("[AUDIO] nextTrack: -> track %u '%s'\n",
                  (unsigned)currentTrack, tracks[currentTrack].filename);

    unloadChannel(0);
    channels[0] = channels[1];
    if (channels[0].streaming && channels[0].streamBuf) {
        memcpy(&s_streamBufs[0], &s_streamBufs[1], sizeof(StreamBuffer));
        channels[0].streamBuf    = &s_streamBufs[0];
        channels[0].streamBufIdx = 0;
    }

    memset(&channels[1], 0, sizeof(AudioChannel));

    channels[1].looping   = tracks[currentTrack].loop;
    channels[1].targetVol =
        (u8)((u32)tracks[currentTrack].baseVolume * musicVolume / 127);
    channels[1].volume    = (playlistFadeFrames == 0) ? channels[1].targetVol : 0;

    if (openDsndStream(1, tracks[currentTrack].filename)) {
        fadeStepA = playlistFadeFrames;
        fadeStepB = 0;
        if (playlistFadeFrames == 0) {
            arm7SetVolume(1, channels[1].volume);
            fading = false;
        } else {
            fading = true;
        }
        playlistDelayCounter = 0;
        logger_printf("[AUDIO] nextTrack: stream OK vol=%u targetVol=%u fade=%u\n",
                      (unsigned)channels[1].volume,
                      (unsigned)channels[1].targetVol,
                      (unsigned)playlistFadeFrames);
    } else {
        logger_printf("[AUDIO] nextTrack: FAILED to open stream '%s'\n",
                      tracks[currentTrack].filename);
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
    playlistFadeFrames  = fadeFrames;
    playlistDelayFrames = delayFrames;
    playlistDelayCounter = 0;
}

// ---------------------------------------------------------------------------
// One-shot positional sound  (fully loaded, not streamed)
// ---------------------------------------------------------------------------
int AudioSystem::playEmitterOnce(const char* path,
                                  float x, float y, float z, u8 vol)
{
    float dist     = fDist(x, y, z, _camX, _camY, _camZ);
    float att      = attenuate(dist, 4.0f, 32.0f);
    u8 scaledVol   = (u8)((float)vol * att);
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
    logger_printf("[AUDIO] stopAllChannels\n");
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++)
        stopChannel(i);
    pxiSend(AUDIO_PXI_CHANNEL, AUDIO_PACK(AUDIO_CMD_STOP_ALL, 0, 0));
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
    if (!f) {
        logger_printf("[AUDIO] loadDsnd ch%d: FAILED fopen '%s'\n", ch, path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    u32 sz = (u32)ftell(f);
    rewind(f);
    if (sz < sizeof(DsndHeader)) {
        logger_printf("[AUDIO] loadDsnd ch%d: file too small (%lu bytes) '%s'\n",
                      ch, (unsigned long)sz, path);
        fclose(f); return false;
    }

    u8* buf = (u8*)malloc(sz);
    if (!buf) {
        logger_printf("[AUDIO] loadDsnd ch%d: malloc(%lu) FAILED\n",
                      ch, (unsigned long)sz);
        fclose(f); return false;
    }
    fread(buf, 1, sz, f);
    fclose(f);

    DsndHeader* hdr = (DsndHeader*)buf;
    if (hdr->magic != DSND_MAGIC) {
        logger_printf("[AUDIO] loadDsnd ch%d: bad magic 0x%08lX (expected 0x%08lX) '%s'\n",
                      ch, (unsigned long)hdr->magic,
                      (unsigned long)DSND_MAGIC, path);
        free(buf); return false;
    }

    channels[ch].data      = buf;
    channels[ch].dataBytes = sz;
    channels[ch].active    = false;
    channels[ch].streaming = false;
    strncpy(channels[ch].filename, path, AUDIO_MAX_PATH - 1);

    logger_printf("[AUDIO] loadDsnd ch%d: OK '%s' "
                  "size=%lu rateDiv=%u flags=0x%02X samples=%lu "
                  "dataAddr=0x%08lX\n",
                  ch, path, (unsigned long)sz,
                  (unsigned)hdr->rateDiv, (unsigned)hdr->flags,
                  (unsigned long)hdr->sampleCount,
                  (unsigned long)(u32)(buf + sizeof(DsndHeader)));
    return true;
}

// ---------------------------------------------------------------------------
// Private: streaming open (music channels)
// ---------------------------------------------------------------------------
bool AudioSystem::openDsndStream(int ch, const char* path)
{
    unloadChannel(ch);

    FILE* f = fopen(path, "rb");
    if (!f) {
        logger_printf("[AUDIO] openDsndStream ch%d: FAILED fopen '%s'\n", ch, path);
        return false;
    }

    DsndHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != DSND_MAGIC) {
        logger_printf("[AUDIO] openDsndStream ch%d: bad/missing DSND header '%s' "
                      "magic=0x%08lX\n",
                      ch, path, (unsigned long)hdr.magic);
        fclose(f); return false;
    }

    bool is16   = (hdr.flags & DSND_FLAG_16BIT)  != 0;
    bool isAdpc = (hdr.flags & DSND_FLAG_ADPCM)  != 0;
    if (isAdpc) is16 = false;

    long payloadStart = ftell(f);
    fseek(f, 0, SEEK_END);
    u32 totalPayload = (u32)(ftell(f) - payloadStart);
    fseek(f, payloadStart, SEEK_SET);

    logger_printf("[AUDIO] openDsndStream ch%d: '%s'\n"
                  "  rateDiv=%u flags=0x%02X is16=%d isAdpcm=%d\n"
                  "  samples=%lu payloadBytes=%lu payloadStart=%ld\n",
                  ch, path,
                  (unsigned)hdr.rateDiv, (unsigned)hdr.flags,
                  (int)is16, (int)isAdpc,
                  (unsigned long)hdr.sampleCount, (unsigned long)totalPayload,
                  payloadStart);

    int slot = ch & 1;
    StreamBuffer* buf = &s_streamBufs[slot];
    memset(buf, 0, sizeof(StreamBuffer));
    buf->active  = 1;
    buf->is16bit = is16   ? 1 : 0;
    buf->isAdpcm = isAdpc ? 1 : 0;

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
    c.streamRefillAcc   = (float)STREAM_BLOCK_SAMPLES;
    strncpy(c.filename, path, AUDIO_MAX_PATH - 1);

    _streamFillBlock(ch, 0);
    _streamFillBlock(ch, 1);

    u32 totalBufBytes = is16 ? (u32)(STREAM_BUF_BYTES_16) : (u32)(STREAM_BUF_BYTES);
    DC_FlushRange(buf->data, totalBufBytes);

    logger_printf("[AUDIO] openDsndStream ch%d: ringBufAddr=0x%08lX "
                  "ringBufBytes=%lu ch.volume=%u\n",
                  ch, (unsigned long)(u32)buf->data,
                  (unsigned long)totalBufBytes, (unsigned)c.volume);

    // Force LOOP flag — the ring buffer must wrap continuously.
    u32 sendFlags = (hdr.flags & ~(u8)DSND_FLAG_LOOP) | DSND_FLAG_LOOP;
    u32 sendSamples = is16
        ? (u32)(STREAM_BLOCK_SAMPLES * STREAM_BLOCKS)
        : (u32)(STREAM_BUF_BYTES);

    arm7PlayStream(ch, buf,
                   hdr.rateDiv,
                   (u8)sendFlags,
                   0,            // loopStart = ring start
                   sendSamples,
                   c.volume);

    c.active = true;

    logger_printf("[AUDIO] openDsndStream ch%d: arm7PlayStream issued — "
                  "rendezvous[%d] = { dataAddr=0x%08lX samples=%lu "
                  "rateDiv=%u flags=0x%02X vol=%u }\n",
                  ch, ch,
                  (unsigned long)arm9PlayInfo(ch).dataAddr,
                  (unsigned long)arm9PlayInfo(ch).sampleCount,
                  (unsigned)arm9PlayInfo(ch).rateDiv,
                  (unsigned)arm9PlayInfo(ch).flags,
                  (unsigned)arm9PlayInfo(ch).volume);

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

    u32 blockBytes = c.streamIs16
        ? (u32)(STREAM_BLOCK_SAMPLES * 2)
        : (u32)(STREAM_BLOCK_SAMPLES);
    if (c.streamIsAdpcm)
        blockBytes = STREAM_BLOCK_SAMPLES / 2;

    u8* dst       = c.streamBuf->data + (u32)block * blockBytes;
    u32 remaining = c.streamTotalBytes - c.streamFilePos;
    u32 toRead    = (blockBytes < remaining) ? blockBytes : remaining;

    if (toRead > 0) {
        fread(dst, 1, toRead, c.streamFd);
        c.streamFilePos += toRead;
    }

    if (toRead < blockBytes) {
        u8 silence = c.streamIs16 ? 0x00 : 0x80;
        memset(dst + toRead, silence, blockBytes - toRead);
        c.streamEof = true;

        if (c.looping) {
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
// Private: unload channel
// ---------------------------------------------------------------------------
void AudioSystem::unloadChannel(int ch)
{
    AudioChannel& c = channels[ch];

    if (c.streaming) {
        if (c.streamBuf) c.streamBuf->active = 0;
        if (c.streamFd)  { fclose(c.streamFd); c.streamFd = nullptr; }
        c.streamBuf    = nullptr;
        c.streamBufIdx = 0;
        c.streamFd     = nullptr;
        c.streaming    = false;
        c.streamEof    = false;
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

    arm9PlayInfo(ch).dataAddr    = (u32)data;
    arm9PlayInfo(ch).sampleCount = hdr->sampleCount;
    arm9PlayInfo(ch).loopStart   = hdr->loopStart;
    arm9PlayInfo(ch).rateDiv     = hdr->rateDiv;
    arm9PlayInfo(ch).flags       = hdr->flags;
    arm9PlayInfo(ch).volume      = vol;
    arm9PlayInfo(ch).channelId   = (u8)ch;

    logger_printf("[AUDIO] arm7Play ch%d: dataAddr=0x%08lX samples=%lu "
                  "rateDiv=%u flags=0x%02X vol=%u\n",
                  ch, (unsigned long)(u32)data,
                  (unsigned long)hdr->sampleCount,
                  (unsigned)hdr->rateDiv,
                  (unsigned)hdr->flags,
                  (unsigned)vol);

    channels[ch].active = true;
    sendFifoPlay(ch);
}

// ---------------------------------------------------------------------------
// Private: ARM7 IPC — streaming
// ---------------------------------------------------------------------------
void AudioSystem::arm7PlayStream(int ch, StreamBuffer* buf,
                                  u8 rateDiv, u8 flags,
                                  u16 loopStart, u32 sampleCount, u8 vol)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;

    arm9PlayInfo(ch).dataAddr    = (u32)buf->data;
    arm9PlayInfo(ch).sampleCount = sampleCount;
    arm9PlayInfo(ch).loopStart   = loopStart;
    arm9PlayInfo(ch).rateDiv     = rateDiv;
    arm9PlayInfo(ch).flags       = flags;
    arm9PlayInfo(ch).volume      = vol;
    arm9PlayInfo(ch).channelId   = (u8)ch;

    logger_printf("[AUDIO] arm7PlayStream ch%d: bufAddr=0x%08lX samples=%lu "
                  "rateDiv=%u flags=0x%02X loopStart=%u vol=%u\n",
                  ch, (unsigned long)(u32)buf->data,
                  (unsigned long)sampleCount,
                  (unsigned)rateDiv, (unsigned)flags,
                  (unsigned)loopStart, (unsigned)vol);

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
// Private: find a free channel
// ---------------------------------------------------------------------------
int AudioSystem::findFreeChannel(bool music)
{
    int start = music ? 0 : 2;
    for (int i = start; i < AUDIO_MAX_CHANNELS; i++)
        if (!channels[i].active && channels[i].data == nullptr
                && !channels[i].streaming)
            return i;
    logger_printf("[AUDIO] findFreeChannel(music=%d): NO FREE CHANNEL\n",
                  (int)music);
    return -1;
}

// ---------------------------------------------------------------------------
// Debug info getters
// ---------------------------------------------------------------------------
const char* AudioSystem::getCurrentTrackFilename() const
{
    if (currentTrack >= trackCount) return "";
    return tracks[currentTrack].filename;
}

u32 AudioSystem::getPlaybackPositionSamples() const
{
    if (channels[1].streaming && channels[1].streamBuf) {
        if (channels[1].streamIsAdpcm) {
            u32 b = channels[1].streamFilePos;
            if (b > 0) b -= 4;
            return b * 2;
        } else {
            u32 bps = channels[1].streamIs16 ? 2u : 1u;
            return channels[1].streamFilePos / bps;
        }
    }
    return 0;
}

u32 AudioSystem::getPlaybackPositionSeconds() const
{
    const u32 rateTable[] = { 32768, 16384, 8192, 5512 };
    if (currentTrack >= trackCount) return 0;
    if (channels[1].streaming) {
        u32 rate = rateTable[channels[1].streamRateDiv];
        return getPlaybackPositionSamples() / rate;
    }
    return 0;
}

void AudioSystem::getTrackFormatInfo(int& outRate, int& outBits,
                                      bool& outIsAdpcm) const
{
    outRate    = 0;
    outBits    = 0;
    outIsAdpcm = false;
    if (!channels[1].streaming) return;
    const u32 rateTable[] = { 32768, 16384, 8192, 5512 };
    outRate    = (int)rateTable[channels[1].streamRateDiv];
    outBits    = channels[1].streamIs16 ? 16 : 8;
    outIsAdpcm = channels[1].streamIsAdpcm;
}