#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <math.h>
#include "MemoryManager.h"
#include "ChunkLibrary.h"
#include "lighting.h"

float g_lightX, g_lightY, g_lightZ;

// ---------------------------------------------------------------------------
// Time of day
// ---------------------------------------------------------------------------
// 24 hours. Each press of A advances by 1 hour.
// The sun travels in a arc: rises in the east (+X), sets in the west (-X).
// At noon it is directly overhead. At midnight it is below the horizon
// (ambient only, very dark). Sky clear colour also shifts.

static int   s_hour    = 8;   // start at 8am
static float s_ambient = 0.0f;
static float s_diffuse = 0.0f;

// Hour name strings (no heap allocation)
static const char* hourName(int h)
{
    switch (h) {
        case  0: return "Midnight";
        case  1: return "1am";
        case  2: return "2am";
        case  3: return "3am";
        case  4: return "4am";
        case  5: return "Pre-dawn";
        case  6: return "Sunrise";
        case  7: return "Early morn";
        case  8: return "Morning";
        case  9: return "9am";
        case 10: return "10am";
        case 11: return "Late morn";
        case 12: return "Noon";
        case 13: return "Afternoon";
        case 14: return "2pm";
        case 15: return "3pm";
        case 16: return "4pm";
        case 17: return "Late aftn";
        case 18: return "Sunset";
        case 19: return "Dusk";
        case 20: return "Evening";
        case 21: return "9pm";
        case 22: return "10pm";
        case 23: return "Late night";
        default: return "??";
    }
}

// Recalculate light direction + ambient/diffuse + sky colour from s_hour.
// Sun angle: hour 6 = sunrise (east horizon), 12 = overhead, 18 = sunset.
// Hours 19-5 = night (sun below horizon, ambient only).
static void applyTimeOfDay()
{
    // Map hour to sun angle in degrees above horizon (-90..+90 arc).
    // noon = 90 degrees (straight up), sunrise/sunset = 0.
    float hourF  = (float)s_hour + 0.5f;              // centre of hour
    float sunDeg = (hourF - 12.0f) * (180.0f / 12.0f); // -90..+90 over 24h
    // Actually we want a half-circle: 6am=0deg overhead at noon=90deg 18pm=0
    float t      = (hourF - 6.0f) / 12.0f;            // 0 at 6am, 1 at 6pm
    float sunEl  = sinf(t * 3.14159f);                // 0->1->0 arc

    // East-west direction: morning sun in +X, evening in -X
    float sunEW  = cosf(t * 3.14159f);                // +1 at 6am, -1 at 6pm

    bool isDay = (s_hour >= 6 && s_hour < 18);
    bool isTwilight = (s_hour == 5 || s_hour == 18 || s_hour == 19);

    if (isDay) {
        // Light comes from sun position
        g_lightX = sunEW * 0.6f;
        g_lightY = sunEl;
        g_lightZ = -0.2f;
        // Normalise
        float mag = sqrtf(g_lightX*g_lightX + g_lightY*g_lightY + g_lightZ*g_lightZ);
        if (mag > 0.001f) { g_lightX/=mag; g_lightY/=mag; g_lightZ/=mag; }

        s_ambient = 0.15f + 0.20f * sunEl;   // 0.15 at horizon, 0.35 at noon
        s_diffuse = 0.50f + 0.30f * sunEl;   // 0.50 at horizon, 0.80 at noon

        // Sky: deep blue at sunrise/set, lighter blue at noon
        u8 skyR = (u8)(2  + (int)(10 * sunEl));
        u8 skyG = (u8)(6  + (int)(20 * sunEl));
        u8 skyB = (u8)(12 + (int)(18 * sunEl));
        glClearColor(skyR, skyG, skyB, 31);
    } else if (isTwilight) {
        g_lightX = (s_hour <= 6) ? 1.0f : -1.0f;
        g_lightY = 0.05f;
        g_lightZ = 0.0f;
        s_ambient = 0.08f;
        s_diffuse = 0.15f;
        glClearColor(4, 4, 8, 31);   // deep purple-blue
    } else {
        // Night: no directional light, very dim ambient only
        g_lightX = 0.0f; g_lightY = 1.0f; g_lightZ = 0.0f;
        s_ambient = 0.04f;
        s_diffuse = 0.0f;
        glClearColor(0, 0, 3, 31);   // near-black
    }
}

// ---------------------------------------------------------------------------
// Screens + VRAM
// ---------------------------------------------------------------------------
static PrintConsole bottomConsole;

static void setupScreens()
{
    videoSetMode(MODE_0_3D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_TEXTURE);
    vramSetBankB(VRAM_B_TEXTURE);
    vramSetBankC(VRAM_C_SUB_BG);
    vramSetBankD(VRAM_D_LCD);
}

