#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <math.h>
#include "MemoryManager.h"
#include "ChunkLibrary.h"
#include "lighting.h"

float g_lightX, g_lightY, g_lightZ;
float g_ambient, g_diffuse;

// ---------------------------------------------------------------------------
// Time / Season system
// ---------------------------------------------------------------------------
static int s_season = 3;   // start mid-summer
static int s_hour   = 12;

static const char* seasonName(int s)
{
    switch (s) {
        case  0: return "Early Spring";
        case  1: return "Mid Spring";
        case  2: return "Late Spring";
        case  3: return "Early Summer";
        case  4: return "Mid Summer";
        case  5: return "Late Summer";
        case  6: return "Early Autumn";
        case  7: return "Mid Autumn";
        case  8: return "Late Autumn";
        case  9: return "Early Winter";
        case 10: return "Mid Winter";
        case 11: return "Late Winter";
        default: return "??";
    }
}

static const float kSunrise[12] = {
    6.5f, 6.0f, 5.2f, 4.7f, 4.5f, 4.8f,
    5.8f, 6.5f, 7.3f, 7.8f, 8.0f, 7.5f,
};
static const float kDaylight[12] = {
    12.0f, 13.5f, 15.0f, 16.0f, 16.5f, 16.0f,
    13.0f, 11.0f,  9.5f,  8.5f,  8.0f,  9.0f,
};

static void getSeasonParams(float& outSunrise, float& outDaylight)
{
    outSunrise  = kSunrise[s_season];
    outDaylight = kDaylight[s_season];
}

// ---------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------
static void applyTimeOfDay()
{
    float sunrise, daylight;
    getSeasonParams(sunrise, daylight);
    float sunset    = sunrise + daylight;
    float solarNoon = sunrise + daylight * 0.5f;
    float hourF     = (float)s_hour + 0.5f;

    bool isDay      = (hourF > sunrise && hourF < sunset);
    bool isTwilight = (!isDay && (hourF > sunrise - 1.0f && hourF < sunset + 1.0f));

    if (isDay) {
        float t       = (hourF - sunrise) / (daylight * 0.5f);
        if (t > 2.0f) t = 2.0f;
        float halfArc = (t <= 1.0f) ? t : (2.0f - t);
        float sunEl   = sinf(halfArc * 3.14159f * 0.5f);
        float sunEW   = cosf(halfArc * 3.14159f * 0.5f) * ((hourF < solarNoon) ? 1.0f : -1.0f);

        g_lightX = sunEW * 0.7f;
        g_lightY = sunEl;
        g_lightZ = -0.15f;
        float mag = sqrtf(g_lightX*g_lightX + g_lightY*g_lightY + g_lightZ*g_lightZ);
        if (mag > 0.001f) { g_lightX/=mag; g_lightY/=mag; g_lightZ/=mag; }

        float sunElSmooth  = sunEl * sunEl;
        float seasonStr    = kDaylight[s_season] / 16.5f;
        g_ambient = (0.10f + 0.18f * sunElSmooth) * (0.6f + 0.4f * seasonStr);
        g_diffuse = (0.25f + 0.35f * sunElSmooth) * (0.6f + 0.4f * seasonStr);

        u8 skyR = (u8)(2  + (int)(8  * sunElSmooth) + (int)(4 * (1.0f - sunElSmooth)));
        u8 skyG = (u8)(4  + (int)(14 * sunElSmooth));
        u8 skyB = (u8)(10 + (int)(16 * sunElSmooth * seasonStr));
        glClearColor(skyR, skyG, skyB, 31);

    } else if (isTwilight) {
        float dist = (hourF < solarNoon) ? (sunrise - hourF) : (hourF - sunset);
        if (dist < 0.0f) dist = 0.0f;
        float t = 1.0f - dist;

        g_lightX = (hourF < solarNoon) ? 1.0f : -1.0f;
        g_lightY = 0.02f;
        g_lightZ = 0.0f;
        g_ambient = 0.04f + 0.06f * t;
        g_diffuse = 0.05f + 0.10f * t;
        glClearColor((u8)(3+(int)(6*t)), (u8)(2+(int)(3*t)), (u8)(6+(int)(4*t)), 31);

    } else {
        g_lightX = 0.0f; g_lightY = 1.0f; g_lightZ = 0.0f;
        g_ambient = 0.03f;
        g_diffuse = 0.0f;
        glClearColor(0, 0, 2, 31);
    }
}

// ---------------------------------------------------------------------------
// Screens / GL
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

