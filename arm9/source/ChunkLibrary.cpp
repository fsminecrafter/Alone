#include "MemoryManager.h"
#include "ChunkLibrary.h"

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

    // widthLog2 3=8px, so subtract 3 to get GL_TEXTURE_SIZE_ENUM offset
    GL_TEXTURE_SIZE_ENUM w = (GL_TEXTURE_SIZE_ENUM)(t.widthLog2  - 3);
    GL_TEXTURE_SIZE_ENUM h = (GL_TEXTURE_SIZE_ENUM)(t.heightLog2 - 3);

    glTexImage2D(0, 0, (GL_TEXTURE_TYPE_ENUM)t.format,
                 w, h, 0, TEXGEN_TEXCOORD, t.data);
}

// ---------------------------------------------------------------------------
// Index chunks — read all ChunkEntry headers, record file offsets,
// skip vertex data so we land at the next entry.
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

        // Skip vertex payload — loaded on demand
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
            if (loadChunk(desc, slot)) return; // one per frame
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
        if (!activeChunks[i].verts) continue;
        if (framePolyCount + activeChunks[i].polyCount > 2000) continue;
        renderChunk(&activeChunks[i]);
        framePolyCount += activeChunks[i].polyCount;
    }
}

void ChunkLibrary::renderChunk(Chunk* c)
{
    // Chunk world origin in f32 fixed-point
    s32 originX = floattof32((float)(c->gridX * CHUNK_WORLD_UNIT));
    s32 originZ = floattof32((float)(c->gridZ * CHUNK_WORLD_UNIT));

    u8 lastTexId = 0xFE; // force bind on first vertex

    // No lighting for now — vertex colour only, cull nothing (top-down safe)
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);

    glBegin(GL_TRIANGLES);

    for (u16 vi = 0; vi < c->vertCount; vi++) {
        ChunkVertex& v = c->verts[vi];

        if (v.texId != lastTexId) {
            glEnd();
            bindTexture(v.texId);
            glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
            glBegin(GL_TRIANGLES);
            lastTexId = v.texId;
        }

        glColor3b(v.r, v.g, v.b);

        if (v.texId != 0xFF)
            glTexCoord2t16(v.u, v.v);

        if (v.nx || v.ny || v.nz)
            glNormal3f(f32tofloat(v.nx), f32tofloat(v.ny), f32tofloat(v.nz));

        // Add chunk origin to local vertex position (both in f32)
        glVertex3v16(
            (s16)((v.x + originX) >> 12),
            (s16)(v.y >> 12),
            (s16)((v.z + originZ) >> 12)
        );
    }

    glEnd();
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

    // Allocate via MemoryManager if available, else plain malloc
    ChunkVertex* buf;
    if (memMgr)
        buf = (ChunkVertex*)memMgr->allocPage();
    else
        buf = (ChunkVertex*)malloc(byteSize);

    if (!buf) return false;

    fseek(worldFd, (long)(desc->fileOffset + sizeof(ChunkEntry)), SEEK_SET);
    if (fread(buf, sizeof(ChunkVertex), desc->vertCount, worldFd) != desc->vertCount) {
        if (memMgr) memMgr->freePage(buf); else free(buf);
        return false;
    }

    slot->verts     = buf;
    slot->gridX     = desc->gridX;
    slot->gridZ     = desc->gridZ;
    slot->vertCount = desc->vertCount;
    slot->polyCount = desc->polyCount;
    return true;
}

void ChunkLibrary::unloadChunk(Chunk* slot)
{
    if (!slot->verts) return;
    if (memMgr) memMgr->freePage(slot->verts);
    else        free(slot->verts);
    slot->verts     = nullptr;
    slot->vertCount = 0;
    slot->polyCount = 0;
}

u32 ChunkLibrary::loadedChunkCount() const
{
    u32 n = 0;
    for (int i = 0; i < CHUNK_GRID_SIZE; i++)
        if (activeChunks[i].verts) n++;
    return n;
}
