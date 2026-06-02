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
// Camera eye position and forward vector — used for visibility culling and per-billboard rotation.
static float s_camEyeX = 0.0f;
static float s_camEyeY = 0.0f;
static float s_camEyeZ = 0.0f;
static float s_camFwdX = 0.0f;
static float s_camFwdY = 0.0f;
static float s_camFwdZ = 1.0f;
// Ground-level camera target — used for cylindrical billboard facing.
// Unlike s_camEye, this is NOT elevated by the camera rig offset.
static float s_camTgtX = 0.0f;
static float s_camTgtZ = 0.0f;

static void updateCameraVectors(float eyeX, float eyeY, float eyeZ,
                                 float tgtX, float tgtY, float tgtZ)
{
    // Full 3-D forward (eye -> target).
    float fwdX = tgtX - eyeX;
    float fwdY = tgtY - eyeY;
    float fwdZ = tgtZ - eyeZ;
    float flen = sqrtf(fwdX*fwdX + fwdY*fwdY + fwdZ*fwdZ);
    if (flen < 0.0001f) {
        s_bbRightX = 1.0f; s_bbRightZ = 0.0f;
        s_bbUpX = 0.0f; s_bbUpY = 1.0f; s_bbUpZ = 0.0f;
        s_camFwdX = 0.0f; s_camFwdY = 0.0f; s_camFwdZ = 1.0f;
        return;
    }
    fwdX /= flen; fwdY /= flen; fwdZ /= flen;
    s_camFwdX = fwdX; s_camFwdY = fwdY; s_camFwdZ = fwdZ;

    // right = forward x world-up(0,1,0)  =>  (-fwd.z, 0, fwd.x)
    float rx = -fwdZ;
    float rz =  fwdX;
    float rlen = sqrtf(rx*rx + rz*rz);
    if (rlen < 0.0001f) {
        // Camera pointing straight down/up; use world +X as fallback
        s_bbRightX = 1.0f; s_bbRightZ = 0.0f;
        s_bbUpX = 0.0f; s_bbUpY = 1.0f; s_bbUpZ = 0.0f;
        s_camEyeX = eyeX; s_camEyeY = eyeY; s_camEyeZ = eyeZ;
        return;
    }
    s_bbRightX = rx / rlen;
    s_bbRightZ = rz / rlen;

    // Store eye for per-billboard cylindrical right computation
    s_camEyeX = eyeX; s_camEyeY = eyeY; s_camEyeZ = eyeZ;
    s_camTgtX = tgtX; s_camTgtZ = tgtZ;  // ground target for cylindrical billboard facing

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
// Render — two passes so transparent billboards sort correctly.
//
// NDS transparency rules:
//   • POLY_ALPHA 31  = fully opaque.  GPU ignores the texture's alpha channel
//     entirely — A1RGB5 bit-15 is discarded, so sprites render as solid quads.
//   • POLY_ALPHA 1-30 = translucent.  GPU honours the texture alpha channel
//     (bit-15 for A1RGB5 cutout; 3-bit or 5-bit alpha for RGB32_A3/RGB8_A5).
//     Translucent polys MUST be drawn AFTER all opaque polys so the depth
//     buffer is already settled, and MUST use a POLY_ID different from all
//     opaque polys so edge-marking doesn't bleed across the boundary.
//
// Pass 1: opaque terrain/objects — POLY_ID 1, POLY_ALPHA 31
// Pass 2: billboard quads        — POLY_ID 2, POLY_ALPHA 30
//   POLY_ALPHA 30 (one step below opaque) activates blending while looking
//   visually identical to fully opaque where the texture is solid.
//   For A1RGB5 textures this gives clean 1-bit cutout edges.
//   For A3/A5 textures this gives smooth partial transparency.
// ---------------------------------------------------------------------------
void ChunkLibrary::render()
{
    framePolyCount = 0;

    // Pass 1 — terrain / opaque objects
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts) continue;

        // Chunk-level visibility culling
        float cx = (float)(c->gridX * CHUNK_WORLD_UNIT);
        float cz = (float)(c->gridZ * CHUNK_WORLD_UNIT);
        float dx = cx - s_camEyeX;
        float dz = cz - s_camEyeZ;
        float dotFwd = dx * s_camFwdX + dz * s_camFwdZ;
        float distSq = dx*dx + dz*dz;

        // If chunk is too far or significantly behind camera, skip it
        if (distSq > 300.0f * 300.0f) continue;
        if (dotFwd < -50.0f && distSq > 50.0f * 50.0f) continue;

        renderChunk(c, i, false);
        framePolyCount += c->polyCount;
    }

    // Pass 2 — billboard quads (drawn after depth buffer is settled)
    for (int i = 0; i < CHUNK_GRID_SIZE; i++) {
        Chunk* c = &activeChunks[i];
        if (!c->verts) continue;

        float cx = (float)(c->gridX * CHUNK_WORLD_UNIT);
        float cz = (float)(c->gridZ * CHUNK_WORLD_UNIT);
        float dx = cx - s_camEyeX;
        float dz = cz - s_camEyeZ;
        float dotFwd = dx * s_camFwdX + dz * s_camFwdZ;
        float distSq = dx*dx + dz*dz;

        if (distSq > 300.0f * 300.0f) continue;
        if (dotFwd < -50.0f && distSq > 50.0f * 50.0f) continue;

        renderChunk(c, i, true);
    }
}


