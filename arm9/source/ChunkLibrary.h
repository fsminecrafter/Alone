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
#define CHUNK_GRID_RADIUS   3           // load 7x7=49 but only DRAW visible ones
#define CHUNK_GRID_SIZE     ((CHUNK_GRID_RADIUS*2+1)*(CHUNK_GRID_RADIUS*2+1))
#define CHUNK_WORLD_UNIT    16          // world-space units per chunk side

// Half-diagonal of a chunk's AABB on the XZ plane (used for frustum sphere test).
// sqrt(2) * (CHUNK_WORLD_UNIT/2) ≈ 11.31 — keep a small margin.
#define CHUNK_HALF_DIAG     12.0f

// ---------------------------------------------------------------------------
// .world binary layout  (all little-endian, packed)
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
    u16   polyCount;
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
    ChunkVertex*  verts;          // nullptr = slot free
    bool          usedMemMgr;
    // Pre-computed split indices to avoid scanning verts twice each frame.
    // opaqueCount  = number of leading verts that are NOT billboards.
    // bbStart      = first billboard vert index (== opaqueCount).
    // bbCount      = number of billboard verts.
    u16           opaqueCount;
    u16           bbStart;
    u16           bbCount;
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

    // Stream chunks in/out around the camera position.
    void update(float camX, float camZ);

    // Submit visible geometry to NDS GL.  Must be called after setCamera().
    void render();

    u32  loadedChunkCount() const;
    u32  totalChunkCount()  const { return worldChunkCount; }
    u32  lastFramePolys()   const { return framePolyCount; }

    void getChunk0WorldPos(float& outX, float& outZ) const {
        if (worldChunkCount == 0) { outX = 0.0f; outZ = 0.0f; return; }
        outX = chunkDesc[0].gridX * (float)CHUNK_WORLD_UNIT;
        outZ = chunkDesc[0].gridZ * (float)CHUNK_WORLD_UNIT;
    }

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

    // Must be called each frame before render() with the same eye/target as
    // applyCamera().  Computes billboard orientation vectors AND the view
    // frustum planes used for culling.
    void setCamera(float eyeX, float eyeY, float eyeZ,
                   float tgtX, float tgtY, float tgtZ);

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
    FILE*          worldFd;

    WorldTexture  textures[CHUNK_MAX_TEXTURES];
    u16           textureCount;

    struct ChunkDesc {
        s16  gridX, gridZ;
        u32  fileOffset;
        u16  vertCount;
        u16  polyCount;
    };

    static const u32 MAX_WORLD_CHUNKS = 1024;
    ChunkDesc  chunkDesc[MAX_WORLD_CHUNKS];
    u32        worldChunkCount;

    Chunk  activeChunks[CHUNK_GRID_SIZE];
    u32    framePolyCount;

    // ---- Frustum (4 vertical planes — left/right/near/far in XZ) ----
    // Each plane: (nx, nz, d) where dot(point.xz, n) + d >= 0 = inside.
    // We only need XZ planes because the NDS view distance is the real
    // near/far limit; Y culling would rarely help on flat worlds.
    struct FrustumPlane { float nx, nz, d; };
    FrustumPlane frustum[4];   // left, right, near, far

    bool chunkInFrustum(s16 gx, s16 gz) const;

    bool  loadTextures();
    bool  indexChunks();
    void  uploadTexture(u16 idx);

    ChunkDesc* findDesc(s16 gx, s16 gz);
    Chunk*     findActive(s16 gx, s16 gz);
    Chunk*     findFreeSlot();
    bool       loadChunk(ChunkDesc* desc, Chunk* slot);
    void       unloadChunk(Chunk* slot);

    // Separate opaque and billboard rendering to minimise GL state changes.
    void  renderChunkOpaque   (Chunk* c);
    void  renderChunkBillboard(Chunk* c);

    // Bind a texture by its world tex-id; no-op if already bound.
    void  bindTexture(u8 texId);
    u8    lastBoundTexId;

    static inline bool isBillboardNx(s16 nx);
    static s16 toGrid(float w) { return (s16)floorf(w / (float)CHUNK_WORLD_UNIT); }
};
