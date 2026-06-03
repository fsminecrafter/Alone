// ---------------------------------------------------------------------------
// AudioArm7.cpp  —  ARM7 audio back-end  (calico build)
// Compiled ONLY for the ARM7 sub-project (Makefile.arm7, -DARM7).
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

#define DSND_FLAG_LOOP   (1<<1)
#define DSND_FLAG_16BIT  (1<<2)

static const u32 kRateHz[4] = { 32768, 16384, 8192, 5512 };

static void arm7StopChannel(int ch)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    SCHANNEL_CR(ch) = 0;
}

static void arm7StartChannel(int ch, const Arm7PlayInfo* info)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    arm7StopChannel(ch);

    u32  hz   = kRateHz[info->rateDiv < 4 ? info->rateDiv : 0];
    bool is16 = (info->flags & DSND_FLAG_16BIT) != 0;
    bool loop = (info->flags & DSND_FLAG_LOOP)  != 0;

    u32 lengthWords = is16 ? (info->sampleCount + 1) / 2
                           : (info->sampleCount + 3) / 4;

    SCHANNEL_SOURCE(ch)       = info->dataAddr;
    SCHANNEL_TIMER(ch)        = SOUND_FREQ(hz);
    SCHANNEL_REPEAT_POINT(ch) = info->loopStart;
    SCHANNEL_LENGTH(ch)       = lengthWords;

    u32 cr = SCHANNEL_ENABLE
           | SOUND_VOL(info->volume)
           | SOUND_PAN(64)
           | (is16  ? SOUND_FORMAT_16BIT : SOUND_FORMAT_8BIT)
           | (loop  ? SOUND_REPEAT       : SOUND_ONE_SHOT);

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
// ---------------------------------------------------------------------------
int main()
{
    // ── Mandatory calico startup sequence (from the combined template) ──
    envReadNvramSettings();
    keypadStartExtServer();

    lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
    irqEnable(IRQ_VBLANK);

    rtcInit();
    rtcSyncTime();
    pmInit();
    blkInit();
    touchInit();
    touchStartServer(80, MAIN_THREAD_PRIO);

    // ── Our audio init — register the raw-FIFO handler ──
    arm7AudioInit();

    // ── Idle loop (calico style) ──
    while (pmMainLoop()) {
        threadWaitForVBlank();
    }

    return 0;
}

#endif  // ARM7