static void setup3D()
{
    glInit();
    glClearColor(2, 6, 12, 31);
    glClearDepth(GL_MAX_DEPTH);
    glClearPolyID(63);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);        // enable alpha blending
    glEnable(GL_ALPHA_TEST);   // enable alpha test for 1-bit cutout (A1RGB5 textures)
    glAlphaFunc(1);            // discard pixels with alpha == 0 (threshold = 1 out of 15)
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
    gluLookAt(px, 30.0f, pz + 15.0f, px, 0.0f, pz, 0.0f, 1.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Test floor (fallback when no world.world)
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
    iprintf("%d.%d", (int)v, (int)((v - (int)v) * 10.0f));
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
    applyTimeOfDay();

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

    static ChunkLibrary world(&mem);
    bool worldLoaded = world.loadWorld("fat:/Alone/world.world");

    consoleClear();
    if (worldLoaded) {
        iprintf("World OK %lu chunks\n", (unsigned long)world.totalChunkCount());
        for (u32 ci = 0; ci < world.totalChunkCount() && ci < 8; ci++) {
            s16 gx, gz; u16 vc;
            world.getChunkInfo(ci, gx, gz, vc);
            iprintf(" [%d,%d] %u v\n", (int)gx, (int)gz, (unsigned)vc);
        }
    } else {
        iprintf("No world.world -- test floor\n");
    }
    for (int i = 0; i < 180; i++) swiWaitForVBlank();
    consoleClear();

    float px = 0.0f, pz = 0.0f;
    if (worldLoaded)
        world.getWorldCenter(px, pz);  // start camera at world centroid

    float vx = 0.0f, vz = 0.0f;
    const float ACCEL   = 0.08f;
    const float DAMP    = 0.80f;
    const float MAX_SPD = 0.6f;
    int frame = 0;

    while (1) {
        scanKeys();
        u32 held = keysHeld();
        u32 down = keysDown();
        if (held & KEY_START) break;

        if (down & KEY_A) { s_hour = (s_hour + 1) % 24; applyTimeOfDay(); }
        if (down & KEY_B) { s_hour = (s_hour + 23) % 24; applyTimeOfDay(); }
        if (down & KEY_X) { s_season = (s_season + 1) % 12; applyTimeOfDay(); }
        if (down & KEY_Y) { s_season = (s_season + 11) % 12; applyTimeOfDay(); }

        if (held & KEY_UP)    vz -= ACCEL;
        if (held & KEY_DOWN)  vz += ACCEL;
        if (held & KEY_LEFT)  vx -= ACCEL;
        if (held & KEY_RIGHT) vx += ACCEL;

        if (vx >  MAX_SPD) vx =  MAX_SPD;
        if (vx < -MAX_SPD) vx = -MAX_SPD;
        if (vz >  MAX_SPD) vz =  MAX_SPD;
        if (vz < -MAX_SPD) vz = -MAX_SPD;
        vx *= DAMP;  vz *= DAMP;
        if (vx > -0.001f && vx < 0.001f) vx = 0.0f;
        if (vz > -0.001f && vz < 0.001f) vz = 0.0f;

        px += vx;  pz += vz;

        applyCamera(px, pz);

        float eyeY = 30.0f, eyeZ = pz + 15.0f;
        if (worldLoaded) {
            world.setCamera(px, eyeY, eyeZ, px, 0.0f, pz);
            world.update(px, pz);
            world.render();
        } else {
            drawTestFloor(px, pz);
        }

        glFlush(0);  // 0 = default Y-sorting of translucent polygons
        swiWaitForVBlank();

        if (frame % 20 == 0) {
            float sunrise, daylight;
            getSeasonParams(sunrise, daylight);

            consoleClear();
            iprintf("%02d:00  A/B=hr  X/Y=ssn\n", s_hour);
            iprintf("%s\n", seasonName(s_season));
            iprintf("Rise:%d:%02d Set:%d:%02d\n",
                (int)sunrise, (int)((sunrise-(int)sunrise)*60.0f),
                (int)(sunrise+daylight),
                (int)(((sunrise+daylight)-(int)(sunrise+daylight))*60.0f));
            iprintf("Amb:"); printFloat1dp(g_ambient);
            iprintf(" Dif:"); printFloat1dp(g_diffuse); iprintf("\n");
            iprintf("X:"); printFloat1dp(px);
            iprintf(" Z:"); printFloat1dp(pz); iprintf("\n");
            if (worldLoaded) {
                iprintf("Chunks:%lu/%lu  Polys:%lu\n",
                    (unsigned long)world.loadedChunkCount(),
                    (unsigned long)world.totalChunkCount(),
                    (unsigned long)world.lastFramePolys());
            }
            iprintf("RAM:%lu\n", (unsigned long)mem.getFreeRAM());
        }
        frame++;
    }

    world.unloadWorld();
    return 0;
}
