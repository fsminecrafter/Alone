#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include "MemoryManager.h"
#include "ChunkLibrary.h"

// ---------------------------------------------------------------------------
// Screens + VRAM
// ---------------------------------------------------------------------------
static PrintConsole bottomConsole;

static void setupScreens()
{
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_TEXTURE);
    vramSetBankC(VRAM_C_SUB_BG);
    vramSetBankD(VRAM_D_SUB_SPRITE);

    videoSetMode(MODE_0_3D);
    videoSetModeSub(MODE_0_2D);

    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256,
                31, 0, false, true);
    consoleSelect(&bottomConsole);
}

// ---------------------------------------------------------------------------
// GL
// ---------------------------------------------------------------------------
static void setup3D()
{
    glInit();
    glClearColor(2, 6, 12, 31);   // dark blue — visible if polys are missing
    glClearDepth(GL_MAX_DEPTH);
    glClearPolyID(0);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_ANTIALIAS);

    glViewport(0, 0, 255, 191);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45, 256.0f / 192.0f, 0.1f, 512.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void applyCamera(float px, float pz)
{
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(
        px,        20.0f, pz + 15.0f,
        px,         0.0f, pz,
        0.0f,       1.0f, 0.0f
    );
}

// ---------------------------------------------------------------------------
// Hardcoded test floor — 4x4 grid of quads, vertex colour only
// Drawn directly with libnds GL so it bypasses ChunkLibrary entirely.
// If this renders, the GL pipeline is working.
// ---------------------------------------------------------------------------
static void drawTestFloor(float px, float pz)
{
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);

    const int TILES = 4;
    const float S   = 2.0f;           // tile size in world units
    const float OX  = px - (TILES/2) * S;
    const float OZ  = pz - (TILES/2) * S;

    glBegin(GL_QUADS);
    for (int z = 0; z < TILES; z++) {
        for (int x = 0; x < TILES; x++) {
            // Checker pattern
            bool checker = ((x + z) & 1);
            if (checker) glColor3b(180, 120, 60);   // warm brown
            else         glColor3b(100, 160, 80);   // green

            float x0 = OX + x * S;
            float x1 = x0 + S;
            float z0 = OZ + z * S;
            float z1 = z0 + S;

            glVertex3f(x0, 0.0f, z0);
            glVertex3f(x1, 0.0f, z0);
            glVertex3f(x1, 0.0f, z1);
            glVertex3f(x0, 0.0f, z1);
        }
    }
    glEnd();
}

// ---------------------------------------------------------------------------
// Float helper
// ---------------------------------------------------------------------------
static void printFloat1dp(float v)
{
    if (v < 0.0f) { iprintf("-"); v = -v; }
    int whole = (int)v;
    int frac  = (int)((v - (float)whole) * 10.0f);
    iprintf("%d.%d", whole, frac);
}

// ---------------------------------------------------------------------------
// Swap
// ---------------------------------------------------------------------------
static void drawBar(float pct)
{
    int filled = (int)(pct / 5.0f);
    iprintf("[");
    for (int i = 0; i < 20; i++) iprintf(i < filled ? "#" : "-");
    iprintf("]");
}

static bool runSwapCreation(MemoryManager& mem)
{
    bool err = false;
    bool ok  = mem.createSwap(32, [&](float pct) {
        if (pct < 0.0f) { err = true; return; }
        consoleClear();
        iprintf("Creating swap...\n\n");
        printFloat1dp(pct);
        iprintf("%%\n\n");
        drawBar(pct);
        swiWaitForVBlank();
    });
    if (!ok || err) {
        consoleClear();
        iprintf("Error creating swap\nSTART=exit\n");
        while (1) { scanKeys(); if (keysDown() & KEY_START) return false; swiWaitForVBlank(); }
    }
    consoleClear();
    iprintf("Swap ready!\n");
    for (int i = 0; i < 60; i++) swiWaitForVBlank();
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main()
{
    fatInitDefault();
    setupScreens();
    setup3D();

    // Swap
    MemoryManager mem;
    mem.setSwappiness(30);
    if (!mem.checkIfSwapExists()) {
        if (!runSwapCreation(mem)) return 0;
    }
    if (!mem.openSwap()) {
        consoleClear();
        iprintf("Swap open failed\n");
        while (1) swiWaitForVBlank();
    }

    // World — optional, falls back to test floor if missing
    ChunkLibrary world(&mem);
    bool worldLoaded = world.loadWorld("fat:/Alone/world.world");

    consoleClear();
    if (worldLoaded)
        iprintf("World OK %lu chunks\n", (unsigned long)world.totalChunkCount());
    else
        iprintf("No world.world\nShowing test floor\n");
    for (int i = 0; i < 90; i++) swiWaitForVBlank();
    consoleClear();

    float px = 0.0f, pz = 0.0f;
    const float SPD = 0.25f;
    int frame = 0;

    while (1) {
        scanKeys();
        u32 held = keysHeld();
        if (held & KEY_START) break;

        if (held & KEY_UP)    pz -= SPD;
        if (held & KEY_DOWN)  pz += SPD;
        if (held & KEY_LEFT)  px -= SPD;
        if (held & KEY_RIGHT) px += SPD;

        // Camera
        applyCamera(px, pz);

        // Draw — world if loaded, test floor if not
        if (worldLoaded) {
            world.update(px, pz);
            world.render();
        } else {
            drawTestFloor(px, pz);
        }

        glFlush(0);

        // UI
        if (frame % 30 == 0) {
            consoleClear();
            if (worldLoaded) {
                iprintf("X:"); printFloat1dp(px);
                iprintf(" Z:"); printFloat1dp(pz); iprintf("\n");
                iprintf("Chunks:%lu/%lu\n",
                    (unsigned long)world.loadedChunkCount(),
                    (unsigned long)world.totalChunkCount());
                iprintf("Polys :%lu\n", (unsigned long)world.lastFramePolys());
            } else {
                iprintf("TEST FLOOR\n");
                iprintf("X:"); printFloat1dp(px);
                iprintf(" Z:"); printFloat1dp(pz); iprintf("\n");
                iprintf("D-pad=move\n");
                iprintf("START=quit\n");
            }
            iprintf("RAM:%lu B\n", (unsigned long)mem.getFreeRAM());
        }

        swiWaitForVBlank();
        frame++;
    }

    world.unloadWorld();
    return 0;
}
