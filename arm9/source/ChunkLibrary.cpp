#include "MemoryManager.h"
#include "ChunkLibrary.h"
#include "lighting.h"

extern float g_lightX, g_lightY, g_lightZ;

// Billboard mode sentinels stored in nx (must match editor.py):
//   0x7FFF  cylindrical
//   0x7FFE  spherical
//   0x7FFD  fixed
#define BILLBOARD_SENTINEL      0x7FFF
#define BILLBOARD_SENTINEL_SPH  0x7FFE
#define BILLBOARD_SENTINEL_FIX  0x7FFD

inline bool ChunkLibrary::isBillboardNx(s16 nx) {
    return nx == (s16)BILLBOARD_SENTINEL  ||
           nx == (s16)BILLBOARD_SENTINEL_SPH ||
           nx == (s16)BILLBOARD_SENTINEL_FIX;
}

// ---------------------------------------------------------------------------
// Camera / frustum state
// ---------------------------------------------------------------------------
static float s_camEyeX = 0.0f;
static float s_camEyeY = 0.0f;
static float s_camEyeZ = 0.0f;

static float s_bbRightX = 1.0f, s_bbRightY = 0.0f, s_bbRightZ = 0.0f;
static float s_bbUpX    = 0.0f, s_bbUpY    = 1.0f, s_bbUpZ    = 0.0f;

// ---------------------------------------------------------------------------
// setCamera — builds billboard vectors AND the 4-plane XZ frustum.
//
// Frustum construction (view-space, projected onto XZ):
//   We derive left / right / near / far planes from the camera orientation
//   and a hard-coded horizontal FOV that matches the NDS perspective setup
//   (45° vertical FOV, 256/192 aspect → ~58° horizontal FOV).
//   A small margin (+1 chunk half-diagonal) is added to each plane so
//   chunks that overlap the edge are never popped out prematurely.
// ---------------------------------------------------------------------------
void ChunkLibrary::setCamera(float eyeX, float eyeY, float eyeZ,
                              float tgtX, float tgtY, float tgtZ)
{
    s_camEyeX = eyeX;
    s_camEyeY = eyeY;
    s_camEyeZ = eyeZ;

    // Forward (XZ only)
    float fwdX = tgtX - eyeX;
    float fwdZ = tgtZ - eyeZ;
    float flen = sqrtf(fwdX*fwdX + fwdZ*fwdZ);
    if (flen < 0.0001f) return;
    fwdX /= flen;  fwdZ /= flen;

    // Right (perpendicular in XZ, 90° CW: (x,z) → (z,-x))
    float rx =  fwdZ;
    float rz = -fwdX;

    // Billboard right/up (full 3-D)
    s_bbRightX = rx;  s_bbRightY = 0.0f;  s_bbRightZ = rz;

    float fwdY  = tgtY - eyeY;
    float flen3 = sqrtf(fwdX*fwdX + fwdY*fwdY + fwdZ*fwdZ);
    if (flen3 > 0.0001f) {
        float fx3 = fwdX/flen3, fy3 = fwdY/flen3, fz3 = fwdZ/flen3;
        float ux = fy3*rz - fz3*0.0f;
        float uy = fz3*rx - fx3*rz;
        float uz = fx3*0.0f - fy3*rx;
        float ulen = sqrtf(ux*ux + uy*uy + uz*uz);
        if (ulen > 0.0001f) {
            s_bbUpX = ux/ulen; s_bbUpY = uy/ulen; s_bbUpZ = uz/ulen;
        } else {
            s_bbUpX = 0.0f; s_bbUpY = 1.0f; s_bbUpZ = 0.0f;
        }
    } else {
        s_bbUpX = 0.0f; s_bbUpY = 1.0f; s_bbUpZ = 0.0f;
    }

    // ---- Build frustum planes ----
    // NDS: 45° vertical FOV, 256/192 aspect → hFOV ≈ 58.7°
    // half-angle tangent for horizontal: tan(29.35°) ≈ 0.561
    // Add CHUNK_HALF_DIAG margin so edge chunks aren't culled mid-tile.
    const float MARGIN  = CHUNK_HALF_DIAG;
    const float NEAR_D  = 0.5f;   // near plane distance along fwd
    const float FAR_D   = 320.0f; // far plane distance (2x render distance)
    const float H_TAN   = 0.561f; // tan(hFOV/2) ≈ 0.561 for ~58.7° hFOV

    // Left plane: rotated (fwdX,fwdZ) by +hFOV/2, inward normal points right
    {
        float c =  H_TAN, s_v = 1.0f; // plane normal in view: n = fwd*cos + right*sin
        float mag = sqrtf(c*c + s_v*s_v);
        c /= mag; s_v /= mag;
        float nx =  fwdX * c - rx * s_v; // rotate fwd toward right
        float nz =  fwdZ * c - rz * s_v;
        // Inward normal for left plane (points right)
        frustum[0].nx =  nz;  frustum[0].nz = -nx;
        frustum[0].d  = -(frustum[0].nx * eyeX + frustum[0].nz * eyeZ) - MARGIN;
    }
    // Right plane
    {
        float c =  H_TAN, s_v = 1.0f;
        float mag = sqrtf(c*c + s_v*s_v);
        c /= mag; s_v /= mag;
        float nx =  fwdX * c + rx * s_v;
        float nz =  fwdZ * c + rz * s_v;
        // Inward normal for right plane (points left)
        frustum[1].nx = -nz;  frustum[1].nz = nx;
        frustum[1].d  = -(frustum[1].nx * eyeX + frustum[1].nz * eyeZ) - MARGIN;
    }
    // Near plane (inward normal = fwd direction)
    {
        frustum[2].nx = fwdX;  frustum[2].nz = fwdZ;
        float px = eyeX + fwdX * NEAR_D;
        float pz = eyeZ + fwdZ * NEAR_D;
        frustum[2].d = -(frustum[2].nx * px + frustum[2].nz * pz) - MARGIN;
    }
    // Far plane (inward normal = -fwd)
    {
        frustum[3].nx = -fwdX;  frustum[3].nz = -fwdZ;
        float px = eyeX + fwdX * FAR_D;
        float pz = eyeZ + fwdZ * FAR_D;
        frustum[3].d = -(frustum[3].nx * px + frustum[3].nz * pz) - MARGIN;
    }
}