// Helper function to check if a texture is transparent based on its format.
// NDS formats with alpha support are considered potentially transparent.
bool ChunkLibrary::isTextureTransparent(u8 texId)
{
    if (texId == NO_TEX) return false;

    for (u16 i = 0; i < textureCount; i++) {
        if (textures[i].id == texId) {
            u8 format = textures[i].format;
            // GL_RGBA (8) is never written to .world files because the NDS hardware
            // texture format field is 3 bits; 8 & 7 = 0 = no texture.
            // Direct-color textures are exported as GL_RGB (7) = A1BGR5.
            // GL_RGB (7), GL_RGB32_A3 (1), and GL_RGB8_A5 (6) all carry alpha bits.
            return (format == GL_RGB   ||   // direct color A1BGR5 (was GL_RGBA, now stored as 7)
                    format == GL_RGBA  ||   // kept for files exported before this fix
                    format == GL_RGB32_A3 ||
                    format == GL_RGB8_A5);
        }
    }
    return false;  // unknown texId — treat as opaque, not transparent
}

void ChunkLibrary::renderChunk(Chunk* c, int debugIdx, bool billboardsOnly)
{
    // Chunk origin in world space.
    float chunkOX = (float)(c->gridX * CHUNK_WORLD_UNIT);
    float chunkOZ = (float)(c->gridZ * CHUNK_WORLD_UNIT);

    // ALWAYS translate by chunk origin for both passes.
    // This keeps local verts in the NDS 4.12 fixed-point range [-8, 8].
    // Using different matrices doesn't break depth sorting on NDS; the hardware
    // sorts translucent polygons based on their final clip-space depth and POLY_ID.
    glPushMatrix();
    glTranslatef(chunkOX, 0.0f, chunkOZ);

    // Pass 1: opaque terrain — POLY_ID 1, POLY_ALPHA 31.
    // Pass 2: billboard quads — UNIQUE POLY_ID per chunk (2-26), POLY_ALPHA 30.
    // Assigning a unique POLY_ID per chunk for translucent geometry allows the
    // NDS hardware to sort them correctly by depth across different chunks.
    const int polyId    = billboardsOnly ? (debugIdx + 2) : 1;
    const int polyAlpha = billboardsOnly ? 30 : 31;

    glPolyFmt(POLY_ALPHA(polyAlpha) | POLY_CULL_NONE | POLY_ID(polyId));

    u8   lastTexId   = 0xFE;

    // Triangle-alignment tracking for degenerate-vertex fix.
    // When a vertex is skipped mid-triangle we must still emit a placeholder
    // so the GPU's vertex counter stays aligned.  We collapse the degenerate
    // triangle to a zero-area point by repeating the first vertex of that tri.
    u16  triVert  = 0;   // 0, 1, 2 within the current triangle
    float degX = 0.0f, degY = 0.0f, degZ = 0.0f; // first vertex of current tri

    glBegin(GL_TRIANGLES);

    for (u16 vi = 0; vi < c->vertCount; vi++) {
        ChunkVertex& v = c->verts[vi];

        bool isBB = isBillboardNx(v.nx);
        bool isTexTransparent = isTextureTransparent(v.texId);

        // Determine if this vertex belongs to the current rendering pass.
        bool wantsTransparentPass = isBB || isTexTransparent;
        bool renderInThisPass     = (billboardsOnly == wantsTransparentPass);

        if (!renderInThisPass) {
            // Emit a degenerate vertex to keep tri alignment intact.
            // The first vert of the tri saves its position; subsequent skipped
            // verts repeat it, collapsing the triangle to a zero-area point.
            if (triVert == 0) {
                // Will be filled by the first real vertex below, but this tri
                // started with a skip — save a neutral position and emit it.
                glVertex3f(degX, degY, degZ);
            } else {
                glVertex3f(degX, degY, degZ);
            }
            triVert = (triVert + 1) % 3;
            continue;
        }

        // Texture-change flush must NOT split a triangle mid-way.
        // Only flush at a triangle boundary (triVert == 0).
        if (v.texId != lastTexId && triVert == 0) {
            glEnd();
            bindTexture(v.texId);
            // Re-apply poly format with the same polyId/polyAlpha.
            glPolyFmt(POLY_ALPHA(polyAlpha) | POLY_CULL_NONE | POLY_ID(polyId));
            glBegin(GL_TRIANGLES);
            lastTexId = v.texId;
        }

        if (isBB) {
            // Billboard vertex:
            //   v.x/y/z = chunk-local anchor in NDS f32 (same for all 6 verts)
            //   v.ny    = camera-right offset in NDS f32 (+/-half-width)
            //   v.nz    = world-up offset in NDS f32 (0=bottom, height=top)
            //   v.nx    = sentinel encoding the mode
            float anchorX = f32tofloat(v.x);
            float anchorY = f32tofloat(v.y);
            float anchorZ = f32tofloat(v.z);

            // World-space anchor for per-billboard rotation calculation.
            // chunkOX/OZ are already applied by the glTranslatef matrix above,
            // so we only add them here for the facing-direction math, NOT for
            // the final vertex position (which stays in chunk-local space).
            float worldX = anchorX + chunkOX;
            float worldZ = anchorZ + chunkOZ;

            float offR = f32tofloat(v.ny);   // spread along right
            float offU = f32tofloat(v.nz);   // rise along up

            float lx, ly, lz;
            float lightNX, lightNY, lightNZ;

            if (v.nx == (s16)BILLBOARD_SENTINEL_FIX) {
                // Fixed: always spread along world +X, rise along world +Y.
                lx = anchorX + offR;
                ly = anchorY + offU;
                lz = anchorZ;
                lightNX = 0.0f; lightNY = 0.5f; lightNZ = 1.0f;

            } else if (v.nx == (s16)BILLBOARD_SENTINEL_SPH) {
                // Spherical (0x7FFE): face camera fully.
                float vdx = s_camEyeX - worldX;
                float vdy = s_camEyeY - anchorY;
                float vdz = s_camEyeZ - worldZ;
                float vlen = sqrtf(vdx*vdx + vdy*vdy + vdz*vdz);
                float rx, ry, rz, ux, uy, uz;
                if (vlen > 0.001f) {
                    vdx /= vlen; vdy /= vlen; vdz /= vlen;
                    // right = cross((0,1,0), forward)
                    rx = -vdz; ry = 0; rz = vdx;
                    float rlen = sqrtf(rx*rx + rz*rz);
                    if (rlen > 0.001f) { rx /= rlen; rz /= rlen; }
                    else { rx = 1; rz = 0; }
                    // up = cross(forward, right)
                    ux = vdy * rz - vdz * ry;
                    uy = vdz * rx - vdx * rz;
                    uz = vdx * ry - vdy * rx;
                } else {
                    rx = s_bbRightX; ry = 0; rz = s_bbRightZ;
                    ux = s_bbUpX; uy = s_bbUpY; uz = s_bbUpZ;
                }
                lx = anchorX + offR * rx + offU * ux;
                ly = anchorY + offR * ry + offU * uy;
                lz = anchorZ + offR * rz + offU * uz;
                lightNX = vdx; lightNY = vdy; lightNZ = vdz;

            } else {
                // Cylindrical (default 0x7FFF): rotate around world Y only.
                // Faces the camera eye directly for correct look at any X position.
                float toEyeX = s_camTgtX - worldX;
                float toEyeZ = s_camTgtZ - worldZ;
                float tlen = sqrtf(toEyeX*toEyeX + toEyeZ*toEyeZ);
                float crx, crz;
                if (tlen > 0.01f) {
                    crx = -toEyeZ / tlen;
                    crz =  toEyeX / tlen;
                } else {
                    crx = s_bbRightX;
                    crz = s_bbRightZ;
                }

                lx = anchorX + offR * crx;
                ly = anchorY + offU;
                lz = anchorZ + offR * crz;
                lightNX = -crx; lightNY = 0.5f; lightNZ = -crz;
            }

            float scale = lightScale(lightNX, lightNY, lightNZ);
            glColorLit(v.r, v.g, v.b, scale);
            if (v.texId != NO_TEX && textureCount > 0) {
                glTexCoord2t16(v.u, v.v);
            }
            // Save first-vert position so skipped verts can degenerate to it.
            if (triVert == 0) { degX = lx; degY = ly; degZ = lz; }
            glVertex3f(lx, ly, lz);
            triVert = (triVert + 1) % 3;

        } else {
            // Normal geometry vertex
            float lx = f32tofloat(v.x);
            float ly = f32tofloat(v.y);
            float lz = f32tofloat(v.z);
            if (v.nx || v.ny || v.nz) {
                float scale = lightScale(f32tofloat(v.nx),
                                         f32tofloat(v.ny),
                                         f32tofloat(v.nz));
                if (isTexTransparent) {
                    glColor3b(255, 255, 255);
                } else {
                    glColorLit(v.r, v.g, v.b, scale);
                }
            } else {
                glColor3b(v.r, v.g, v.b);
            }
            if (v.texId != 0xFF)
                glTexCoord2t16(v.u, v.v);
            // Save first-vert position so skipped verts can degenerate to it.
            if (triVert == 0) { degX = lx; degY = ly; degZ = lz; }
            glVertex3f(lx, ly, lz);
            triVert = (triVert + 1) % 3;
        }
    }

    glEnd();
    glPopMatrix(1);
}

void ChunkLibrary::bindTexture(u8 texId)
{
    if (texId == NO_TEX) { glBindTexture(0, 0); return; }
    for (u16 i = 0; i < textureCount; i++) {
        if (textures[i].id == texId && textures[i].glTexId >= 0) {
            glBindTexture(0, textures[i].glTexId);
            return;
        }
    }
    // Texture not found/uploaded — unbind so we don't render garbage
    glBindTexture(0, 0);
}

void ChunkLibrary::setCamera(float eyeX, float eyeY, float eyeZ,
                           float tgtX, float tgtY, float tgtZ)
{
    updateCameraVectors(eyeX, eyeY, eyeZ, tgtX, tgtY, tgtZ);
}

void ChunkLibrary::getChunkInfo(u32 idx, s16& gx, s16& gz, u16& vc) const
{
    if (idx >= worldChunkCount) {
        gx = 0; gz = 0; vc = 0;
        return;
    }
    gx = chunkDesc[idx].gridX;
    gz = chunkDesc[idx].gridZ;
    vc = chunkDesc[idx].vertCount;
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
