#include "MemoryManager.h"
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
MemoryManager::MemoryManager()
    : swapFd(nullptr), swappiness(30), totalPages(0), residentCount(0)
{
    memset(table, 0, sizeof(table));
}

MemoryManager::~MemoryManager()
{
    closeSwap();
}

// ---------------------------------------------------------------------------
// Swap file creation
// ---------------------------------------------------------------------------
bool MemoryManager::createSwap(u32 sizeMB, std::function<void(float)> progressCallback)
{
    // Ensure the directory exists
    mkdir(SWAP_DIR, 0777);

    FILE* f = fopen(SWAP_PATH, "wb");
    if (!f) {
        if (progressCallback) progressCallback(-1.0f);
        return false;
    }

    const u32 totalBytes = sizeMB * 1024u * 1024u;
    const u32 chunkSize  = SWAP_PAGE_SIZE;
    const u32 numChunks  = totalBytes / chunkSize;

    u8 zeroBuf[SWAP_PAGE_SIZE] = {0};

    for (u32 i = 0; i < numChunks; i++) {
        size_t written = fwrite(zeroBuf, 1, chunkSize, f);
        if (written != chunkSize) {
            fclose(f);
            if (progressCallback) progressCallback(-1.0f);
            return false;
        }

        if (progressCallback) {
            float pct = ((float)(i + 1) / (float)numChunks) * 100.0f;
            progressCallback(pct);
        }
    }

    fclose(f);

    // Initialise the page table to map the new file
    totalPages = (numChunks < MAX_PAGES) ? numChunks : MAX_PAGES;
    for (u32 i = 0; i < totalPages; i++) {
        table[i].ramAddr    = nullptr;
        table[i].swapOffset = i * chunkSize;
        table[i].dirty      = false;
        table[i].used       = false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Swap file helpers
// ---------------------------------------------------------------------------
bool MemoryManager::checkIfSwapExists()
{
    FILE* f = fopen(SWAP_PATH, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool MemoryManager::openSwap()
{
    if (swapFd) return true;
    swapFd = fopen(SWAP_PATH, "r+b");
    if (!swapFd) return false;
    setvbuf(swapFd, nullptr, _IONBF, 0);
    return true;
}

void MemoryManager::closeSwap()
{
    if (!swapFd) return;

    // Flush all dirty resident pages before closing
    for (u32 i = 0; i < totalPages; i++) {
        if (table[i].ramAddr && table[i].dirty)
            pageOut(i);
    }

    fclose(swapFd);
    swapFd = nullptr;
}

// ---------------------------------------------------------------------------
// Swappiness
// ---------------------------------------------------------------------------
void MemoryManager::setSwappiness(int value)
{
    if (value < 0)   value = 0;
    if (value > 100) value = 100;
    swappiness = value;
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------
void MemoryManager::evict()
{
    if (!swapFd) return;

    for (u32 i = 0; i < totalPages; i++) {
        if (!table[i].ramAddr) continue;
        // swappiness as a probability weight: higher = evict more
        if ((rand() % 100) < swappiness)
            pageOut(i);
    }
}

// ---------------------------------------------------------------------------
// Page allocation
// ---------------------------------------------------------------------------
void* MemoryManager::allocPage()
{
    // Find a free slot in the table
    for (u32 i = 0; i < totalPages; i++) {
        if (table[i].used) continue;

        void* buf = malloc(SWAP_PAGE_SIZE);
        if (!buf) return nullptr;

        memset(buf, 0, SWAP_PAGE_SIZE);
        table[i].ramAddr = buf;
        table[i].dirty   = false;
        table[i].used    = true;
        residentCount++;
        return buf;
    }
    return nullptr;
}

void MemoryManager::freePage(void* ptr)
{
    int idx = findPage(ptr);
    if (idx < 0) return;

    if (table[idx].dirty && swapFd)
        pageOut(idx);

    free(table[idx].ramAddr);
    table[idx].ramAddr = nullptr;
    table[idx].dirty   = false;
    table[idx].used    = false;
    residentCount--;
}

void MemoryManager::markDirty(void* ptr)
{
    int idx = findPage(ptr);
    if (idx >= 0) table[idx].dirty = true;
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------
u32 MemoryManager::getFreeRAM() const
{
    // libnds: mallinfo gives heap stats
    struct mallinfo mi = mallinfo();
    return (u32)mi.fordblks;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
int MemoryManager::findPage(void* ptr) const
{
    for (u32 i = 0; i < totalPages; i++)
        if (table[i].ramAddr == ptr) return (int)i;
    return -1;
}

void MemoryManager::pageOut(u32 idx)
{
    if (!swapFd || !table[idx].ramAddr) return;

    fseek(swapFd, table[idx].swapOffset, SEEK_SET);
    fwrite(table[idx].ramAddr, 1, SWAP_PAGE_SIZE, swapFd);
    table[idx].dirty   = false;
    // Keep ramAddr set so the slot stays "used" but not dirty
}

void MemoryManager::pageIn(u32 idx, void* buf)
{
    if (!swapFd) return;

    fseek(swapFd, table[idx].swapOffset, SEEK_SET);
    fread(buf, 1, SWAP_PAGE_SIZE, swapFd);
    table[idx].ramAddr = buf;
    table[idx].dirty   = false;
}
