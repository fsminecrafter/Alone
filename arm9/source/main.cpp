#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include "MemoryManager.h"

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
static PrintConsole bottomConsole;

static void setupScreens()
{
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    videoSetMode(MODE_0_3D);
    videoSetModeSub(MODE_0_2D);

    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256,
                31, 0, false, true);
    consoleSelect(&bottomConsole);
}

// ---------------------------------------------------------------------------
// 3D
// ---------------------------------------------------------------------------
static void setup3D()
{
    glInit();
    glEnable(GL_ANTIALIAS);
    glEnable(GL_TEXTURE_2D);

    glClearColor(0, 0, 0, 31);
    glClearDepth(GL_MAX_DEPTH);
    glClearPolyID(0);

    glViewport(0, 0, 255, 191);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(70, 256.0f / 192.0f, 0.1f, 40.0f);
}

static void draw3D(int frame)
{
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 0, 3,
              0, 0, 0,
              0, 1, 0);

    glRotatef(frame * 2, 0, 1, 0);

    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);

    glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0);  glVertex3f( 0.0f,  1.0f, 0.0f);
        glColor3f(0, 1, 0);  glVertex3f(-1.0f, -1.0f, 0.0f);
        glColor3f(0, 0, 1);  glVertex3f( 1.0f, -1.0f, 0.0f);
    glEnd();

    glFlush(0);
}

// ---------------------------------------------------------------------------
// Float printing helper
// iprintf/printf on NDS libc often lacks %f support — do it manually
// ---------------------------------------------------------------------------
static void printFloat1dp(float v)
{
    int whole = (int)v;
    int frac  = (int)((v - (float)whole) * 10.0f);
    iprintf("%d.%d", whole, frac);
}

// ---------------------------------------------------------------------------
// Swap creation screen
// ---------------------------------------------------------------------------
static void drawProgressBar(float pct)
{
    int filled = (int)(pct / 5.0f); // 20-char wide bar
    iprintf("[");
    for (int i = 0; i < 20; i++)
        iprintf(i < filled ? "#" : "-");
    iprintf("]");
}

static bool runSwapCreation(MemoryManager& mem)
{
    bool errorFlag = false;

    bool ok = mem.createSwap(32, [&](float pct) {
        if (pct < 0.0f) {
            errorFlag = true;
            consoleClear();
            iprintf("Error when creating swap\n");
            return;
        }

        consoleClear();
        iprintf("Creating swap...\n\n");
        printFloat1dp(pct);
        iprintf("%%\n\n");
        drawProgressBar(pct);
        iprintf("\n");

        swiWaitForVBlank();
    });

    if (!ok || errorFlag) {
        consoleClear();
        iprintf("Error when creating swap\n");
        iprintf("Press START to exit\n");
        while (1) {
            scanKeys();
            if (keysDown() & KEY_START) return false;
            swiWaitForVBlank();
        }
    }

    // Brief "done" flash
    consoleClear();
    iprintf("Swap created!\n");
    for (int i = 0; i < 90; i++) swiWaitForVBlank();

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

    MemoryManager mem;
    mem.setSwappiness(30);

    if (!mem.checkIfSwapExists()) {
        if (!runSwapCreation(mem)) return 0;
    }

    if (!mem.openSwap()) {
        consoleClear();
        iprintf("Failed to open swap\n");
        while (1) swiWaitForVBlank();
    }

    int frame = 0;
    while (1) {
        scanKeys();
        if (keysHeld() & KEY_START) break;

        // Periodic eviction + UI refresh
        if (frame % 60 == 0) {
            mem.evict();

            consoleClear();
            iprintf("---- NDS Demo ----\n\n");
            iprintf("Swappiness : %d\n",   mem.getSwappiness());
            iprintf("Resident   : %lu\n",  (unsigned long)mem.getResidentPages());
            iprintf("Free RAM   : %lu B\n",(unsigned long)mem.getFreeRAM());
            iprintf("\nSTART to quit");
        }

        draw3D(frame);
        swiWaitForVBlank();
        frame++;
    }

    return 0;
}
