#pragma once

// ---------------------------------------------------------------------------
// AudioSystem.h  —  Music playlist + 3-D positional audio for NDS
//
// Architecture
// ------------
//   ARM9 side  (this file + AudioSystem.cpp)
//     • Manages .dsnd sample data / stream buffers in EWRAM
//     • Pushes play/stop/volume commands to ARM7 via raw IPC FIFO
//     • Streams music from SD card using a double-buffer refilled per frame
//     • Updates positional emitter volumes once per frame
//
//   ARM7 side  (AudioArm7.cpp)
//     • Receives play/stop/volume commands via FIFO_USER_01
//     • Drives SCHANNEL hardware registers directly
//     • For streaming channels: plays the ring buffer in SOUND_REPEAT mode;
//       ARM9 keeps refilling the inactive half each frame
//
//   .dsnd format  (produced by the editor's DSify / convert pipeline)
//     Offset  Size  Field
//      0       4    magic        "DSND" (0x444E5344)
//      4       1    rateDiv      0=32768 Hz, 1=16384 Hz, 2=8192 Hz, 3=5512 Hz
//                                Use rateDiv=0 with 22050 Hz source for best fit
//                                (hardware plays at 32768; slight pitch shift is
//                                 inaudible on NDS speakers — or encode at 32768)
//      5       1    flags        DSND_FLAG_* bitmask
//      6       2    loopStart    sample index (0 for most tracks)
//      8       4    sampleCount  total PCM samples (or ADPCM nibbles for ADPCM)
//     12       …    payload      see below
//
//     PCM8  payload : signed 8-bit samples
//     PCM16 payload : signed 16-bit LE samples
//     ADPCM payload : 4-byte IMA preamble (s16 predictor, u8 stepIndex, u8 pad)
//                     followed by 4-bit nibble-packed IMA-ADPCM data
//                     The hardware expects exactly this layout; no software
//                     decompression is needed — SCHANNEL decodes it natively.
//
//   Streaming
//     Music tracks are streamed from SD using a 2 × STREAM_BLOCK_SAMPLES
//     ring buffer per channel.  Total streaming RAM = 4 KB for two channels
//     (vs potentially several MB for a full pre-load).
//     SFX / emitters are still fully loaded into EWRAM (they are short).
//
//   IMA-ADPCM
//     NDS hardware (SCHANNEL_CR bits 29-30 = 0b10) decodes IMA-ADPCM
//     natively.  4:1 compression vs PCM16 — ideal for music.
//     22050 Hz IMA-ADPCM fits in ~1.35 KB/s vs ~44 KB/s for PCM16 22050 Hz.
//     Encode with: ffmpeg -i in.wav -ar 22050 -ac 1 -c:a adpcm_ima_wav out.wav
//     then convert the WAV IMA chunk to .dsnd with the editor's pipeline.
// ---------------------------------------------------------------------------

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// IPC command packet  (fits in one 32-bit FIFO word)
// ---------------------------------------------------------------------------
#define AUDIO_CMD_PLAY      0x01
#define AUDIO_CMD_STOP      0x02
#define AUDIO_CMD_VOL       0x03
#define AUDIO_CMD_STOP_ALL  0x04

#define AUDIO_PACK(cmd, ch, vol) \
    (u32)(((cmd)<<24) | ((ch)<<16) | ((vol)&0x7F))

// ---------------------------------------------------------------------------
// DSND header
// ---------------------------------------------------------------------------
#define DSND_MAGIC          0x444E5344u  // "DSND"
#define DSND_FLAG_STEREO    (1<<0)       // interleaved stereo (rare on NDS)
#define DSND_FLAG_LOOP      (1<<1)       // loop back to loopStart on end
#define DSND_FLAG_16BIT     (1<<2)       // PCM16 (else PCM8 or ADPCM)
#define DSND_FLAG_ADPCM     (1<<3)       // IMA-ADPCM (overrides 16BIT)

// NDS SCHANNEL_CR format bits (bits 29-30)
#define SOUND_FORMAT_ADPCM  (2 << 29)   // may not be in older libnds headers

#pragma pack(push,1)
struct DsndHeader {
    u32 magic;          // DSND_MAGIC
    u8  rateDiv;        // 0=32768 Hz, 1=16384 Hz, 2=8192 Hz, 3=5512 Hz
    u8  flags;          // DSND_FLAG_*
    u16 loopStart;      // sample index for loop point (0 = start)
    u32 sampleCount;    // total samples (nibbles for ADPCM)
    // raw payload follows (PCM8 / PCM16 / IMA-ADPCM)
};
#pragma pack(pop)