// ---------------------------------------------------------------------------
// Frustum test — sphere in XZ (fast, no sqrt)
// Tests the chunk centre against all 4 planes.  Returns false (cull) only
// if the sphere is fully outside at least one plane.
// ---------------------------------------------------------------------------
bool ChunkLibrary::chunkInFrustum(s16 gx, s16 gz) const
{
    // Chunk centre in world space
    float cx = (gx + 0.5f) * CHUNK_WORLD_UNIT;
    float cz = (gz + 0.5f) * CHUNK_WORLD_UNIT;
    const float R = CHUNK_HALF_DIAG;

    for (int i = 0; i < 4; i++) {
        float dist = frustum[i].nx * cx + frustum[i].nz * cz + frustum[i].d;
        if (dist < -R) return false;  // sphere fully outside this plane
    }
    return true;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
ChunkLibrary::ChunkLibrary(MemoryManager* mem)
    : memMgr(mem), worldFd(nullptr),
      textureCount(0), worldChunkCount(0), framePolyCount(0),
      lastBoundTexId(0xFE)
{
    memset(textures,     0, sizeof(textures));
    memset(chunkDesc,    0, sizeof(chunkDesc));
    memset(activeChunks, 0, sizeof(activeChunks));
    for (int i = 0; i < CHUNK_MAX_TEXTURES; i++)
        textures[i].glTexId = -1;
    memset(frustum, 0, sizeof(frustum));
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
    if (fread(&hdr, sizeof(hdr), 1, worldFd) != 1) return false;
    if (memcmp(hdr.magic, "ALWF", 4) != 0)          return false;
    if (hdr.version != 1)                           return false;

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
        if (textures[i].data)    { free(textures[i].data);  textures[i].data = nullptr; }
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
        if (fread(textures[i].data, 1, te.dataBytes, worldFd) != te.dataBytes) return false;
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

    // Evict chunks outside streaming radius
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk& c = activeChunks[i];
        if (!c.verts) continue;
        s16 dx = c.gridX - camGX;
        s16 dz = c.gridZ - camGZ;
        if (dx < -CHUNK_GRID_RADIUS || dx > CHUNK_GRID_RADIUS ||
            dz < -CHUNK_GRID_RADIUS || dz > CHUNK_GRID_RADIUS)
            unloadChunk(&c);
    }

    // Load missing chunks within streaming radius
    for (s16 dz = -CHUNK_GRID_RADIUS; dz <= CHUNK_GRID_RADIUS; dz++) {
        for (s16 dx = -CHUNK_GRID_RADIUS; dx <= CHUNK_GRID_RADIUS; dx++) {
            s16 gx = camGX + dx;
            s16 gz = camGZ + dz;
            if (findActive(gx, gz)) continue;
            ChunkDesc* desc = findDesc(gx, gz);
            if (!desc) continue;
            Chunk* slot = findFreeSlot();
            if (!slot) return;
            loadChunk(desc, slot);
        }
    }
}

