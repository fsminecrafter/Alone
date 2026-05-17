#include "MemoryManager.h"
#include "ChunkLibrary.h"
#include "lighting.h"

extern float g_lightX, g_lightY, g_lightZ;

// Billboard mode sentinels stored in nx (must match editor.py):
//   0x7FFF  cylindrical -- rotates around Y only, stays vertical
//   0x7FFE  spherical   -- fully faces camera (tilts too)
//   0x7FFD  fixed       -- no rotation, always faces world +Z
#define BILLBOARD_SENTINEL      0x7FFF
#define BILLBOARD_SENTINEL_SPH  0x7FFE
#define BILLBOARD_SENTINEL_FIX  0x7FFD

static inline bool isBillboardNx(s16 nx) {
    return nx == (s16)BILLBOARD_SENTINEL ||
           nx == (s16)BILLBOARD_SENTINEL_SPH ||
           nx == (s16)BILLBOARD_SENTINEL_FIX;
}

// ---------------------------------------------------------------------------
// Cylindrical billboard orientation
// ---------------------------------------------------------------------------
// Billboard vertex layout (nx == BILLBOARD_SENTINEL):
//   v.x, v.y, v.z  = chunk-local anchor (base of sprite, same for all 6 verts)
//   v.ny            = camera-RIGHT offset in NDS f32 (+/-half-width)
//   v.nz            = world-UP offset in NDS f32 (0=bottom edge, height=top)
//
// Final world pos = translate(chunk_origin) + anchor
//                 + right * ny   (horizontal spread, faces camera)
//                 + up    * nz   (vertical rise, always Y-up)
//
// "Cylindrical" = sprite always stands vertically, only rotates around Y.

static float s_bbRightX = 1.0f;  // global right (spherical/fallback only)
static float s_bbRightZ = 0.0f;
// Up vector for spherical billboards (camera-derived); cylindrical always uses world Y.
static float s_bbUpX = 0.0f;
static float s_bbUpY = 1.0f;
static float s_bbUpZ = 0.0f;
// Camera eye position — used to compute per-billboard right vector for cylindrical mode.
static float s_camEyeX = 0.0f;
static float s_camEyeY = 0.0f;
static float s_camEyeZ = 0.0f;

static void updateCameraVectors(float eyeX, float eyeY, float eyeZ,
                                 float tgtX, float tgtY, float tgtZ)
{
    // Full 3-D forward (eye -> target).
    float fwdX = tgtX - eyeX;
    float fwdY = tgtY - eyeY;
    float fwdZ = tgtZ - eyeZ;
    float flen = sqrtf(fwdX*fwdX + fwdY*fwdY + fwdZ*fwdZ);
    s_camEyeX = eyeX; s_camEyeY = eyeY; s_camEyeZ = eyeZ;
    if (flen < 0.0001f) {
        s_bbRightX = 1.0f; s_bbRightZ = 0.0f;
        s_bbUpX = 0.0f; s_bbUpY = 1.0f; s_bbUpZ = 0.0f;
        return;
    }
    fwdX /= flen; fwdY /= flen; fwdZ /= flen;

    // right = forward x world-up(0,1,0)  =>  (-fwd.z, 0, fwd.x)
    float rx = -fwdZ;
    float rz =  fwdX;
    float rlen = sqrtf(rx*rx + rz*rz);
    if (rlen < 0.0001f) {
        // Camera pointing straight down/up; use world +X as fallback
        s_bbRightX = 1.0f; s_bbRightZ = 0.0f;
        s_bbUpX = 0.0f; s_bbUpY = 0.0f; s_bbUpZ = -1.0f;
        return;
    }
    s_bbRightX = rx / rlen;
    s_bbRightZ = rz / rlen;

    // Store eye for per-billboard cylindrical right computation
    s_camEyeX = eyeX; s_camEyeY = eyeY; s_camEyeZ = eyeZ;

    // Spherical camera-derived up = right x forward
    // right = (s_bbRightX, 0, s_bbRightZ)
    float ux = 0.0f   * fwdZ - s_bbRightZ * fwdY;
    float uy = s_bbRightZ * fwdX - s_bbRightX * fwdZ;
    float uz = s_bbRightX * fwdY - 0.0f   * fwdX;
    float ulen = sqrtf(ux*ux + uy*uy + uz*uz);
    if (ulen > 0.0001f) { ux/=ulen; uy/=ulen; uz/=ulen; }
    else                 { ux=0.0f; uy=1.0f; uz=0.0f; }
    s_bbUpX = ux; s_bbUpY = uy; s_bbUpZ = uz;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
ChunkLibrary::ChunkLibrary(MemoryManager* mem)
    : memMgr(mem), worldFd(nullptr),
      textureCount(0), worldChunkCount(0), framePolyCount(0)
{
    memset(textures,     0, sizeof(textures));
    memset(chunkDesc,    0, sizeof(chunkDesc));
    memset(activeChunks, 0, sizeof(activeChunks));
    for (int i = 0; i < CHUNK_MAX_TEXTURES; i++)
        textures[i].glTexId = -1;
}

ChunkLibrary::~ChunkLibrary() { unloadWorld(); }

// ---------------------------------------------------------------------------
// loadWorld / unloadWorld
// ---------------------------------------------------------------------------
bool ChunkLibrary::loadWorld(const char* path)
{
    worldFd = fopen(path, "rb");
    if (!worldFd) return false;
    setvbuf(worldFd, nullptr, _IONBF, 0);

    WorldHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, worldFd) != 1)  return false;
    if (memcmp(hdr.magic, "ALWF", 4) != 0)           return false;
    if (hdr.version != 1)                            return false;

    textureCount    = hdr.textureCount;
    worldChunkCount = hdr.chunkCount;

    if (!loadTextures()) return false;
    if (!indexChunks())  return false;
    return true;
}

