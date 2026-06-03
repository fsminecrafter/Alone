#pragma once
// ---------------------------------------------------------------------------
// ipc_fifo.h  —  IPC FIFO shim for devkitPro installations that predate the
//                libnds high-level fifo helpers (fifoSendValue32 etc.).
//
// The NDS hardware exposes two 16-entry 32-bit FIFOs:
//   ARM9 → ARM7  (write via REG_IPC_FIFO_TX on ARM9 side)
//   ARM7 → ARM9  (write via REG_IPC_FIFO_TX on ARM7 side)
//
// Hardware registers (both CPUs, same addresses):
//   0x04000184  REG_IPC_FIFO_TX   write-only: push a word into the send FIFO
//   0x04100000  REG_IPC_FIFO_RX   read-only:  pop a word from the receive FIFO
//   0x04000184  REG_IPC_FIFO_CR   control (bits):
//       bit  0  send FIFO empty flag (read)
//       bit  1  send FIFO full  flag (read)
//       bit  2  send FIFO empty IRQ enable
//       bit  3  clear send FIFO
//       bit  8  receive FIFO empty flag (read)
//       bit  9  receive FIFO full  flag (read)
//       bit 10  receive FIFO not-empty IRQ enable
//       bit 14  FIFO error flag (read)
//       bit 15  FIFO enable (must be 1 to use the FIFO)
//
// This shim provides:
//   FIFO_USER_01               — a user channel ID constant (we use 0x01)
//   fifoSendValue32(ch, val)   — send one 32-bit word, blocking until not full
//   fifoSetValue32Handler(...) — register a per-word receive callback (ARM7 only)
//   fifoCheckValue32(ch)       — return true if a word is waiting to be read
//   fifoGetValue32(ch)         — pop and return the next word
//
// Channel parameter is accepted for API compatibility but ignored — the NDS
// hardware FIFO is a single shared queue.  The caller is responsible for
// embedding a channel/command field inside the 32-bit value itself, which is
// exactly what AudioSystem.cpp already does via AUDIO_PACK().
// ---------------------------------------------------------------------------

#include <nds/ipc.h>   // REG_IPC_FIFO_TX, REG_IPC_FIFO_RX, REG_IPC_FIFO_CR
                       // IPC_FIFO_ENABLE, IPC_FIFO_SEND_FULL,
                       // IPC_FIFO_RECV_EMPTY  — present in all libnds versions

// Channel ID constant (value is arbitrary; embedded in the message payload).
#define FIFO_USER_01   0x01u

// ---------------------------------------------------------------------------
// fifoSendValue32  — push one word into the send FIFO, spin if full.
//
// Enables the FIFO on first call (IPC_FIFO_ENABLE in bit 15 of CR).
// Both CPUs must enable the FIFO before the first transfer; calling this
// function on both sides before any data is sent satisfies that requirement.
// ---------------------------------------------------------------------------
static inline void fifoSendValue32(u32 /*channel*/, u32 value)
{
    // Enable the FIFO if it hasn't been already.
    REG_IPC_FIFO_CR |= IPC_FIFO_ENABLE;

    // Spin while the send FIFO is full (16 entries; should never spin in practice
    // because the ARM7 drains entries faster than the ARM9 can fill them at 60 fps).
    while (REG_IPC_FIFO_CR & IPC_FIFO_SEND_FULL);

    REG_IPC_FIFO_TX = value;
}

// ---------------------------------------------------------------------------
// fifoCheckValue32  — return non-zero if at least one word is waiting.
// ---------------------------------------------------------------------------
static inline int fifoCheckValue32(u32 /*channel*/)
{
    return (REG_IPC_FIFO_CR & IPC_FIFO_RECV_EMPTY) == 0;
}

// ---------------------------------------------------------------------------
// fifoGetValue32  — pop and return the next word from the receive FIFO.
// Caller should check fifoCheckValue32() first.
// ---------------------------------------------------------------------------
static inline u32 fifoGetValue32(u32 /*channel*/)
{
    return (u32)REG_IPC_FIFO_RX;
}

// ---------------------------------------------------------------------------
// fifoSetValue32Handler  — install a per-word receive callback.
//
// Only meaningful on the ARM7 side.  When a word arrives on the receive FIFO
// the ARM7's IPC_NOT_EMPTY IRQ fires; the handler is called from the IRQ.
//
// Implementation: we store the callback pointer in a static variable and
// enable the receive-not-empty IRQ.  The ARM7 main loop must call
// swiIntrWait(1, IRQ_ALL) or have IPC_SYNC enabled for the IRQ to fire.
//
// If handler is nullptr the IRQ is disabled (ARM9 calls this to disable
// the receive side, which it never uses).
// ---------------------------------------------------------------------------
typedef void (*FifoValue32Handler)(u32 value, void* userdata);

// Storage for the registered handler (one slot; channel arg ignored).
// Placed in DTCM on ARM7 so IRQ latency is minimal.
#ifdef ARM7
static FifoValue32Handler _s_fifo_handler   = 0;
static void*              _s_fifo_userdata  = 0;

// Call this from the ARM7's IPC_NOT_EMPTY IRQ handler, or hook it
// directly as irqSet(IRQ_FIFO_NOT_EMPTY, _ipc_fifo_isr).
static void _ipc_fifo_isr()
{
    while (fifoCheckValue32(FIFO_USER_01)) {
        u32 val = fifoGetValue32(FIFO_USER_01);
        if (_s_fifo_handler)
            _s_fifo_handler(val, _s_fifo_userdata);
    }
}
#endif  // ARM7

static inline void fifoSetValue32Handler(u32 /*channel*/,
                                          FifoValue32Handler handler,
                                          void* userdata)
{
#ifdef ARM7
    _s_fifo_handler  = handler;
    _s_fifo_userdata = userdata;

    if (handler) {
        // Enable the FIFO and the receive-not-empty IRQ.
        REG_IPC_FIFO_CR |= IPC_FIFO_ENABLE | IPC_FIFO_RECV_IRQ;
        irqSet(IRQ_FIFO_NOT_EMPTY, _ipc_fifo_isr);
        irqEnable(IRQ_FIFO_NOT_EMPTY);
    } else {
        irqDisable(IRQ_FIFO_NOT_EMPTY);
    }
#else
    // ARM9 side: just enable the FIFO for sending; we never receive.
    (void)handler; (void)userdata;
    REG_IPC_FIFO_CR |= IPC_FIFO_ENABLE;
#endif
}