// IMA-ADPCM data starts with a 4-byte preamble per NDS hardware spec
#pragma pack(push,1)
struct AdpcmPreamble {
    s16 initialPredictor;   // initial sample value
    u8  initialStepIndex;   // initial step-table index (0-88)
    u8  pad;                // must be 0
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define AUDIO_MAX_CHANNELS      16
#define AUDIO_MAX_EMITTERS      32
#define AUDIO_MAX_TRACKS        16
#define AUDIO_MAX_PATH          48
#define AUDIO_FADE_STEPS        64      // frames for a full crossfade (~1 s @ 60fps)
#define AUDIO_MUSIC_CHANNEL_A   0       // outgoing track during crossfade
#define AUDIO_MUSIC_CHANNEL_B   1       // incoming track

// Streaming double-buffer sizing.
// STREAM_BLOCK_SAMPLES is the number of *decoded* samples per half.
// For ADPCM each byte holds 2 nibbles, so file bytes read = STREAM_BLOCK_SAMPLES/2
// plus the 4-byte preamble on the very first fill.
// At 22050 Hz, 1024 samples = ~46 ms — well within a 60fps frame budget.
#define STREAM_BLOCK_SAMPLES    1024
#define STREAM_BLOCKS           2
#define STREAM_BUF_BYTES        (STREAM_BLOCK_SAMPLES * STREAM_BLOCKS)
// For PCM16 streaming each sample is 2 bytes:
#define STREAM_BUF_BYTES_16     (STREAM_BLOCK_SAMPLES * STREAM_BLOCKS * 2)

// ---------------------------------------------------------------------------
// Streaming ring buffer  (lives in EWRAM so ARM7 can DMA-read it)
// ---------------------------------------------------------------------------
struct alignas(4) StreamBuffer {
    // Raw bytes the ARM7 plays from.  For PCM8/ADPCM this is byte-per-sample;
    // for PCM16 it is 2 bytes per sample.  We allocate the PCM16-sized buffer
    // and use only the first half for PCM8/ADPCM.
    u8   data[STREAM_BUF_BYTES_16];

    volatile u8  arm9_block;    // 0 or 1 — last block ARM9 finished filling
    volatile u8  active;        // 1 = streaming, 0 = stopped/done
    u8           is16bit;       // 1 = data[] holds s16 samples
    u8           isAdpcm;       // 1 = ADPCM (block size in bytes = block/2)
};

// ---------------------------------------------------------------------------
// Runtime types
// ---------------------------------------------------------------------------
struct AudioChannel {
    bool    active;
    bool    looping;
    bool    streaming;
    u8      volume;
    u8      targetVol;
    char    filename[AUDIO_MAX_PATH];
    // Non-streaming: malloced DSND data
    u8*     data;
    u32     dataBytes;
    // Streaming fields (valid when streaming == true)
    FILE*         streamFd;
    u32           streamDataOffset; // file offset of first payload byte
    u32           streamTotalBytes; // total payload bytes in file
    u32           streamFilePos;    // current read position (bytes from start of payload)
    StreamBuffer* streamBuf;
    int           streamBufIdx;
    bool          streamIs16;
    bool          streamIsAdpcm;
    bool          streamEof;        // true after last byte read (looping will seek back)
    u8            streamRateDiv;
    u8            streamFlags;
    u16           streamLoopStart;
    u32           streamSampleCount;
};

struct RuntimeEmitter {
    bool    active;
    float   x, y, z;
    float   innerRadius;
    float   outerRadius;
    u8      baseVolume;
    bool    loop;
    char    filename[AUDIO_MAX_PATH];
    int     channelId;
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

    // Call once at startup (after fatInitDefault)
    void init();

    // Load audio sections from an open .world v2 file.
    // fd must be positioned right after the AudioTableHeader.
    void loadFromWorld(FILE* fd, u8 trackCount, u8 emitterCount);

    // Per-frame update: advance crossfade, refill stream buffers, update emitters.
    void update(float camX, float camY, float camZ);

    // ---- Music ----
    void startPlaylist();
    void stopPlaylist();
    void nextTrack();
    void prevTrack();
    void setMusicVolume(u8 vol);    // 0-127

    // ---- One-shot positional sound (fully loaded, not streamed) ----
    int  playEmitterOnce(const char* path, float x, float y, float z, u8 vol = 100);
    void stopChannel(int channelId);
    void stopAllChannels();

    bool isChannelActive(int ch) const;

private:
    AudioChannel   channels[AUDIO_MAX_CHANNELS];
    RuntimeEmitter emitters[AUDIO_MAX_EMITTERS];
    MusicTrack     tracks[AUDIO_MAX_TRACKS];

    u8   trackCount;
    u8   emitterCount;
    u8   currentTrack;
    u8   nextTrackIdx;

    u8   fadeStepA;
    u8   fadeStepB;
    bool fading;
    bool playlistActive;
    u8   musicVolume;

    float _camX, _camY, _camZ;

    // ---- Non-streaming (SFX / emitters) ----
    bool loadDsnd(int ch, const char* path);

    // ---- Streaming (music channels 0-1) ----
    bool openDsndStream(int ch, const char* path);
    void _streamFillBlock(int ch, int block);

    void unloadChannel(int ch);

    // ARM7 IPC helpers
    void arm7Play(int ch, const DsndHeader* hdr, u8* data, u32 bytes, u8 vol);
    void arm7PlayStream(int ch, StreamBuffer* buf, u8 rateDiv, u8 flags,
                        u16 loopStart, u32 sampleCount, u8 vol);
    void arm7SetVolume(int ch, u8 vol);
    void arm7Stop(int ch);

    int  findFreeChannel(bool music = false);

    static float attenuate(float dist, float inner, float outer) {
        if (dist <= inner) return 1.0f;
        if (dist >= outer) return 0.0f;
        return (outer - dist) / (outer - inner);
    }
    static float fDist(float ax, float ay, float az,
                       float bx, float by, float bz) {
        float dx = ax-bx, dy = ay-by, dz = az-bz;
        return sqrtf(dx*dx + dy*dy + dz*dz);
    }
};

extern AudioSystem g_audio;