void ChunkLibrary::unloadWorld()
{
    for (int i = 0; i < CHUNK_GRID_SIZE; i++)
        unloadChunk(&activeChunks[i]);
    for (int i = 0; i < CHUNK_MAX_TEXTURES; i++) {
        if (textures[i].data) { free(textures[i].data); textures[i].data = nullptr; }
        if (textures[i].glTexId >= 0) {
            glDeleteTextures(1, &textures[i].glTexId);
            textures[i].glTexId = -1;
        }
    }
    if (worldFd) { fclose(worldFd); worldFd = nullptr; }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
bool ChunkLibrary::loadTextures()
{
    for (u16 i = 0; i < textureCount && i < CHUNK_MAX_TEXTURES; i++) {
        TextureEntry te;
        if (fread(&te, sizeof(te), 1, worldFd) != 1) return false;

        textures[i].id         = te.id;
        textures[i].widthLog2  = te.widthLog2;
        textures[i].heightLog2 = te.heightLog2;
        textures[i].format     = te.format;
        textures[i].dataBytes  = te.dataBytes;
        textures[i].glTexId    = -1;
        textures[i].data       = (u8*)malloc(te.dataBytes);
        if (!textures[i].data) return false;
        if (fread(textures[i].data, 1, te.dataBytes, worldFd) != te.dataBytes)
            return false;
        uploadTexture(i);
    }
    return true;
}

void ChunkLibrary::uploadTexture(u16 idx)
{
    WorldTexture& t = textures[idx];
    if (t.glTexId >= 0 || !t.data) return;
    glGenTextures(1, &t.glTexId);
    glBindTexture(0, t.glTexId);
    GL_TEXTURE_SIZE_ENUM w = (GL_TEXTURE_SIZE_ENUM)(t.widthLog2 - 3);
    GL_TEXTURE_SIZE_ENUM h = (GL_TEXTURE_SIZE_ENUM)(t.heightLog2 - 3);
    glTexImage2D(0, 0, (GL_TEXTURE_TYPE_ENUM)t.format, w, h, 0, TEXGEN_TEXCOORD, t.data);
    free(t.data);
    t.data = nullptr;
}

// ---------------------------------------------------------------------------
// Index chunks
// ---------------------------------------------------------------------------
bool ChunkLibrary::indexChunks()
{
    for (u32 i = 0; i < worldChunkCount && i < MAX_WORLD_CHUNKS; i++) {
        chunkDesc[i].fileOffset = (u32)ftell(worldFd);
        ChunkEntry ce;
        if (fread(&ce, sizeof(ce), 1, worldFd) != 1) return false;
        chunkDesc[i].gridX     = ce.gridX;
        chunkDesc[i].gridZ     = ce.gridZ;
        chunkDesc[i].vertCount = ce.vertCount;
        chunkDesc[i].polyCount = ce.polyCount;
        fseek(worldFd, (long)(sizeof(ChunkVertex) * ce.vertCount), SEEK_CUR);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Streaming update
// ---------------------------------------------------------------------------
void ChunkLibrary::update(float camX, float camZ)
{
    s16 camGX = toGrid(camX);
    s16 camGZ = toGrid(camZ);

    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk& c = activeChunks[i];
        if (!c.verts) continue;
        s16 dx = c.gridX - camGX;
        s16 dz = c.gridZ - camGZ;
        if (dx < -CHUNK_GRID_RADIUS || dx > CHUNK_GRID_RADIUS ||
            dz < -CHUNK_GRID_RADIUS || dz > CHUNK_GRID_RADIUS)
            unloadChunk(&c);
    }

    for (s16 dz = -CHUNK_GRID_RADIUS; dz <= CHUNK_GRID_RADIUS; dz++) {
        for (s16 dx = -CHUNK_GRID_RADIUS; dx <= CHUNK_GRID_RADIUS; dx++) {
            s16 gx = camGX + dx;
            s16 gz = camGZ + dz;
            if (findActive(gx, gz)) continue;
            ChunkDesc* desc = findDesc(gx, gz);
            if (!desc) continue;
            Chunk* slot = findFreeSlot();
            if (!slot) return;
            if (loadChunk(desc, slot)) return;
        }
    }
}

// ---------------------------------------------------------------------------
// Render — two passes so translucent billboards sort correctly.
//
// NDS transparency rule: translucent polys (POLY_ALPHA 1-30) must have a
// POLY_ID different from all opaque polys, and must be drawn AFTER opaque
// geometry so the depth buffer is already populated.
//
// Pass 1: opaque geometry only (POLY_ID 1, POLY_ALPHA 31)
// Pass 2: billboard geometry only (POLY_ID 2, POLY_ALPHA 31 for opaque
//         pixels; the texture's A1 bit handles per-pixel cutout)
// ---------------------------------------------------------------------------
void ChunkLibrary::render()
{
    framePolyCount = 0;

    // Pass 1 — terrain / opaque objects
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts) continue;
        renderChunk(c, i, false);
        framePolyCount += c->polyCount;
    }

    // Pass 2 — billboard quads (drawn after depth buffer is settled)
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts) continue;
        renderChunk(c, i, true);
    }
}