// ---------------------------------------------------------------------------
// GL init
// ---------------------------------------------------------------------------
static void setup3D()
{
    glInit();
    glClearColor(2, 6, 12, 31);
    glClearDepth(GL_MAX_DEPTH);
    glClearPolyID(63);
    glEnable(GL_TEXTURE_2D);
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
    gluLookAt(px, 20.0f, pz + 15.0f, px, 0.0f, pz, 0.0f, 1.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Test floor
// ---------------------------------------------------------------------------
static void drawTestFloor(float px, float pz)
{
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
    const int   TILES = 4;
    const float S     = 2.0f;
    const float OX    = px - (TILES / 2) * S;
    const float OZ    = pz - (TILES / 2) * S;
    float scale = lightScale(0.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS);
    for (int z = 0; z < TILES; z++) {
        for (int x = 0; x < TILES; x++) {
            if ((x + z) & 1) glColorLit(180, 120, 60, scale);
            else             glColorLit(100, 160, 80, scale);
            float x0 = OX + x * S, x1 = x0 + S;
            float z0 = OZ + z * S, z1 = z0 + S;
            glVertex3f(x0, 0.0f, z0);
            glVertex3f(x1, 0.0f, z0);
            glVertex3f(x1, 0.0f, z1);
            glVertex3f(x0, 0.0f, z1);
        }
    }
    glEnd();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void printFloat1dp(float v)
{
    if (v < 0.0f) { iprintf("-"); v = -v; }
    int whole = (int)v;
    int frac  = (int)((v - (float)whole) * 10.0f);
    iprintf("%d.%d", whole, frac);
}

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
        while (1) {
            scanKeys();
            if (keysDown() & KEY_START) return false;
            swiWaitForVBlank();
        }
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
    setupScreens();
    setup3D();
    initLight();
    applyTimeOfDay();   // set initial 8am light

    fatInitDefault();
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256,
                31, 0, false, true);
    consoleSelect(&bottomConsole);

    static MemoryManager mem;
    mem.setSwappiness(30);

    if (!mem.checkIfSwapExists()) {
        if (!runSwapCreation(mem)) return 0;
    }
    if (!mem.openSwap()) {
        consoleClear();
        iprintf("Swap open failed\n");
        while (1) swiWaitForVBlank();
    }

    consoleClear();
    iprintf("RAM before load:%lu\n", (unsigned long)mem.getFreeRAM());

    static ChunkLibrary world(&mem);
    bool worldLoaded = world.loadWorld("fat:/Alone/world.world");

    consoleClear();
    iprintf("RAM after load:%lu\n", (unsigned long)mem.getFreeRAM());
    if (worldLoaded) {
        iprintf("World OK %lu chunks\n", (unsigned long)world.totalChunkCount());
        for (u32 ci = 0; ci < world.totalChunkCount() && ci < 8; ci++) {
            s16 gx, gz; u16 vc;
            world.getChunkInfo(ci, gx, gz, vc);
            iprintf(" [%d,%d] %u v\n", (int)gx, (int)gz, (unsigned)vc);
        }
    } else {
        iprintf("No world.world — test floor\n");
    }
    for (int i = 0; i < 180; i++) swiWaitForVBlank();
    consoleClear();

    float px = 0.0f, pz = 0.0f;
    if (worldLoaded)
        world.getChunk0WorldPos(px, pz);

    float vx = 0.0f, vz = 0.0f;
    const float ACCEL   = 0.08f;
    const float DAMP    = 0.80f;
    const float MAX_SPD = 0.6f;
    int frame = 0;

    u32 fpsLastTick = 0;
    int fpsSamples  = 0;
    int fpsDisplay  = 0;
    cpuStartTiming(0);

    while (1) {
        scanKeys();
        u32 held = keysHeld();
        u32 down = keysDown();
        if (held & KEY_START) break;

        // A = advance one hour
        if (down & KEY_A) {
            s_hour = (s_hour + 1) % 24;
            applyTimeOfDay();
        }

        if (held & KEY_UP)    vz -= ACCEL;
        if (held & KEY_DOWN)  vz += ACCEL;
        if (held & KEY_LEFT)  vx -= ACCEL;
        if (held & KEY_RIGHT) vx += ACCEL;

        if (vx >  MAX_SPD) vx =  MAX_SPD;
        if (vx < -MAX_SPD) vx = -MAX_SPD;
        if (vz >  MAX_SPD) vz =  MAX_SPD;
        if (vz < -MAX_SPD) vz = -MAX_SPD;
        vx *= DAMP;
        vz *= DAMP;
        if (vx > -0.001f && vx < 0.001f) vx = 0.0f;
        if (vz > -0.001f && vz < 0.001f) vz = 0.0f;

        px += vx;
        pz += vz;

        applyCamera(px, pz);

        if (worldLoaded) {
            world.update(px, pz);
            world.render();
        } else {
            drawTestFloor(px, pz);
        }

        glFlush(0);
        swiWaitForVBlank();

        fpsSamples++;
        u32 now = cpuEndTiming();
        if ((now - fpsLastTick) >= 33513982u) {
            fpsDisplay  = fpsSamples;
            fpsSamples  = 0;
            fpsLastTick = now;
        }

        if (frame % 20 == 0) {
            consoleClear();
            iprintf("FPS:%d  A=+1hr\n", fpsDisplay);
            iprintf("%02d:00 %s\n", s_hour, hourName(s_hour));
            iprintf("Amb:"); printFloat1dp(s_ambient);
            iprintf(" Dif:"); printFloat1dp(s_diffuse); iprintf("\n");
            if (worldLoaded) {
                iprintf("X:"); printFloat1dp(px);
                iprintf(" Z:"); printFloat1dp(pz); iprintf("\n");
                iprintf("Chunks:%lu/%lu\n",
                    (unsigned long)world.loadedChunkCount(),
                    (unsigned long)world.totalChunkCount());
                iprintf("Polys:%lu\n", (unsigned long)world.lastFramePolys());
            } else {
                iprintf("TEST FLOOR\n");
                iprintf("X:"); printFloat1dp(px);
                iprintf(" Z:"); printFloat1dp(pz); iprintf("\n");
            }
            iprintf("RAM:%lu\n", (unsigned long)mem.getFreeRAM());
        }
        frame++;
    }

    world.unloadWorld();
    return 0;
}
