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
// Camera state — set every frame via ChunkLibrary::setCamera()
// ---------------------------------------------------------------------------
// All values are in world space.  Must be kept in sync with the OpenGL
// view matrix (call setCamera right after gluLookAt / applyCamera).
//
// Cylindrical mode uses s_camEyeX/Z to compute a per-billboard right
// vector so each sprite independently faces the camera around the Y axis.
//
// Spherical and fixed modes use the pre-computed s_bbRightX/Y/Z and
// s_bbUpX/Y/Z that are consistent with the camera orientation.

static float s_camEyeX = 0.0f;
static float s_camEyeY = 0.0f;
static float s_camEyeZ = 0.0f;

// Camera-space right vector (full 3-D, used for spherical billboards).
static float s_bbRightX = 1.0f;
static float s_bbRightY = 0.0f;
static float s_bbRightZ = 0.0f;

// Camera-space up vector (used for spherical billboards).
static float s_bbUpX = 0.0f;
static float s_bbUpY = 1.0f;
static float s_bbUpZ = 0.0f;

// ---------------------------------------------------------------------------
// updateCameraVectors — derived from eye & target, stored for the render pass
// ---------------------------------------------------------------------------
static void updateCameraVectors(float eyeX, float eyeY, float eyeZ,
                                 float tgtX, float tgtY, float tgtZ)
{
    s_camEyeX = eyeX;
    s_camEyeY = eyeY;
    s_camEyeZ = eyeZ;

    // Forward vector: camera looks from eye toward target.
    float fwdX = tgtX - eyeX;
    float fwdY = tgtY - eyeY;
    float fwdZ = tgtZ - eyeZ;
    float flen = sqrtf(fwdX*fwdX + fwdY*fwdY + fwdZ*fwdZ);
    if (flen < 0.0001f) {
        // Degenerate: eye == target.  Keep existing vectors.
        return;
    }
    fwdX /= flen;  fwdY /= flen;  fwdZ /= flen;

    // Camera-right = world_up × forward = (0,1,0) × fwd
    //              = (1*fwdZ - 0*fwdY,  0*fwdX - 0*fwdZ,  0*fwdY - 1*fwdX)
    //              = (fwdZ, 0, -fwdX)   (always horizontal)
    float rx = fwdZ;
    float rz = -fwdX;
    float rlen = sqrtf(rx*rx + rz*rz);
    if (rlen < 0.0001f) {
        // Camera pointing straight up or down — use world +X as fallback right.
        s_bbRightX = 1.0f;  s_bbRightY = 0.0f;  s_bbRightZ = 0.0f;
        // Up is forward rotated 90° around right: just use -Z or +Z.
        s_bbUpX = 0.0f;  s_bbUpY = 0.0f;  s_bbUpZ = (fwdY > 0.0f) ? -1.0f : 1.0f;
        return;
    }
    s_bbRightX = rx / rlen;
    s_bbRightY = 0.0f;        // right is always horizontal (world_up × fwd)
    s_bbRightZ = rz / rlen;

    // Camera-up = forward × right  (points screen-upward for spherical billboards)
    float ux = fwdY * s_bbRightZ - fwdZ * 0.0f;
    float uy = fwdZ * s_bbRightX - fwdX * s_bbRightZ;
    float uz = fwdX * 0.0f       - fwdY * s_bbRightX;
    float ulen = sqrtf(ux*ux + uy*uy + uz*uz);
    if (ulen > 0.0001f) {
        s_bbUpX = ux / ulen;
        s_bbUpY = uy / ulen;
        s_bbUpZ = uz / ulen;
    } else {
        s_bbUpX = 0.0f;  s_bbUpY = 1.0f;  s_bbUpZ = 0.0f;
    }
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
// setCamera — MUST be called every frame before render(), right after the
// OpenGL view matrix is set (i.e. right after applyCamera / gluLookAt).
// ---------------------------------------------------------------------------
void ChunkLibrary::setCamera(float eyeX, float eyeY, float eyeZ,
                              float tgtX, float tgtY, float tgtZ)
{
    updateCameraVectors(eyeX, eyeY, eyeZ, tgtX, tgtY, tgtZ);
}

// ---------------------------------------------------------------------------
// Render — two passes so billboard transparency composites correctly.
//
// NDS transparency note:
//   Translucent polys (POLY_ALPHA 1-30) require a POLY_ID different from
//   all opaque polys AND must be submitted AFTER opaque geometry so the
//   depth buffer is already populated.  We use POLY_ID 1 for terrain and
//   POLY_ID 2 for billboard quads (even though they use alpha-test / fully
//   opaque pixels here; the separate pass future-proofs cutout transparency).
// ---------------------------------------------------------------------------
void ChunkLibrary::render()
{
    framePolyCount = 0;

    // Pass 1 — terrain / opaque geometry
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts) continue;
        renderChunk(c, false);
        framePolyCount += c->polyCount;
    }

    // Pass 2 — billboard quads (after depth buffer is settled)
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts) continue;
        renderChunk(c, true);
    }
}