// ---------------------------------------------------------------------------
// render — frustum-culled, two-pass (opaque then billboards).
//
// Optimisations vs the original:
//  1. chunkInFrustum() early-outs invisible chunks entirely.
//  2. opaqueCount/bbStart/bbCount are pre-computed at load time — no
//     per-vertex branch to separate passes at render time.
//  3. lastBoundTexId tracks the current texture so bindTexture() is
//     a no-op when the same texture is used for adjacent verts.
//  4. glBegin/glEnd are only called once per pass per chunk instead of
//     once per texture-group (we re-bind mid-batch instead).
// ---------------------------------------------------------------------------
void ChunkLibrary::render()
{
    framePolyCount   = 0;
    lastBoundTexId   = 0xFE;  // invalid → forces first bind

    // ---- Pass 1: opaque geometry ----
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK | POLY_ID(1));

    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts || c->opaqueCount == 0) continue;
        if (!chunkInFrustum(c->gridX, c->gridZ)) continue;
        renderChunkOpaque(c);
        framePolyCount += c->polyCount;
    }

    // ---- Pass 2: billboard quads (submitted after depth buffer is settled) ----
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(2));

    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts || c->bbCount == 0) continue;
        if (!chunkInFrustum(c->gridX, c->gridZ)) continue;
        renderChunkBillboard(c);
    }
}

// ---------------------------------------------------------------------------
// renderChunkOpaque
//
// Renders the leading opaqueCount verts.  The verts were sorted by texId
// at load time (see loadChunk) so texture binds are minimised.
// ---------------------------------------------------------------------------
void ChunkLibrary::renderChunkOpaque(Chunk* c)
{
    float chunkOX = (float)(c->gridX * CHUNK_WORLD_UNIT);
    float chunkOZ = (float)(c->gridZ * CHUNK_WORLD_UNIT);

    glBegin(GL_TRIANGLES);

    for (u16 vi = 0; vi < c->opaqueCount; vi++) {
        ChunkVertex& v = c->verts[vi];

        if (v.texId != lastBoundTexId) {
            glEnd();
            bindTexture(v.texId);
            glBegin(GL_TRIANGLES);
        }

        if (v.nx | v.ny | v.nz) {
            float scale = lightScale(f32tofloat(v.nx),
                                     f32tofloat(v.ny),
                                     f32tofloat(v.nz));
            glColorLit(v.r, v.g, v.b, scale);
        } else {
            glColor3b(v.r, v.g, v.b);
        }

        if (v.texId != 0xFF) glTexCoord2t16(v.u, v.v);

        glVertex3f(
            chunkOX + f32tofloat(v.x),
                      f32tofloat(v.y),
            chunkOZ + f32tofloat(v.z)
        );
    }

    glEnd();
}

