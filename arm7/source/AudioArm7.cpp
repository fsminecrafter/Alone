// ---------------------------------------------------------------------------
// AudioArm7.cpp  —  ARM7 audio back-end  (calico build)
// Compiled ONLY for the ARM7 sub-project (Makefile.arm7, -DARM7).
//
// Supports:
//   • PCM8  (signed 8-bit)
//   • PCM16 (signed 16-bit)
//   • IMA-ADPCM (4-bit, decoded by NDS hardware — SOUND_FORMAT_ADPCM)
//
// For streaming channels the ARM9 keeps a ring buffer in EWRAM filled from SD.
// The ARM7 plays it in SOUND_REPEAT mode; loopStart=0 wraps at buffer start.
// No change needed here for streaming — the ARM7 just sees a looping buffer.
// ---------------------------------------------------------------------------

#ifdef ARM7

#include <calico.h>
#include <nds.h>
#include "ipc_fifo.h"
#include <string.h>

// ---- shared constants (mirrored from AudioSystem.h) ----
#define AUDIO_CMD_PLAY      0x01
#define AUDIO_CMD_STOP      0x02
#define AUDIO_CMD_VOL       0x03
#define AUDIO_CMD_STOP_ALL  0x04
#define AUDIO_MAX_CHANNELS  16

// DSND flags (must match AudioSystem.h)
#define DSND_FLAG_STEREO    (1<<0)
#define DSND_FLAG_LOOP      (1<<1)
#define DSND_FLAG_16BIT     (1<<2)
#define DSND_FLAG_ADPCM     (1<<3)

// NDS hardware SCHANNEL format bits (SCHANNEL_CR bits 29-30)
// These may already be defined in nds/arm7/audio.h; guard against redefinition.
#ifndef SOUND_FORMAT_PSG
#define SOUND_FORMAT_PSG    (3 << 29)
#endif
// ADPCM = 0b10 in bits 29-30
#define SOUND_FORMAT_ADPCM_NDS  (2 << 29)

struct Arm7PlayInfo {
    u32  dataAddr;
    u32  sampleCount;
    u16  loopStart;
    u8   rateDiv;
    u8   flags;     // DSND_FLAG_*
    u8   volume;
    u8   channelId;
    u16  pad;
};

static const u32 kRateHz[4] = { 32768, 16384, 8192, 5512 };

// ---------------------------------------------------------------------------
// Channel helpers
// ---------------------------------------------------------------------------
static void arm7StopChannel(int ch)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    SCHANNEL_CR(ch) = 0;
}

static void arm7StartChannel(int ch, const Arm7PlayInfo* info)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    arm7StopChannel(ch);

    u32  hz      = kRateHz[info->rateDiv < 4 ? info->rateDiv : 0];
    bool isAdpcm = (info->flags & DSND_FLAG_ADPCM) != 0;
    bool is16    = (!isAdpcm) && ((info->flags & DSND_FLAG_16BIT) != 0);
    bool loop    = (info->flags & DSND_FLAG_LOOP) != 0;

    // For ADPCM the hardware counts nibbles as the sample unit, but
    // SCHANNEL_LENGTH still takes 32-bit words covering the data.
    // sampleCount in ADPCM = number of nibbles; bytes = sampleCount/2 (+4 preamble).
    u32 lengthWords;
    if (isAdpcm) {
        // Byte count = 4 (preamble) + nibbles/2; round up to 32-bit words
        u32 dataBytes = 4 + (info->sampleCount + 1) / 2;
        lengthWords   = (dataBytes + 3) / 4;
    } else if (is16) {
        lengthWords = (info->sampleCount + 1) / 2;
    } else {
        lengthWords = (info->sampleCount + 3) / 4;
    }

    SCHANNEL_SOURCE(ch)       = info->dataAddr;
    SCHANNEL_TIMER(ch)        = SOUND_FREQ(hz);
    SCHANNEL_REPEAT_POINT(ch) = info->loopStart;
    SCHANNEL_LENGTH(ch)       = lengthWords;

    // Build CR: format bits determine decoder
    u32 fmtBits;
    if (isAdpcm)      fmtBits = SOUND_FORMAT_ADPCM_NDS;
    else if (is16)    fmtBits = SOUND_FORMAT_16BIT;
    else              fmtBits = SOUND_FORMAT_8BIT;

    u32 cr = SCHANNEL_ENABLE
           | SOUND_VOL(info->volume)
           | SOUND_PAN(64)          // centre
           | fmtBits
           | (loop ? SOUND_REPEAT : SOUND_ONE_SHOT);

    SCHANNEL_CR(ch) = cr;
}

static void arm7SetVolume(int ch, u8 vol)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    u32 cr = SCHANNEL_CR(ch);
    cr &= ~0x7Fu;
    cr |= SOUND_VOL(vol);
    SCHANNEL_CR(ch) = cr;
}

// ---------------------------------------------------------------------------
// FIFO handler
// ---------------------------------------------------------------------------
static bool s_awaitingPlayInfo = false;
static int  s_pendingChannel   = 0;

static void audioFifoHandler(u32 value, void* /*userdata*/)
{
    if (s_awaitingPlayInfo) {
        s_awaitingPlayInfo = false;
        arm7StartChannel(s_pendingChannel, (const Arm7PlayInfo*)value);
        return;
    }

    u8  cmd = (u8)((value >> 24) & 0xFF);
    int ch  = (int)((value >> 16) & 0x0F);
    u8  vol = (u8)(value & 0x7F);

    switch (cmd) {
        case AUDIO_CMD_PLAY:
            s_pendingChannel   = ch;
            s_awaitingPlayInfo = true;
            break;
        case AUDIO_CMD_STOP:
            arm7StopChannel(ch);
            break;
        case AUDIO_CMD_VOL:
            arm7SetVolume(ch, vol);
            break;
        case AUDIO_CMD_STOP_ALL:
            for (int i = 0; i < AUDIO_MAX_CHANNELS; i++)
                arm7StopChannel(i);
            break;
        default:
            break;
    }
}

static void arm7AudioInit()
{
    REG_SOUNDCNT = SOUND_ENABLE | SOUND_VOL(127);
    fifoSetValue32Handler(FIFO_USER_01, audioFifoHandler, nullptr);
}

// ---------------------------------------------------------------------------
// ARM7 main  —  calico build
//
// The full calico subsystem startup is required.  Skipping any of these calls
// (especially pmInit / pmMainLoop) causes the ARM7 to exit its idle loop and
// halt, which kills all IPC including our audio FIFO handler.
//
// soundStartServer() is intentionally NOT called — we write SCHANNEL registers
// directly.  Calling it alongside direct register writes causes conflicts.
// ---------------------------------------------------------------------------
int main()
{
    // Read NVRAM settings (firmware language, username, etc.)
    envReadNvramSettings();

    // Extended keypad server (X, Y, hinge buttons via SPI)
    keypadStartExtServer();

    // VBlank IRQ — required by calico's scheduler
    lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
    irqEnable(IRQ_VBLANK);

    // Real-time clock
    rtcInit();
    rtcSyncTime();

    // Power management — pmMainLoop() returns false when the system shuts down
    pmInit();

    // Block device (SD / slot-2) — needed even if we don't use it on ARM7
    blkInit();

    // Touch screen
    touchInit();
    touchStartServer(80, MAIN_THREAD_PRIO);

    // Our audio FIFO handler
    arm7AudioInit();

    // Calico idle loop — keeps the ARM7 alive and services all IRQs/threads
    while (pmMainLoop()) {
        threadWaitForVBlank();
    }

    return 0;
}

#endif  // ARM7