void ChunkLibrary::setCamera(float eyeX, float eyeY, float eyeZ,
                              float tgtX, float tgtY, float tgtZ)
{
    updateCameraVectors(eyeX, eyeY, eyeZ, tgtX, tgtY, tgtZ);
}

void ChunkLibrary::renderChunk(Chunk* c, int /*debugIdx*/, bool billboardsOnly)
{
    // Translate by chunk origin so local verts stay in [-8, 8] (NDS 4.12 range)
    glPushMatrix();
    glTranslatef(
        (float)(c->gridX * CHUNK_WORLD_UNIT),
        0.0f,
        (float)(c->gridZ * CHUNK_WORLD_UNIT)
    );

    // Pass 1 uses POLY_ID 1 (opaque terrain).
    // Pass 2 uses POLY_ID 2 (billboard quads, drawn after depth buffer is settled).
    // Using a different POLY_ID for translucent polys is required by the NDS GPU.
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(billboardsOnly ? 2 : 1));

    u8   lastTexId   = 0xFE;
    bool inBillboard = false;
    bool anyDrawn    = false;

    glBegin(GL_TRIANGLES);

    for (u16 vi = 0; vi < c->vertCount; vi++) {
        ChunkVertex& v = c->verts[vi];

        bool isBB = isBillboardNx(v.nx);

        // Skip vertices that don't belong to this pass
        if (isBB != billboardsOnly) continue;

        if (v.texId != lastTexId) {
            glEnd();
            bindTexture(v.texId);
            glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(billboardsOnly ? 2 : 1));
            glBegin(GL_TRIANGLES);
            lastTexId   = v.texId;
        }
        anyDrawn = true;
        inBillboard = isBB;

        if (isBB) {
            // Billboard vertex:
            //   v.x/y/z = chunk-local anchor in NDS f32 (same for all 6 verts)
            //   v.ny    = camera-right offset in NDS f32 (+/-half-width)
            //   v.nz    = world-up offset in NDS f32 (0=bottom, height=top)
            //   v.nx    = sentinel encoding the mode
            float anchorX = f32tofloat(v.x);
            float anchorY = f32tofloat(v.y);
            float anchorZ = f32tofloat(v.z);
            float offR    = f32tofloat(v.ny);   // spread along right
            float offU    = f32tofloat(v.nz);   // rise along up

            float lx, ly, lz;
            float lightNX, lightNY, lightNZ;

            if (v.nx == (s16)BILLBOARD_SENTINEL_FIX) {
                // Fixed: always spread along world +X, rise along world +Y
                lx = anchorX + offR;
                ly = anchorY + offU;
                lz = anchorZ;
                lightNX = 0.0f; lightNY = 0.5f; lightNZ = 1.0f;

            } else if (v.nx == (s16)BILLBOARD_SENTINEL_SPH) {
                // Spherical: spread along camera right, rise along camera up
                lx = anchorX + offR * s_bbRightX + offU * s_bbUpX;
                ly = anchorY + offR * 0.0f        + offU * s_bbUpY;
                lz = anchorZ + offR * s_bbRightZ  + offU * s_bbUpZ;
                lightNX = s_bbUpZ; lightNY = s_bbUpY; lightNZ = -s_bbUpX;

            } else {
                // Cylindrical (default 0x7FFF): per-billboard right from eye -> anchor.
                // Compute world-space anchor so the right vector is correct for THIS
                // billboard regardless of camera yaw (which never changes in a fixed
                // top-down camera).
                float chunkOX = (float)(c->gridX * CHUNK_WORLD_UNIT);
                float chunkOZ = (float)(c->gridZ * CHUNK_WORLD_UNIT);
                float wax = chunkOX + anchorX;
                float waz = chunkOZ + anchorZ;

                // Forward from camera eye to billboard anchor (XZ only — cylindrical)
                float cfx = wax - s_camEyeX;
                float cfz = waz - s_camEyeZ;
                float cflen = sqrtf(cfx*cfx + cfz*cfz);
                float crx, crz;
                if (cflen > 0.0001f) {
                    cfx /= cflen; cfz /= cflen;
                    // right = forward rotated 90 CW around Y: (-fwdZ, 0, fwdX)
                    crx = -cfz;
                    crz =  cfx;
                } else {
                    crx = s_bbRightX;
                    crz = s_bbRightZ;
                }

                lx = anchorX + offR * crx;
                ly = anchorY + offU;
                lz = anchorZ + offR * crz;
                lightNX = crz; lightNY = 0.5f; lightNZ = -crx;
            }

            float scale = lightScale(lightNX, lightNY, lightNZ);
            glColorLit(v.r, v.g, v.b, scale);
            if (v.texId != 0xFF)
                glTexCoord2t16(v.u, v.v);
            glVertex3f(lx, ly, lz);

        } else {
            // Normal geometry vertex
            if (v.nx || v.ny || v.nz) {
                float scale = lightScale(f32tofloat(v.nx),
                                         f32tofloat(v.ny),
                                         f32tofloat(v.nz));
                glColorLit(v.r, v.g, v.b, scale);
            } else {
                glColor3b(v.r, v.g, v.b);
            }
            if (v.texId != 0xFF)
                glTexCoord2t16(v.u, v.v);
            glVertex3f(f32tofloat(v.x), f32tofloat(v.y), f32tofloat(v.z));
        }
    }

    glEnd();
    glPopMatrix(1);
}

