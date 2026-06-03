#ifdef ARM7

#include <calico.h>
#include <nds.h>
#include "ipc_fifo.h"

#define AUDIO_CMD_PLAY      0x01
#define AUDIO_CMD_STOP      0x02
#define AUDIO_CMD_VOL       0x03
#define AUDIO_CMD_STOP_ALL  0x04
#define AUDIO_CMD_PING      0x10
#define AUDIO_CMD_PING_ACK  0x11
#define AUDIO_CMD_DEBUG     0x20
#define AUDIO_MAX_CHANNELS  16

#define DSND_FLAG_STEREO    (1<<0)
#define DSND_FLAG_LOOP      (1<<1)
#define DSND_FLAG_16BIT     (1<<2)
#define DSND_FLAG_ADPCM     (1<<3)

#define SOUND_FORMAT_ADPCM_NDS  (2 << 29)

// Use PxiChannel_User0 for audio commands
#define AUDIO_PXI_CHANNEL   PxiChannel_User0

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

    u32  hz      = kRateHz[info->rateDiv < 4 ? info->rateDiv : 0];
    bool isAdpcm = (info->flags & DSND_FLAG_ADPCM) != 0;
    bool is16    = (!isAdpcm) && ((info->flags & DSND_FLAG_16BIT) != 0);
    bool loop    = (info->flags & DSND_FLAG_LOOP) != 0;

    u32 lengthWords;
    if (isAdpcm) {
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

    u32 fmtBits;
    if (isAdpcm)   fmtBits = SOUND_FORMAT_ADPCM_NDS;
    else if (is16) fmtBits = SOUND_FORMAT_16BIT;
    else           fmtBits = SOUND_FORMAT_8BIT;

    u32 cr = SCHANNEL_ENABLE
           | SOUND_VOL(info->volume)
           | SOUND_PAN(64)
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
// PXI handler — calico calls this from IRQ context when ARM9 sends a packet
// The 26-bit immediate from pxiPacketGetImmediate() contains our command word.
// For PLAY we need a second word (the Arm7PlayInfo pointer) sent as extended.
// ---------------------------------------------------------------------------
static bool s_awaitingPlayInfo = false;
static int  s_pendingChannel   = 0;

static void audioPxiHandler(void* /*user*/, u32 packet)
{
    // Extract our 26-bit payload — upper bits are cmd/ch/vol packed the same
    // way as before: cmd=bits25-18, ch=bits17-14, vol=bits6-0
    u32 value = pxiPacketGetImmediate(packet);

    if (s_awaitingPlayInfo) {
        s_awaitingPlayInfo = false;
        // value IS the pointer (sent as the immediate of a second packet)
        arm7StartChannel(s_pendingChannel, (const Arm7PlayInfo*)value);
        return;
    }

    u8  cmd = (u8)((value >> 18) & 0xFF);
    int ch  = (int)((value >> 14) & 0x0F);
    u8  vol = (u8)(value & 0x7F);

    switch (cmd) {
        case AUDIO_CMD_PING:
            pxiReply(AUDIO_PXI_CHANNEL, AUDIO_CMD_PING_ACK);
            break;
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

int main()
{
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

    REG_SOUNDCNT = SOUND_ENABLE | SOUND_VOL(127);

    // Register our handler on calico's PXI channel
    pxiSetHandler(AUDIO_PXI_CHANNEL, audioPxiHandler, nullptr);

    // Signal ARM9 we are ready
    pxiSend(AUDIO_PXI_CHANNEL, (AUDIO_CMD_DEBUG << 18) | 0x0099);

    while (pmMainLoop()) {
        threadWaitForVBlank();
    }

    return 0;
}

#endif // ARM7