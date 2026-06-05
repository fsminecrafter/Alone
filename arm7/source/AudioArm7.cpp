#ifdef ARM7

#include <calico.h>
#include <nds.h>
// AudioSystem.h lives in common/include — ARM7 makefile must add that to -I.
// It provides: Arm7PlayInfo, AUDIO_PLAY_INFO_ARRAY(), AUDIO_SHARED_EWRAM_ADDR,
//              AUDIO_CMD_*, AUDIO_PACK, AUDIO_MAX_CHANNELS, DSND_FLAG_*
#include "AudioSystem.h"

// SOUND_FORMAT_ADPCM_NDS may not appear in older libnds headers
#ifndef SOUND_FORMAT_ADPCM_NDS
#define SOUND_FORMAT_ADPCM_NDS  (2 << 29)
#endif

#define AUDIO_PXI_CHANNEL  PxiChannel_User0
#define AUDIO_CMD_DEBUG    0x20

// ---------------------------------------------------------------------------
// ARM7-side debug: write to the IPC SEND FIFO so ARM9 can read it back,
// OR emit via a simple memory log if ARM9 is not listening.
// For now we use REG_IPC_FIFO_TX as a last-resort: one u32 "debug beacon"
// sent on PxiChannel_User1 so it never collides with audio traffic.
// Most useful thing: use the NDS debug register (mapped to no$gba / melonDS).
// ---------------------------------------------------------------------------
#define ARM7_DBG_CHANNEL  PxiChannel_User1

// no$gba / melonDS debug output register — writes here appear in the
// emulator's debug console immediately, even before FAT / logger is up.
#define REG_NOCASH_DBG   (*(volatile char*)0x04FFFA00)

static void arm7DbgStr(const char* s)
{
    // Write each character to the no$gba string register.
    // melonDS also honours this.
    volatile char* p = (volatile char*)0x04FFFA00;
    while (*s) *p = *s++;
    *p = '\n';
}

static void arm7DbgHex(const char* label, u32 val)
{
    // Build "label=0xXXXXXXXX\n" in a tiny buffer and send to no$gba.
    char buf[64];
    const char hex[] = "0123456789ABCDEF";
    int i = 0;
    while (label[i] && i < 40) { buf[i] = label[i]; i++; }
    buf[i++] = '='; buf[i++] = '0'; buf[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4) buf[i++] = hex[(val >> s) & 0xF];
    buf[i] = '\0';
    arm7DbgStr(buf);
}

// ---------------------------------------------------------------------------
// Sound channel helpers
// ---------------------------------------------------------------------------
static const u32 kRateHz[4] = { 32768, 16384, 8192, 5512 };

static void arm7StopChannel(int ch)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;
    SCHANNEL_CR(ch) = 0;
}

