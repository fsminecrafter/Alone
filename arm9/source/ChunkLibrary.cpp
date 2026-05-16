#include "MemoryManager.h"
#include "ChunkLibrary.h"
#include "lighting.h"

extern float g_lightX, g_lightY, g_lightZ;

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

ChunkLibrary::~ChunkLibrary()
{
    unloadWorld();
}

// ---------------------------------------------------------------------------
// loadWorld
// ---------------------------------------------------------------------------
bool ChunkLibrary::loadWorld(const char* path)
{
    worldFd = fopen(path, "rb");
    if (!worldFd) return false;
    setvbuf(worldFd, nullptr, _IONBF, 0);

    WorldHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, worldFd) != 1)    return false;
    if (memcmp(hdr.magic, "ALWF", 4) != 0)             return false;
    if (hdr.version != 1)                              return false;

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
        if (textures[i].data) {
            free(textures[i].data);
            textures[i].data = nullptr;
        }
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

    glTexImage2D(0, 0, (GL_TEXTURE_TYPE_ENUM)t.format,
                 w, h, 0, TEXGEN_TEXCOORD, t.data);

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

    // Evict out-of-range chunks
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk& c = activeChunks[i];
        if (!c.verts) continue;
        s16 dx = c.gridX - camGX;
        s16 dz = c.gridZ - camGZ;
        if (dx < -CHUNK_GRID_RADIUS || dx > CHUNK_GRID_RADIUS ||
            dz < -CHUNK_GRID_RADIUS || dz > CHUNK_GRID_RADIUS)
            unloadChunk(&c);
    }

    // Load one missing chunk per frame
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
// Render
// ---------------------------------------------------------------------------
void ChunkLibrary::render()
{
    framePolyCount = 0;

    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts) continue;
        renderChunk(c, i);
        framePolyCount += c->polyCount;
    }
}

void ChunkLibrary::renderChunk(Chunk* c, int /*debugIdx*/)
{
    // KEY FIX: The NDS GPU vertex coordinate range is +-7.999 world units
    // (4-bit integer part in 4.12 fixed point). Adding the chunk world origin
    // to each vertex in float and passing via glVertex3f does NOT work for
    // chunks beyond grid (0,0) because the hardware clamps/wraps the value.
    //
    // Solution: push the modelview matrix, translate by the chunk origin,
    // then submit vertices in local [-8, 8] space which always fits in range.
    glPushMatrix();
    glTranslatef(
        (float)(c->gridX * CHUNK_WORLD_UNIT),
        0.0f,
        (float)(c->gridZ * CHUNK_WORLD_UNIT)
    );

    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));

    u8 lastTexId = 0xFE;

    glBegin(GL_TRIANGLES);

    for (u16 vi = 0; vi < c->vertCount; vi++) {
        ChunkVertex& v = c->verts[vi];

        if (v.texId != lastTexId) {
            glEnd();
            bindTexture(v.texId);
            glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
            glBegin(GL_TRIANGLES);
            lastTexId = v.texId;
        }

        if (v.nx || v.ny || v.nz) {
            float scale = lightScale(f32tofloat(v.nx), f32tofloat(v.ny), f32tofloat(v.nz));
            glColorLit(v.r, v.g, v.b, scale);
        } else {
            glColor3b(v.r, v.g, v.b);
        }

        if (v.texId != 0xFF)
            glTexCoord2t16(v.u, v.v);

        // Vertices are in local chunk space [-8, 8] — safe for NDS hardware.
        // The glTranslate above positions them correctly in the world.
        glVertex3f(f32tofloat(v.x), f32tofloat(v.y), f32tofloat(v.z));
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
    if (got != desc->vertCount) {
        free(buf);
        return false;
    }

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
