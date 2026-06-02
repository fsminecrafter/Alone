// ---------------------------------------------------------------------------
// AudioArm7.cpp  —  ARM7 audio back-end
//
// This file is compiled ONLY for the ARM7 sub-project (Makefile.arm7).
// It must never appear in the ARM9 source list.
//
// Responsibilities
// ----------------
//   • Install a FIFO_USER_01 handler during ARM7 startup.
//   • Receive AUDIO_CMD_PLAY / STOP / VOL / STOP_ALL messages from the ARM9.
//   • Drive the NDS hardware sound channels directly via mmio registers
//     (SCHANNEL_* macros from libnds).
//
// Hardware notes
// --------------
//   The NDS has 16 hardware sound channels (0-15).  Each is a DMA engine that
//   reads PCM or ADPCM samples from RAM and feeds a DAC.
//
//   Key registers per channel n (base = 0x04000400 + n*0x10):
//     SCHANNEL_CR(n)       — control: format, loop, start/stop, volume, panning
//     SCHANNEL_SOURCE(n)   — 32-bit ARM7-bus data pointer (must be ARM7-visible)
//     SCHANNEL_TIMER(n)    — 16-bit sample rate timer (0x10000 - (33513982/rate))
//     SCHANNEL_REPEAT_POINT(n) — loop start in samples
//     SCHANNEL_LENGTH(n)   — total sample count in words
//
//   SCHANNEL_CR fields (libnds constants):
//     SCHANNEL_ENABLE      — bit 31: start/stop
//     SOUND_FORMAT_8BIT    — bits [30:29] = 00  (PCM8, signed)
//     SOUND_FORMAT_16BIT   — bits [30:29] = 01  (PCM16, signed little-endian)
//     SOUND_ONE_SHOT       — bit 27 = 0 (play once)
//     SOUND_REPEAT         — bit 27 = 1 (loop at loopStart)
//     SOUND_VOL(v)         — bits [6:0]  (0-127)
//     SOUND_PAN(p)         — bits [15:8] (0=left, 64=center, 127=right)
//
//   Timer value for sample rate f:
//     timer = 0x10000 - (BUS_CLOCK / f)
//     BUS_CLOCK = 33513982 Hz  (NDS ARM7 bus)
//
//   libnds provides SOUND_FREQ(f) which computes this for you.
//
// Rate-divider encoding (matches .dsnd rateDiv field):
//   0 → 32768 Hz   1 → 16384 Hz   2 → 8192 Hz   3 → 5512 Hz
// ---------------------------------------------------------------------------

#ifdef ARM7

#include <nds.h>
#include <nds/arm7/audio.h>
#include <string.h>

// ---- shared constants with ARM9 (duplicated to avoid including AudioSystem.h) ----
// AudioSystem.h pulls in <functional> and other ARM9-only headers, so we just
// restate the few constants we need here.
#define AUDIO_CMD_PLAY      0x01
#define AUDIO_CMD_STOP      0x02
#define AUDIO_CMD_VOL       0x03
#define AUDIO_CMD_STOP_ALL  0x04

#define AUDIO_MAX_CHANNELS  16

// Shared staging structs written by ARM9, read by ARM7.
// Must match the layout in AudioSystem.cpp exactly.
struct Arm7PlayInfo {
    u32  dataAddr;      // ARM7-visible address of raw PCM data (past DsndHeader)
    u32  sampleCount;
    u16  loopStart;
    u8   rateDiv;
    u8   flags;         // DSND_FLAG_LOOP(1), DSND_FLAG_16BIT(4)
    u8   volume;        // 0-127
    u8   channelId;
    u16  pad;
};

#define DSND_FLAG_LOOP   (1<<1)
#define DSND_FLAG_16BIT  (1<<2)

// Rate-divider → Hz lookup
static const u32 kRateHz[4] = { 32768, 16384, 8192, 5512 };

// ---------------------------------------------------------------------------
// stopChannel  — silence one hardware channel
// ---------------------------------------------------------------------------
static void arm7StopChannel(int ch)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    SCHANNEL_CR(ch) = 0;   // clear SCHANNEL_ENABLE and all other bits
}