// ---------------------------------------------------------------------------
// renderChunkBillboard
//
// Renders only the billboard verts (bbStart..bbStart+bbCount-1).
// Orientation is computed per-billboard anchor (same logic as before).
// ---------------------------------------------------------------------------
void ChunkLibrary::renderChunkBillboard(Chunk* c)
{
    float chunkOX = (float)(c->gridX * CHUNK_WORLD_UNIT);
    float chunkOZ = (float)(c->gridZ * CHUNK_WORLD_UNIT);

    glBegin(GL_TRIANGLES);

    u16 end = c->bbStart + c->bbCount;
    for (u16 vi = c->bbStart; vi < end; vi++) {
        ChunkVertex& v = c->verts[vi];

        if (v.texId != lastBoundTexId) {
            glEnd();
            bindTexture(v.texId);
            glBegin(GL_TRIANGLES);
        }

        float anchorX = f32tofloat(v.x);
        float anchorY = f32tofloat(v.y);
        float anchorZ = f32tofloat(v.z);
        float offR    = f32tofloat(v.ny);
        float offU    = f32tofloat(v.nz);

        float lx, ly, lz;
        float lightNX, lightNY, lightNZ;

        if (v.nx == (s16)BILLBOARD_SENTINEL_FIX) {
            lx = chunkOX + anchorX + offR;
            ly =           anchorY + offU;
            lz = chunkOZ + anchorZ;
            lightNX = 0.0f;  lightNY = 0.0f;  lightNZ = 1.0f;

        } else if (v.nx == (s16)BILLBOARD_SENTINEL_SPH) {
            lx = chunkOX + anchorX + offR * s_bbRightX + offU * s_bbUpX;
            ly =           anchorY + offR * s_bbRightY + offU * s_bbUpY;
            lz = chunkOZ + anchorZ + offR * s_bbRightZ + offU * s_bbUpZ;
            lightNX = -s_bbUpZ;
            lightNY =  s_bbUpY;
            lightNZ =  s_bbUpX;

        } else {
            // Cylindrical
            float wax = chunkOX + anchorX;
            float waz = chunkOZ + anchorZ;
            float dX  = s_camEyeX - wax;
            float dZ  = s_camEyeZ - waz;
            float dlen = sqrtf(dX*dX + dZ*dZ);
            float crx, crz;
            if (dlen > 0.0001f) {
                dX /= dlen;  dZ /= dlen;
                crx =  dZ;  crz = -dX;
            } else {
                crx = s_bbRightX;  crz = s_bbRightZ;
            }
            lx = chunkOX + anchorX + offR * crx;
            ly =           anchorY + offU;
            lz = chunkOZ + anchorZ + offR * crz;
            lightNX = -crz;
            lightNY =  0.5f;
            lightNZ =  crx;
        }

        float scale = lightScale(lightNX, lightNY, lightNZ);
        glColorLit(v.r, v.g, v.b, scale);
        if (v.texId != 0xFF) glTexCoord2t16(v.u, v.v);
        glVertex3f(lx, ly, lz);
    }

    glEnd();
}

