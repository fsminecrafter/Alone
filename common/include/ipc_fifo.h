#pragma once
// ---------------------------------------------------------------------------
// ipc_fifo.h  —  ARM9 sends raw PXI-format packets via hardware registers.
// calico on ARM7 receives them through its normal PXI dispatch.
// ---------------------------------------------------------------------------
#include <nds/ipc.h>
#include <nds/ndstypes.h>
#include "logger.h"

// Our audio PXI channel — must match AUDIO_PXI_CHANNEL in AudioArm7.cpp
// PxiChannel_User0 = 23 = 0x17
#define FIFO_USER_01  23u

// ---------------------------------------------------------------------------
// PXI packet format (from calico/nds/pxi.h):
//   bits 4:0  = channel (5 bits)
//   bit  5    = direction (0=request)
//   bits 31:6 = immediate (26 bits)
//
// Our audio payload fits in 26 bits:
//   cmd  = bits 25:18  (8 bits)
//   ch   = bits 17:14  (4 bits)
//   vol  = bits  6:0   (7 bits)
// ---------------------------------------------------------------------------
static inline u32 _pxi_make_packet(u32 channel, u32 imm26)
{
    return (channel & 0x1f) | (imm26 << 6);
}

static inline void _pxi_send_raw(u32 word)
{
    // Enable FIFO if needed
    REG_IPC_FIFO_CR |= IPC_FIFO_ENABLE;
    // Spin while full
    while (REG_IPC_FIFO_CR & IPC_FIFO_SEND_FULL) {}
    REG_IPC_FIFO_TX = word;
}

// ---------------------------------------------------------------------------
// Public API — matches what AudioSystem.cpp calls
// ---------------------------------------------------------------------------

// Send an AUDIO_PACK(cmd, ch, vol) value — repacked into 26-bit PXI immediate
static inline void fifoSendValue32(u32 channel, u32 value)
{
    // value = AUDIO_PACK format: cmd<<24 | ch<<16 | vol
    u8  cmd = (value >> 24) & 0xFF;
    u8  ch  = (value >> 16) & 0x0F;
    u8  vol = (value)       & 0x7F;
    u32 imm = ((u32)cmd << 18) | ((u32)ch << 14) | vol;
    _pxi_send_raw(_pxi_make_packet(channel, imm));
}

// Send a raw pointer as a second PXI packet (for PLAY command)
static inline void fifoSendPtr(u32 channel, u32 ptr)
{
    // Pointer is word-aligned so bottom 2 bits are 0.
    // Shift right 2 to fit in 26 bits (max EWRAM addr = 0x023FFFFF >> 2 = 0x008FFFFF, fits)
    _pxi_send_raw(_pxi_make_packet(channel, ptr >> 2));
}

static inline int fifoCheckValue32(u32 /*channel*/)
{
    return (REG_IPC_FIFO_CR & IPC_FIFO_RECV_EMPTY) == 0;
}

static inline u32 fifoGetValue32(u32 /*channel*/)
{
    return REG_IPC_FIFO_RX;
}

typedef void (*FifoValue32Handler)(u32 value, void* userdata);

static inline void fifoSetValue32Handler(u32 /*channel*/,
                                          FifoValue32Handler /*handler*/,
                                          void* /*userdata*/)
{
    // ARM9 does not receive — no-op
}