// ---------------------------------------------------------------------------
// playChannel  — start one hardware channel from an Arm7PlayInfo
// ---------------------------------------------------------------------------
static void arm7StartChannel(int ch, const Arm7PlayInfo* info)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;

    // Stop anything already running on this channel
    arm7StopChannel(ch);

    u32 hz  = kRateHz[info->rateDiv < 4 ? info->rateDiv : 0];
    bool is16 = (info->flags & DSND_FLAG_16BIT) != 0;
    bool loop = (info->flags & DSND_FLAG_LOOP)  != 0;

    // The NDS sound DMA requires the source address to be word-aligned
    // for 16-bit PCM, and the length expressed in 32-bit words.
    u32 srcAddr = info->dataAddr;
    u32 lengthWords;
    if (is16) {
        // PCM16: each sample is 2 bytes; length register wants 32-bit word count
        // (i.e. sampleCount / 2, rounded up)
        lengthWords = (info->sampleCount + 1) / 2;
    } else {
        // PCM8: each sample is 1 byte; length register wants 32-bit word count
        // (i.e. sampleCount / 4, rounded up)
        lengthWords = (info->sampleCount + 3) / 4;
    }

    SCHANNEL_SOURCE(ch)       = srcAddr;
    SCHANNEL_TIMER(ch)        = SOUND_FREQ(hz);
    SCHANNEL_REPEAT_POINT(ch) = info->loopStart;
    SCHANNEL_LENGTH(ch)       = lengthWords;

    u32 cr = SCHANNEL_ENABLE
           | SOUND_VOL(info->volume)
           | SOUND_PAN(64)                // centre pan; ARM9 can send VOL commands to adjust
           | (is16 ? SOUND_FORMAT_16BIT : SOUND_FORMAT_8BIT)
           | (loop ? SOUND_REPEAT : SOUND_ONE_SHOT);

    SCHANNEL_CR(ch) = cr;
}

// ---------------------------------------------------------------------------
// setChannelVolume  — adjust volume without restarting playback
// ---------------------------------------------------------------------------
static void arm7SetVolume(int ch, u8 vol)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    // Preserve all bits except SOUND_VOL field (bits 6:0)
    u32 cr = SCHANNEL_CR(ch);
    cr &= ~0x7Fu;           // clear old volume
    cr |= SOUND_VOL(vol);
    SCHANNEL_CR(ch) = cr;
}

// ---------------------------------------------------------------------------
// FIFO handler  — called by libnds whenever the ARM9 sends a word on FIFO_USER_01
// ---------------------------------------------------------------------------
// The ARM9 sends one or two 32-bit words per command:
//   Word 0:  bits[31:24]=cmd  bits[23:16]=channel  bits[7:0]=volume
//   Word 1:  (PLAY only) pointer to Arm7PlayInfo in shared RAM
//
// Because fifo callbacks are called synchronously from the IRQ handler we must
// keep this routine short and not touch the SD card or fat filesystem.
// ---------------------------------------------------------------------------

// We need to buffer the first word and wait for a possible second word, so we
// use a tiny two-word ring.  In practice PLAY is always two words and the FIFO
// interrupt fires per word, so we just track "expecting second word" state.
static bool    s_awaitingPlayInfo = false;
static int     s_pendingChannel   = 0;

static void fifoHandler(u32 value, void* /*userdata*/)
{
    if (s_awaitingPlayInfo) {
        // This is the second word of a PLAY command — a pointer to Arm7PlayInfo
        s_awaitingPlayInfo = false;
        const Arm7PlayInfo* info = (const Arm7PlayInfo*)value;
        // Invalidate D-cache line so we read what ARM9 actually wrote.
        // On NDS the ARM7 has no cache, so this is a no-op, but it documents intent.
        arm7StartChannel(s_pendingChannel, info);
        return;
    }

    u8  cmd = (u8)((value >> 24) & 0xFF);
    int ch  = (int)((value >> 16) & 0x0F);
    u8  vol = (u8)(value & 0x7F);

    switch (cmd) {
        case AUDIO_CMD_PLAY:
            // Remember which channel; next word will be the info pointer
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
            break;  // unknown command — ignore
    }
}

// ---------------------------------------------------------------------------
// arm7AudioInit  — call once from the ARM7 main() before entering the loop
// ---------------------------------------------------------------------------
void arm7AudioInit()
{
    // Enable the hardware sound mixer
    REG_SOUNDCNT = SOUND_ENABLE | SOUND_VOL(127);

    // Register our FIFO handler on FIFO_USER_01
    fifoSetValue32Handler(FIFO_USER_01, fifoHandler, nullptr);
}

// ---------------------------------------------------------------------------
// ARM7 main
//
// The NDS requires a custom ARM7 binary whenever you want to handle
// FIFO_USER_01 yourself.  This is the minimal ARM7 main() that:
//   1. Initialises the system (power, sound, touchscreen, IPC defaults)
//   2. Installs our FIFO handler
//   3. Loops forever processing IRQs (as recommended by libnds)
//
// libnds provides defaultARM7() as a convenience function that sets up
// everything a typical homebrew game needs.  We call it for everything
// *except* the FIFO_USER_01 handler, which we override after.
// ---------------------------------------------------------------------------
int main()
{
    // Full libnds ARM7 init: power manager, sound hardware, touch, wifi stubs
    defaultARM7();

    // Override the user FIFO channel with our audio handler
    arm7AudioInit();

    // Spin in the IRQ sleep loop — all work is done in fifoHandler()
    while (1) {
        swiIntrWait(1, IRQ_ALL);
    }

    return 0;  // unreachable
}

#endif  // ARM7
