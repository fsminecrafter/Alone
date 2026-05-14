#pragma once

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

class MemoryManager;  // forward — include MemoryManager.h before this header

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
#define CHUNK_MAX_TEXTURES  8
#define CHUNK_GRID_RADIUS   1
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
    u8    widthLog2;     // 3=8px … 8=256px
    u8    heightLog2;
    u8    format;        // GL_RGBA, GL_RGB, GL_RGB4 … cast to GL_TEXTURE_TYPE_ENUM
    u32   dataBytes;
    // followed by dataBytes of raw texel data
};

// Vertex baked by editor — bottom faces already stripped
struct ChunkVertex {
    s16   x, y, z;      // local position in f32 fixed-point (floattof32)
    s16   nx, ny, nz;   // normal in f32, zero = unused
    u8    r, g, b;      // vertex colour 0-255
    u8    texId;         // 0xFF = no texture
    t16   u, v;          // texture coords (t16 = NDS texcoord fixed-point)
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

    // Call once per frame — streams one chunk in or out as player moves
    void update(float camX, float camZ);

    // Call once per frame after update — submits geometry to NDS GL
    void render();

    u32  loadedChunkCount() const;
    u32  totalChunkCount()  const { return worldChunkCount; }
    u32  lastFramePolys()   const { return framePolyCount; }

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

    void  renderChunk(Chunk* c);
    void  bindTexture(u8 texId);

    static s16 toGrid(float w) { return (s16)(w / CHUNK_WORLD_UNIT); }
};
