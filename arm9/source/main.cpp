#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <math.h>
#include "MemoryManager.h"
#include "ChunkLibrary.h"
#include "ObjectSystem.h"
#include "AudioSystem.h"
#include "lighting.h"
#include "ipc_fifo.h"

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
    glDisable(GL_OUTLINE);
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
    fatInitDefault();
    setupScreens();
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256,
                31, 0, false, true);
    consoleSelect(&bottomConsole);
    iprintf("Boot OK\n");
    for (int i = 0; i < 60; i++) swiWaitForVBlank();  // can you see this?

    setup3D();
    
    // Skip initLight() and applyTimeOfDay() to isolate hang
    initLight();
    applyTimeOfDay();
    
    // Drain any debug messages from ARM7 startup before initializing logger.
    // ARM7 sends 0x20 command with u16 status code.
    for (int i = 0; i < 100; i++) {
        if (fifoCheckValue32(FIFO_USER_01)) {
            u32 v = fifoGetValue32(FIFO_USER_01);
            u8 cmd = (u8)((v >> 24) & 0xFF);
            u16 status = (u16)(v & 0xFFFF);
            if (cmd == 0x20) {  // AUDIO_CMD_DEBUG
                iprintf("ARM7 debug: 0x%04X\n", (unsigned)status);
            }
        } else {
            break;  // No more messages waiting
        }
    }
    
    // Initialize persistent logger (writes to fat:/Alone/log.txt)
    iprintf("Initializing logger...\n");
    logger_init();
    iprintf("Logger OK\n");

    static MemoryManager mem;
    mem.setSwappiness(30);

    iprintf("STAGE 3: About to check world.loadWorld\n");
    
    // Check if world file exists
    FILE* testFd = fopen("fat:/Alone/world.world", "rb");
    if (testFd) {
        fclose(testFd);
        iprintf("world.world EXISTS on SD\n");
    } else {
        iprintf("ERROR: world.world NOT FOUND\n");
        iprintf("Check fat:/Alone/ path\n");
    }
    
    static ChunkLibrary world(&mem);
    bool worldLoaded = world.loadWorld("fat:/Alone/world.world");
    
    iprintf("STAGE 4: world load result=%d\n", worldLoaded);

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

    // ObjectSystem: re-open world file to read v2 sections (OBJS/TAGS/AUDI).
    // ChunkLibrary::loadWorld() reads textures + chunks then leaves the fd open;
    // here we call loadFromWorld() which reads the sections that follow.
    if (worldLoaded) {
        FILE* wfd = fopen("fat:/Alone/world.world", "rb");
        if (wfd) {
            // Seek past the v1 content to the v2 sections.
            // The simplest approach: ObjectSystem::loadFromWorld() reads magic bytes
            // and silently succeeds on pre-v2 files, so it's safe to call regardless.
            // We need to position the fd after the last ChunkEntry + vertex block.
            // ChunkLibrary has already indexed all chunks; skip to end-of-v1 data.
            WorldHeader whdr;
            fread(&whdr, sizeof(whdr), 1, wfd);
            // Skip textures
            for (u16 ti = 0; ti < whdr.textureCount; ti++) {
                u8 tid, wl, hl, fmt; u32 dbytes;
                struct { u8 id, wl, hl, fmt; u32 bytes; } te;
                fread(&te, sizeof(te), 1, wfd);
                fseek(wfd, te.bytes, SEEK_CUR);
            }
            // Skip chunks
            for (u32 ci = 0; ci < whdr.chunkCount; ci++) {
                s16 gx, gz; u16 vc, pc;
                fread(&gx, 2, 1, wfd); fread(&gz, 2, 1, wfd);
                fread(&vc, 2, 1, wfd); fread(&pc, 2, 1, wfd);
                fseek(wfd, (long)(vc * 20), SEEK_CUR); // sizeof(ChunkVertex) = 20
            }
            g_objects.loadFromWorld(wfd);
            fclose(wfd);
        }
        g_objects.fireStart();
        g_audio.startPlaylist();
    }

    for (int i = 0; i < 180; i++) swiWaitForVBlank();
    // DON'T clear console - keep it visible for diagnostics
    iprintf("\n=== Main loop starting ===\n");

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
        
        // Diagnostic: print every 120 frames (~2 seconds)
        if ((frame % 120) == 0) {
            iprintf("Frame %d: cam=[%.1f, %.1f] vel=[%.2f, %.2f]\n",
                    frame, px, pz, vx, vz);
        }
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
            // Per-frame script and audio updates
            g_objects.update(frame, px, eyeY, eyeZ);
            g_audio.update(px, eyeY, eyeZ);
        } else {
            drawTestFloor(px, pz);
        }

        glFlush(0);  // 0 = default Y-sorting of translucent polygons
        swiWaitForVBlank();

        if (frame % 20 == 0) {
            float sunrise, daylight;
            getSeasonParams(sunrise, daylight);

            // Clear and redraw full menu
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
            iprintf("\n");
            
            // ---- Audio Menu Section ----
            iprintf("=== MUSIC ===\n");
            if (g_audio.getPlaylistTrackCount() > 0) {
                u8 trackIdx = g_audio.getCurrentTrackIndex();
                u32 posSec = g_audio.getPlaybackPositionSeconds();
                int sampleRate, bits;
                bool isAdpcm;
                g_audio.getTrackFormatInfo(sampleRate, bits, isAdpcm);
                
                const char* trackName = g_audio.getCurrentTrackFilename();
                const char* slashPos = trackName ? strrchr(trackName, '/') : nullptr;
                if (slashPos) trackName = slashPos + 1;
                if (!trackName) trackName = "???";
                
                iprintf("Track: [%d/%d] %s\n",
                    (int)trackIdx+1, (int)g_audio.getPlaylistTrackCount(),
                    trackName);
                iprintf("Time: %us\n", (unsigned)posSec);
                if (sampleRate > 0) {
                    iprintf("Format: %dHz %s\n",
                        sampleRate,
                        isAdpcm ? "IMA-ADPCM" : (bits == 16 ? "PCM16" : "PCM8"));
                }
            } else {
                iprintf("(No playlist)\n");
            }
            
            iprintf("\nRAM:%lu\n", (unsigned long)mem.getFreeRAM());
        }
        frame++;
    }

    world.unloadWorld();
    return 0;
}