void ChunkLibrary::bindTexture(u8 texId)
{
    if (texId == 0xFF) { glBindTexture(0, 0); return; }
    for (u16 i = 0; i < textureCount; i++) {
        if (textures[i].id == texId && textures[i].glTexId >= 0) {
            glBindTexture(0, textures[i].glTexId);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
ChunkLibrary::ChunkDesc* ChunkLibrary::findDesc(s16 gx, s16 gz)
{
    for (u32 i = 0; i < worldChunkCount; i++)
        if (chunkDesc[i].gridX == gx && chunkDesc[i].gridZ == gz)
            return &chunkDesc[i];
    return nullptr;
}

Chunk* ChunkLibrary::findActive(s16 gx, s16 gz)
{
    for (int i = 0; i < CHUNK_GRID_SIZE; i++)
        if (activeChunks[i].verts &&
            activeChunks[i].gridX == gx && activeChunks[i].gridZ == gz)
            return &activeChunks[i];
    return nullptr;
}

Chunk* ChunkLibrary::findFreeSlot()
{
    for (int i = 0; i < CHUNK_GRID_SIZE; i++)
        if (!activeChunks[i].verts)
            return &activeChunks[i];
    return nullptr;
}

bool ChunkLibrary::loadChunk(ChunkDesc* desc, Chunk* slot)
{
    u32 byteSize = sizeof(ChunkVertex) * desc->vertCount;
    ChunkVertex* buf = (ChunkVertex*)malloc(byteSize);
    if (!buf) return false;
    fseek(worldFd, (long)(desc->fileOffset + sizeof(ChunkEntry)), SEEK_SET);
    size_t got = fread(buf, sizeof(ChunkVertex), desc->vertCount, worldFd);
    if (got != desc->vertCount) { free(buf); return false; }
    slot->verts      = buf;
    slot->gridX      = desc->gridX;
    slot->gridZ      = desc->gridZ;
    slot->vertCount  = desc->vertCount;
    slot->polyCount  = desc->polyCount;
    slot->usedMemMgr = false;
    return true;
}

void ChunkLibrary::unloadChunk(Chunk* slot)
{
    if (!slot->verts) return;
    if (slot->usedMemMgr) memMgr->freePage(slot->verts);
    else                  free(slot->verts);
    slot->verts      = nullptr;
    slot->vertCount  = 0;
    slot->polyCount  = 0;
    slot->usedMemMgr = false;
}

u32 ChunkLibrary::loadedChunkCount() const
{
    u32 n = 0;
    for (int i = 0; i < CHUNK_GRID_SIZE; i++)
        if (activeChunks[i].verts) n++;
    return n;
}
