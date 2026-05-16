#pragma once

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

class MemoryManager;  // forward — include MemoryManager.h before this header

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
#define CHUNK_MAX_TEXTURES  8
#define CHUNK_GRID_RADIUS   2
#define CHUNK_GRID_SIZE     ((CHUNK_GRID_RADIUS*2+1)*(CHUNK_GRID_RADIUS*2+1))
#define CHUNK_WORLD_UNIT    16      // world-space units per chunk side

// ---------------------------------------------------------------------------
// .world binary layout  (all little-endian, packed)
//
//   WorldHeader
//   TextureEntry[textureCount]   each immediately followed by raw texel bytes
//   ChunkEntry  [chunkCount]     each immediately followed by ChunkVertex[]
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

struct WorldHeader {
    char  magic[4];      // "ALWF"
    u16   version;       // 1
    u16   textureCount;
    u32   chunkCount;
    s16   chunkUnitSize; // should match CHUNK_WORLD_UNIT
};

struct TextureEntry {
    u8    id;
    u8    widthLog2;     // 3=8px ... 8=256px
    u8    heightLog2;
    u8    format;        // GL_RGBA, GL_RGB, GL_RGB4 ... cast to GL_TEXTURE_TYPE_ENUM
    u32   dataBytes;
    // followed by dataBytes of raw texel data
};

// Vertex baked by the editor.
// x, y, z  — local position in NDS f32 fixed-point (float * 4096), stored as
//             s16. Range: +/-8.0 world units fits in +/-32768 fp units, exactly
//             at the s16 limit. The editor clamps values before writing.
// nx,ny,nz — surface normal in f32, zero = unused.
// texId    — 0xFF = no texture.
// u, v     — NDS t16 texture coordinates (texel * 16).
struct ChunkVertex {
    s16   x, y, z;
    s16   nx, ny, nz;
    u8    r, g, b;
    u8    texId;
    t16   u, v;
};

struct ChunkEntry {
    s16   gridX, gridZ;
    u16   vertCount;
    u16   polyCount;     // triangle count
    // followed by vertCount * ChunkVertex
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Runtime types
// ---------------------------------------------------------------------------
struct WorldTexture {
    u8   id;
    u8   widthLog2, heightLog2;
    u8   format;
    int  glTexId;   // -1 = not uploaded
    u8*  data;
    u32  dataBytes;
};

struct Chunk {
    s16           gridX, gridZ;
    u16           vertCount;
    u16           polyCount;
    ChunkVertex*  verts;        // nullptr = slot free
    bool          usedMemMgr;   // true = free via MemoryManager, false = free()
};

// ---------------------------------------------------------------------------
// ChunkLibrary
// ---------------------------------------------------------------------------
class ChunkLibrary {
public:
    explicit ChunkLibrary(MemoryManager* mem);
    ~ChunkLibrary();

    bool loadWorld(const char* path);
    void unloadWorld();

    // Call once per frame — streams chunks in or out as the player moves
    void update(float camX, float camZ);

    // Call once per frame after update — submits geometry to NDS GL
    void render();

    u32  loadedChunkCount() const;
    u32  totalChunkCount()  const { return worldChunkCount; }
    u32  lastFramePolys()   const { return framePolyCount; }

    // World-space centre of chunk 0 — use as a starting camera position.
    void getChunk0WorldPos(float& outX, float& outZ) const {
        if (worldChunkCount == 0) { outX = 0.0f; outZ = 0.0f; return; }
        outX = chunkDesc[0].gridX * (float)CHUNK_WORLD_UNIT;
        outZ = chunkDesc[0].gridZ * (float)CHUNK_WORLD_UNIT;
    }

    // World-space centroid of all chunks — better starting camera position.
    void getWorldCenter(float& outX, float& outZ) const {
        if (worldChunkCount == 0) { outX = 0.0f; outZ = 0.0f; return; }
        float sumX = 0.0f, sumZ = 0.0f;
        for (u32 i = 0; i < worldChunkCount; i++) {
            sumX += chunkDesc[i].gridX;
            sumZ += chunkDesc[i].gridZ;
        }
        outX = (sumX / (float)worldChunkCount) * (float)CHUNK_WORLD_UNIT;
        outZ = (sumZ / (float)worldChunkCount) * (float)CHUNK_WORLD_UNIT;
    }

    // Debug: read back a chunk descriptor by index
    void getChunkInfo(u32 idx, s16& gx, s16& gz, u16& vc) const {
        if (idx < worldChunkCount) {
            gx = chunkDesc[idx].gridX;
            gz = chunkDesc[idx].gridZ;
            vc = chunkDesc[idx].vertCount;
        } else {
            gx = gz = 0; vc = 0;
        }
    }

private:
    MemoryManager* memMgr;

    FILE*  worldFd;

    WorldTexture  textures[CHUNK_MAX_TEXTURES];
    u16           textureCount;

    struct ChunkDesc {
        s16  gridX, gridZ;
        u32  fileOffset;  // byte offset of ChunkEntry header
        u16  vertCount;
        u16  polyCount;
    };

    static const u32 MAX_WORLD_CHUNKS = 1024;
    ChunkDesc  chunkDesc[MAX_WORLD_CHUNKS];
    u32        worldChunkCount;

    Chunk  activeChunks[CHUNK_GRID_SIZE];
    u32    framePolyCount;

    bool  loadTextures();
    bool  indexChunks();
    void  uploadTexture(u16 idx);

    ChunkDesc* findDesc(s16 gx, s16 gz);
    Chunk*     findActive(s16 gx, s16 gz);
    Chunk*     findFreeSlot();
    bool       loadChunk(ChunkDesc* desc, Chunk* slot);
    void       unloadChunk(Chunk* slot);

    void  renderChunk(Chunk* c, int debugIdx);
    void  bindTexture(u8 texId);

    // floorf ensures negative coords round toward -inf, not zero.
    // Without this, toGrid(-0.1) returns 0 instead of -1.
    static s16 toGrid(float w) {
        return (s16)floorf(w / (float)CHUNK_WORLD_UNIT);
    }
};
