#pragma once

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define SWAP_PATH       "fat:/Alone/swapfile.swap"
#define SWAP_DIR        "fat:/Alone"
#define SWAP_PAGE_SIZE  4096

// ---------------------------------------------------------------------------
// MemoryManager
// ---------------------------------------------------------------------------
class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();

    // Swap file
    // Creates the swap file. Calls progressCallback(float) repeatedly with
    // 0.0–100.0 as progress, or -1.0 on error. Returns true on success.
    bool        createSwap(u32 sizeMB, std::function<void(float)> progressCallback = nullptr);
    bool        checkIfSwapExists();
    bool        openSwap();
    void        closeSwap();

    // Swappiness 0–100. Higher = evict pages more eagerly.
    void        setSwappiness(int value);
    int         getSwappiness() const { return swappiness; }

    // Page management
    // Call evict() periodically (e.g. once per frame) to let the manager
    // move pages to/from SD based on swappiness.
    void        evict();

    // Allocate a tracked page-aligned block. Returns nullptr on failure.
    void*       allocPage();
    // Free a tracked block and write it to swap if dirty.
    void        freePage(void* ptr);
    // Mark a page dirty (needs to be written on eviction).
    void        markDirty(void* ptr);

    // Info
    u32         getFreeRAM()    const;   // bytes remaining in heap
    u32         getResidentPages() const { return residentCount; }
    u32         getTotalPages()    const { return totalPages; }
    bool        isSwapOpen()       const { return swapFd != nullptr; }

private:
    static const u32 MAX_PAGES = 256;

    struct PageEntry {
        void*   ramAddr;    // nullptr = not resident
        u32     swapOffset; // byte offset inside swap file
        bool    dirty;
        bool    used;
    };

    FILE*       swapFd;
    int         swappiness;
    u32         totalPages;
    u32         residentCount;
    PageEntry   table[MAX_PAGES];

    int         findPage(void* ptr) const;
    void        pageOut(u32 idx);
    void        pageIn(u32 idx, void* buf);
};