// ---------------------------------------------------------------------------
// renderChunk
//
// billboardsOnly = false  → render normal geometry verts (nx not a sentinel)
// billboardsOnly = true   → render billboard verts only  (nx is a sentinel)
//
// Billboard vertex layout (set by editor export):
//   v.x, v.y, v.z  = chunk-local anchor in NDS f32 (same for all 6 verts)
//   v.nx           = mode sentinel (BILLBOARD_SENTINEL / SPH / FIX)
//   v.ny           = camera-RIGHT offset in NDS f32 (+/-half-width)
//   v.nz           = world-UP offset in NDS f32 (0 = bottom edge, height = top)
//
// Final world position of each vertex:
//   world_pos = chunk_origin + anchor + right_vec*ny + up_vec*nz
// ---------------------------------------------------------------------------
void ChunkLibrary::renderChunk(Chunk* c, bool billboardsOnly)
{
    float chunkOX = (float)(c->gridX * CHUNK_WORLD_UNIT);
    float chunkOZ = (float)(c->gridZ * CHUNK_WORLD_UNIT);

    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(billboardsOnly ? 2 : 1));

    u8 lastTexId = 0xFE;

    glBegin(GL_TRIANGLES);

    for (u16 vi = 0; vi < c->vertCount; vi++) {
        ChunkVertex& v = c->verts[vi];

        bool isBB = isBillboardNx(v.nx);
        if (isBB != billboardsOnly) continue;

        // ---- Texture change ----
        if (v.texId != lastTexId) {
            glEnd();
            bindTexture(v.texId);
            glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(billboardsOnly ? 2 : 1));
            glBegin(GL_TRIANGLES);
            lastTexId = v.texId;
        }

        if (isBB) {
            // ------------------------------------------------------------------
            // Billboard vertex
            // ------------------------------------------------------------------
            float anchorX = f32tofloat(v.x);
            float anchorY = f32tofloat(v.y);
            float anchorZ = f32tofloat(v.z);
            float offR    = f32tofloat(v.ny);   // offset along right
            float offU    = f32tofloat(v.nz);   // offset along up

            float lx, ly, lz;
            float lightNX, lightNY, lightNZ;

            if (v.nx == (s16)BILLBOARD_SENTINEL_FIX) {
                // ---- Fixed mode ----
                // Always spread along world +X, rise along world +Y.
                // Face normal points along world +Z.
                lx = chunkOX + anchorX + offR;
                ly =           anchorY + offU;
                lz = chunkOZ + anchorZ;
                lightNX = 0.0f;  lightNY = 0.0f;  lightNZ = 1.0f;

            } else if (v.nx == (s16)BILLBOARD_SENTINEL_SPH) {
                // ---- Spherical mode ----
                // Spread along camera right, rise along camera up.
                // s_bbRightX/Y/Z and s_bbUpX/Y/Z are set by setCamera().
                lx = chunkOX + anchorX + offR * s_bbRightX + offU * s_bbUpX;
                ly =           anchorY + offR * s_bbRightY + offU * s_bbUpY;
                lz = chunkOZ + anchorZ + offR * s_bbRightZ + offU * s_bbUpZ;
                // Light normal = camera forward (face points at camera).
                // Use camera up as a reasonable approximation for diffuse.
                lightNX = -s_bbUpZ;
                lightNY =  s_bbUpY;
                lightNZ =  s_bbUpX;

            } else {
                // ---- Cylindrical mode (default 0x7FFF) ----
                //
                // The billboard stays vertical (up = world Y).
                // It rotates around the Y axis so its face always points
                // toward the camera eye position projected onto the XZ plane.
                //
                // Compute the per-billboard right vector:
                //   dX = s_camEyeX - world_anchor_X   (camera → anchor, XZ only)
                //   dZ = s_camEyeZ - world_anchor_Z
                //   right = normalize(dX, dZ) rotated 90° CW around Y:
                //           (x, z) → (z, -x)
                //
                // This gives a smooth, continuous rotation with no sudden flips
                // because (dZ, -dX) is the perpendicular to the camera→anchor
                // direction, and it varies continuously as the camera orbits.
                //
                // Requires setCamera() to have been called this frame so that
                // s_camEyeX/Z hold the correct world-space eye position.

                float wax = chunkOX + anchorX;
                float waz = chunkOZ + anchorZ;
                float dX  = s_camEyeX - wax;
                float dZ  = s_camEyeZ - waz;
                float dlen = sqrtf(dX*dX + dZ*dZ);

                float crx, crz;
                if (dlen > 0.0001f) {
                    dX /= dlen;  dZ /= dlen;
                    // 90° CW rotation: (x, z) → (z, -x)
                    crx =  dZ;
                    crz = -dX;
                } else {
                    // Camera eye is directly above this billboard anchor.
                    // Fall back to the global camera right vector.
                    crx = s_bbRightX;
                    crz = s_bbRightZ;
                }

                lx = chunkOX + anchorX + offR * crx;
                ly =           anchorY + offU;          // world Y always vertical
                lz = chunkOZ + anchorZ + offR * crz;
                // Face normal is perpendicular to right, pointing toward camera.
                lightNX = -crz;
                lightNY =  0.5f;    // slight upward bias for nicer ambient shading
                lightNZ =  crx;
            }

            // Apply diffuse lighting using the face normal we computed.
            float scale = lightScale(lightNX, lightNY, lightNZ);
            glColorLit(v.r, v.g, v.b, scale);
            if (v.texId != 0xFF)
                glTexCoord2t16(v.u, v.v);
            glVertex3f(lx, ly, lz);

        } else {
            // ------------------------------------------------------------------
            // Normal geometry vertex
            // ------------------------------------------------------------------
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
            glVertex3f(
                chunkOX + f32tofloat(v.x),
                          f32tofloat(v.y),
                chunkOZ + f32tofloat(v.z)
            );
        }
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