static void arm7StartChannel(int ch, const Arm7PlayInfo* info)
{
    if (ch < 0 || ch >= AUDIO_MAX_CHANNELS) return;

    arm7DbgStr("arm7StartChannel");
    arm7DbgHex("  ch",       (u32)ch);
    arm7DbgHex("  dataAddr", info->dataAddr);
    arm7DbgHex("  samples",  info->sampleCount);
    arm7DbgHex("  rateDiv",  info->rateDiv);
    arm7DbgHex("  flags",    info->flags);
    arm7DbgHex("  volume",   info->volume);

    // Validate dataAddr is in main RAM (EWRAM 0x02000000-0x023FFFFF or
    // EWRAM mirror 0x02400000+ on DSi).  If it's outside, the sample data
    // never arrived — most likely the rendezvous address is wrong.
    if (info->dataAddr < 0x02000000u || info->dataAddr > 0x02FFFFFFu) {
        arm7DbgStr("  ERROR: dataAddr out of EWRAM range — CHECK RENDEZVOUS");
        return;
    }

    if (info->sampleCount == 0) {
        arm7DbgStr("  ERROR: sampleCount=0 — bad Arm7PlayInfo");
        return;
    }

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

    arm7DbgHex("  hz",          hz);
    arm7DbgHex("  lengthWords", lengthWords);
    arm7DbgHex("  loop",        (u32)loop);

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

    arm7DbgHex("  SCHANNEL_CR", cr);
    SCHANNEL_CR(ch) = cr;
    arm7DbgStr("  arm7StartChannel: DONE");
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
// PXI handler
// ---------------------------------------------------------------------------
static void audioPxiHandler(void* /*user*/, u32 packet)
{
    u32 value = pxiPacketGetImmediate(packet);
    u8  cmd   = (u8)((value >> 18) & 0xFF);
    int ch    = (int)((value >> 14) & 0x0F);
    u8  vol   = (u8)(value & 0x7F);

    arm7DbgHex("PXI rx value", value);
    arm7DbgHex("  cmd", (u32)cmd);
    arm7DbgHex("  ch",  (u32)ch);

    switch (cmd) {

        case AUDIO_CMD_PLAY: {
            // Read Arm7PlayInfo from the fixed EWRAM rendezvous address.
            // No pointer was sent — ARM9 and ARM7 both know this address
            // from the shared header.  No PXI truncation issue.
            const volatile Arm7PlayInfo* info = &AUDIO_PLAY_INFO_ARRAY()[ch];

            arm7DbgStr("AUDIO_CMD_PLAY");
            arm7DbgHex("  rendezvous", (u32)AUDIO_SHARED_EWRAM_ADDR + (u32)ch * sizeof(Arm7PlayInfo));
            arm7DbgHex("  dataAddr",   info->dataAddr);
            arm7DbgHex("  samples",    info->sampleCount);
            arm7DbgHex("  flags",      info->flags);
            arm7DbgHex("  volume",     info->volume);

            arm7StartChannel(ch, (const Arm7PlayInfo*)info);
            break;
        }

        case AUDIO_CMD_STOP:
            arm7DbgStr("AUDIO_CMD_STOP");
            arm7DbgHex("  ch", (u32)ch);
            arm7StopChannel(ch);
            break;

        case AUDIO_CMD_VOL:
            arm7DbgStr("AUDIO_CMD_VOL");
            arm7DbgHex("  ch",  (u32)ch);
            arm7DbgHex("  vol", (u32)vol);
            arm7SetVolume(ch, vol);
            break;

        case AUDIO_CMD_STOP_ALL:
            arm7DbgStr("AUDIO_CMD_STOP_ALL");
            for (int i = 0; i < AUDIO_MAX_CHANNELS; i++)
                arm7StopChannel(i);
            break;

        case AUDIO_CMD_PING:
            arm7DbgStr("AUDIO_CMD_PING — sending ACK");
            pxiSend(AUDIO_PXI_CHANNEL,
                    ((u32)AUDIO_CMD_PING_ACK << 18));
            break;

        default:
            arm7DbgStr("AUDIO_CMD unknown");
            arm7DbgHex("  cmd", (u32)cmd);
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

    arm7DbgStr("=== AudioArm7 starting ===");
    arm7DbgHex("AUDIO_SHARED_EWRAM_ADDR", AUDIO_SHARED_EWRAM_ADDR);
    arm7DbgHex("sizeof(Arm7PlayInfo)",    sizeof(Arm7PlayInfo));
    arm7DbgHex("REG_SOUNDCNT",           REG_SOUNDCNT);

    pxiSetHandler(AUDIO_PXI_CHANNEL, audioPxiHandler, nullptr);

    // Signal ARM9 we are ready
    pxiSend(AUDIO_PXI_CHANNEL, ((u32)AUDIO_CMD_DEBUG << 18) | 0x0099);
    arm7DbgStr("AudioArm7 ready signal sent");

    while (pmMainLoop()) {
        threadWaitForVBlank();
    }

    return 0;
}

#endif // ARM7