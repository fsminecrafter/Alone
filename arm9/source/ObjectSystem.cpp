#include "ObjectSystem.h"
#include "AudioSystem.h"
#include <string.h>

// ---- globals ----
ObjectSystem g_objects;
ScriptRegistration* ObjectSystem::s_scriptRegistry = nullptr;

// External time state (from main.cpp)
extern int s_hour;
extern int s_season;

// ---------------------------------------------------------------------------
// ObjectSystem
// ---------------------------------------------------------------------------
ObjectSystem::ObjectSystem()
    : objCount(0), tagCount(0), _frame(0),
      _camX(0), _camY(0), _camZ(0), _startFired(false)
{
    memset(objects, 0, sizeof(objects));
    memset(tags,    0, sizeof(tags));
}

ObjectSystem::~ObjectSystem() { unload(); }

// ---------------------------------------------------------------------------
// Script registry
// ---------------------------------------------------------------------------
void ObjectSystem::registerScript(ScriptRegistration* reg)
{
    reg->next = s_scriptRegistry;
    s_scriptRegistry = reg;
}

ScriptComponent* ObjectSystem::createScript(const char* name)
{
    for (ScriptRegistration* r = s_scriptRegistry; r; r = r->next)
        if (strcmp(r->name, name) == 0)
            return r->factory();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Load from .world v2 file
// The file pointer must be positioned right after the last ChunkEntry.
// We do a safe read: if the magic doesn't match we simply return true
// (pre-v2 worlds have no object/tag/audio sections).
// ---------------------------------------------------------------------------
bool ObjectSystem::loadFromWorld(FILE* fd)
{
    // --- Objects ---
    ObjectTableHeader oh;
    if (fread(&oh, sizeof(oh), 1, fd) != 1) return true;
    
    // Check if this is actually an OBJS header or an old format
    if (memcmp(oh.magic, "OBJS", 4) == 0) {
        // v2 format with objects
        objCount = (oh.objectCount < OBJ_MAX_OBJECTS) ? oh.objectCount : OBJ_MAX_OBJECTS;
        for (u16 i = 0; i < objCount; i++) {
            ObjectEntry oe;
            if (fread(&oe, sizeof(oe), 1, fd) != 1) return false;
            WorldObject& o = objects[i];
            o.id      = oe.id;
            memcpy(o.name, oe.name, OBJ_MAX_NAME);
            o.x       = (float)oe.worldX / 4096.0f;
            o.y       = (float)oe.worldY / 4096.0f;
            o.z       = (float)oe.worldZ / 4096.0f;
            o.rotY    = 0.0f;
            o.tagMask = oe.tagMask;
            o.flags   = oe.flags;
            o.active  = (oe.flags & OBJ_FLAG_ACTIVE) != 0;
            o.scriptCount = 0;
            memset(o.scripts, 0, sizeof(o.scripts));
        }

        // --- Tags ---
        TagTableHeader th;
        if (fread(&th, sizeof(th), 1, fd) != 1) return true;
        if (memcmp(th.magic, "TAGS", 4) != 0)   return true;

        tagCount = (th.tagCount < OBJ_MAX_TAGS) ? th.tagCount : OBJ_MAX_TAGS;
        for (u8 t = 0; t < tagCount; t++) {
            TagEntry te;
            if (fread(&te, sizeof(te), 1, fd) != 1) return false;
            RuntimeTag& rt = tags[t];
            rt.index = te.tagIndex;
            memcpy(rt.name, te.name, OBJ_MAX_TAG_NAME);
            rt.memberCount = 0;
            u16 count = (te.memberCount < OBJ_MAX_OBJECTS) ? te.memberCount : OBJ_MAX_OBJECTS;
            for (u16 m = 0; m < te.memberCount; m++) {
                u16 oid;
                if (fread(&oid, sizeof(u16), 1, fd) != 1) return false;
                if (m < count) {
                    rt.members[rt.memberCount++] = oid;
                }
            }
        }
    } else if (memcmp(oh.magic, "AUDI", 4) == 0) {
        // No OBJS/TAGS, but AUDI is right here; seek back to read it
        fseek(fd, -(long)sizeof(oh), SEEK_CUR);
    } else {
        // Old format with no v2 sections
        return true;
    }

    // --- Audio ---
    // Delegated to AudioSystem
    AudioTableHeader ah;
    if (fread(&ah, sizeof(ah), 1, fd) != 1) return true;
    if (memcmp(ah.magic, "AUDI", 4) != 0)   return true;

    g_audio.loadFromWorld(fd, ah.trackCount, ah.emitterCount);
    return true;
}

void ObjectSystem::unload()
{
    // free scripts
    for (u16 i = 0; i < objCount; i++) {
        WorldObject& o = objects[i];
        for (u8 s = 0; s < o.scriptCount; s++) {
            delete o.scripts[s];
            o.scripts[s] = nullptr;
        }
        o.scriptCount = 0;
    }
    objCount  = 0;
    tagCount  = 0;
    _startFired = false;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
void ObjectSystem::update(int frame, float camX, float camY, float camZ)
{
    _frame = frame;
    _camX = camX; _camY = camY; _camZ = camZ;

    bool doLower = (frame % 2 == 0);

    for (u16 i = 0; i < objCount; i++) {
        WorldObject& o = objects[i];
        if (!o.active) continue;
        for (u8 s = 0; s < o.scriptCount; s++) {
            if (!o.scripts[s]) continue;
            o.scripts[s]->update();
            if (doLower) o.scripts[s]->lowerUpdate();
        }
    }
}

void ObjectSystem::fireStart()
{
    if (_startFired) return;
    _startFired = true;
    for (u16 i = 0; i < objCount; i++) {
        WorldObject& o = objects[i];
        for (u8 s = 0; s < o.scriptCount; s++)
            if (o.scripts[s]) o.scripts[s]->start();
    }
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------
WorldObject* ObjectSystem::findByName(const char* name)
{
    for (u16 i = 0; i < objCount; i++)
        if (strncmp(objects[i].name, name, OBJ_MAX_NAME) == 0)
            return &objects[i];
    return nullptr;
}

WorldObject* ObjectSystem::findById(u16 id)
{
    for (u16 i = 0; i < objCount; i++)
        if (objects[i].id == id)
            return &objects[i];
    return nullptr;
}

int ObjectSystem::findByTag(const char* tagName, u16* outIds, int maxOut) const
{
    int ti = tagIndexByName(tagName);
    if (ti < 0) return 0;
    const RuntimeTag& rt = tags[ti];
    int n = (rt.memberCount < (u16)maxOut) ? rt.memberCount : (u16)maxOut;
    for (int i = 0; i < n; i++) outIds[i] = rt.members[i];
    return n;
}

bool ObjectSystem::objectHasTag(u16 objectId, const char* tagName) const
{
    int ti = tagIndexByName(tagName);
    if (ti < 0) return false;
    if (ti >= 32) return false;
    WorldObject* o = const_cast<ObjectSystem*>(this)->findById(objectId);
    if (!o) return false;
    return (o->tagMask & (1u << ti)) != 0;
}

bool ObjectSystem::attachScript(u16 objectId, const char* scriptName)
{
    WorldObject* o = findById(objectId);
    if (!o) return false;
    if (o->flags & OBJ_FLAG_FLOOR_CHUNK) return false;  // no scripts on floor/chunk
    if (o->scriptCount >= OBJ_MAX_SCRIPTS_PER_OBJ) return false;

    ScriptComponent* sc = createScript(scriptName);
    if (!sc) return false;

    sc->object = o;
    o->scripts[o->scriptCount++] = sc;

    if (_startFired) sc->start();
    return true;
}

int ObjectSystem::tagIndexByName(const char* name) const
{
    for (u8 i = 0; i < tagCount; i++)
        if (strncmp(tags[i].name, name, OBJ_MAX_TAG_NAME) == 0)
            return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// ScriptComponent helpers
// ---------------------------------------------------------------------------
void ScriptComponent::moveTo(float tx, float ty, float tz)
{
    if (!object) return;
    object->x = tx; object->y = ty; object->z = tz;
}

void ScriptComponent::moveBy(float dx, float dy, float dz)
{
    if (!object) return;
    object->x += dx; object->y += dy; object->z += dz;
}

void ScriptComponent::moveForward(float dist)
{
    if (!object) return;
    float rad = object->rotY * (3.14159f / 180.0f);
    object->x -= sinf(rad) * dist;
    object->z -= cosf(rad) * dist;
}

void ScriptComponent::rotateY(float deg)
{
    if (!object) return;
    object->rotY += deg;
    while (object->rotY >= 360.0f) object->rotY -= 360.0f;
    while (object->rotY <    0.0f) object->rotY += 360.0f;
}

void ScriptComponent::rotateTo(float deg)
{
    if (!object) return;
    object->rotY = deg;
    while (object->rotY >= 360.0f) object->rotY -= 360.0f;
    while (object->rotY <    0.0f) object->rotY += 360.0f;
}

void ScriptComponent::setObjectColor(u8 r, u8 g, u8 b)
{
    // Color changes are deferred: ChunkLibrary re-tints the object on next render
    // by looking up a per-object color override table (see ChunkLibrary additions).
    // For now we store the request on the object via an extension field.
    // (Requires ChunkLibrary patching to read g_objects overrides.)
    (void)r; (void)g; (void)b;
}

bool ScriptComponent::isDay() const
{
    extern int s_hour;
    return s_hour >= 6 && s_hour < 20;
}

int ScriptComponent::getHour() const
{
    extern int s_hour;
    return s_hour;
}

int ScriptComponent::getSeason() const
{
    extern int s_season;
    return s_season;
}

float ScriptComponent::distToCamera() const
{
    if (!object) return 9999.0f;
    float dx = object->x - g_objects.getCamX();
    float dy = object->y - g_objects.getCamY();
    float dz = object->z - g_objects.getCamZ();
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

void ScriptComponent::playSound(const char* filename, u8 volume)
{
    g_audio.playEmitterOnce(filename, object ? object->x : 0,
                             object ? object->y : 0,
                             object ? object->z : 0, volume);
}

void ScriptComponent::stopSound()
{
    if (_channelId >= 0) {
        g_audio.stopChannel(_channelId);
        _channelId = -1;
    }
}
