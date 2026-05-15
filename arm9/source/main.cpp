#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include "MemoryManager.h"
#include "ChunkLibrary.h"

// ---------------------------------------------------------------------------
// Lighting parameters — edit these to change how the scene looks.
// ---------------------------------------------------------------------------
// Direction the light comes FROM (need not be normalised — code normalises it).
// (0, 1, 0) = directly overhead. Tilt X/Z for side-lit shadows on walls.
#define LIGHT_DIR_X   ( 0.4f)
#define LIGHT_DIR_Y   ( 1.0f)
#define LIGHT_DIR_Z   (-0.3f)

// Ambient: minimum brightness of any surface (0.0 = black shadows, 1.0 = flat lit).
#define LIGHT_AMBIENT  0.30f

// Diffuse: extra brightness added on fully-lit faces.
// Ambient + Diffuse should be <= 1.0 or lit faces will saturate/clamp.
#define LIGHT_DIFFUSE  0.70f

// ---------------------------------------------------------------------------
// Screens + VRAM
// ---------------------------------------------------------------------------
static PrintConsole bottomConsole;

static void setupScreens()
{
    videoSetMode(MODE_0_3D);
    videoSetModeSub(MODE_0_2D);

    // Bank A must be TEXTURE — 3D engine requires it.
    vramSetBankA(VRAM_A_TEXTURE);
    vramSetBankB(VRAM_B_TEXTURE);
    vramSetBankC(VRAM_C_SUB_BG);
    vramSetBankD(VRAM_D_LCD);   // parked
}

// ---------------------------------------------------------------------------
// GL init
// ---------------------------------------------------------------------------
static void setup3D()
{
    glInit();
    glClearColor(2, 6, 12, 31);
    glClearDepth(GL_MAX_DEPTH);
    glClearPolyID(63);          // must differ from geometry POLY_ID

    glEnable(GL_TEXTURE_2D);

    glViewport(0, 0, 255, 191);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45, 256.0f / 192.0f, 0.1f, 512.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // NO hardware lighting (no glLight / glMaterialf / POLY_FORMAT_LIGHT0).
    // We use SOFTWARE lighting — see applyLight() below.
    // Reason: NDS glMaterialf ignores vertex colour; the light colour IS the
    // output colour. Vertex RGB only works correctly without POLY_FORMAT_LIGHTx.
}

static void applyCamera(float px, float pz)
{
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(
        px,   20.0f, pz + 15.0f,
        px,    0.0f, pz,
        0.0f,  1.0f, 0.0f
    );
}

// ---------------------------------------------------------------------------
// Software lighting helper
// ---------------------------------------------------------------------------
// Normalised light direction (world space, toward the light).
// Computed once at startup from the #define'd constants.
static float s_lightX, s_lightY, s_lightZ;

static void initLight()
{
    float mag = sqrtf(LIGHT_DIR_X*LIGHT_DIR_X +
                      LIGHT_DIR_Y*LIGHT_DIR_Y +
                      LIGHT_DIR_Z*LIGHT_DIR_Z);
    if (mag < 0.0001f) mag = 1.0f;
    s_lightX = LIGHT_DIR_X / mag;
    s_lightY = LIGHT_DIR_Y / mag;
    s_lightZ = LIGHT_DIR_Z / mag;
}

// Compute a lighting scale [0..1] for a surface normal (nx,ny,nz).
// Returns ambient + diffuse * max(0, dot(N, L)).
static inline float lightScale(float nx, float ny, float nz)
{
    float dot = nx*s_lightX + ny*s_lightY + nz*s_lightZ;
    if (dot < 0.0f) dot = 0.0f;
    float scale = LIGHT_AMBIENT + LIGHT_DIFFUSE * dot;
    if (scale > 1.0f) scale = 1.0f;
    return scale;
}

// Apply lighting scale to a u8 RGB and submit via glColor3b.
// glColor3b in libnds takes uint8 (0-255) and shifts >>3 internally.
static inline void glColorLit(u8 r, u8 g, u8 b, float scale)
{
    glColor3b(
        (u8)(r * scale),
        (u8)(g * scale),
        (u8)(b * scale)
    );
}

// ---------------------------------------------------------------------------
// Test floor — shown when no world.world found
// ---------------------------------------------------------------------------
static void drawTestFloor(float px, float pz)
{
    // No POLY_FORMAT_LIGHT0 — lighting is done in software via glColorLit().
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));

    const int   TILES = 4;
    const float S     = 2.0f;
    const float OX    = px - (TILES / 2) * S;
    const float OZ    = pz - (TILES / 2) * S;

    // Floor faces up — compute scale once.
    float scale = lightScale(0.0f, 1.0f, 0.0f);

    glBegin(GL_QUADS);
    for (int z = 0; z < TILES; z++) {
        for (int x = 0; x < TILES; x++) {
            if ((x + z) & 1) glColorLit(180, 120, 60, scale);   // warm brown
            else             glColorLit(100, 160, 80, scale);   // green

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
    // 1. GL first — before FAT or anything else touches VRAM
    setupScreens();
    setup3D();
    initLight();

    // 2. FAT + console
    fatInitDefault();
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256,
                31, 0, false, true);
    consoleSelect(&bottomConsole);

    // 3. Swap — static to live in BSS, not on the 16 KB ARM9 stack
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

    // 4. World — static for same stack reason
    static ChunkLibrary world(&mem);
    bool worldLoaded = world.loadWorld("fat:/Alone/world.world");

    consoleClear();
    iprintf("RAM after load:%lu\n", (unsigned long)mem.getFreeRAM());
    if (worldLoaded) {
        iprintf("World OK %lu chunks\n", (unsigned long)world.totalChunkCount());
        iprintf("Chunk list:\n");
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

    // 5. Game loop
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
        if (held & KEY_START) break;

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
            iprintf("FPS:%d\n", fpsDisplay);
            // Show lighting params for easy tuning
            iprintf("Amb:"); printFloat1dp(LIGHT_AMBIENT); iprintf("\n");
            iprintf("Dif:"); printFloat1dp(LIGHT_DIFFUSE); iprintf("\n");
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