// ---------------------------------------------------------------------------
// bindTexture — only issues GL call when the texture actually changes.
// ---------------------------------------------------------------------------
void ChunkLibrary::bindTexture(u8 texId)
{
    if (texId == lastBoundTexId) return;
    lastBoundTexId = texId;

    if (texId == 0xFF) {
        glBindTexture(0, 0);
        return;
    }
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

// ---------------------------------------------------------------------------
// loadChunk
//
// After reading verts from disk we do two things that save work every frame:
//
//  1. Sort the vertex list so all opaque verts come first, then billboards.
//     Within each group, sort by texId to minimise GL texture binds.
//     This is O(n) using a stable counting sort on texId (256 buckets).
//
//  2. Record opaqueCount, bbStart, bbCount so the two render passes never
//     need to scan the whole array.
// ---------------------------------------------------------------------------
bool ChunkLibrary::loadChunk(ChunkDesc* desc, Chunk* slot)
{
    u32 byteSize = sizeof(ChunkVertex) * desc->vertCount;
    ChunkVertex* buf = (ChunkVertex*)malloc(byteSize);
    if (!buf) return false;

    fseek(worldFd, (long)(desc->fileOffset + sizeof(ChunkEntry)), SEEK_SET);
    size_t got = fread(buf, sizeof(ChunkVertex), desc->vertCount, worldFd);
    if (got != desc->vertCount) { free(buf); return false; }

    // ---- Sort: opaque first (by texId), then billboards (by texId) ----
    // We use a simple temp-buffer stable sort — vertex count is small (<2048).
    u16 n = desc->vertCount;

    if (n > 0) {
        ChunkVertex* tmp = (ChunkVertex*)malloc(byteSize);
        if (tmp) {
            // Count opaque and billboard verts
            u16 nOpaque = 0, nBB = 0;
            for (u16 i = 0; i < n; i++) {
                if (isBillboardNx(buf[i].nx)) nBB++;
                else                          nOpaque++;
            }

            // Counting sort by texId within each group.
            // 256 possible texId values (0x00..0xFF).
            // Two-pass: first into opaque region, then billboard region.
            u16 oCount[256] = {};
            u16 bCount[256] = {};
            for (u16 i = 0; i < n; i++) {
                if (isBillboardNx(buf[i].nx)) bCount[(u8)buf[i].texId]++;
                else                          oCount[(u8)buf[i].texId]++;
            }
            // Build prefix sums
            u16 oOff[256], bOff[256];
            u16 oRunning = 0, bRunning = nOpaque;
            for (int t = 0; t < 256; t++) {
                oOff[t] = oRunning;  oRunning += oCount[t];
                bOff[t] = bRunning;  bRunning += bCount[t];
            }
            // Scatter
            for (u16 i = 0; i < n; i++) {
                u8 tid = (u8)buf[i].texId;
                if (isBillboardNx(buf[i].nx)) tmp[bOff[tid]++] = buf[i];
                else                          tmp[oOff[tid]++] = buf[i];
            }
            memcpy(buf, tmp, byteSize);
            free(tmp);

            slot->opaqueCount = nOpaque;
            slot->bbStart     = nOpaque;
            slot->bbCount     = nBB;
        } else {
            // malloc failed — fall back to unsorted, no split
            slot->opaqueCount = n;
            slot->bbStart     = n;
            slot->bbCount     = 0;
        }
    } else {
        slot->opaqueCount = 0;
        slot->bbStart     = 0;
        slot->bbCount     = 0;
    }

    slot->verts      = buf;
    slot->gridX      = desc->gridX;
    slot->gridZ      = desc->gridZ;
    slot->vertCount  = n;
    slot->polyCount  = desc->polyCount;
    slot->usedMemMgr = false;
    return true;
}

void ChunkLibrary::unloadChunk(Chunk* slot)
{
    if (!slot->verts) return;
    if (slot->usedMemMgr) memMgr->freePage(slot->verts);
    else                  free(slot->verts);
    slot->verts        = nullptr;
    slot->vertCount    = 0;
    slot->polyCount    = 0;
    slot->opaqueCount  = 0;
    slot->bbStart      = 0;
    slot->bbCount      = 0;
    slot->usedMemMgr   = false;
}

u32 ChunkLibrary::loadedChunkCount() const
{
    u32 n = 0;
    for (int i = 0; i < CHUNK_GRID_SIZE; i++)
        if (activeChunks[i].verts) n++;
    return n;
}
